# 主备切换窗口期数据零丢失方案对比

## 问题回顾

```
┌─────────────────────────────────────────────────────────────────┐
│                    窗口期数据丢失场景                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  T0: Client ──▶ Primary ──▶ 内存状态（未同步）                   │
│  T1: Primary 宕机                                                │
│  T2-T5: 无主窗口期（3-5秒）                                      │
│  T6: Standby 升主                                               │
│                                                                 │
│  结果：T0 的写入数据丢失 ✗                                       │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 方案全景图

```
┌─────────────────────────────────────────────────────────────────┐
│                    解决方案分类                                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  1. 客户端侧方案                                                 │
│     ├─ 客户端缓冲重试                                            │
│     ├─ 客户端多主写入                                            │
│     └─ 客户端幂等性保证                                         │
│                                                                 │
│  2. 服务端侧方案                                                 │
│     ├─ 乐观接管（已讨论）                                        │
│     ├─ 消息队列（已讨论）                                        │
│     ├─ 直接写入 etcd                                             │
│     ├─ 共享存储（NFS/分布式文件系统）                             │
│     ├─ 数据库事务日志                                            │
│     └─ Redis Streams                                            │
│                                                                 │
│  3. 架构级方案                                                   │
│     ├─ 多主架构（Multi-Master）                                  │
│     ├─ 基于 Raft 的强一致性写入                                  │
│     ├─ 事件溯源（Event Sourcing）                                │
│     └─ CQRS（命令查询职责分离）                                  │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 方案 1：客户端缓冲重试

### 核心思想

> **客户端在窗口期检测到 Master 不可用，自动缓冲请求并重试，直到新 Master 上线。**

### 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                    客户端缓冲重试架构                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌──────────────┐                                             │
│   │   Client     │                                             │
│   │ ┌──────────┐ │                                             │
│   │ │ 写入请求  │ │                                             │
│   │ └────┬─────┘ │                                             │
│   │      │       │                                             │
│   │      ▼       │                                             │
│   │ ┌──────────┐ │                                             │
│   │ │ 请求缓冲  │ │ ◀── 窗口期请求暂存                          │
│   │ │ (内存/磁盘)│ │                                             │
│   │ └────┬─────┘ │                                             │
│   │      │       │                                             │
│   │      ▼       │                                             │
│   │ ┌──────────┐ │                                             │
│   │ │ 重试逻辑  │ │ ◀── 检测 Master 恢复后自动重试               │
│   │ └────┬─────┘ │                                             │
│   └──────┼───────┘                                             │
│          │                                                     │
│          ▼                                                     │
│   ┌──────────────┐                                             │
│   │ Master       │                                             │
│   └──────────────┘                                             │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 实现细节

