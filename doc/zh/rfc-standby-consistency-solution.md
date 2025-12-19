# Standby Master 数据一致性方案设计

## 方案概述

本方案旨在解决 Hot Standby 模式下，Standby Master 与 Primary Master 之间的数据一致性问题，特别是：
1. **Delete 事件同步**：确保显式删除操作在 Standby 上正确应用
2. **租约续约同步**：确保 Standby 能够感知租约续约，避免误驱逐
3. **驱逐策略一致性**：Standby 独立执行驱逐，与 Primary 保持一致
4. **备升主清理**：备升主时执行一次性清理，确保数据一致性

---

## 核心设计原则

### 1. Delete 事件写入 etcd

- **显式 Delete 事件**：`Remove`、`RemoveByRegex`、`RemoveAll`、`PutRevoke`（导致完整对象删除）、`UnmountSegment`（导致完整对象删除）
- **不写入驱逐事件**：由租约到期导致的自动驱逐不写入 etcd
- **原因**：驱逐事件频率极高（可达 130,000 次/秒），写入 etcd 会造成性能瓶颈

### 2. 租约续约批量同步

- **周期性扫描**：Primary 定期扫描（如 1 秒）哪些对象的租约被续约
- **批量写入 etcd**：将续约信息批量写入 etcd，减少写入频率
- **Standby 同步**：Standby 通过 Watch etcd 获取续约信息，更新本地 metadata

### 3. Standby 独立执行驱逐

- **基于本地 metadata**：Standby 根据本地 metadata 的 `lease_timeout` 独立判断是否驱逐
- **与 Primary 一致**：使用相同的驱逐逻辑和策略
- **不依赖 Primary**：不需要 Primary 同步驱逐事件

### 4. 备升主时一次性清理

- **触发时机**：Standby 升为主时（`PromoteToLeader`）
- **清理范围**：所有租约已过期的 metadata
- **目的**：确保新 Primary 的数据状态与旧 Primary 一致

---

## 详细设计

### 1. Delete 事件同步机制

#### 1.1 Delete 事件类型

```cpp
enum class DeleteEventType {
    REMOVE,              // 显式 Remove 操作
    REMOVE_BY_REGEX,     // RemoveByRegex 操作
    REMOVE_ALL,          // RemoveAll 操作
    PUT_REVOKE,          // PutRevoke 导致完整对象删除
    UNMOUNT_SEGMENT      // UnmountSegment 导致完整对象删除
};

struct DeleteEvent {
    DeleteEventType type;
    std::string key;                    // 对象 key（对于 REMOVE_BY_REGEX 和 REMOVE_ALL，可能为空）
    std::string regex_pattern;          // 正则表达式（仅用于 REMOVE_BY_REGEX）
    std::chrono::steady_clock::time_point timestamp;
    uint64_t version;                   // 事件版本号（用于去重）
};
```

#### 1.2 Primary 端：写入 Delete 事件

```cpp
class MasterService {
private:
    void WriteDeleteEventToEtcd(const DeleteEvent& event) {
        std::string etcd_key = BuildDeleteEventKey(event.key, event.version);
        std::string etcd_value = SerializeDeleteEvent(event);
        
        // 写入 etcd，带 TTL（如 60 秒）
        EtcdHelper::PutWithTTL(etcd_key, etcd_value, 60);
    }
    
public:
    auto Remove(const std::string& key) -> tl::expected<void, ErrorCode> {
        // ... 执行删除逻辑 ...
        
        // 写入 Delete 事件到 etcd
        DeleteEvent event;
        event.type = DeleteEventType::REMOVE;
        event.key = key;
        event.timestamp = std::chrono::steady_clock::now();
        event.version = GetNextEventVersion();
        WriteDeleteEventToEtcd(event);
        
        return {};
    }
    
    auto RemoveByRegex(const std::string& regex_pattern) -> ... {
        // ... 执行删除逻辑 ...
        
        // 为每个匹配的对象写入 Delete 事件
        for (const auto& key : matched_keys) {
            DeleteEvent event;
            event.type = DeleteEventType::REMOVE_BY_REGEX;
            event.key = key;
            event.regex_pattern = regex_pattern;
            event.timestamp = std::chrono::steady_clock::now();
            event.version = GetNextEventVersion();
            WriteDeleteEventToEtcd(event);
        }
        
        return {};
    }
    
    auto RemoveAll() -> ... {
        // ... 执行删除逻辑 ...
        
        // 为每个删除的对象写入 Delete 事件
        for (const auto& key : all_keys) {
            DeleteEvent event;
            event.type = DeleteEventType::REMOVE_ALL;
            event.key = key;
            event.timestamp = std::chrono::steady_clock::now();
            event.version = GetNextEventVersion();
            WriteDeleteEventToEtcd(event);
        }
        
        return {};
    }
};
```

