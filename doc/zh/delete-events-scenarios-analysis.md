# Delete 事件场景完整分析

## 问题

除了驱逐（Eviction）产生的 delete 事件，还有其他场景能产生 delete 事件吗？

---

## 代码分析结果

通过分析 `mooncake-store/src/master_service.cpp`，发现以下场景可能产生 delete 事件：

---

## 1. 用户显式删除操作

### 1.1 `Remove(key)` - 删除单个对象

**代码位置**：`mooncake-store/src/master_service.cpp:490-513`

```cpp
auto MasterService::Remove(const std::string& key)
    -> tl::expected<void, ErrorCode> {
    MetadataAccessor accessor(this, key);
    if (!accessor.Exists()) {
        return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
    }
    
    auto& metadata = accessor.Get();
    
    // 前置条件检查
    if (!metadata.IsLeaseExpired()) {
        return tl::make_unexpected(ErrorCode::OBJECT_HAS_LEASE);
    }
    if (!metadata.IsAllReplicasComplete()) {
        return tl::make_unexpected(ErrorCode::REPLICA_IS_NOT_READY);
    }
    
    // 删除对象 metadata
    accessor.Erase();  // ← 产生 delete 事件
    return {};
}
```

**触发条件**：
- 用户显式调用 `Client::Remove(key)`
- 对象 lease 已过期
- 所有 replica 都是 COMPLETE 状态

**频率**：
- 取决于用户操作频率
- 通常较低（用户主动删除）

**是否需要同步到 etcd**：
- ✅ **需要**：这是用户显式操作，需要同步

---

### 1.2 `RemoveByRegex(regex_pattern)` - 正则表达式批量删除

**代码位置**：`mooncake-store/src/master_service.cpp:515-562`

```cpp
auto MasterService::RemoveByRegex(const std::string& regex_pattern)
    -> tl::expected<long, ErrorCode> {
    // ...
    for (size_t i = 0; i < kNumShards; ++i) {
        for (auto it = metadata_shards_[i].metadata.begin();
             it != metadata_shards_[i].metadata.end();) {
            if (std::regex_search(it->first, pattern)) {
                if (!it->second.IsLeaseExpired()) {
                    ++it;  // 跳过有 lease 的对象
                    continue;
                }
                if (!it->second.IsAllReplicasComplete()) {
                    ++it;  // 跳过 replica 未完成的对象
                    continue;
                }
                
                // 删除匹配的对象
                it = metadata_shards_[i].metadata.erase(it);  // ← 产生 delete 事件
                removed_count++;
            } else {
                ++it;
            }
        }
    }
    return removed_count;
}
```

**触发条件**：
- 用户显式调用 `Client::RemoveByRegex(pattern)`
- 匹配的对象 lease 已过期
- 所有 replica 都是 COMPLETE 状态

**频率**：
- 取决于用户操作频率
- 可能一次删除多个对象（批量操作）

**是否需要同步到 etcd**：
- ✅ **需要**：这是用户显式操作，需要同步
- 可以批量写入 etcd（一次写入多个 key）

---

### 1.3 `RemoveAll()` - 删除所有对象

**代码位置**：`mooncake-store/src/master_service.cpp:564-595`

```cpp
long MasterService::RemoveAll() {
    long removed_count = 0;
    auto now = std::chrono::steady_clock::now();
    
    for (auto& shard : metadata_shards_) {
        auto it = shard.metadata.begin();
        while (it != shard.metadata.end()) {
            // 只删除 lease 已过期的对象
            if (it->second.IsLeaseExpired(now)) {
                it = shard.metadata.erase(it);  // ← 产生 delete 事件
                removed_count++;
            } else {
                ++it;
            }
        }
    }
    return removed_count;
}
```

**触发条件**：
- 用户显式调用 `Client::RemoveAll()`
- 只删除 lease 已过期的对象

