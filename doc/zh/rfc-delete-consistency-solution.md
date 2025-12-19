# RFC：Delete 操作主备一致性保障方案

## 1. 问题描述

### 1.1 问题场景

```
┌─────────────────────────────────────────────────────────────────┐
│                    问题场景时间线                                │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  T0: Client 发起 Delete(key) 请求                                │
│      │                                                           │
│      ▼                                                           │
│  T1: Master Primary 执行删除                                    │
│      - 从本地 metadata 中删除 key                               │
│      - 返回成功给 Client                                         │
│      │                                                           │
│      ▼                                                           │
│  T2: Master Primary 尝试同步 Delete 事件到 Standby              │
│      │                                                           │
│      ▼                                                           │
│  T3: Master Primary 在同步过程中宕机 ✗                           │
│      │                                                           │
│      ▼                                                           │
│  T4: Standby 未收到 Delete 事件                                 │
│      - Standby 的 metadata 中 key 仍然存在                      │
│      │                                                           │
│      ▼                                                           │
│  T5: Standby 升主                                                │
│      │                                                           │
│      ▼                                                           │
│  T6: 新 Master 处理 Get(key) 请求                                │
│      - 从 metadata 中查询到 key（误报存在）                      │
│      - 返回 replica 地址给 Client                                │
│      │                                                           │
│      ▼                                                           │
│  T7: Client 使用 replica 地址访问                                │
│      - Replica 已被删除，访问失败 ✗                              │
│      - 产生错误输出                                              │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 1.2 问题本质

- **误报存在（False Positive）**：备 Master 认为对象存在，但实际已被删除
- **主备状态不一致**：主已删除，备未删除
- **数据完整性风险**：客户端拿到无效地址，导致业务错误

### 1.3 影响范围

- **读操作**：返回已删除对象的地址
- **写操作**：可能尝试写入已删除的位置
- **业务影响**：客户端错误、数据不一致

---

## 2. 解决方案设计

### 2.1 方案概览

```
┌─────────────────────────────────────────────────────────────────┐
│                    解决方案分类                                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  方案A：两阶段删除（2PC）                                        │
│  ────────────────────────                                       │
│  先同步后删除，确保主备一致                                       │
│                                                                 │
│  方案B：删除标记 + 延迟删除                                      │
│  ─────────────────────────                                     │
│  先标记为删除，同步后再真正删除                                   │
│                                                                 │
│  方案C：版本号验证机制                                           │
│  ───────────────────                                           │
│  查询时验证 replica 版本，检测已删除                              │
│                                                                 │
│  方案D：客户端验证机制                                           │
│  ───────────────────                                           │
│  客户端拿到地址后验证 replica 是否存在                            │
│                                                                 │
│  方案E：基于消息队列的删除                                       │
│  ─────────────────────                                         │
│  删除操作也通过队列，保证不丢                                     │
│                                                                 │
│  方案F：定期一致性校验                                           │
│  ───────────────────                                           │
│  定期检测并修复主备不一致                                         │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 方案 A：两阶段删除（2PC）

### 核心思想

> **删除操作分为两个阶段：Prepare（准备）和 Commit（提交），只有多数派确认后才真正删除。**

### 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                    两阶段删除流程                                │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  阶段1：Prepare（准备删除）                                      │
│  ────────────────────────                                      │
│  Client ──▶ Master Primary                                     │
│              │                                                  │
│              ├─▶ 1. 标记 key 为 "PREPARED_DELETE"               │
│              │                                                  │
│              ├─▶ 2. 发送 PrepareDelete 到所有 Standby           │
│              │                                                  │
│              └─▶ 3. 等待多数派确认                               │
│                                                                 │
│  阶段2：Commit（提交删除）                                       │
│  ─────────────────────                                         │
│  Master Primary                                                │
│    │                                                           │
│    ├─▶ 1. 真正删除 key                                         │
│    │                                                           │
│    ├─▶ 2. 发送 CommitDelete 到所有 Standby                     │
│    │                                                           │
│    └─▶ 3. 返回成功给 Client                                     │
│                                                                 │
│  如果 Prepare 阶段失败：                                         │
│  - 回滚删除标记                                                 │
│  - 返回失败给 Client                                            │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 实现细节

