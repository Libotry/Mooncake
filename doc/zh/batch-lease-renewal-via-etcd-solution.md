# 基于定时器批量扫描的 Lease 续约同步方案

## 方案概述

使用定时器批量扫描哪些 metadata 在周期内续过约，然后批量写入 etcd，而不是每次 `GrantLease` 都立即写入。

---

## 方案设计

### 1. 核心思路

**问题**：
- 每次 `GetReplicaList` 和 `ExistKey` 都会续约 lease
- 如果每次都写入 etcd，频率可能很高（< 15,000 次/秒）

**解决方案**：
- 使用**定时器批量扫描**，定期检查哪些对象的 lease 被续约了
- **批量写入** etcd，减少写入频率
- 使用**增量同步**：只同步"续约时间"在扫描周期内的对象

---

### 2. 数据结构设计

#### 2.1 Metadata 扩展

```cpp
struct ObjectMetadata {
    std::vector<Replica> replicas;
    size_t size;
    std::chrono::steady_clock::time_point lease_timeout;
    std::optional<std::chrono::steady_clock::time_point> soft_pin_timeout;
    
    // 新增：记录 lease 续约时间（用于批量扫描）
    std::chrono::steady_clock::time_point last_lease_renew_time;
    bool lease_renewed_since_last_sync;  // 标记是否在本次周期内续约过
};
```

#### 2.2 Lease 续约时只标记

```cpp
void ObjectMetadata::GrantLease(uint64_t lease_ttl, 
                                uint64_t soft_pin_ttl) {
    auto now = std::chrono::steady_clock::now();
    lease_timeout = now + std::chrono::milliseconds(lease_ttl);
    
    // 只标记，不立即写入 etcd
    last_lease_renew_time = now;
    lease_renewed_since_last_sync = true;  // 标记为"需要同步"
}
```

---

### 3. 定时器批量扫描

#### 3.1 扫描线程

```cpp
class LeaseRenewalSyncService {
private:
    std::thread sync_thread_;
    std::atomic<bool> running_{false};
    static constexpr uint64_t kSyncIntervalMs = 1000;  // 1 秒扫描一次
    
    void SyncThreadFunc() {
        while (running_) {
            BatchSyncLeaseRenewals();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kSyncIntervalMs));
        }
    }
    
    void BatchSyncLeaseRenewals() {
        std::vector<LeaseRenewalEvent> events;
        auto now = std::chrono::steady_clock::now();
        auto sync_window_start = now - std::chrono::milliseconds(kSyncIntervalMs);
        
        // 扫描所有 metadata，找出在周期内续约的对象
        for (auto& shard : metadata_shards_) {
            MutexLocker lock(&shard.mutex);
            
            for (auto& [key, metadata] : shard.metadata) {
                // 检查是否在本次周期内续约过
                if (metadata.lease_renewed_since_last_sync &&
                    metadata.last_lease_renew_time >= sync_window_start) {
                    
                    LeaseRenewalEvent event;
                    event.key = key;
                    event.lease_timeout = metadata.lease_timeout;
                    event.soft_pin_timeout = metadata.soft_pin_timeout;
                    events.push_back(event);
                    
                    // 重置标记
                    metadata.lease_renewed_since_last_sync = false;
                }
            }
        }
        
        // 批量写入 etcd
        if (!events.empty()) {
            BatchWriteToEtcd(events);
        }
    }
};
```

#### 3.2 批量写入 etcd

```cpp
void LeaseRenewalSyncService::BatchWriteToEtcd(
    const std::vector<LeaseRenewalEvent>& events) {
    
    // 方案 A：使用 etcd 事务批量写入
    std::vector<EtcdOperation> operations;
    for (const auto& event : events) {
        std::string etcd_key = BuildLeaseRenewKey(event.key);
        std::string etcd_value = SerializeLeaseRenewal(event);
        
        operations.push_back(EtcdOperation{
            .type = EtcdOpType::PUT,
            .key = etcd_key,
            .value = etcd_value,
            .ttl = kLeaseRenewalEventTTL  // 如 60 秒
        });
    }
    
    // 批量写入 etcd
    EtcdHelper::BatchPut(operations);
}
```

---

### 4. Standby 端：批量读取和更新