**频率**：
- 通常很低（运维操作）
- 可能一次删除大量对象

**是否需要同步到 etcd**：
- ✅ **需要**：这是用户显式操作，需要同步
- 可以批量写入 etcd

---

## 2. Put 操作失败回滚

### 2.1 `PutRevoke(key, replica_type)` - 撤销 Put 操作

**代码位置**：`mooncake-store/src/master_service.cpp:448-468`

```cpp
auto MasterService::PutRevoke(const std::string& key, ReplicaType replica_type)
    -> tl::expected<void, ErrorCode> {
    MetadataAccessor accessor(this, key);
    if (!accessor.Exists()) {
        return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
    }
    
    auto& metadata = accessor.Get();
    
    // 撤销指定类型的 replica
    metadata.EraseReplica(replica_type);
    
    // 如果对象没有其他 replica，删除整个 metadata
    if (metadata.IsValid() == false) {
        accessor.Erase();  // ← 可能产生 delete 事件
    }
    return {};
}
```

**触发条件**：
- Put 操作失败（传输失败、磁盘写入失败等）
- Client 调用 `PutRevoke` 回滚
- 如果对象只有这一个 replica，会删除整个 metadata

**代码示例**：`mooncake-store/src/client.cpp:811-817`

```cpp
ErrorCode transfer_err = TransferWrite(replica, slices);
if (transfer_err != ErrorCode::OK) {
    // 传输失败，撤销 Put 操作
    auto revoke_result = master_client_.PutRevoke(key, ReplicaType::MEMORY);
    return tl::unexpected(transfer_err);
}
```

**频率**：
- 取决于 Put 操作失败率
- 通常较低（失败是异常情况）
- 但如果对象只有这一个 replica，会产生 delete 事件

**是否需要同步到 etcd**：
- ⚠️ **视情况而定**：
  - 如果只是撤销一个 replica（对象还有其他 replica），**不需要**同步
  - 如果对象被完全删除（没有其他 replica），**需要**同步

---

## 3. Segment 卸载相关

### 3.1 `UnmountSegment()` - 卸载 Segment

**代码位置**：`mooncake-store/src/master_service.cpp:166-198`

```cpp
auto MasterService::UnmountSegment(const UUID& segment_id,
                                   const UUID& client_id)
    -> tl::expected<void, ErrorCode> {
    // 1. 准备卸载 segment
    segment_access.PrepareUnmountSegment(segment_id, ...);
    
    // 2. 清理相关对象的 metadata
    ClearInvalidHandles();  // ← 可能产生 delete 事件
    
    // 3. 提交卸载操作
    segment_access.CommitUnmountSegment(segment_id, ...);
    return {};
}
```

**触发条件**：
- Client 节点下线或 Segment 被卸载
- 调用 `ClearInvalidHandles()` 清理失效的 handles

**频率**：
- 取决于节点下线频率
- 可能一次清理多个对象

**是否需要同步到 etcd**：
- ⚠️ **视情况而定**：
  - 如果只是清理失效的 replica（对象还有其他 replica），**不需要**同步
  - 如果对象被完全删除（所有 replica 都失效），**需要**同步

---

### 3.2 `ClearInvalidHandles()` - 清理失效的 Handles

**代码位置**：`mooncake-store/src/master_service.cpp:151-164`

```cpp
void MasterService::ClearInvalidHandles() {
    for (auto& shard : metadata_shards_) {
        auto it = shard.metadata.begin();
        while (it != shard.metadata.end()) {
            // 清理失效的 handles
            if (CleanupStaleHandles(it->second)) {
                // 如果对象没有有效的 replica，删除整个 metadata
                it = shard.metadata.erase(it);  // ← 可能产生 delete 事件
            } else {
                ++it;
            }
        }
    }
}
```

**`CleanupStaleHandles` 实现**：`mooncake-store/src/master_service.cpp:597-614`