```cpp
// ============================================================
// 删除状态枚举
// ============================================================

enum class DeleteState {
    NORMAL,              // 正常状态
    PREPARED_DELETE,     // 准备删除（已同步，未真正删除）
    DELETED              // 已删除
};

struct ObjectMetadata {
    // ... 原有字段 ...
    DeleteState delete_state = DeleteState::NORMAL;
    uint64_t delete_prepare_time = 0;  // Prepare 时间戳
};

// ============================================================
// 两阶段删除实现
// ============================================================

class MasterService {
public:
    auto Remove(const std::string& key)
        -> tl::expected<void, ErrorCode> {
        
        MetadataAccessor accessor(this, key);
        if (!accessor.Exists()) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }
        
        auto& metadata = accessor.Get();
        
        // 检查前置条件
        if (!metadata.IsLeaseExpired()) {
            return tl::make_unexpected(ErrorCode::OBJECT_HAS_LEASE);
        }
        
        if (!metadata.IsAllReplicasComplete()) {
            return tl::make_unexpected(ErrorCode::REPLICA_IS_NOT_READY);
        }
        
        // ========== 阶段1：Prepare ==========
        
        // 1. 标记为准备删除
        metadata.delete_state = DeleteState::PREPARED_DELETE;
        metadata.delete_prepare_time = Now();
        
        // 2. 同步到 Standby（需要多数派确认）
        auto prepare_result = ReplicationManager::PrepareDelete(key, metadata);
        
        if (!prepare_result.ok()) {
            // Prepare 失败，回滚
            metadata.delete_state = DeleteState::NORMAL;
            return tl::make_unexpected(ErrorCode::REPLICATION_FAILED);
        }
        
        // ========== 阶段2：Commit ==========
        
        // 3. 真正删除
        accessor.Erase();
        
        // 4. 通知 Standby 提交删除
        ReplicationManager::CommitDelete(key);
        
        return {};
    }
    
    // Standby 端处理 PrepareDelete
    void HandlePrepareDelete(const std::string& key, 
                            const ObjectMetadata& metadata) {
        MetadataAccessor accessor(this, key);
        if (!accessor.Exists()) {
            // 如果不存在，创建标记为准备删除的条目
            // 这样可以保证主备一致
            accessor.Create(metadata);
            accessor.Get().delete_state = DeleteState::PREPARED_DELETE;
        } else {
            accessor.Get().delete_state = DeleteState::PREPARED_DELETE;
        }
        
        // 返回确认
        return ReplicationResponse::OK;
    }
    
    // Standby 端处理 CommitDelete
    void HandleCommitDelete(const std::string& key) {
        MetadataAccessor accessor(this, key);
        if (accessor.Exists() && 
            accessor.Get().delete_state == DeleteState::PREPARED_DELETE) {
            accessor.Erase();
        }
    }
    
    // 查询时检查删除状态
    auto GetReplicaList(const std::string& key)
        -> tl::expected<std::vector<Replica::Descriptor>, ErrorCode> {
        
        MetadataAccessor accessor(this, key);
        if (!accessor.Exists()) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }
        
        auto& metadata = accessor.Get();
        
        // 如果处于准备删除状态，返回错误
        if (metadata.delete_state == DeleteState::PREPARED_DELETE) {
            // 检查是否超时（防止 Prepare 后主宕机，一直处于准备状态）
            if (Now() - metadata.delete_prepare_time > PREPARE_TIMEOUT) {
                // 超时，自动提交删除
                accessor.Erase();
                return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
            }
            
            // 未超时，返回错误（对象正在删除中）
            return tl::make_unexpected(ErrorCode::OBJECT_DELETING);
        }
        
        // 正常返回
        return metadata.GetReplicaDescriptors();
    }
    
private:
    static constexpr uint64_t PREPARE_TIMEOUT = 5000;  // 5秒超时
};
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| ✅ 保证主备一致 | ❌ 删除延迟增加（需等待确认） |
| ✅ 可以回滚 | ❌ 实现复杂度高 |
| ✅ 强一致性 | ❌ 需要处理超时和恢复 |

---

## 方案 B：删除标记 + 延迟删除

### 核心思想

> **先标记为删除（Tombstone），同步后再真正删除，查询时过滤已标记的对象。**

### 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                    删除标记 + 延迟删除流程                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  T0: Client ──▶ Delete(key)                                    │
│      │                                                          │
│      ▼                                                          │
│  T1: Master Primary                                             │
│      - 标记 key 为 DELETED（Tombstone）                         │
│      - 设置删除时间戳                                            │
│      - 同步标记到 Standby                                        │
│      - 返回成功给 Client                                         │
│      │                                                          │
│      ▼                                                          │
│  T2: 延迟删除线程（定期扫描）                                    │
│      - 扫描所有 DELETED 标记的对象                              │
│      - 检查是否已同步到所有 Standby                              │
│      - 如果已同步，真正删除 metadata                             │
│      │                                                          │
│  T3: 查询时过滤                                                 │
│      - Get(key) 时检查是否有 DELETED 标记                       │
│      - 如果有标记，返回 NOT_FOUND                                │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 实现细节

```cpp
// ============================================================
// 删除标记实现
// ============================================================