#### 1.3 Standby 端：接收 Delete 事件

```cpp
class HotStandbyService {
private:
    std::thread delete_event_watcher_thread_;
    
    void WatchDeleteEvents() {
        std::string watch_prefix = etcd_prefix_ + "/delete_events/" + cluster_id_ + "/";
        
        while (running_) {
            auto watch_result = EtcdHelper::WatchPrefix(watch_prefix);
            
            for (const auto& event : watch_result.events) {
                if (event.type == EventType::PUT) {
                    ProcessDeleteEvent(event.key, event.value);
                }
            }
        }
    }
    
    void ProcessDeleteEvent(const std::string& etcd_key,
                            const std::string& etcd_value) {
        auto delete_event = DeserializeDeleteEvent(etcd_value);
        
        // 应用 Delete 事件到本地 metadata
        switch (delete_event.type) {
            case DeleteEventType::REMOVE:
                ApplyRemoveEvent(delete_event.key);
                break;
            case DeleteEventType::REMOVE_BY_REGEX:
                ApplyRemoveByRegexEvent(delete_event.regex_pattern);
                break;
            case DeleteEventType::REMOVE_ALL:
                ApplyRemoveAllEvent();
                break;
            // ...
        }
    }
    
    void ApplyRemoveEvent(const std::string& key) {
        auto& metadata = GetMetadata(key);
        if (metadata.Exists()) {
            metadata.Erase();
            MasterMetricManager::instance().dec_key_count(1);
        }
    }
};
```

---

### 2. 租约续约批量同步机制

#### 2.1 数据结构扩展

```cpp
struct ObjectMetadata {
    std::vector<Replica> replicas;
    size_t size;
    std::chrono::steady_clock::time_point lease_timeout;
    std::optional<std::chrono::steady_clock::time_point> soft_pin_timeout;
    
    // 新增：标记是否在本次周期内续约过
    bool lease_renewed_since_last_sync{false};
    std::chrono::steady_clock::time_point last_lease_renew_time;
};
```

#### 2.2 Primary 端：批量扫描和写入

```cpp
class LeaseRenewalSyncService {
private:
    std::thread sync_thread_;
    std::atomic<bool> running_{false};
    static constexpr uint64_t kSyncIntervalMs = 1000;  // 1 秒扫描一次
    
    MasterService* master_service_;
    
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
        
        // 扫描所有 metadata，找出在周期内续约的对象
        for (auto& shard : master_service_->metadata_shards_) {
            MutexLocker lock(&shard.mutex);
            
            for (auto& [key, metadata] : shard.metadata) {
                // 检查是否在本次周期内续约过
                if (metadata.lease_renewed_since_last_sync) {
                    LeaseRenewalEvent event;
                    event.key = key;
                    event.lease_timeout = metadata.lease_timeout;
                    event.soft_pin_timeout = metadata.soft_pin_timeout;
                    event.timestamp = metadata.last_lease_renew_time;
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
        for (const auto& event : events) {
            std::string etcd_key = BuildLeaseRenewKey(event.key);
            std::string etcd_value = SerializeLeaseRenewal(event);
            
            // 写入 etcd（带 TTL，如 60 秒）
            EtcdHelper::PutWithTTL(etcd_key, etcd_value, 60);
        }
    }
};

// 修改 GrantLease 方法
void ObjectMetadata::GrantLease(const uint64_t ttl, const uint64_t soft_ttl) {
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    lease_timeout =
        std::max(lease_timeout, now + std::chrono::milliseconds(ttl));
    
    if (soft_pin_timeout) {
        soft_pin_timeout =
            std::max(*soft_pin_timeout,
                     now + std::chrono::milliseconds(soft_ttl));
    }
    
    // 标记为需要同步
    last_lease_renew_time = now;
    lease_renewed_since_last_sync = true;
}
```

