# RFC：嵌入式分布式队列（EDQ）设计

## 1. 概述

### 1.1 背景

在 Master Service 主备切换期间（3-5秒窗口期），存在数据丢失风险。为了实现零数据丢失，需要一个持久化的消息队列来缓冲客户端请求。

### 1.2 设计目标

- **零数据丢失**：窗口期内的写入请求不丢失
- **轻量级**：不引入 Kafka 等重型外部依赖
- **嵌入式**：与 Master Service 集成，无需独立部署
- **高可用**：队列本身具备容错能力
- **低延迟**：最小化对正常写入路径的影响

### 1.3 核心思想

```
┌─────────────────────────────────────────────────────────────────┐
│                    嵌入式分布式队列（EDQ）                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   传统架构：                                                     │
│   Client ──▶ Master ──▶ 内存状态                               │
│              (单点)     (易丢失)                                │
│                                                                 │
│   EDQ 架构：                                                     │
│   Client ──▶ EDQ Cluster ──▶ Master Consumer                   │
│              (多副本持久化)   (消费应用)                         │
│                                                                 │
│   关键：写入先到达 EDQ 并持久化，再被 Master 消费处理            │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. 架构设计

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                         EDQ 集群架构                                 │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   ┌─────────┐     ┌─────────────────────────────────────────────┐   │
│   │ Client  │────▶│              EDQ Cluster                    │   │
│   └─────────┘     │  ┌─────────┐  ┌─────────┐  ┌─────────┐     │   │
│                   │  │ Node 1  │  │ Node 2  │  │ Node 3  │     │   │
│                   │  │ (Leader)│  │(Follower)│ │(Follower)│    │   │
│                   │  │┌───────┐│  │┌───────┐│  │┌───────┐│     │   │
│                   │  ││ WAL   ││  ││ WAL   ││  ││ WAL   ││     │   │
│                   │  ││(持久化)││ ││(副本) ││  ││(副本) ││     │   │
│                   │  │└───────┘│  │└───────┘│  │└───────┘│     │   │
│                   │  └────┬────┘  └─────────┘  └─────────┘     │   │
│                   └───────┼─────────────────────────────────────┘   │
│                           │                                         │
│                           ▼                                         │
│                   ┌───────────────┐                                 │
│                   │ Master Service│                                 │
│                   │  (Consumer)   │                                 │
│                   │ ┌───────────┐ │                                 │
│                   │ │ Metadata  │ │                                 │
│                   │ │  Store    │ │                                 │
│                   │ └───────────┘ │                                 │
│                   └───────────────┘                                 │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2 部署模式

```
┌─────────────────────────────────────────────────────────────────────┐
│                       部署拓扑选项                                   │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  模式A：独立 EDQ 集群（推荐大规模部署）                              │
│  ────────────────────────────────────                               │
│                                                                     │
│  ┌─────────┐    ┌─────────────────────┐    ┌─────────────────────┐  │
│  │ Client  │───▶│    EDQ Cluster      │───▶│   Master Cluster    │  │
│  │         │    │  (3节点独立部署)     │    │  (3节点独立部署)     │  │
│  └─────────┘    └─────────────────────┘    └─────────────────────┘  │
│                                                                     │
│                                                                     │
│  模式B：嵌入式 EDQ（推荐中小规模）                                   │
│  ─────────────────────────────────                                  │
│                                                                     │
│  ┌─────────┐    ┌──────────────────────────────────────────────┐    │
│  │ Client  │───▶│              Master Node                      │    │
│  │         │    │  ┌────────────┐    ┌──────────────────────┐  │    │
│  │         │    │  │ EDQ Module │───▶│   Master Service     │  │    │
│  │         │    │  │ (嵌入式)   │    │   (同进程消费)        │  │    │
│  │         │    │  └────────────┘    └──────────────────────┘  │    │
│  └─────────┘    └──────────────────────────────────────────────┘    │
│                                     × 3 节点                         │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 3. 核心组件设计

### 3.1 WAL（Write-Ahead Log）