struct ObjectMetadata {
    // ... 原有字段 ...
    bool is_deleted = false;              // 删除标记
    uint64_t delete_timestamp = 0;        // 删除时间戳
    uint64_t delete_synced_to = 0;         // 已同步到的 Standby 版本
};

class MasterService {
public:
    auto Remove(const std::string& key)
        -> tl::expected<void, ErrorCode> {
        
        MetadataAccessor accessor(this, key);
        if (!accessor.Exists()) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }
        
        auto& metadata = accessor.Get();
        
        // 检查前置条件
        if (!metadata.IsLeaseExpired()) {
            return tl::make_unexpected(ErrorCode::OBJECT_HAS_LEASE);
        }
        
        // 1. 标记为删除（不真正删除）
        metadata.is_deleted = true;
        metadata.delete_timestamp = Now();
        
        // 2. 同步删除标记到 Standby
        ReplicationManager::ReplicateDelete(key, metadata);
        
        // 3. 立即返回成功（不等待同步完成）
        return {};
    }
    
    // 查询时过滤已删除对象
    auto GetReplicaList(const std::string& key)
        -> tl::expected<std::vector<Replica::Descriptor>, ErrorCode> {
        
        MetadataAccessor accessor(this, key);
        if (!accessor.Exists()) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }
        
        auto& metadata = accessor.Get();
        
        // 检查删除标记
        if (metadata.is_deleted) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }
        
        return metadata.GetReplicaDescriptors();
    }
    
    // 延迟删除线程
    void DelayedDeleteThread() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            CleanupDeletedObjects();
        }
    }
    
private:
    void CleanupDeletedObjects() {
        auto now = Now();
        
        for (auto& shard : metadata_shards_) {
            MutexLocker lock(&shard.mutex);
            
            auto it = shard.metadata.begin();
            while (it != shard.metadata.end()) {
                auto& metadata = it->second;
                
                if (metadata.is_deleted) {
                    // 检查是否已同步到所有 Standby
                    if (IsDeleteSyncedToAllStandby(it->first)) {
                        // 检查延迟时间（确保 Standby 有时间同步）
                        if (now - metadata.delete_timestamp > DELETE_DELAY) {
                            // 真正删除
                            it = shard.metadata.erase(it);
                            continue;
                        }
                    }
                }
                
                ++it;
            }
        }
    }
    
    bool IsDeleteSyncedToAllStandby(const std::string& key) {
        // 检查所有 Standby 是否已收到删除标记
        // 可以通过版本号或确认机制实现
        return replication_manager_->IsDeleteSynced(key);
    }
    
    static constexpr uint64_t DELETE_DELAY = 10000;  // 10秒延迟
};
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| ✅ 删除操作立即返回 | ❌ 需要额外的清理线程 |
| ✅ 查询时自动过滤 | ❌ 删除标记占用空间 |
| ✅ 实现相对简单 | ❌ 需要处理 Standby 同步确认 |

---

## 方案 C：版本号验证机制

### 核心思想

> **每个对象和 replica 都有版本号，查询时验证版本号，检测已删除的 replica。**

