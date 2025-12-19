# 驱逐事件频率分析

## 代码分析结果

### 1. 驱逐线程检查频率

**关键代码**：`mooncake-store/include/master_service.h:401-402`

```cpp
static constexpr uint64_t kEvictionThreadSleepMs = 10;  // 10 ms sleep between eviction checks
```

**结论**：
- 驱逐线程**每 10 毫秒检查一次**是否需要执行驱逐
- 检查频率：**100 次/秒**

---

### 2. 驱逐触发条件

**关键代码**：`mooncake-store/src/master_service.cpp:658-669`

```cpp
void MasterService::EvictionThreadFunc() {
    while (eviction_running_) {
        double used_ratio = 
            MasterMetricManager::instance().get_global_used_ratio();
        if (used_ratio > eviction_high_watermark_ratio_ ||
            (need_eviction_ && eviction_ratio_ > 0.0)) {
            // 执行驱逐
            BatchEvict(evict_ratio_target, evict_ratio_lowerbound);
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kEvictionThreadSleepMs));
    }
}
```

**触发条件**：

1. **条件 A：内存使用率超过高水位线**
   ```
   used_ratio > eviction_high_watermark_ratio_
   ```
   - 默认配置：`eviction_high_watermark_ratio_ = 0.95` (95%)
   - 或 `1.0` (100%，取决于配置)

2. **条件 B：分配失败触发**
   ```
   need_eviction_ == true && eviction_ratio_ > 0.0
   ```
   - 当 `PutStart` 分配失败时，设置 `need_eviction_ = true`
   - 代码位置：`mooncake-store/src/master_service.cpp:397`

---

### 3. 默认配置参数

**配置文件**：`mooncake-store/conf/master.json`

```json
{
  "eviction_ratio": 0.1,                    // 10% 驱逐比例
  "eviction_high_watermark_ratio": 1.0      // 100% 高水位线
}
```

**常量定义**：`mooncake-store/include/types.h:30-31`

```cpp
static constexpr double DEFAULT_EVICTION_RATIO = 0.05;  // 5%
static constexpr double DEFAULT_EVICTION_HIGH_WATERMARK_RATIO = 0.95;  // 95%
```

---

### 4. 每次驱逐的对象数量

**关键代码**：`mooncake-store/src/master_service.cpp:663-668`

```cpp
double evict_ratio_target = std::max(
    eviction_ratio_,
    used_ratio - eviction_high_watermark_ratio_ + eviction_ratio_);
double evict_ratio_lowerbound = 
    std::max(evict_ratio_target * 0.5,
             used_ratio - eviction_high_watermark_ratio_);
```

**计算逻辑**：

- **目标驱逐比例** (`evict_ratio_target`)：
  - 取 `eviction_ratio_` 和 `(used_ratio - high_watermark + eviction_ratio_)` 的最大值
  - 例如：如果 `used_ratio = 0.98`, `high_watermark = 0.95`, `eviction_ratio = 0.1`
  - 则 `evict_ratio_target = max(0.1, 0.98 - 0.95 + 0.1) = max(0.1, 0.13) = 0.13`
  - 即驱逐 **13%** 的对象

- **最低驱逐比例** (`evict_ratio_lowerbound`)：
  - 取 `evict_ratio_target * 0.5` 和 `(used_ratio - high_watermark)` 的最大值
  - 例如：`evict_ratio_lowerbound = max(0.13 * 0.5, 0.98 - 0.95) = max(0.065, 0.03) = 0.065`
  - 即至少驱逐 **6.5%** 的对象

**实际驱逐数量**：
- 假设系统中有 **10,000 个对象**
- 目标驱逐：`10,000 * 0.13 = 1,300 个对象`
- 最低驱逐：`10,000 * 0.065 = 650 个对象`

---

## 驱逐频率评估

### 场景 1：正常情况（内存使用率 < 高水位线）

**频率**：
- 检查频率：**100 次/秒**（每 10ms）
- **实际执行频率**：**0 次/秒**（不触发驱逐）

**结论**：在正常情况下，**不会执行驱逐**，只是定期检查。

---

### 场景 2：内存使用率超过高水位线

**假设条件**：
- `eviction_high_watermark_ratio = 0.95` (95%)
- `eviction_ratio = 0.1` (10%)
- 当前内存使用率：`98%`

**频率**：
- 检查频率：**100 次/秒**
- **实际执行频率**：**100 次/秒**（每次检查都会触发）

**每次驱逐的对象数量**：
- `evict_ratio_target = max(0.1, 0.98 - 0.95 + 0.1) = 0.13`
- 假设有 10,000 个对象：**每次驱逐约 1,300 个对象**

**结论**：在高内存使用率下，**每秒执行 100 次驱逐**，每次驱逐大量对象。

---

### 场景 3：分配失败触发（need_eviction_ = true）

**触发时机**：
- 当 `PutStart` 分配内存失败时（`ErrorCode::NO_AVAILABLE_HANDLE`）
- 设置 `need_eviction_ = true`

**频率**：
- 检查频率：**100 次/秒**
- **实际执行频率**：**100 次/秒**（直到 `need_eviction_` 被重置为 `false`）

**重置条件**：
- 代码位置：`mooncake-store/src/master_service.cpp:909`
- 当 `evicted_count > 0` 时，设置 `need_eviction_ = false`