```cpp
// ============================================================
// WAL 文件格式
// ============================================================

/*
WAL 文件结构：
┌─────────────────────────────────────────────────────────┐
│ Header (32 bytes)                                        │
├─────────────────────────────────────────────────────────┤
│ magic: 4 bytes    "EDQW"                                │
│ version: 4 bytes  1                                      │
│ node_id: 8 bytes  节点标识                               │
│ created: 8 bytes  创建时间戳                             │
│ reserved: 8 bytes                                        │
├─────────────────────────────────────────────────────────┤
│ Entry 1                                                  │
├─────────────────────────────────────────────────────────┤
│ Entry 2                                                  │
├─────────────────────────────────────────────────────────┤
│ ...                                                      │
└─────────────────────────────────────────────────────────┘

Entry 结构：
┌─────────────────────────────────────────────────────────┐
│ length: 4 bytes       数据长度                           │
│ seq: 8 bytes          全局序列号                         │
│ term: 8 bytes         任期（用于 Leader 变更）           │
│ timestamp: 8 bytes    写入时间                           │
│ type: 1 byte          消息类型                           │
│ flags: 1 byte         标志位                             │
│ reserved: 2 bytes                                        │
│ key_len: 4 bytes      Key 长度                           │
│ key: variable         Key 内容                           │
│ value: variable       Value 内容                         │
│ crc32: 4 bytes        校验和                             │
└─────────────────────────────────────────────────────────┘
*/

struct WALEntry {
    uint64_t seq;           // 全局唯一递增序列号
    uint64_t term;          // Leader 任期
    uint64_t timestamp;     // 毫秒时间戳
    uint8_t type;           // WRITE, DELETE, CHECKPOINT, etc.
    uint8_t flags;          // COMMITTED, REPLICATED, etc.
    std::string key;
    std::string value;
    uint32_t crc32;
    
    static constexpr uint8_t TYPE_WRITE = 1;
    static constexpr uint8_t TYPE_DELETE = 2;
    static constexpr uint8_t TYPE_CHECKPOINT = 3;
    
    static constexpr uint8_t FLAG_REPLICATED = 0x01;  // 已复制到多数派
    static constexpr uint8_t FLAG_COMMITTED = 0x02;   // 已提交
    static constexpr uint8_t FLAG_CONSUMED = 0x04;    // 已被消费
};

// ============================================================
// WAL 管理器
// ============================================================

class WALManager {
public:
    explicit WALManager(const std::string& wal_dir, const WALConfig& config);
    
    // 追加写入（返回序列号）
    Result<uint64_t> Append(const WALEntry& entry);
    
    // 批量追加（原子性）
    Result<uint64_t> AppendBatch(const std::vector<WALEntry>& entries);
    
    // 读取指定范围的 Entry
    Result<std::vector<WALEntry>> Read(uint64_t start_seq, uint64_t end_seq);
    
    // 读取从某个位置开始的所有 Entry
    Result<std::vector<WALEntry>> ReadFrom(uint64_t start_seq);
    
    // 标记为已提交
    void MarkCommitted(uint64_t seq);
    
    // 获取最后提交的序列号
    uint64_t GetLastCommittedSeq() const;
    
    // 截断已消费的日志（日志压缩）
    void Truncate(uint64_t before_seq);
    
    // 同步到磁盘
    void Sync();
    
private:
    std::string wal_dir_;
    WALConfig config_;
    std::atomic<uint64_t> next_seq_{1};
    std::atomic<uint64_t> committed_seq_{0};
    
    // 使用 mmap 提高性能
    MappedFile current_file_;
    std::mutex write_mutex_;
};

struct WALConfig {
    size_t max_file_size = 64 * 1024 * 1024;  // 64MB per file
    size_t sync_interval_ms = 10;              // 10ms fsync
    bool sync_on_append = false;               // 是否每次写入都 sync
    size_t max_entries_in_memory = 10000;      // 内存缓存条目数
};
```

### 3.2 复制协议