```cpp
class ResilientClient {
public:
    ErrorCode Put(const std::string& key, const ObjectMetadata& metadata) {
        // 1. 尝试直接写入
        auto result = master_client_.Put(key, metadata);
        
        if (result == ErrorCode::OK) {
            return ErrorCode::OK;
        }
        
        // 2. 如果失败，缓冲请求
        if (IsRetryableError(result)) {
            BufferRequest(key, metadata);
            
            // 3. 异步重试
            StartRetryLoop();
            
            // 4. 立即返回（或阻塞等待）
            if (sync_mode_) {
                return WaitForRetry(key);
            } else {
                return ErrorCode::OK;  // 异步模式
            }
        }
        
        return result;
    }
    
private:
    void StartRetryLoop() {
        if (retry_thread_running_) return;
        
        retry_thread_running_ = true;
        retry_thread_ = std::thread([this]() {
            while (!buffered_requests_.empty() || retry_thread_running_) {
                // 检测 Master 是否恢复
                if (ProbeMaster()) {
                    // 批量重试
                    RetryBufferedRequests();
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });
    }
    
    void BufferRequest(const std::string& key, const ObjectMetadata& metadata) {
        BufferedRequest req{
            .key = key,
            .value = Serialize(metadata),
            .timestamp = Now(),
            .retry_count = 0
        };
        
        // 持久化到本地（防止进程重启丢失）
        if (persistent_buffer_) {
            persistent_buffer_->Append(req);
        }
        
        buffered_requests_.push_back(req);
    }
    
    void RetryBufferedRequests() {
        auto it = buffered_requests_.begin();
        while (it != buffered_requests_.end()) {
            auto result = master_client_.Put(it->key, 
                Deserialize<ObjectMetadata>(it->value));
            
            if (result == ErrorCode::OK) {
                // 成功，从缓冲中移除
                if (persistent_buffer_) {
                    persistent_buffer_->MarkCompleted(it->key);
                }
                it = buffered_requests_.erase(it);
            } else if (it->retry_count++ > MAX_RETRIES) {
                // 超过重试次数，丢弃或告警
                LOG_ERROR("Request failed after max retries: " << it->key);
                it = buffered_requests_.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    bool ProbeMaster() {
        // 方式1: 尝试连接
        // 方式2: 查询 etcd 获取 Leader
        // 方式3: 发送心跳请求
        return master_client_.Heartbeat();
    }
    
    std::vector<BufferedRequest> buffered_requests_;
    std::unique_ptr<PersistentBuffer> persistent_buffer_;
    std::thread retry_thread_;
    bool retry_thread_running_ = false;
    
    static constexpr int MAX_RETRIES = 100;
};
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| ✅ 无需服务端改动 | ❌ 客户端内存/磁盘占用 |
| ✅ 实现简单 | ❌ 客户端进程重启可能丢数据 |
| ✅ 对服务端透明 | ❌ 需要客户端配合改造 |
| ✅ 可配置同步/异步 | ❌ 重试可能造成重复写入（需幂等） |

### 适用场景

- 客户端可控（内部服务）
- 可接受客户端内存/磁盘开销
- 写入操作具备幂等性

---

## 方案 2：直接写入 etcd

### 核心思想

> **绕过 Master，客户端直接写入 etcd，Master 从 etcd 读取并应用。**

### 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                    直接写入 etcd 架构                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌──────────────┐                                             │
│   │   Client     │                                             │
│   │              │                                             │
│   │  Put(key, val)│                                             │
│   └──────┬───────┘                                             │
│          │                                                     │
│          │ 直接写入                                             │
│          ▼                                                     │
│   ┌──────────────┐                                             │
│   │     etcd     │ ◀── 强一致性存储，天然高可用                 │
│   │   Cluster    │                                             │
│   │  /mooncake/  │                                             │
│   │   metadata/  │                                             │
│   └──────┬───────┘                                             │
│          │                                                     │
│          │ Watch 监听                                           │
│          ▼                                                     │
│   ┌──────────────┐                                             │
│   │ Master       │                                             │
│   │ (Consumer)   │ ◀── 从 etcd Watch 变化并应用到内存           │
│   └──────────────┘                                             │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 实现细节

```cpp
// ============================================================
// 客户端直接写 etcd
// ============================================================

class EtcdDirectClient {
public:
    ErrorCode Put(const std::string& key, const ObjectMetadata& metadata) {
        std::string etcd_key = "/mooncake/metadata/" + key;
        std::string etcd_value = Serialize(metadata);
        
        // 使用 etcd 事务保证原子性
        auto txn = etcd_client_.CreateTransaction();
        
        // 条件：key 不存在或版本匹配
        txn.If(etcd::Compare::Version(etcd_key) == expected_version_);
        
        // 操作：写入
        txn.Then(etcd::Put(etcd_key, etcd_value));
        
        auto result = txn.Commit();
        
        if (result.succeeded) {
            return ErrorCode::OK;
        } else {
            return ErrorCode::ETCD_WRITE_FAILED;
        }
    }
};

// ============================================================
// Master 从 etcd Watch 并应用
// ============================================================