#### 2.3 Standby 端：接收租约续约事件

```cpp
class HotStandbyService {
private:
    std::thread lease_renewal_watcher_thread_;
    
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
        auto& metadata = GetMetadata(renewal.key);
        if (metadata.Exists()) {
            metadata.lease_timeout = renewal.lease_timeout;
            metadata.soft_pin_timeout = renewal.soft_pin_timeout;
        }
    }
};
```

---

### 3. Standby 独立执行驱逐

#### 3.1 Standby 驱逐逻辑

```cpp
class HotStandbyService {
private:
    std::thread eviction_thread_;
    std::atomic<bool> eviction_running_{false};
    static constexpr uint64_t kEvictionThreadSleepMs = 10;  // 10ms
    
    void EvictionThreadFunc() {
        while (eviction_running_) {
            // 执行驱逐逻辑（与 Primary 相同）
            BatchEvict();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kEvictionThreadSleepMs));
        }
    }
    
    void BatchEvict() {
        // 使用与 Primary 相同的驱逐逻辑
        // 基于本地 metadata 的 lease_timeout 判断是否驱逐
        auto now = std::chrono::steady_clock::now();
        
        for (auto& shard : metadata_shards_) {
            MutexLocker lock(&shard.mutex);
            
            for (auto it = shard.metadata.begin(); 
                 it != shard.metadata.end();) {
                // 检查租约是否过期
                if (it->second.IsLeaseExpired(now)) {
                    // 执行驱逐（与 Primary 逻辑相同）
                    it->second.EraseReplica(ReplicaType::MEMORY);
                    if (!it->second.IsValid()) {
                        it = shard.metadata.erase(it);
                    } else {
                        ++it;
                    }
                } else {
                    ++it;
                }
            }
        }
    }
};
```

#### 3.2 驱逐策略一致性

- **相同的判断条件**：`IsLeaseExpired(now)`
- **相同的驱逐逻辑**：`EraseReplica(ReplicaType::MEMORY)`
- **相同的 soft pin 处理**：考虑 `IsSoftPinned(now)` 和 `allow_evict_soft_pinned_objects_`

---

### 4. 备升主时一次性清理

#### 4.1 PromoteToLeader 实现

```cpp
class HotStandbyService {
public:
    void PromoteToLeader() {
        // 1. 停止 Standby 服务
        StopStandbyServices();
        
        // 2. 执行一次性清理：驱逐所有租约已过期的 metadata
        CleanupExpiredMetadata();
        
        // 3. 将 Standby 的 metadata 转换为 Primary 的 MasterService
        PromoteMetadataToMasterService();
        
        // 4. 启动 Primary 服务
        StartPrimaryServices();
    }
    
private:
    void CleanupExpiredMetadata() {
        auto now = std::chrono::steady_clock::now();
        uint64_t total_freed_size = 0;
        size_t evicted_count = 0;
        
        LOG(INFO) << "Promoting to leader: cleaning up expired metadata";
        
        for (auto& shard : metadata_shards_) {
            MutexLocker lock(&shard.mutex);
            
            auto it = shard.metadata.begin();
            while (it != shard.metadata.end()) {
                // 检查租约是否过期
                if (it->second.IsLeaseExpired(now)) {
                    // 统计信息
                    total_freed_size += 
                        it->second.size * it->second.GetMemReplicaCount();
                    evicted_count++;
                    
                    // 执行驱逐
                    it->second.EraseReplica(ReplicaType::MEMORY);
                    if (!it->second.IsValid()) {
                        it = shard.metadata.erase(it);
                    } else {
                        ++it;
                    }
                } else {
                    ++it;
                }
            }
        }
        
        LOG(INFO) << "Promoting to leader: evicted " << evicted_count 
                  << " expired objects, freed " << total_freed_size 
                  << " bytes";
    }
};
```