```cpp
// ============================================================
// 简化的复制协议（类似 Raft 日志复制，但更轻量）
// ============================================================

/*
复制流程：
┌─────────────────────────────────────────────────────────────────┐
│                                                                 │
│   1. Client 写入 Leader                                         │
│      Client ──▶ Leader.Append(entry)                           │
│                                                                 │
│   2. Leader 写入本地 WAL                                        │
│      Leader: wal_.Append(entry)  // seq = 100                  │
│                                                                 │
│   3. Leader 并行复制到 Followers                                │
│      Leader ──▶ Follower1.Replicate(entry)                     │
│      Leader ──▶ Follower2.Replicate(entry)                     │
│                                                                 │
│   4. 等待多数派确认（Quorum = 2/3）                              │
│      Leader: wait_for_quorum(seq=100)                          │
│                                                                 │
│   5. 标记为已提交，返回客户端                                    │
│      Leader: MarkCommitted(100)                                │
│      Leader ──▶ Client: OK                                     │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
*/

// 复制请求
struct ReplicateRequest {
    uint64_t term;                    // Leader 任期
    uint64_t leader_committed_seq;    // Leader 已提交的序列号
    std::vector<WALEntry> entries;    // 待复制的条目
};

struct ReplicateResponse {
    bool success;
    uint64_t last_seq;       // Follower 最后的序列号
    uint64_t term;           // Follower 的任期
};

// ============================================================
// EDQ 节点
// ============================================================

class EDQNode {
public:
    EDQNode(const EDQConfig& config);
    
    // ================== 写入接口 ==================
    
    // 追加消息（仅 Leader 可调用）
    Result<uint64_t> Append(const std::string& key, const std::string& value) {
        if (state_ != NodeState::LEADER) {
            return Error("Not leader, redirect to: " + leader_addr_);
        }
        
        // 1. 写入本地 WAL
        WALEntry entry{
            .seq = next_seq_++,
            .term = current_term_,
            .timestamp = Now(),
            .type = WALEntry::TYPE_WRITE,
            .key = key,
            .value = value
        };
        entry.crc32 = ComputeCRC32(entry);
        
        wal_.Append(entry);
        
        // 2. 复制到 Followers
        auto replicated = ReplicateToFollowers(entry);
        
        // 3. 等待 Quorum 确认
        if (replicated >= quorum_) {
            wal_.MarkCommitted(entry.seq);
            NotifyConsumers(entry.seq);
            return entry.seq;
        } else {
            return Error("Replication failed");
        }
    }
    
    // ================== 复制接口（Follower 端）==================
    
    ReplicateResponse Replicate(const ReplicateRequest& req) {
        std::lock_guard lock(mutex_);
        
        // 任期检查
        if (req.term < current_term_) {
            return {false, wal_.GetLastSeq(), current_term_};
        }
        
        // 更新任期
        if (req.term > current_term_) {
            current_term_ = req.term;
            state_ = NodeState::FOLLOWER;
        }
        
        // 追加日志
        for (const auto& entry : req.entries) {
            wal_.Append(entry);
        }
        
        // 更新提交位置
        if (req.leader_committed_seq > committed_seq_) {
            committed_seq_ = req.leader_committed_seq;
            wal_.MarkCommitted(committed_seq_);
        }
        
        return {true, wal_.GetLastSeq(), current_term_};
    }
    
    // ================== 消费接口 ==================
    
    // 拉取已提交的消息
    std::vector<WALEntry> Poll(uint64_t from_seq, size_t max_count) {
        return wal_.Read(from_seq, from_seq + max_count);
    }
    
    // 确认消费位置
    void Commit(const std::string& consumer_id, uint64_t seq) {
        consumer_offsets_[consumer_id] = seq;
        PersistConsumerOffset(consumer_id, seq);
    }
    
private:
    NodeState state_ = NodeState::FOLLOWER;
    uint64_t current_term_ = 0;
    uint64_t next_seq_ = 1;
    uint64_t committed_seq_ = 0;
    
    WALManager wal_;
    std::vector<std::string> peer_addrs_;
    std::string leader_addr_;
    
    std::unordered_map<std::string, uint64_t> consumer_offsets_;
    
    size_t quorum_;  // 多数派数量
    
    int ReplicateToFollowers(const WALEntry& entry);
};
```

### 3.3 Leader 选举

