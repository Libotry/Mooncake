# Standby 驱逐与 Lease 续约冲突问题及解决方案

## 问题描述

### 核心矛盾

1. **Lease 续约机制**：
   - `ExistKey()` 会续约 lease（`master_service.cpp:213`）
   - `GetReplicaList()` 会续约 lease（`master_service.cpp:334`）
   - `GetReplicaListByRegex()` 会续约 lease（`master_service.cpp:301`）

2. **Standby 驱逐问题**：
   - 如果 Standby 自己根据 `lease_timeout` 判断是否驱逐
   - 但 Standby **没有收到 lease 续约信息**
   - Standby 会错误地认为 lease 已过期，**错误地驱逐一个实际上 lease 已续约的对象**

### 问题场景

```
时间线：
T1: Client 调用 GetReplicaList(key_A)
    → Primary: GrantLease(key_A)  // 续约 lease
    → Standby: ❌ 没有收到续约信息

T2: Standby 检查 lease_timeout
    → Standby 认为 key_A 的 lease 已过期（因为没收到续约）
    → Standby 错误地驱逐 key_A

T3: Primary 故障，Standby 升主
    → Standby 的 metadata 中 key_A 已被删除
    → 但实际 key_A 的 lease 在 Primary 上已续约，不应该被驱逐
```

---

## 解决方案

### 方案 A：同步所有 Lease 续约操作（推荐）

**核心思想**：将 `GrantLease` 操作也写入 OpLog/etcd，确保 Standby 能收到 lease 续约信息。

#### 设计要点

1. **OpLog 增加 LEASE_RENEW 类型**：

```cpp
enum class OpType : uint8_t {
    PUT_START = 1,
    PUT_END = 2,
    PUT_REVOKE = 3,
    REMOVE = 4,
    MOUNT_SEGMENT = 5,
    UNMOUNT_SEGMENT = 6,
    EVICTION = 7,
    LEASE_RENEW = 8,  // 新增：Lease 续约
};
```

2. **Primary 端：生成 LEASE_RENEW OpLog**：

```cpp
auto MasterService::GetReplicaList(std::string_view key)
    -> tl::expected<GetReplicaListResponse, ErrorCode> {
    // ... 获取 replica list ...
    
    // 续约 lease
    metadata.GrantLease(default_kv_lease_ttl_, default_kv_soft_pin_ttl_);
    
    // 生成 LEASE_RENEW OpLog
    if (enable_ha_) {
        op_log_manager_.Append(OpType::LEASE_RENEW, key, 
                              SerializeLeaseInfo(metadata.lease_timeout));
    }
    
    return GetReplicaListResponse(...);
}
```

3. **Standby 端：应用 LEASE_RENEW OpLog**：

```cpp
void HotStandbyService::ApplyOpLog(const OpLogEntry& entry) {
    if (entry.op_type == OpType::LEASE_RENEW) {
        // 更新 lease_timeout
        auto& metadata = GetMetadata(entry.object_key);
        auto lease_info = DeserializeLeaseInfo(entry.payload);
        metadata.lease_timeout = lease_info.lease_timeout;
    }
}
```

4. **Standby 可以安全地执行驱逐**：

```cpp
void HotStandbyService::BatchEvict(...) {
    auto now = std::chrono::steady_clock::now();
    
    for (auto& [key, metadata] : metadata_) {
        // 现在可以安全地判断，因为 lease_timeout 已同步
        if (metadata.IsLeaseExpired(now)) {
            EvictObject(key);
        }
    }
}
```

#### 优点

- ✅ Standby 和 Primary 的 `lease_timeout` **完全同步**
- ✅ Standby 可以安全地执行驱逐
- ✅ 逻辑清晰，易于实现

#### 缺点

- ⚠️ **需要同步大量 lease 续约操作**（每次 Get 都会续约）
- ⚠️ OpLog 压力增加（但比驱逐事件少，因为 Get 频率通常低于驱逐频率）

#### 频率估算

- **Get 操作频率**：取决于用户访问模式，通常 **< 10,000 次/秒**
- **驱逐事件频率**：可能达到 **130,000 次/秒**

**结论**：同步 lease 续约比同步驱逐事件更合理（频率更低）。

---

### 方案 B：Standby 不执行驱逐，只同步驱逐事件