class MasterService {
public:
    void StartEtcdWatcher() {
        // Watch etcd 前缀
        etcd_watcher_.Watch("/mooncake/metadata/", [this](const WatchEvent& event) {
            HandleEtcdChange(event);
        });
    }
    
private:
    void HandleEtcdChange(const WatchEvent& event) {
        switch (event.type) {
            case WatchEvent::PUT:
                // 从 etcd 读取并应用到内存
                auto metadata = Deserialize<ObjectMetadata>(event.value);
                metadata_store_.Put(event.key, metadata);
                break;
                
            case WatchEvent::DELETE:
                metadata_store_.Delete(event.key);
                break;
        }
    }
};
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| ✅ 零数据丢失（etcd 保证） | ❌ etcd 性能瓶颈（不适合高频写入） |
| ✅ 强一致性 | ❌ 写入延迟增加（+5-20ms） |
| ✅ 无需额外组件 | ❌ etcd 容量限制 |
| ✅ 天然高可用 | ❌ Master 需要 Watch 机制 |
| ✅ 可回溯历史 | ❌ 客户端需要 etcd 访问权限 |

### 适用场景

- 写入频率较低（<1K QPS）
- 已有 etcd 基础设施
- 对一致性要求极高

---

## 方案 3：共享存储（NFS/分布式文件系统）

### 核心思想

> **所有 Master 节点共享同一个持久化存储（NFS/GlusterFS/CephFS），写入直接落盘。**

### 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                    共享存储架构                                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌──────────────┐      ┌──────────────┐      ┌──────────────┐ │
│   │ Master Node1 │      │ Master Node2 │      │ Master Node3 │ │
│   │              │      │              │      │              │ │
│   │ ┌──────────┐ │      │ ┌──────────┐ │      │ ┌──────────┐ │ │
│   │ │Metadata  │ │      │ │Metadata  │ │      │ │Metadata  │ │ │
│   │ │  Store   │ │      │ │  Store   │ │      │ │  Store   │ │ │
│   │ └────┬─────┘ │      │ └────┬─────┘ │      │ └────┬─────┘ │ │
│   └──────┼───────┘      └──────┼───────┘      └──────┼───────┘ │
│          │                     │                     │         │
│          └─────────────────────┼─────────────────────┘         │
│                                │                               │
│                                ▼                               │
│                    ┌──────────────────────┐                    │
│                    │   共享存储            │                    │
│                    │  (NFS/GlusterFS/     │                    │
│                    │   CephFS)            │                    │
│                    │                      │                    │
│                    │  /mooncake/metadata/ │                    │
│                    │    ├─ object1.json   │                    │
│                    │    ├─ object2.json   │                    │
│                    │    └─ ...            │                    │
│                    └──────────────────────┘                    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 实现细节