### 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                    版本号验证机制                                │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  写入时：                                                         │
│  ──────                                                          │
│  Put(key, val) ──▶ 生成版本号 v1 ──▶ 存储到 metadata             │
│                                                                 │
│  删除时：                                                         │
│  ──────                                                          │
│  Delete(key) ──▶ 记录删除版本号 v2 ──▶ 同步到 Standby            │
│                                                                 │
│  查询时：                                                         │
│  ──────                                                          │
│  Get(key) ──▶ 返回 replica 地址 + 版本号 v1                     │
│                                                                 │
│  客户端验证：                                                     │
│  ────────                                                        │
│  Client 访问 replica ──▶ 携带版本号 v1                           │
│  Replica 检查版本号 ──▶ 如果已删除，返回版本不匹配                │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 实现细节

```cpp
// ============================================================
// 版本号机制实现
// ============================================================

struct ObjectMetadata {
    // ... 原有字段 ...
    uint64_t version = 1;                 // 对象版本号
    uint64_t delete_version = 0;          // 删除版本号（0表示未删除）
};

class MasterService {
public:
    auto Remove(const std::string& key)
        -> tl::expected<void, ErrorCode> {
        
        MetadataAccessor accessor(this, key);
        if (!accessor.Exists()) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }
        
        auto& metadata = accessor.Get();
        
        // 1. 设置删除版本号（不真正删除）
        metadata.delete_version = metadata.version + 1;
        
        // 2. 同步删除版本号到 Standby
        ReplicationManager::ReplicateDeleteVersion(key, metadata.delete_version);
        
        // 3. 延迟真正删除（给 Standby 时间同步）
        ScheduleDelayedDelete(key, metadata.delete_version);
        
        return {};
    }
    
    auto GetReplicaList(const std::string& key)
        -> tl::expected<std::vector<Replica::Descriptor>, ErrorCode> {
        
        MetadataAccessor accessor(this, key);
        if (!accessor.Exists()) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }
        
        auto& metadata = accessor.Get();
        
        // 检查是否已删除
        if (metadata.delete_version > 0) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }
        
        // 返回 replica 描述符，包含版本号
        auto descriptors = metadata.GetReplicaDescriptors();
        for (auto& desc : descriptors) {
            desc.version = metadata.version;  // 附加版本号
        }
        
        return descriptors;
    }
};

// ============================================================
// Replica 端版本验证
// ============================================================

class ReplicaService {
public:
    ErrorCode Read(const std::string& key, 
                   uint64_t expected_version,
                   void* buffer, size_t size) {
        
        // 1. 检查对象是否存在
        if (!Exists(key)) {
            return ErrorCode::OBJECT_NOT_FOUND;
        }
        
        // 2. 检查版本号
        auto current_version = GetVersion(key);
        if (current_version != expected_version) {
            // 版本不匹配，可能已被删除
            return ErrorCode::VERSION_MISMATCH;
        }
        
        // 3. 读取数据
        return DoRead(key, buffer, size);
    }
};
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| ✅ 客户端可以检测到已删除 | ❌ 需要 Replica 端支持版本验证 |
| ✅ 不依赖主备同步 | ❌ 增加系统复杂度 |
| ✅ 可以检测并发修改 | ❌ 版本号管理复杂 |

---

## 方案 D：客户端验证机制

### 核心思想

> **客户端拿到 replica 地址后，先验证 replica 是否存在，再使用。**

### 实现细节

```cpp
class ResilientClient {
public:
    Result<Data> Get(const std::string& key) {
        // 1. 从 Master 获取 replica 地址
        auto replica_list = master_client_.GetReplicaList(key);
        if (!replica_list.ok()) {
            return replica_list.error();
        }
        
        // 2. 验证 replica 是否存在
        for (const auto& replica : replica_list.value()) {
            if (ValidateReplica(replica)) {
                // 3. 验证通过，读取数据
                return ReadFromReplica(replica);
            }
        }
        
        // 所有 replica 都无效，返回错误
        return ErrorCode::ALL_REPLICAS_INVALID;
    }
    
private:
    bool ValidateReplica(const Replica::Descriptor& replica) {
        // 方式1: 发送轻量级探测请求
        auto result = replica_client_.Probe(replica.address);
        if (!result.ok()) {
            return false;
        }
        
        // 方式2: 尝试读取少量数据验证
        // 方式3: 检查 replica 的元数据
        
        return true;
    }
};
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| ✅ 实现简单 | ❌ 增加客户端延迟 |
| ✅ 不依赖服务端 | ❌ 需要额外的网络请求 |
| ✅ 可以检测各种错误 | ❌ 客户端逻辑复杂 |

