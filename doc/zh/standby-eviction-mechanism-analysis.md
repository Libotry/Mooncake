# Standby Master 驱逐机制分析

## 问题

Standby master 的驱逐事件是由主 master 同步过来，还是 standby 自己根据租约（lease）过期时间决定？

---

## 代码分析

### 1. 当前实现（非热备模式）

**关键代码**：`mooncake-store/src/ha_helper.cpp:130`

```cpp
mv_helper.ElectLeader(config_.local_hostname, view_version, lease_id);
```

**行为**：
- Standby master 在 `ElectLeader` 时**阻塞等待**（`WatchUntilDeleted`）
- 只有在**成为 leader 后**，才创建 `WrappedMasterService` 和 `MasterService`
- Standby **不会**运行自己的驱逐线程
- Standby **不会**维护 metadata

**结论**：
- 当前实现中，standby **不执行驱逐**
- 只有成为 leader 后，才会启动驱逐线程

---

### 2. 热备模式设计（RFC）

**关键设计**：`doc/zh/rfc-master-hot-standby-mode.md`

#### 2.1 OpLog 包含驱逐事件

```cpp
enum class OpType : uint8_t {
    PUT_START = 1,
    PUT_END = 2,
    PUT_REVOKE = 3,
    REMOVE = 4,
    MOUNT_SEGMENT = 5,
    UNMOUNT_SEGMENT = 6,
    EVICTION = 7,       // 对象驱逐
};
```

**说明**：热备模式设计中，OpLog **包含 `EVICTION` 类型的操作**，这意味着：
- Primary 执行驱逐时，会生成 `EVICTION` OpLog
- Standby 接收 OpLog 后，会应用驱逐操作

#### 2.2 Standby 维护 Metadata 副本

根据热备模式设计：
- Standby 运行 `HotStandbyService`
- Standby 维护完整的 metadata 副本（包括 `lease_timeout`）
- Standby 通过 OpLog 同步所有操作

---

### 3. 驱逐逻辑分析

**关键代码**：`mooncake-store/src/master_service.cpp:721`

```cpp
// Skip objects that are not expired or have incomplete replicas
if (!it->second.IsLeaseExpired(now) ||
    it->second.HasDiffRepStatus(ReplicaStatus::COMPLETE,
                                ReplicaType::MEMORY)) {
    continue;
}
```

**驱逐条件**：
1. **Lease 已过期**：`IsLeaseExpired(now)` 返回 `true`
2. **Replica 状态完整**：所有 memory replica 都是 `COMPLETE` 状态

**`IsLeaseExpired` 实现**：`mooncake-store/include/master_service.h:339-341`

```cpp
bool IsLeaseExpired(std::chrono::steady_clock::time_point& now) const {
    return now >= lease_timeout;
}
```

**关键发现**：
- 驱逐判断是**确定性的**：基于 `lease_timeout` 和当前时间
- 如果 standby 维护了 `lease_timeout`，可以**独立判断**是否过期

---

## 两种可能的实现方式

### 方式 A：Standby 自己根据租约判断（推荐）

**设计**：
- Standby 维护完整的 metadata（包括 `lease_timeout`）
- Standby 运行自己的驱逐线程
- Standby 独立判断 lease 是否过期，执行驱逐

**优点**：
- ✅ **不需要同步驱逐事件**，减少 OpLog 压力
- ✅ 驱逐是确定性的，基于 lease 过期时间
- ✅ Standby 和 Primary 会得到**相同的结果**（因为 lease_timeout 相同）

**实现**：
```cpp
class HotStandbyService {
    void EvictionThreadFunc() {
        while (running_) {
            auto now = std::chrono::steady_clock::now();
            
            // 遍历所有 metadata，检查 lease 是否过期
            for (auto& [key, metadata] : metadata_) {
                if (metadata.IsLeaseExpired(now)) {
                    // 执行驱逐
                    EvictObject(key);
                }
            }
            
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kEvictionThreadSleepMs));
        }
    }
};
```

**前提条件**：
- Standby 必须维护完整的 metadata（包括 `lease_timeout`）
- Standby 必须通过 OpLog 同步 `PutEnd` 操作（因为 `PutEnd` 会设置 `lease_timeout`）

---

### 方式 B：Primary 同步驱逐事件

**设计**：
- Primary 执行驱逐时，生成 `EVICTION` OpLog
- Standby 接收 OpLog 后，应用驱逐操作

**优点**：
- ✅ Standby 和 Primary **完全同步**，不会有时序差异
- ✅ 可以处理复杂的驱逐逻辑（如内存使用率触发）

**缺点**：
- ❌ **需要同步大量驱逐事件**（可能 130,000 次/秒）
- ❌ OpLog 压力大
- ❌ 如果 OpLog 延迟，Standby 的驱逐可能滞后