#### 4.1 Watch etcd 前缀

```cpp
class LeaseRenewalWatcher {
    void WatchLeaseRenewals() {
        std::string watch_prefix = etcd_prefix_ + "/lease_renewals/" + cluster_id_ + "/";
        
        while (running_) {
            // Watch 所有 lease renewal 事件
            auto watch_result = EtcdHelper::WatchPrefix(watch_prefix);
            
            for (const auto& event : watch_result.events) {
                if (event.type == EventType::PUT) {
                    ProcessLeaseRenewal(event.key, event.value);
                }
            }
        }
    }
    
    void ProcessLeaseRenewal(const std::string& etcd_key,
                              const std::string& etcd_value) {
        auto renewal = DeserializeLeaseRenewal(etcd_value);
        
        // 更新本地 metadata
        auto& metadata = GetMetadata(renewal.key);
        metadata.lease_timeout = renewal.lease_timeout;
        metadata.soft_pin_timeout = renewal.soft_pin_timeout;
    }
};
```

#### 4.2 定期全量同步（兜底）

```cpp
void LeaseRenewalWatcher::FullSync() {
    std::string prefix = etcd_prefix_ + "/lease_renewals/" + cluster_id_ + "/";
    
    // 获取 etcd 中所有 lease renewal 事件
    auto all_events = EtcdHelper::List(prefix);
    
    for (const auto& event : all_events) {
        ProcessLeaseRenewal(event.key, event.value);
    }
}
```

---

## 方案优势

### 1. ✅ 大幅减少 etcd 写入频率

**对比**：

| 方案 | 写入频率 | 备注 |
|------|---------|------|
| **每次续约都写入** | < 15,000 次/秒 | 每次 Get 都写入 |
| **批量扫描写入** | **< 1,000 次/秒** | 1 秒扫描一次，批量写入 |

**减少比例**：**15 倍以上**

### 2. ✅ 批量写入提高效率

- 使用 etcd 事务批量写入
- 减少网络往返次数
- 提高 etcd 处理效率

### 3. ✅ 容忍 OpLog 延迟

- 即使 OpLog 延迟，Standby 也能从 etcd 获取 lease 续约信息
- 双重保障：OpLog + etcd

---

## 潜在问题和解决方案

### 问题 1：扫描周期内的续约可能丢失

**问题描述**：
- 如果对象在扫描周期内续约了多次，只记录最后一次
- 如果 Primary 在扫描后立即崩溃，最后一次续约可能未写入 etcd

**解决方案**：

#### 方案 A：缩短扫描周期

```cpp
static constexpr uint64_t kSyncIntervalMs = 100;  // 100ms 扫描一次
```

- 减少数据丢失窗口
- 但仍然可能丢失（100ms 窗口）

#### 方案 B：立即写入关键续约

```cpp
void ObjectMetadata::GrantLease(uint64_t lease_ttl, 
                                uint64_t soft_pin_ttl) {
    auto now = std::chrono::steady_clock::now();
    lease_timeout = now + std::chrono::milliseconds(lease_ttl);
    
    last_lease_renew_time = now;
    lease_renewed_since_last_sync = true;
    
    // 如果 lease 即将过期（< 1 秒），立即写入
    if (lease_timeout - now < std::chrono::seconds(1)) {
        WriteLeaseRenewalToEtcdImmediately(key, lease_timeout);
    }
}
```

#### 方案 C：使用内存缓冲区 + 定期刷新

```cpp
class LeaseRenewalBuffer {
    std::unordered_map<std::string, LeaseRenewalEvent> buffer_;
    std::mutex mutex_;
    
    void AddRenewal(const std::string& key, 
                   const std::chrono::steady_clock::time_point& lease_timeout) {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_[key] = LeaseRenewalEvent{key, lease_timeout};
    }
    
    void Flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (buffer_.empty()) return;
        
        // 批量写入 etcd
        BatchWriteToEtcd(buffer_);
        buffer_.clear();
    }
};
```

---

### 问题 2：etcd Key 设计

**Key 结构**：

```
{etcd_prefix}/lease_renewals/{cluster_id}/{key_hash}
```

**Value 结构**：