---

## 方案 E：基于消息队列的删除

### 核心思想

> **删除操作也通过消息队列（EDQ/Kafka），保证主备都能收到删除事件。**

### 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                    基于消息队列的删除                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Client ──▶ Delete(key)                                        │
│      │                                                          │
│      ▼                                                          │
│  Master Primary                                                 │
│      │                                                          │
│      ├─▶ 1. 写入删除事件到消息队列                               │
│      │                                                          │
│      ├─▶ 2. 等待队列确认（Quorum）                               │
│      │                                                          │
│      ├─▶ 3. 从本地 metadata 删除                                │
│      │                                                          │
│      └─▶ 4. 返回成功给 Client                                   │
│                                                                 │
│  消息队列 ──▶ Standby Master                                    │
│      │                                                          │
│      └─▶ 消费删除事件，从 metadata 删除                         │
│                                                                 │
│  结果：主备都能收到删除事件，保证一致                             │
└─────────────────────────────────────────────────────────────────┘
```

### 实现细节

```cpp
class MasterService {
public:
    auto Remove(const std::string& key)
        -> tl::expected<void, ErrorCode> {
        
        MetadataAccessor accessor(this, key);
        if (!accessor.Exists()) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }
        
        // 1. 构造删除事件
        DeleteEvent event{
            .key = key,
            .timestamp = Now(),
            .version = accessor.Get().version
        };
        
        // 2. 写入消息队列（保证持久化）
        auto result = message_queue_->Produce("delete-events", event);
        if (!result.ok()) {
            return tl::make_unexpected(ErrorCode::QUEUE_WRITE_FAILED);
        }
        
        // 3. 应用到本地（作为消费者也会收到）
        // 或者直接删除
        accessor.Erase();
        
        return {};
    }
    
    // 消费者处理删除事件
    void HandleDeleteEvent(const DeleteEvent& event) {
        MetadataAccessor accessor(this, event.key);
        if (accessor.Exists()) {
            // 检查版本号（防止旧事件覆盖新数据）
            if (accessor.Get().version <= event.version) {
                accessor.Erase();
            }
        }
    }
};
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| ✅ 保证删除事件不丢失 | ❌ 需要消息队列基础设施 |
| ✅ 主备都能收到事件 | ❌ 删除延迟增加 |
| ✅ 可以回溯删除历史 | ❌ 需要处理重复事件 |

---

## 方案 F：定期一致性校验

### 核心思想

> **定期比较主备 metadata，检测不一致并修复。**

### 实现细节