**实现**：
```cpp
// Primary 端
void MasterService::BatchEvict(...) {
    // ... 执行驱逐 ...
    
    // 生成 EVICTION OpLog
    for (const auto& evicted_key : evicted_keys) {
        op_log_manager_.Append(OpType::EVICTION, evicted_key, "");
    }
}

// Standby 端
void HotStandbyService::ApplyOpLog(const OpLogEntry& entry) {
    if (entry.op_type == OpType::EVICTION) {
        // 应用驱逐操作
        EvictObject(entry.object_key);
    }
}
```

---

## 推荐方案

### 推荐：方式 A（Standby 自己根据租约判断）

**理由**：

1. **驱逐是确定性的**
   - 驱逐基于 `lease_timeout` 和当前时间
   - 如果 standby 维护了 `lease_timeout`，可以独立判断
   - Standby 和 Primary 会得到**相同的结果**

2. **减少 OpLog 压力**
   - 不需要同步驱逐事件（可能 130,000 次/秒）
   - OpLog 只包含用户操作（Put/Remove/Mount 等）
   - 减少网络和存储开销

3. **实现简单**
   - Standby 只需要维护 metadata（包括 `lease_timeout`）
   - Standby 运行自己的驱逐线程
   - 不需要额外的同步机制

4. **符合设计原则**
   - 驱逐是**自动的、周期性的**操作
   - 不是用户显式操作，不需要强一致性同步
   - 基于 lease 过期时间，是确定性的

---

## 实现建议

### 1. Standby 维护 Metadata

```cpp
class HotStandbyService {
    // Standby 维护完整的 metadata 副本
    std::array<MetadataShard, kNumShards> metadata_shards_;
    
    // 通过 OpLog 同步 PutEnd 操作（设置 lease_timeout）
    void ApplyOpLog(const OpLogEntry& entry) {
        if (entry.op_type == OpType::PUT_END) {
            // 解析 payload，更新 metadata（包括 lease_timeout）
            UpdateMetadata(entry.object_key, entry.payload);
        }
    }
};
```

### 2. Standby 运行驱逐线程

```cpp
class HotStandbyService {
    void EvictionThreadFunc() {
        while (running_) {
            double used_ratio = GetGlobalUsedRatio();
            if (used_ratio > eviction_high_watermark_ratio_) {
                BatchEvict(evict_ratio_target, evict_ratio_lowerbound);
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kEvictionThreadSleepMs));
        }
    }
    
    void BatchEvict(...) {
        auto now = std::chrono::steady_clock::now();
        
        // 与 Primary 相同的驱逐逻辑
        for (auto& shard : metadata_shards_) {
            for (auto& [key, metadata] : shard.metadata) {
                if (metadata.IsLeaseExpired(now)) {
                    EvictObject(key);
                }
            }
        }
    }
};
```

### 3. 不生成 EVICTION OpLog

```cpp
// Primary 端：不生成 EVICTION OpLog
void MasterService::BatchEvict(...) {
    // 执行驱逐，但不生成 OpLog
    // Standby 会自己判断 lease 过期
}
```

---

## 特殊情况处理

### 情况 1：时钟不同步

**问题**：如果 Standby 和 Primary 的时钟不同步，可能导致驱逐时间不一致。

**解决方案**：
- 使用相对时间（`lease_timeout` 是相对于 `PutEnd` 的时间点）
- 或者使用 NTP 同步时钟
- 或者 Standby 使用 Primary 的时间戳（通过 OpLog 传递）

### 情况 2：内存使用率触发

**问题**：驱逐不仅基于 lease 过期，还基于内存使用率。

**解决方案**：
- Standby 也需要监控内存使用率
- 或者 Standby 不执行内存使用率触发的驱逐（只执行 lease 过期的驱逐）
- 或者 Standby 接收 Primary 的内存使用率信息

---

## 总结

### 当前实现（非热备模式）
- Standby **不执行驱逐**
- 只有成为 leader 后，才会启动驱逐线程

### 热备模式推荐方案
- **Standby 自己根据租约判断**
- Standby 维护完整的 metadata（包括 `lease_timeout`）
- Standby 运行自己的驱逐线程
- **不需要同步驱逐事件**

### 理由
1. 驱逐是确定性的（基于 lease 过期时间）
2. 减少 OpLog 压力（不需要同步 130,000 次/秒的驱逐事件）
3. 实现简单，符合设计原则

### 对 etcd Delete 事件方案的影响
- **只同步显式 Delete 事件**（用户调用 `Remove()`）
- **不同步驱逐事件**（Standby 自己判断）
- 这样可以大大减少 etcd 的压力