**核心思想**：Standby 不执行驱逐，只接收 Primary 的驱逐事件。

#### 设计要点

1. **Primary 执行驱逐并生成 OpLog**：

```cpp
void MasterService::BatchEvict(...) {
    // ... 执行驱逐 ...
    
    // 生成 EVICTION OpLog
    for (const auto& evicted_key : evicted_keys) {
        op_log_manager_.Append(OpType::EVICTION, evicted_key, "");
    }
}
```

2. **Standby 只接收驱逐事件**：

```cpp
void HotStandbyService::ApplyOpLog(const OpLogEntry& entry) {
    if (entry.op_type == OpType::EVICTION) {
        // 应用驱逐操作
        EvictObject(entry.object_key);
    }
}
```

3. **Standby 不运行驱逐线程**：

```cpp
class HotStandbyService {
    // ❌ 不运行驱逐线程
    // void EvictionThreadFunc() { ... }  // 不实现
};
```

#### 优点

- ✅ Standby 不需要判断 lease 是否过期
- ✅ 逻辑简单，Standby 只负责接收和应用

#### 缺点

- ❌ **需要同步大量驱逐事件**（130,000 次/秒）
- ❌ OpLog/etcd 压力大
- ❌ Standby 无法独立判断，完全依赖 Primary

---

### 方案 C：保守的延迟驱逐策略

**核心思想**：Standby 使用更保守的策略，延迟执行驱逐，等待可能的 lease 续约信息。

#### 设计要点

1. **Standby 延迟驱逐**：

```cpp
void HotStandbyService::BatchEvict(...) {
    auto now = std::chrono::steady_clock::now();
    auto safe_eviction_time = now - std::chrono::seconds(lease_ttl_);  // 延迟一个 lease TTL
    
    for (auto& [key, metadata] : metadata_) {
        // 只驱逐"确定过期"的对象（lease_timeout < now - lease_ttl）
        if (metadata.lease_timeout < safe_eviction_time) {
            EvictObject(key);
        }
    }
}
```

2. **逻辑**：
   - 如果 lease TTL = 5 秒
   - Standby 只驱逐 `lease_timeout < now - 5秒` 的对象
   - 这样可以容忍最多 5 秒的 OpLog 延迟

#### 优点

- ✅ 不需要同步 lease 续约操作
- ✅ 可以容忍 OpLog 延迟

#### 缺点

- ❌ **内存效率低**：延迟驱逐导致内存不能及时释放
- ❌ **不精确**：仍然可能错误地驱逐（如果 OpLog 延迟 > lease TTL）

---

### 方案 D：混合方案 - Lease 续约同步 + Standby 驱逐

**核心思想**：同步 lease 续约操作，但 Standby 使用更保守的驱逐策略。

#### 设计要点

1. **同步 lease 续约**（同方案 A）
2. **Standby 使用保守策略**：

```cpp
void HotStandbyService::BatchEvict(...) {
    auto now = std::chrono::steady_clock::now();
    auto conservative_time = now - std::chrono::milliseconds(100);  // 保守 100ms
    
    for (auto& [key, metadata] : metadata_) {
        // 使用保守的时间点，容忍 OpLog 延迟
        if (metadata.lease_timeout < conservative_time) {
            EvictObject(key);
        }
    }
}
```

#### 优点

- ✅ 结合了方案 A 和 C 的优点
- ✅ 可以容忍 OpLog 延迟
- ✅ 内存效率较高

#### 缺点

- ⚠️ 需要同步 lease 续约操作
- ⚠️ 实现复杂度较高

---

## 推荐方案

### 首选：方案 A（同步所有 Lease 续约操作）

**理由**：

1. **频率可接受**：
   - Get 操作频率通常 < 10,000 次/秒
   - 远低于驱逐事件的 130,000 次/秒
   - 比同步驱逐事件更合理

2. **逻辑清晰**：
   - Standby 和 Primary 的 `lease_timeout` 完全同步
   - Standby 可以安全地执行驱逐
   - 不需要复杂的保守策略

3. **实现简单**：
   - 只需要在 `GrantLease` 时生成 OpLog
   - Standby 应用 OpLog 更新 `lease_timeout`

### 备选：方案 B（Standby 不执行驱逐）

**适用场景**：
- 如果 Get 操作频率非常高（> 10,000 次/秒）
- 或者 OpLog 同步 lease 续约的开销不可接受