```json
{
    "key": "original_key",
    "lease_timeout_ms": 1234567890,
    "soft_pin_timeout_ms": null,
    "timestamp_ms": 1234567890
}
```

**考虑**：
- 使用 `key_hash` 避免 etcd key 过长
- 存储原始 key 用于 Standby 端恢复

---

### 问题 3：etcd 容量和清理

**问题**：
- 大量 lease renewal 事件可能积累在 etcd 中

**解决方案**：

#### 方案 A：使用 TTL 自动过期

```cpp
// 写入时设置 TTL
EtcdHelper::PutWithTTL(etcd_key, etcd_value, 
                      kLeaseRenewalEventTTL);  // 如 60 秒
```

- 利用 etcd 的 TTL 机制自动清理
- 不需要额外的清理线程

#### 方案 B：定期清理

```cpp
void CleanupOldLeaseRenewals() {
    std::string prefix = etcd_prefix_ + "/lease_renewals/" + cluster_id_ + "/";
    auto all_events = EtcdHelper::List(prefix);
    
    auto now = NowInMilliseconds();
    for (const auto& event : all_events) {
        auto renewal = DeserializeLeaseRenewal(event.value);
        
        // 如果事件超过保留时间（如 2 分钟），删除
        if (now - renewal.timestamp_ms > 120000) {
            EtcdHelper::Delete(event.key);
        }
    }
}
```

---

## 实现细节

### 1. Primary 端实现

```cpp
class MasterService {
private:
    LeaseRenewalSyncService lease_sync_service_;
    
public:
    MasterService(...) {
        // 启动 lease renewal 同步服务
        if (enable_ha_) {
            lease_sync_service_.Start();
        }
    }
    
    auto GetReplicaList(std::string_view key)
        -> tl::expected<GetReplicaListResponse, ErrorCode> {
        // ... 获取 replica list ...
        
        // 续约 lease（只标记，不立即写入）
        metadata.GrantLease(default_kv_lease_ttl_, default_kv_soft_pin_ttl_);
        
        // 标记为需要同步（由定时器批量处理）
        // lease_sync_service_.MarkForSync(key, metadata);
        
        return GetReplicaListResponse(...);
    }
};

class LeaseRenewalSyncService {
private:
    std::thread sync_thread_;
    std::atomic<bool> running_{false};
    static constexpr uint64_t kSyncIntervalMs = 1000;  // 1 秒
    
    MasterService* master_service_;  // 访问 metadata
    
public:
    void Start() {
        running_ = true;
        sync_thread_ = std::thread(&LeaseRenewalSyncService::SyncThreadFunc, this);
    }
    
    void SyncThreadFunc() {
        while (running_) {
            BatchSyncLeaseRenewals();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kSyncIntervalMs));
        }
    }
    
    void BatchSyncLeaseRenewals() {
        std::vector<LeaseRenewalEvent> events;
        auto now = std::chrono::steady_clock::now();
        
        // 扫描所有 metadata
        for (auto& shard : master_service_->metadata_shards_) {
            MutexLocker lock(&shard.mutex);
            
            for (auto& [key, metadata] : shard.metadata) {
                if (metadata.lease_renewed_since_last_sync) {
                    LeaseRenewalEvent event;
                    event.key = key;
                    event.lease_timeout = metadata.lease_timeout;
                    event.soft_pin_timeout = metadata.soft_pin_timeout;
                    events.push_back(event);
                    
                    // 重置标记
                    metadata.lease_renewed_since_last_sync = false;
                }
            }
        }
        
        // 批量写入 etcd
        if (!events.empty()) {
            BatchWriteToEtcd(events);
        }
    }
    
    void BatchWriteToEtcd(const std::vector<LeaseRenewalEvent>& events) {
        // 使用 etcd 事务批量写入
        for (const auto& event : events) {
            std::string etcd_key = BuildLeaseRenewKey(event.key);
            std::string etcd_value = SerializeLeaseRenewal(event);
            
            // 写入 etcd（带 TTL）
            EtcdHelper::PutWithTTL(etcd_key, etcd_value, 60);  // 60 秒 TTL
        }
    }
};
```

### 2. Standby 端实现