```cpp
bool MasterService::CleanupStaleHandles(ObjectMetadata& metadata) {
    // 移除失效的 replica
    auto replica_it = metadata.replicas.begin();
    while (replica_it != metadata.replicas.end()) {
        if (replica_it->has_invalid_mem_handle()) {
            replica_it = metadata.replicas.erase(replica_it);
        } else {
            ++replica_it;
        }
    }
    
    // 如果所有 replica 都失效，返回 true（表示对象应该被删除）
    return metadata.replicas.empty();
}
```

**触发条件**：
- Segment 被卸载
- Allocator 失效（weak_ptr expired）
- 定期清理（如果实现）

**频率**：
- 取决于 Segment 卸载频率
- 可能一次清理多个对象

**是否需要同步到 etcd**：
- ⚠️ **视情况而定**：
  - 如果只是清理失效的 replica（对象还有其他 replica），**不需要**同步
  - 如果对象被完全删除（所有 replica 都失效），**需要**同步

---

## 4. 驱逐（Eviction）

### 4.1 `BatchEvict()` - 批量驱逐

**代码位置**：`mooncake-store/src/master_service.cpp:679-919`

```cpp
void MasterService::BatchEvict(double evict_ratio_target,
                               double evict_ratio_lowerbound) {
    // ...
    // 驱逐 lease 已过期的对象
    it->second.EraseReplica(ReplicaType::MEMORY);
    if (it->second.IsValid() == false) {
        it = shard.metadata.erase(it);  // ← 产生 delete 事件
    }
    // ...
}
```

**触发条件**：
- 内存使用率超过高水位线
- 或分配失败触发

**频率**：
- 可能达到 **130,000 次/秒**（高内存使用率时）

**是否需要同步到 etcd**：
- ❌ **不需要**：Standby 可以自己根据 lease 过期时间判断

---

## 总结表

| 场景 | 操作 | 触发条件 | 频率 | 是否需要同步 etcd | 备注 |
|------|------|---------|------|------------------|------|
| **1.1** | `Remove(key)` | 用户显式删除 | 低 | ✅ **需要** | 用户操作 |
| **1.2** | `RemoveByRegex()` | 用户批量删除 | 低（批量） | ✅ **需要** | 用户操作 |
| **1.3** | `RemoveAll()` | 用户删除所有 | 很低 | ✅ **需要** | 运维操作 |
| **2.1** | `PutRevoke()` | Put 失败回滚 | 低（异常） | ⚠️ **条件** | 仅当对象完全删除时 |
| **3.1** | `UnmountSegment()` | Segment 卸载 | 低 | ⚠️ **条件** | 通过 ClearInvalidHandles |
| **3.2** | `ClearInvalidHandles()` | Handle 失效 | 低 | ⚠️ **条件** | 仅当对象完全删除时 |
| **4.1** | `BatchEvict()` | 内存压力/lease 过期 | **极高** | ❌ **不需要** | Standby 自己判断 |

---

## 对 etcd Delete 事件方案的影响

### 需要同步到 etcd 的场景

1. **用户显式删除**：
   - `Remove(key)` ✅
   - `RemoveByRegex()` ✅
   - `RemoveAll()` ✅

2. **Put 失败导致对象完全删除**：
   - `PutRevoke()` 且对象没有其他 replica ⚠️

3. **Segment 卸载导致对象完全删除**：
   - `ClearInvalidHandles()` 且对象所有 replica 都失效 ⚠️

### 不需要同步到 etcd 的场景

1. **驱逐（Eviction）**：
   - `BatchEvict()` ❌
   - Standby 可以自己根据 lease 过期时间判断

2. **部分删除**：
   - `PutRevoke()` 但对象还有其他 replica
   - `ClearInvalidHandles()` 但对象还有其他 replica

---

## 实现建议

### 1. 区分完全删除和部分删除