```cpp
class SharedStorageMetadataStore {
public:
    ErrorCode Put(const std::string& key, const ObjectMetadata& metadata) {
        std::string file_path = GetFilePath(key);
        
        // 1. 写入临时文件
        std::string temp_path = file_path + ".tmp";
        {
            std::ofstream ofs(temp_path);
            ofs << Serialize(metadata);
            ofs.flush();
            fsync(ofs);  // 强制刷盘
        }
        
        // 2. 原子重命名（保证一致性）
        std::filesystem::rename(temp_path, file_path);
        
        // 3. 同步到共享存储
        SyncToSharedStorage(file_path);
        
        return ErrorCode::OK;
    }
    
    Result<ObjectMetadata> Get(const std::string& key) {
        std::string file_path = GetFilePath(key);
        
        // 从共享存储读取
        std::ifstream ifs(file_path);
        if (!ifs) {
            return ErrorCode::NOT_FOUND;
        }
        
        std::string content((std::istreambuf_iterator<char>(ifs)),
                          std::istreambuf_iterator<char>());
        
        return Deserialize<ObjectMetadata>(content);
    }
    
private:
    std::string GetFilePath(const std::string& key) {
        return shared_storage_path_ + "/" + Hash(key) + ".json";
    }
    
    std::string shared_storage_path_ = "/mnt/shared/mooncake/metadata";
};
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| ✅ 数据持久化到共享存储 | ❌ 共享存储是单点故障 |
| ✅ 实现简单 | ❌ 文件系统锁竞争 |
| ✅ 无需额外组件 | ❌ 性能较差（网络文件系统） |
| ✅ 可读性强（JSON文件） | ❌ 扩展性差 |
| | ❌ 需要共享存储基础设施 |

### 适用场景

- 已有共享存储基础设施
- 写入频率极低
- 对性能要求不高

---

## 方案 4：多主架构（Multi-Master）

### 核心思想

> **所有 Master 节点同时接受写入，通过冲突解决机制保证一致性。**

### 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                    多主架构                                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌──────────────┐      ┌──────────────┐      ┌──────────────┐ │
│   │ Master Node1 │      │ Master Node2 │      │ Master Node3 │ │
│   │  (Active)    │      │  (Active)    │      │  (Active)    │ │
│   │              │      │              │      │              │ │
│   │ ┌──────────┐ │      │ ┌──────────┐ │      │ ┌──────────┐ │ │
│   │ │Metadata  │ │      │ │Metadata  │ │      │ │Metadata  │ │ │
│   │ │  Store   │ │      │ │  Store   │ │      │ │  Store   │ │ │
│   │ └────┬─────┘ │      │ └────┬─────┘ │      │ └────┬─────┘ │ │
│   └──────┼───────┘      └──────┼───────┘      └──────┼───────┘ │
│          │                     │                     │         │
│          └─────────────────────┼─────────────────────┘         │
│                                │                               │
│                                ▼                               │
│                    ┌──────────────────────┐                    │
│                    │   冲突解决层          │                    │
│                    │  (Vector Clock/      │                    │
│                    │   Last-Write-Wins)   │                    │
│                    └──────────────────────┘                    │
│                                                                 │
│   客户端可以写入任意 Master，无窗口期问题                        │
└─────────────────────────────────────────────────────────────────┘
```

### 实现细节