```cpp
// ============================================================
// 基于 etcd 的 Leader 选举（复用已有基础设施）
// ============================================================

class EDQLeaderElection {
public:
    EDQLeaderElection(EtcdClient& etcd, const std::string& node_id)
        : etcd_(etcd), node_id_(node_id) {}
    
    // 尝试成为 Leader
    bool TryBecomeLeader() {
        // 使用 etcd 事务实现原子选举
        auto txn = etcd_.CreateTransaction();
        
        // 条件：Leader Key 不存在
        txn.If(etcd::Compare::Version(LEADER_KEY) == 0);
        
        // 操作：创建 Leader Key，绑定 Lease
        auto lease_id = etcd_.GrantLease(LEASE_TTL);
        txn.Then(etcd::Put(LEADER_KEY, node_id_, lease_id));
        
        auto result = txn.Commit();
        
        if (result.succeeded) {
            current_lease_ = lease_id;
            StartLeaseKeepAlive();
            return true;
        }
        
        return false;
    }
    
    // 持续保持 Leader 状态
    void KeepLeader() {
        while (running_) {
            if (!etcd_.KeepAlive(current_lease_)) {
                // Lease 续约失败，失去 Leader 身份
                OnLoseLeadership();
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    // 获取当前 Leader
    std::string GetCurrentLeader() {
        auto result = etcd_.Get(LEADER_KEY);
        if (result.ok()) {
            return result.value();
        }
        return "";
    }
    
private:
    static constexpr const char* LEADER_KEY = "/mooncake/edq/leader";
    static constexpr int LEASE_TTL = 5;  // 5秒
    
    EtcdClient& etcd_;
    std::string node_id_;
    int64_t current_lease_ = 0;
};
```

### 3.4 消费者接口

```cpp
// ============================================================
// Master Service 作为 EDQ 消费者
// ============================================================

class EDQConsumer {
public:
    EDQConsumer(EDQClient& client, const std::string& consumer_id)
        : client_(client), consumer_id_(consumer_id) {}
    
    void Start() {
        // 恢复上次的消费位置
        last_consumed_seq_ = LoadLastConsumedSeq();
        
        while (running_) {
            // 拉取新消息
            auto entries = client_.Poll(last_consumed_seq_ + 1, BATCH_SIZE);
            
            if (entries.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            
            // 处理消息
            for (const auto& entry : entries) {
                ProcessEntry(entry);
                last_consumed_seq_ = entry.seq;
            }
            
            // 提交消费位置
            client_.Commit(consumer_id_, last_consumed_seq_);
        }
    }
    
private:
    void ProcessEntry(const WALEntry& entry) {
        switch (entry.type) {
            case WALEntry::TYPE_WRITE:
                // 应用到 Metadata Store
                metadata_store_.Put(entry.key, 
                    Deserialize<ObjectMetadata>(entry.value));
                break;
                
            case WALEntry::TYPE_DELETE:
                metadata_store_.Delete(entry.key);
                break;
                
            default:
                break;
        }
    }
    
    uint64_t LoadLastConsumedSeq() {
        // 从持久化存储加载
        return client_.GetConsumerOffset(consumer_id_);
    }
    
    static constexpr size_t BATCH_SIZE = 100;
    
    EDQClient& client_;
    std::string consumer_id_;
    uint64_t last_consumed_seq_ = 0;
    MetadataStore& metadata_store_;
};
```

---

## 4. 故障处理

### 4.1 主备切换流程

```
┌─────────────────────────────────────────────────────────────────┐
│                    EDQ 保障的故障切换                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│ 正常运行：                                                       │
│ ─────────                                                       │
│ T0: Client ──▶ EDQ Leader ──▶ WAL(seq=100) ──▶ 复制到 Follower │
│ T1: Master Primary 消费 seq=100，应用到 Metadata                │
│ T2: Master Primary 提交 offset=100                              │
│                                                                 │
│ 故障发生：                                                       │
│ ─────────                                                       │
│ T3: Client ──▶ EDQ Leader ──▶ WAL(seq=101,102,103)             │
│ T4: Master Primary 宕机（消费到 seq=100）                        │
│     │                                                           │
│     │   消息 101-103 安全存储在 EDQ 的 WAL 中                    │
│     │                                                           │
│ T5: Master Standby 检测到 Primary 故障                          │
│ T6: Master Standby 升主                                         │
│ T7: 新 Master 从 offset=100 继续消费 EDQ                        │
│ T8: 处理消息 101, 102, 103                                      │
│                                                                 │
│ 结果：零数据丢失 ✓                                               │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 4.2 EDQ 自身故障处理

```
┌─────────────────────────────────────────────────────────────────┐
│                    EDQ 节点故障处理                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│ 场景1：Follower 宕机                                             │
│ ─────────────────────                                           │
│ - 不影响写入（只要 Quorum 满足）                                 │
│ - Follower 恢复后从 Leader 同步                                  │
│                                                                 │
│ 场景2：Leader 宕机                                               │
│ ─────────────────                                               │
│ - etcd 检测到 Lease 过期                                        │
│ - 其他节点竞选新 Leader                                          │
│ - 新 Leader 拥有所有已复制的日志                                 │
│ - 继续接受写入                                                  │
│                                                                 │
│ 场景3：网络分区                                                  │
│ ─────────────────                                               │
│ - 少数派无法选举成功                                             │
│ - 多数派继续服务                                                 │
│ - 分区恢复后自动同步                                             │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 5. 性能优化