```cpp
class ConsistencyChecker {
public:
    void Start() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::minutes(5));
            
            CheckConsistency();
        }
    }
    
private:
    void CheckConsistency() {
        // 1. 获取主 Master 的所有 key
        auto primary_keys = GetPrimaryKeys();
        
        // 2. 获取备 Master 的所有 key
        auto standby_keys = GetStandbyKeys();
        
        // 3. 比较差异
        std::set<std::string> only_in_primary;
        std::set<std::string> only_in_standby;
        
        std::set_difference(primary_keys.begin(), primary_keys.end(),
                          standby_keys.begin(), standby_keys.end(),
                          std::inserter(only_in_primary, only_in_primary.begin()));
        
        std::set_difference(standby_keys.begin(), standby_keys.end(),
                          primary_keys.begin(), primary_keys.end(),
                          std::inserter(only_in_standby, only_in_standby.begin()));
        
        // 4. 修复不一致
        for (const auto& key : only_in_primary) {
            // 主有备无：同步到备
            SyncToStandby(key);
        }
        
        for (const auto& key : only_in_standby) {
            // 备有主无：可能是主已删除但备未收到
            // 验证 replica 是否真的存在
            if (!ValidateReplicaExists(key)) {
                // Replica 不存在，删除备上的记录
                RemoveFromStandby(key);
            }
        }
    }
    
    bool ValidateReplicaExists(const std::string& key) {
        // 尝试访问 replica，验证是否存在
        auto replica_list = GetReplicaList(key);
        for (const auto& replica : replica_list) {
            if (ProbeReplica(replica.address)) {
                return true;
            }
        }
        return false;
    }
};
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| ✅ 可以修复历史不一致 | ❌ 只能定期检测，不能实时 |
| ✅ 作为兜底机制 | ❌ 需要额外的网络开销 |
| ✅ 实现相对简单 | ❌ 可能误删有效数据 |

---

## 3. 推荐方案组合

### 最优组合：删除标记 + 消息队列 + 客户端验证

```
┌─────────────────────────────────────────────────────────────────┐
│                    三层防护机制                                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  第一层：删除标记（服务端）                                      │
│  ────────────────────────                                      │
│  - Delete 操作立即标记为删除                                     │
│  - 查询时自动过滤已删除对象                                      │
│  - 延迟真正删除（给 Standby 时间同步）                           │
│                                                                 │
│  第二层：消息队列（持久化）                                      │
│  ────────────────────────                                      │
│  - 删除事件写入消息队列                                          │
│  - 保证 Standby 能收到删除事件                                   │
│  - 即使主宕机，事件也不丢失                                      │
│                                                                 │
│  第三层：客户端验证（兜底）                                      │
│  ────────────────────────                                      │
│  - 客户端拿到地址后验证 replica 是否存在                          │
│  - 如果不存在，重试其他 replica 或报错                           │
│  - 作为最后一道防线                                              │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 实现示例

```cpp
class MasterService {
public:
    auto Remove(const std::string& key)
        -> tl::expected<void, ErrorCode> {
        
        MetadataAccessor accessor(this, key);
        if (!accessor.Exists()) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }
        
        auto& metadata = accessor.Get();
        
        // 1. 标记为删除
        metadata.is_deleted = true;
        metadata.delete_timestamp = Now();
        
        // 2. 写入消息队列（保证持久化）
        DeleteEvent event{.key = key, .version = metadata.version};
        message_queue_->Produce("delete-events", event);
        
        // 3. 同步到 Standby（异步）
        ReplicationManager::ReplicateDelete(key, metadata);
        
        // 4. 立即返回
        return {};
    }
    
    auto GetReplicaList(const std::string& key)
        -> tl::expected<std::vector<Replica::Descriptor>, ErrorCode> {
        
        MetadataAccessor accessor(this, key);
        if (!accessor.Exists()) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }
        
        auto& metadata = accessor.Get();
        
        // 检查删除标记
        if (metadata.is_deleted) {
            return tl::make_unexpected(ErrorCode::OBJECT_NOT_FOUND);
        }
        
        return metadata.GetReplicaDescriptors();
    }
};
```

---

## 4. 总结

### 方案对比

| 方案 | 一致性保证 | 实现复杂度 | 性能影响 | 推荐度 |
|------|-----------|-----------|---------|--------|
| 两阶段删除 | 强一致 | 高 | 延迟+5-10ms | ⭐⭐⭐ |
| 删除标记 | 最终一致 | 中 | 无影响 | ⭐⭐⭐⭐ |
| 版本号验证 | 强一致 | 高 | 客户端延迟 | ⭐⭐⭐ |
| 客户端验证 | 最终一致 | 低 | 客户端延迟 | ⭐⭐⭐⭐ |
| 消息队列 | 强一致 | 中 | 延迟+2-5ms | ⭐⭐⭐⭐⭐ |
| 一致性校验 | 最终一致 | 中 | 定期开销 | ⭐⭐⭐ |

### 最终推荐

> **推荐采用「删除标记 + 消息队列 + 客户端验证」三层防护机制**：
> - **删除标记**：服务端立即标记，查询时过滤
> - **消息队列**：保证删除事件不丢失，Standby 能收到
> - **客户端验证**：最后一道防线，检测无效 replica

这样可以最大程度避免误报存在的问题，同时保证系统性能和可用性。