---

## 数据流图

```
┌─────────────────────────────────────────────────────────────┐
│                    Primary Master                           │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────┐      ┌──────────────┐                    │
│  │ Delete Event │─────▶│   etcd       │                    │
│  │   (立即写入)  │      │              │                    │
│  └──────────────┘      └──────────────┘                    │
│                                                              │
│  ┌──────────────┐      ┌──────────────┐                    │
│  │ Lease Renewal│─────▶│ Lease Renewal│                    │
│  │   (标记)      │      │ Sync Service │                    │
│  └──────────────┘      └──────┬───────┘                    │
│                                 │                            │
│                                 │ 批量扫描 (1秒)              │
│                                 ▼                            │
│                          ┌──────────────┐                    │
│                          │   etcd       │                    │
│                          └──────────────┘                    │
│                                                              │
│  ┌──────────────┐                                           │
│  │   Eviction   │  (不写入 etcd)                            │
│  │   (独立执行)  │                                           │
│  └──────────────┘                                           │
└─────────────────────────────────────────────────────────────┘
                              │
                              │ Watch
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Standby Master                            │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────┐      ┌──────────────┐                    │
│  │ Delete Event │◀─────│   etcd       │                    │
│  │   Watcher    │      │              │                    │
│  └──────┬───────┘      └──────────────┘                    │
│         │                                                    │
│         │ 应用 Delete 事件                                   │
│         ▼                                                    │
│  ┌──────────────┐                                           │
│  │   Metadata   │                                           │
│  └──────────────┘                                           │
│                                                              │
│  ┌──────────────┐      ┌──────────────┐                    │
│  │ Lease Renewal│◀─────│   etcd       │                    │
│  │   Watcher    │      │              │                    │
│  └──────┬───────┘      └──────────────┘                    │
│         │                                                    │
│         │ 更新 lease_timeout                                 │
│         ▼                                                    │
│  ┌──────────────┐                                           │
│  │   Metadata   │                                           │
│  └──────┬───────┘                                           │
│         │                                                    │
│         │ 基于 lease_timeout 判断                            │
│         ▼                                                    │
│  ┌──────────────┐                                           │
│  │   Eviction   │  (独立执行，与 Primary 相同逻辑)            │
│  │   (独立执行)  │                                           │
│  └──────────────┘                                           │
│                                                              │
│  ┌──────────────┐                                           │
│  │ PromoteTo    │  (备升主时执行一次性清理)                   │
│  │   Leader     │                                           │
│  └──────────────┘                                           │
└─────────────────────────────────────────────────────────────┘
```

---

## 关键设计决策

### 1. 为什么 Delete 事件写入 etcd，而驱逐事件不写入？

**原因**：
- **Delete 事件频率低**：显式删除操作频率远低于驱逐事件
- **驱逐事件频率极高**：可达 130,000 次/秒，写入 etcd 会造成性能瓶颈
- **Standby 可以独立判断**：Standby 可以根据本地 metadata 的 `lease_timeout` 独立判断是否驱逐

### 2. 为什么租约续约要批量同步？