```cpp
class HotStandbyService {
private:
    LeaseRenewalWatcher lease_watcher_;
    
public:
    void Start() {
        // 启动 lease renewal watcher
        lease_watcher_.Start();
        
        // 启动定期全量同步（兜底）
        full_sync_thread_ = std::thread([this]() {
            while (running_) {
                lease_watcher_.FullSync();
                std::this_thread::sleep_for(
                    std::chrono::seconds(kFullSyncIntervalSeconds));
            }
        });
    }
};

class LeaseRenewalWatcher {
private:
    std::thread watch_thread_;
    std::atomic<bool> running_{false};
    HotStandbyService* standby_service_;
    
public:
    void Start() {
        running_ = true;
        watch_thread_ = std::thread(&LeaseRenewalWatcher::WatchLeaseRenewals, this);
    }
    
    void WatchLeaseRenewals() {
        std::string watch_prefix = etcd_prefix_ + "/lease_renewals/" + cluster_id_ + "/";
        
        while (running_) {
            auto watch_result = EtcdHelper::WatchPrefix(watch_prefix);
            
            for (const auto& event : watch_result.events) {
                if (event.type == EventType::PUT) {
                    ProcessLeaseRenewal(event.key, event.value);
                }
            }
        }
    }
    
    void ProcessLeaseRenewal(const std::string& etcd_key,
                              const std::string& etcd_value) {
        auto renewal = DeserializeLeaseRenewal(etcd_value);
        
        // 更新本地 metadata
        auto& metadata = standby_service_->GetMetadata(renewal.key);
        metadata.lease_timeout = renewal.lease_timeout;
        metadata.soft_pin_timeout = renewal.soft_pin_timeout;
    }
    
    void FullSync() {
        // 定期全量同步（兜底）
        std::string prefix = etcd_prefix_ + "/lease_renewals/" + cluster_id_ + "/";
        auto all_events = EtcdHelper::List(prefix);
        
        for (const auto& event : all_events) {
            ProcessLeaseRenewal(event.key, event.value);
        }
    }
};
```

---

## 方案对比

| 方案 | 写入频率 | 延迟 | 数据丢失风险 | 实现复杂度 |
|------|---------|------|------------|----------|
| **每次续约都写入** | < 15,000 次/秒 | 0 | 无 | ⭐⭐ 简单 |
| **定时器批量扫描** | < 1,000 次/秒 | 0-1 秒 | 低（1 秒窗口） | ⭐⭐⭐ 中等 |
| **定时器 + 立即写入关键** | < 2,000 次/秒 | 0-1 秒 | 极低 | ⭐⭐⭐⭐ 较复杂 |

---

## 推荐实现

### 方案：定时器批量扫描 + TTL 自动清理

**配置**：
- 扫描周期：**1 秒**
- etcd TTL：**60 秒**
- 立即写入阈值：lease 剩余时间 < 1 秒

**优点**：
- ✅ 大幅减少 etcd 写入频率（15 倍以上）
- ✅ 使用 TTL 自动清理，无需额外清理线程
- ✅ 对关键续约立即写入，降低数据丢失风险

**实现步骤**：
1. 在 `ObjectMetadata` 中添加 `lease_renewed_since_last_sync` 标记
2. `GrantLease` 时只标记，不立即写入
3. 定时器线程定期扫描并批量写入 etcd
4. Standby 通过 watch 接收 lease renewal 事件

---

## 总结

### 方案优势

1. ✅ **大幅减少 etcd 写入频率**：从 < 15,000 次/秒 降到 < 1,000 次/秒
2. ✅ **批量写入提高效率**：使用 etcd 事务批量写入
3. ✅ **容忍 OpLog 延迟**：Standby 可以从 etcd 获取 lease 续约信息

### 注意事项

1. ⚠️ **扫描周期内的续约可能丢失**：可以通过缩短周期或立即写入关键续约来缓解
2. ⚠️ **需要 etcd TTL 或定期清理**：避免 etcd 积累大量数据
3. ⚠️ **实现复杂度中等**：需要额外的同步服务

### 最终方案

**定时器批量扫描 + TTL 自动清理 + 关键续约立即写入**

- 扫描周期：1 秒
- etcd TTL：60 秒
- 立即写入：lease 剩余时间 < 1 秒

这样可以**在减少 etcd 压力和保证数据一致性之间取得很好的平衡**。