### 5.1 批量写入

```cpp
class BatchWriter {
public:
    // 收集请求，批量提交
    Future<uint64_t> Append(const std::string& key, const std::string& value) {
        auto promise = std::make_shared<Promise<uint64_t>>();
        
        {
            std::lock_guard lock(mutex_);
            pending_entries_.push_back({key, value, promise});
        }
        
        // 触发批量提交
        MaybeFlush();
        
        return promise->get_future();
    }
    
private:
    void MaybeFlush() {
        if (pending_entries_.size() >= BATCH_SIZE ||
            TimeSinceLastFlush() >= BATCH_INTERVAL) {
            Flush();
        }
    }
    
    void Flush() {
        std::vector<PendingEntry> entries;
        {
            std::lock_guard lock(mutex_);
            std::swap(entries, pending_entries_);
        }
        
        if (entries.empty()) return;
        
        // 批量写入 WAL
        std::vector<WALEntry> wal_entries;
        for (const auto& e : entries) {
            wal_entries.push_back(CreateWALEntry(e.key, e.value));
        }
        
        auto result = wal_.AppendBatch(wal_entries);
        
        // 批量复制
        ReplicateToFollowers(wal_entries);
        
        // 通知所有等待者
        for (size_t i = 0; i < entries.size(); ++i) {
            entries[i].promise->set_value(wal_entries[i].seq);
        }
    }
    
    static constexpr size_t BATCH_SIZE = 100;
    static constexpr auto BATCH_INTERVAL = std::chrono::milliseconds(1);
};
```

### 5.2 异步复制

```cpp
class AsyncReplicator {
public:
    void ReplicateAsync(const std::vector<WALEntry>& entries) {
        std::vector<std::future<ReplicateResponse>> futures;
        
        // 并行发送到所有 Follower
        for (const auto& peer : peers_) {
            futures.push_back(std::async(std::launch::async, [&]() {
                return peer.Replicate(entries);
            }));
        }
        
        // 等待 Quorum 确认
        size_t success_count = 1;  // 自己算一票
        for (auto& f : futures) {
            auto result = f.wait_for(REPLICATE_TIMEOUT);
            if (result == std::future_status::ready && f.get().success) {
                success_count++;
                if (success_count >= quorum_) {
                    break;  // 已达到 Quorum，可以提前返回
                }
            }
        }
    }
    
private:
    static constexpr auto REPLICATE_TIMEOUT = std::chrono::milliseconds(100);
};
```

### 5.3 日志压缩

```cpp
class LogCompactor {
public:
    void CompactIfNeeded() {
        // 找到所有消费者的最小 offset
        uint64_t min_consumed = GetMinConsumedOffset();
        
        // 保留一定的安全边界
        uint64_t safe_to_truncate = min_consumed - SAFETY_MARGIN;
        
        if (safe_to_truncate > last_truncated_) {
            wal_.Truncate(safe_to_truncate);
            last_truncated_ = safe_to_truncate;
        }
    }
    
private:
    static constexpr uint64_t SAFETY_MARGIN = 1000;  // 保留最近 1000 条
};
```

---

## 6. 接口设计

### 6.1 客户端 API