---

## 实现细节

### 1. OpLog 条目结构

```cpp
struct LeaseRenewPayload {
    std::chrono::steady_clock::time_point lease_timeout;
    std::optional<std::chrono::steady_clock::time_point> soft_pin_timeout;
    
    YLT_REFL(LeaseRenewPayload, lease_timeout, soft_pin_timeout);
};
```

### 2. Primary 端实现

```cpp
auto MasterService::GetReplicaList(std::string_view key)
    -> tl::expected<GetReplicaListResponse, ErrorCode> {
    // ... 获取 replica list ...
    
    // 续约 lease
    metadata.GrantLease(default_kv_lease_ttl_, default_kv_soft_pin_ttl_);
    
    // 生成 LEASE_RENEW OpLog
    if (enable_ha_ && op_log_manager_) {
        LeaseRenewPayload payload;
        payload.lease_timeout = metadata.lease_timeout;
        payload.soft_pin_timeout = metadata.soft_pin_timeout;
        
        op_log_manager_->Append(OpType::LEASE_RENEW, std::string(key),
                                Serialize(payload));
    }
    
    return GetReplicaListResponse(...);
}

auto MasterService::ExistKey(const std::string& key)
    -> tl::expected<bool, ErrorCode> {
    // ... 检查对象是否存在 ...
    
    if (found) {
        // 续约 lease
        metadata.GrantLease(default_kv_lease_ttl_, default_kv_soft_pin_ttl_);
        
        // 生成 LEASE_RENEW OpLog
        if (enable_ha_ && op_log_manager_) {
            // 同上
        }
    }
    
    return found;
}
```

### 3. Standby 端实现

```cpp
void HotStandbyService::ApplyOpLog(const OpLogEntry& entry) {
    switch (entry.op_type) {
        case OpType::LEASE_RENEW: {
            auto payload = Deserialize<LeaseRenewPayload>(entry.payload);
            
            // 更新 metadata 的 lease_timeout
            auto& metadata = GetMetadata(entry.object_key);
            metadata.lease_timeout = payload.lease_timeout;
            metadata.soft_pin_timeout = payload.soft_pin_timeout;
            break;
        }
        // ... 其他操作类型 ...
    }
}

void HotStandbyService::BatchEvict(...) {
    auto now = std::chrono::steady_clock::now();
    
    // 现在可以安全地判断，因为 lease_timeout 已同步
    for (auto& [key, metadata] : metadata_) {
        if (metadata.IsLeaseExpired(now)) {
            EvictObject(key);
        }
    }
}
```

---

## 频率对比

| 操作 | 频率 | 是否需要同步 | 备注 |
|------|------|-------------|------|
| **GetReplicaList** | < 10,000 次/秒 | ✅ 需要（续约 lease） | 用户访问 |
| **ExistKey** | < 5,000 次/秒 | ✅ 需要（续约 lease） | 用户查询 |
| **驱逐事件** | 130,000 次/秒 | ❌ 不需要 | Standby 自己判断 |

**结论**：
- 同步 lease 续约：< 15,000 次/秒
- 同步驱逐事件：130,000 次/秒

**同步 lease 续约比同步驱逐事件更合理**。

---

## 总结

### 问题确认

✅ **你的担心是正确的**：
- Lease 可以通过 `GetReplicaList` 和 `ExistKey` 续约
- 如果 Standby 没有收到续约信息，会错误地驱逐对象

### 推荐解决方案

**方案 A：同步所有 Lease 续约操作**

1. **在 OpLog 中增加 `LEASE_RENEW` 类型**
2. **Primary 在 `GrantLease` 时生成 OpLog**
3. **Standby 应用 OpLog 更新 `lease_timeout`**
4. **Standby 可以安全地执行驱逐**

### 优势

- ✅ 频率可接受（< 15,000 次/秒 vs 130,000 次/秒）
- ✅ 逻辑清晰，实现简单
- ✅ Standby 和 Primary 的 `lease_timeout` 完全同步

### 对 etcd Delete 事件方案的影响

- **仍然只同步显式 Delete 事件**（Remove/RemoveByRegex/RemoveAll）
- **不同步驱逐事件**（Standby 自己判断，但需要先同步 lease 续约）
- **需要同步 lease 续约操作**（通过 OpLog，不是 etcd）