```cpp
class MultiMasterService {
public:
    ErrorCode Put(const std::string& key, const ObjectMetadata& metadata) {
        // 1. 添加向量时钟
        VectorClock clock = GetCurrentClock();
        clock.Increment(node_id_);
        
        metadata.vector_clock = clock;
        metadata.timestamp = Now();
        metadata.node_id = node_id_;
        
        // 2. 写入本地
        metadata_store_.Put(key, metadata);
        
        // 3. 异步复制到其他节点
        ReplicateAsync(key, metadata);
        
        return ErrorCode::OK;
    }
    
    Result<ObjectMetadata> Get(const std::string& key) {
        // 1. 从本地读取
        auto local = metadata_store_.Get(key);
        
        // 2. 从其他节点读取（可选，用于一致性检查）
        auto remotes = FetchFromPeers(key);
        
        // 3. 冲突解决
        return ResolveConflict(local, remotes);
    }
    
private:
    ObjectMetadata ResolveConflict(
        const ObjectMetadata& local,
        const std::vector<ObjectMetadata>& remotes) {
        
        // 策略1: Last-Write-Wins（时间戳）
        // 策略2: Vector Clock 比较
        // 策略3: 应用层语义解决
        
        ObjectMetadata latest = local;
        for (const auto& remote : remotes) {
            if (remote.timestamp > latest.timestamp) {
                latest = remote;
            }
        }
        
        return latest;
    }
};
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| ✅ 无窗口期问题 | ❌ 冲突解决复杂 |
| ✅ 高可用（任意节点可用） | ❌ 最终一致性（可能读到旧数据） |
| ✅ 负载均衡 | ❌ 需要向量时钟等机制 |
| ✅ 写入延迟低 | ❌ 数据可能不一致 |

### 适用场景

- 可接受最终一致性
- 写入冲突较少
- 需要高可用和高性能

---

## 方案 5：基于 Raft 的强一致性写入

### 核心思想

> **Master Service 本身基于 Raft 实现，写入需要 Quorum 确认。**

### 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                    Raft 强一致性架构                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌──────────────┐      ┌──────────────┐      ┌──────────────┐ │
│   │ Master Node1 │      │ Master Node2 │      │ Master Node3 │ │
│   │  (Leader)    │      │ (Follower)   │      │ (Follower)   │ │
│   │              │      │              │      │              │ │
│   │ ┌──────────┐ │      │ ┌──────────┐ │      │ ┌──────────┐ │ │
│   │ │  Raft    │ │      │ │  Raft    │ │      │ │  Raft    │ │ │
│   │ │  State   │ │      │ │  State   │ │      │ │  State   │ │ │
│   │ │ Machine  │ │      │ │ Machine  │ │      │ │ Machine  │ │ │
│   │ └────┬─────┘ │      │ └────┬─────┘ │      │ └────┬─────┘ │ │
│   │      │       │      │      │       │      │      │       │ │
│   │      │ 日志复制 (Quorum)                                          │
│   │      └───────┼──────┴───────┼───────┘       │ │
│   │              │              │              │ │
│   │              ▼              ▼              ▼ │
│   │         ┌──────────────────────────────┐     │
│   │         │      Metadata Store          │     │
│   │         └──────────────────────────────┘     │
│   └──────────────┘                                │
│                                                                 │
│   写入流程：                                                     │
│   Client ──▶ Leader ──▶ 复制到 Follower ──▶ Quorum 确认 ──▶ 返回 │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| ✅ 强一致性，零数据丢失 | ❌ 写入延迟增加（+2-10ms） |
| ✅ 无窗口期问题 | ❌ 实现复杂度高 |
| ✅ 自主选举，无外部依赖 | ❌ 吞吐量受限 |
| ✅ 已提交数据绝不丢失 | ❌ 需要 Raft 库或自研 |

### 适用场景

- 对一致性要求极高
- 可接受延迟增加
- 有 Raft 实现经验

---

## 方案 6：事件溯源（Event Sourcing）

### 核心思想

> **所有操作作为不可变事件持久化，Master 通过重放事件重建状态。**

### 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                    事件溯源架构                                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌──────────────┐                                             │
│   │   Client     │                                             │
│   │              │                                             │
│   │  Put(key, val)│                                             │
│   └──────┬───────┘                                             │
│          │                                                     │
│          ▼                                                     │
│   ┌──────────────┐                                             │
│   │ Master       │                                             │
│   │              │                                             │
│   │ 1. 生成事件   │                                             │
│   │    ObjectCreated(key, val)                                 │
│   │              │                                             │
│   │ 2. 持久化事件  │                                             │
│   │    └─▶ Event Store (WAL/DB)                                │
│   │              │                                             │
│   │ 3. 应用事件   │                                             │
│   │    └─▶ Metadata Store (内存状态)                            │
│   └──────────────┘                                             │
│                                                                 │
│   故障恢复：                                                     │
│   Master ──▶ 从 Event Store 重放所有事件 ──▶ 重建状态             │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 实现细节

```cpp
// 事件定义
struct Event {
    uint64_t seq;
    uint64_t timestamp;
    std::string type;  // "ObjectCreated", "ObjectUpdated", "ObjectDeleted"
    std::string key;
    std::string payload;  // JSON
};

class EventSourcedMasterService {
public:
    ErrorCode Put(const std::string& key, const ObjectMetadata& metadata) {
        // 1. 生成事件
        Event event{
            .seq = next_event_seq_++,
            .timestamp = Now(),
            .type = "ObjectCreated",
            .key = key,
            .payload = Serialize(metadata)
        };
        
        // 2. 持久化事件（先写后应用）
        event_store_.Append(event);
        
        // 3. 应用到内存状态
        ApplyEvent(event);
        
        return ErrorCode::OK;
    }
    