```cpp
enum class DeleteType {
    EXPLICIT,        // 用户显式删除（Remove/RemoveByRegex/RemoveAll）
    PUT_REVOKE,      // Put 失败导致完全删除
    HANDLE_CLEANUP,  // Handle 失效导致完全删除
    EVICTION         // 驱逐（不需要同步）
};

bool ShouldSyncToEtcd(DeleteType type, bool is_complete_delete) {
    if (type == DeleteType::EVICTION) {
        return false;  // 驱逐不需要同步
    }
    
    if (!is_complete_delete) {
        return false;  // 部分删除不需要同步
    }
    
    // 完全删除需要同步
    return true;
}
```

### 2. 在删除点添加同步逻辑

```cpp
auto MasterService::Remove(const std::string& key)
    -> tl::expected<void, ErrorCode> {
    // ... 执行删除 ...
    accessor.Erase();
    
    // 同步到 etcd
    if (enable_ha_) {
        WriteDeleteEventToEtcd(key, DeleteType::EXPLICIT);
    }
    
    return {};
}

auto MasterService::PutRevoke(const std::string& key, ReplicaType replica_type)
    -> tl::expected<void, ErrorCode> {
    metadata.EraseReplica(replica_type);
    
    bool is_complete_delete = !metadata.IsValid();
    if (is_complete_delete) {
        accessor.Erase();
        
        // 同步到 etcd（仅当完全删除时）
        if (enable_ha_) {
            WriteDeleteEventToEtcd(key, DeleteType::PUT_REVOKE);
        }
    }
    
    return {};
}

void MasterService::ClearInvalidHandles() {
    for (auto& shard : metadata_shards_) {
        auto it = shard.metadata.begin();
        while (it != shard.metadata.end()) {
            if (CleanupStaleHandles(it->second)) {
                std::string key = it->first;
                it = shard.metadata.erase(it);
                
                // 同步到 etcd（仅当完全删除时）
                if (enable_ha_) {
                    WriteDeleteEventToEtcd(key, DeleteType::HANDLE_CLEANUP);
                }
            } else {
                ++it;
            }
        }
    }
}
```

---

## 频率估算

### 需要同步到 etcd 的事件频率

| 场景 | 频率估算 |
|------|---------|
| `Remove(key)` | 取决于用户操作，通常 **< 100 次/秒** |
| `RemoveByRegex()` | 取决于用户操作，通常 **< 10 次/秒**（但可能一次删除多个对象） |
| `RemoveAll()` | 很低，**< 1 次/小时**（运维操作） |
| `PutRevoke()` 完全删除 | 取决于 Put 失败率，通常 **< 10 次/秒** |
| `ClearInvalidHandles()` 完全删除 | 取决于节点下线频率，通常 **< 100 次/秒** |

**总计**：通常 **< 200 次/秒**，远低于驱逐事件的 130,000 次/秒

### 不需要同步的事件频率

| 场景 | 频率估算 |
|------|---------|
| `BatchEvict()` | **130,000 次/秒**（高内存使用率时） |

---

## 结论

### 需要同步到 etcd 的 Delete 事件

1. ✅ **用户显式删除**：`Remove()`, `RemoveByRegex()`, `RemoveAll()`
2. ⚠️ **Put 失败导致完全删除**：`PutRevoke()` 且对象没有其他 replica
3. ⚠️ **Handle 失效导致完全删除**：`ClearInvalidHandles()` 且对象所有 replica 都失效

### 不需要同步到 etcd 的 Delete 事件

1. ❌ **驱逐（Eviction）**：`BatchEvict()` - Standby 可以自己判断
2. ❌ **部分删除**：只删除部分 replica，对象仍然存在

### 频率对比

- **需要同步**：通常 < 200 次/秒
- **不需要同步（驱逐）**：可能达到 130,000 次/秒

**结论**：只同步用户显式删除和异常情况下的完全删除，可以大大减少 etcd 的压力，同时保证数据一致性。