**结论**：分配失败时，**每秒执行 100 次驱逐**，直到成功驱逐对象。

---

## 驱逐事件的实际频率总结

| 场景 | 检查频率 | 执行频率 | 每次驱逐对象数 | 备注 |
|------|---------|---------|--------------|------|
| **正常情况** | 100 次/秒 | **0 次/秒** | 0 | 内存使用率 < 高水位线 |
| **高内存使用** | 100 次/秒 | **100 次/秒** | 5-13% 对象 | 内存使用率 > 高水位线 |
| **分配失败** | 100 次/秒 | **100 次/秒** | 5-13% 对象 | 直到成功驱逐 |

---

## 对 Delete 事件写入 etcd 方案的影响

### 1. 驱逐产生的 Delete 事件频率

**关键发现**：
- 驱逐操作会调用 `EraseReplica(ReplicaType::MEMORY)`
- 这相当于对每个被驱逐的对象执行"删除内存副本"操作
- 如果将这些驱逐事件也写入 etcd，频率会非常高

**频率估算**：
- **正常情况**：0 次/秒（无驱逐）
- **高内存使用**：假设每次驱逐 1,300 个对象
  - **130,000 次 Delete 事件/秒**（100 次/秒 × 1,300 对象/次）
- **分配失败**：同样可能达到 **130,000 次 Delete 事件/秒**

### 2. 对 etcd 的影响

**问题**：
- etcd 不适合处理如此高频的写入（130,000 次/秒）
- 会导致 etcd 性能严重下降
- 可能超过 etcd 的容量限制

**解决方案**：

#### 方案 A：不将驱逐事件写入 etcd（推荐）

**理由**：
- 驱逐是**自动的、周期性的**操作
- 驱逐的对象是**lease 已过期**的对象
- 这些对象在 Standby Master 上也会因为 lease 过期而被驱逐
- **不需要同步驱逐事件**

**实现**：
```cpp
auto MasterService::Remove(const std::string& key) 
    -> tl::expected<void, ErrorCode> {
    // 1. 执行本地删除
    auto result = RemoveLocal(key);
    
    // 2. 只将"显式删除"写入 etcd，不写入驱逐事件
    if (enable_ha_ && is_explicit_delete_) {  // 只处理显式删除
        WriteDeleteEventToEtcd(key);
    }
    
    return {};
}
```

#### 方案 B：批量写入驱逐事件

如果确实需要同步驱逐事件：

```cpp
class EvictionEventBuffer {
    std::vector<std::string> buffer_;
    std::mutex mutex_;
    
    void Flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (buffer_.empty()) return;
        
        // 批量写入 etcd（如每 1000 个事件一批）
        EtcdHelper::BatchPut(eviction_events_);
        buffer_.clear();
    }
};
```

- 批量写入减少 etcd 压力
- 但仍然可能影响性能

#### 方案 C：只同步关键驱逐事件

```cpp
bool ShouldSyncEvictionEvent(const std::string& key) {
    // 只同步：
    // 1. 最近活跃的 key
    // 2. 大对象的 key
    // 3. 有特殊标记的 key
    return IsRecentlyActive(key) || IsLargeObject(key) || HasSpecialFlag(key);
}
```

---

## 推荐方案

### 1. 只将显式 Delete 事件写入 etcd

**理由**：
- 驱逐是自动的、周期性的，Standby 也会因为 lease 过期而驱逐
- 显式 Delete 是用户操作，需要同步
- 减少 etcd 压力

### 2. 驱逐事件不需要同步

**理由**：
- 驱逐基于 lease 过期时间，这是**确定性的**
- Standby Master 可以通过检查 lease 过期时间来决定是否驱逐
- 不需要通过 etcd 同步

### 3. 实现建议

```cpp
// 区分显式删除和驱逐删除
enum class DeleteType {
    EXPLICIT,    // 用户显式调用 Remove()
    EVICTION     // 自动驱逐
};

auto MasterService::Remove(const std::string& key) 
    -> tl::expected<void, ErrorCode> {
    // 显式删除
    return RemoveInternal(key, DeleteType::EXPLICIT);
}

void MasterService::EvictObject(const std::string& key) {
    // 驱逐删除（不写入 etcd）
    RemoveInternal(key, DeleteType::EVICTION);
}

auto MasterService::RemoveInternal(const std::string& key, DeleteType type) 
    -> tl::expected<void, ErrorCode> {
    // 1. 执行本地删除
    auto result = RemoveLocal(key);
    
    // 2. 只将显式删除写入 etcd
    if (enable_ha_ && type == DeleteType::EXPLICIT) {
        WriteDeleteEventToEtcd(key);
    }
    
    return result;
}
```

---

## 总结

### 驱逐检查频率
- **100 次/秒**（每 10ms 检查一次）

### 驱逐执行频率
- **正常情况**：0 次/秒
- **高内存使用/分配失败**：100 次/秒

### 每次驱逐的对象数量
- **5-13%** 的对象（取决于配置和内存使用率）

### 对 etcd Delete 事件方案的影响
- **建议只同步显式 Delete 事件**，不同步驱逐事件
- 驱逐事件频率过高（可能达到 130,000 次/秒），不适合写入 etcd
- 驱逐基于 lease 过期时间，是确定性的，Standby 可以独立判断