    void Recover() {
        // 从事件存储重放所有事件
        auto events = event_store_.ReadAll();
        for (const auto& event : events) {
            ApplyEvent(event);
        }
    }
    
private:
    void ApplyEvent(const Event& event) {
        if (event.type == "ObjectCreated" || event.type == "ObjectUpdated") {
            auto metadata = Deserialize<ObjectMetadata>(event.payload);
            metadata_store_.Put(event.key, metadata);
        } else if (event.type == "ObjectDeleted") {
            metadata_store_.Delete(event.key);
        }
    }
    
    EventStore event_store_;
    MetadataStore metadata_store_;
};
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| ✅ 完整历史记录 | ❌ 存储空间大 |
| ✅ 可回溯任意时间点 | ❌ 恢复时间长（需重放） |
| ✅ 审计友好 | ❌ 实现复杂 |
| ✅ 天然支持 CQRS | ❌ 需要快照机制优化 |

### 适用场景

- 需要完整审计日志
- 需要时间旅行查询
- 可接受恢复时间

---

## 方案对比矩阵

| 方案 | 数据丢失 | 窗口期 | 实现复杂度 | 性能影响 | 外部依赖 | 推荐度 |
|------|---------|--------|-----------|---------|---------|--------|
| **客户端缓冲重试** | 可能丢（进程重启） | 客户端重试延迟 | 低 | 无 | 无 | ⭐⭐⭐ |
| **直接写 etcd** | 零丢失 | 0 | 中 | 延迟+5-20ms | etcd | ⭐⭐⭐ |
| **共享存储** | 零丢失 | 0 | 低 | 延迟+10-50ms | 共享存储 | ⭐⭐ |
| **多主架构** | 可能不一致 | 0 | 高 | 无影响 | 无 | ⭐⭐ |
| **Raft 强一致** | 零丢失 | 0 | 高 | 延迟+2-10ms | Raft库 | ⭐⭐⭐⭐ |
| **事件溯源** | 零丢失 | 0 | 高 | 无影响 | 事件存储 | ⭐⭐⭐ |
| **乐观接管** | 极少丢 | ~100ms | 中高 | 无影响 | etcd | ⭐⭐⭐⭐⭐ |
| **消息队列** | 零丢失 | 0（写入） | 中 | 延迟+2-10ms | 队列 | ⭐⭐⭐⭐⭐ |

---

## 推荐方案组合

### 方案 A：轻量级组合（推荐）

```
乐观接管 + 客户端重试
────────────────────
- 服务端：乐观接管（窗口期 ~100ms）
- 客户端：自动重试（兜底保障）
- 优点：实现简单，性能好
- 缺点：极端情况可能丢少量数据
```

### 方案 B：强一致性组合

```
Raft + 事件溯源
───────────────
- 写入：Raft 强一致
- 存储：事件溯源
- 优点：零丢失，强一致
- 缺点：延迟增加，实现复杂
```

### 方案 C：混合方案（最优）

```
消息队列 + 乐观接管 + 客户端幂等
───────────────────────────────
- 写入：消息队列（零丢失）
- 读取：乐观接管（低延迟）
- 客户端：幂等重试（兜底）
- 优点：零丢失 + 低延迟
- 缺点：需要消息队列基础设施
```

---

## 总结

### 一句话总结各方案

| 方案 | 一句话 |
|------|--------|
| **客户端缓冲重试** | 客户端自己兜底，简单但不可靠 |
| **直接写 etcd** | 绕过 Master，etcd 保证不丢，但性能差 |
| **共享存储** | 所有节点共享文件系统，简单但性能差 |
| **多主架构** | 所有节点都接受写入，高可用但可能冲突 |
| **Raft 强一致** | 写入需多数派确认，强一致但延迟高 |
| **事件溯源** | 所有操作存事件，可回溯但恢复慢 |
| **乐观接管** | 检测到故障立即接管，快速但需回滚机制 |
| **消息队列** | 写入先到队列，零丢失但需额外组件 |

### 最终建议

> **对于 Mooncake 场景，推荐「消息队列（EDQ/Kafka）+ 乐观接管」混合方案**：
> - 写入路径：消息队列保证零丢失
> - 读取路径：乐观接管保证低延迟
> - 综合效果：近乎完美的故障切换体验