```cpp
class EDQClient {
public:
    // 创建客户端
    static std::unique_ptr<EDQClient> Create(const std::vector<std::string>& endpoints);
    
    // ================== 生产者接口 ==================
    
    // 同步写入（等待 Quorum 确认）
    Result<uint64_t> Produce(const std::string& key, const std::string& value);
    
    // 异步写入
    Future<uint64_t> ProduceAsync(const std::string& key, const std::string& value);
    
    // 批量写入
    Result<std::vector<uint64_t>> ProduceBatch(
        const std::vector<std::pair<std::string, std::string>>& entries);
    
    // ================== 消费者接口 ==================
    
    // 拉取消息
    std::vector<Message> Poll(size_t max_count, std::chrono::milliseconds timeout);
    
    // 提交消费位置
    void Commit();
    void Commit(uint64_t offset);
    
    // 设置消费位置
    void Seek(uint64_t offset);
    void SeekToBeginning();
    void SeekToEnd();
    
    // ================== 管理接口 ==================
    
    // 获取集群状态
    ClusterStatus GetClusterStatus();
    
    // 获取 Leader 地址
    std::string GetLeaderAddr();
};

struct Message {
    uint64_t seq;
    std::string key;
    std::string value;
    uint64_t timestamp;
};
```

### 6.2 与 Master Service 集成

```cpp
// ============================================================
// 集成示例
// ============================================================

class MasterService {
public:
    void Initialize() {
        // 初始化 EDQ 客户端
        edq_client_ = EDQClient::Create(config_.edq_endpoints);
        
        // 启动消费者线程
        consumer_thread_ = std::thread([this]() {
            ConsumeLoop();
        });
    }
    
    // 写入操作通过 EDQ
    ErrorCode Put(const std::string& key, const ObjectMetadata& metadata) {
        auto result = edq_client_->Produce(key, Serialize(metadata));
        if (!result.ok()) {
            return ErrorCode::QUEUE_WRITE_FAILED;
        }
        return ErrorCode::OK;
    }
    
    // 读取操作直接访问本地状态
    Result<ObjectMetadata> Get(const std::string& key) {
        return metadata_store_.Get(key);
    }
    
private:
    void ConsumeLoop() {
        while (running_) {
            auto messages = edq_client_->Poll(100, std::chrono::milliseconds(100));
            
            for (const auto& msg : messages) {
                ApplyToMetadataStore(msg);
            }
            
            edq_client_->Commit();
        }
    }
    
    void ApplyToMetadataStore(const Message& msg) {
        auto metadata = Deserialize<ObjectMetadata>(msg.value);
        metadata_store_.Put(msg.key, metadata);
    }
    
    std::unique_ptr<EDQClient> edq_client_;
    MetadataStore metadata_store_;
    std::thread consumer_thread_;
};
```

---

## 7. 与 Kafka 方案对比

| 维度 | EDQ（自研） | Kafka |
|------|------------|-------|
| **部署复杂度** | 低（嵌入式可选） | 高（独立集群） |
| **外部依赖** | 仅 etcd | Kafka + ZK/KRaft |
| **运维成本** | 低 | 高 |
| **吞吐量** | 中（10-50K QPS） | 高（100K+ QPS） |
| **延迟** | 低（1-5ms） | 较低（2-10ms） |
| **功能丰富度** | 基础 | 丰富 |
| **生态** | 无 | 完善 |
| **适用规模** | 中小 | 大规模 |

---

## 8. 实施计划

### 阶段 1：核心功能（2-3周）

- [ ] WAL 存储引擎
- [ ] 基于 etcd 的 Leader 选举
- [ ] 单节点读写

### 阶段 2：复制功能（2周）

- [ ] 日志复制协议
- [ ] Quorum 确认机制
- [ ] Follower 同步

### 阶段 3：客户端 SDK（1周）

- [ ] 生产者接口
- [ ] 消费者接口
- [ ] 自动重连

### 阶段 4：与 Master Service 集成（1周）

- [ ] 写路径改造
- [ ] 消费者集成
- [ ] 故障切换测试

### 阶段 5：优化与测试（1-2周）

- [ ] 性能优化
- [ ] 混沌测试
- [ ] 文档完善

---

## 9. 总结

### 核心价值

> **EDQ = 轻量级 Kafka**：用最小的代价实现窗口期数据零丢失

### 关键设计

1. **WAL 持久化**：消息写入即持久化
2. **多副本复制**：Quorum 确认保证不丢
3. **消费者 Offset**：故障恢复从断点继续
4. **嵌入式部署**：无需独立运维

### 一句话总结

> **自研嵌入式分布式队列（EDQ）**：基于 WAL + Quorum 复制，嵌入 Master 进程，复用 etcd 选举，实现窗口期数据零丢失，无需引入 Kafka 等外部依赖。