**原因**：
- **减少 etcd 写入频率**：从 < 15,000 次/秒 降到 < 1,000 次/秒
- **提高效率**：批量写入比单次写入更高效
- **容忍延迟**：1 秒的延迟对租约续约影响很小

### 3. 为什么 Standby 要独立执行驱逐？

**原因**：
- **性能考虑**：不需要同步大量驱逐事件
- **逻辑一致性**：使用相同的驱逐逻辑，确保一致性
- **实时性**：Standby 可以实时执行驱逐，不需要等待 Primary 同步

### 4. 为什么备升主时要执行一次性清理？

**原因**：
- **数据一致性**：确保新 Primary 的数据状态与旧 Primary 一致
- **清理过期数据**：清理所有租约已过期的 metadata
- **避免误用**：防止新 Primary 返回已过期但未清理的 metadata

---

## 实现步骤

### Phase 1: Delete 事件同步

1. 定义 `DeleteEvent` 数据结构
2. 在 `Remove`、`RemoveByRegex`、`RemoveAll` 等操作中写入 Delete 事件到 etcd
3. 在 Standby 端实现 Delete 事件 Watcher
4. 应用 Delete 事件到 Standby 的 metadata

### Phase 2: 租约续约批量同步

1. 在 `ObjectMetadata` 中添加 `lease_renewed_since_last_sync` 标记
2. 修改 `GrantLease` 方法，标记续约
3. 实现 `LeaseRenewalSyncService`，定期扫描和批量写入
4. 在 Standby 端实现 Lease Renewal Watcher
5. 更新 Standby 的 metadata 的 `lease_timeout`

### Phase 3: Standby 独立执行驱逐

1. 在 `HotStandbyService` 中实现驱逐线程
2. 使用与 Primary 相同的驱逐逻辑
3. 基于本地 metadata 的 `lease_timeout` 判断是否驱逐

### Phase 4: 备升主时一次性清理

1. 在 `PromoteToLeader` 中实现 `CleanupExpiredMetadata`
2. 清理所有租约已过期的 metadata
3. 记录清理统计信息

---

## 性能考虑

### 1. etcd 写入频率

| 事件类型 | 频率 | 写入方式 |
|---------|------|---------|
| **Delete 事件** | < 1,000 次/秒 | 立即写入 |
| **租约续约** | < 15,000 次/秒 | 批量写入（< 1,000 次/秒） |
| **驱逐事件** | 130,000 次/秒 | **不写入** |

### 2. etcd 容量管理

- **TTL 机制**：所有事件都设置 TTL（如 60 秒），自动清理
- **定期清理**：可选，定期清理过期事件

### 3. 网络开销

- **批量写入**：减少网络往返次数
- **Watch 机制**：使用 etcd Watch，减少轮询开销

---

## 容错和一致性保证

### 1. Delete 事件丢失

- **影响**：Standby 可能保留已删除的对象
- **缓解**：备升主时执行一次性清理，清理所有过期对象
- **最终一致性**：通过定期清理保证最终一致性

### 2. 租约续约延迟

- **影响**：Standby 可能误驱逐已续约的对象
- **缓解**：
  - 缩短扫描周期（如 100ms）
  - 关键续约立即写入（lease 剩余时间 < 1 秒）

### 3. 时钟不同步

- **影响**：Standby 和 Primary 的时钟不同步可能导致判断不一致
- **缓解**：使用 NTP 同步时钟，或使用相对时间

---

## 总结

本方案通过以下机制确保 Standby Master 与 Primary Master 的数据一致性：

1. ✅ **Delete 事件同步**：显式删除操作写入 etcd，Standby 同步应用
2. ✅ **租约续约批量同步**：定期批量同步租约续约，避免误驱逐
3. ✅ **Standby 独立执行驱逐**：基于本地 metadata 独立判断，与 Primary 逻辑一致
4. ✅ **备升主时一次性清理**：清理所有过期 metadata，确保数据一致性

该方案在**性能**、**一致性**和**实现复杂度**之间取得了良好的平衡。

