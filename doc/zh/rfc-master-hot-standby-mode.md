# RFC: Master Service 元数据高可用 - 热备模式设计

## 文档信息

| 项目 | 内容 |
|------|------|
| 作者 | Mooncake Team |
| 状态 | Draft |
| 创建日期 | 2024-12 |
| 版本 | v1.0 |

---

## 1. 背景

### 1.1 问题陈述

Mooncake Master Service 管理所有 KV Cache 对象的元数据，包括副本位置、分配信息和租约管理。当前，当 Master Leader 故障时：

- **所有元数据丢失** - 没有备份机制
- **服务完全不可用** - Follower 没有数据可服务
- **恢复时间长** - 必须等待客户端重新注册所有数据

### 1.2 设计目标

| 目标 | 指标 |
|------|------|
| **RPO（恢复点目标）** | < 1 秒（近零数据丢失） |
| **RTO（恢复时间目标）** | < 10 秒 |
| **数据一致性** | 带验证的强一致性 |
| **性能影响** | 写路径延迟增加 < 5% |

---

## 2. 架构概览

### 2.1 高层架构

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           热备架构                                       │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│   ┌─────────────────────────────────────────────────────────────────┐  │
│   │                        Master 集群                               │  │
│   │                                                                  │  │
│   │   ┌──────────────────┐          ┌──────────────────┐            │  │
│   │   │  Primary Master  │  OpLog   │  Standby Master  │            │  │
│   │   │    (Leader)      │ ──────►  │   (热备节点)      │            │  │
│   │   │                  │   流式   │                  │            │  │
│   │   │  ┌────────────┐  │          │  ┌────────────┐  │            │  │
│   │   │  │ 元数据存储  │  │  核查   │  │ 元数据存储  │  │            │  │
│   │   │  │           │◄─┼──────────┼──│  (副本)    │  │            │  │
│   │   │  └────────────┘  │  请求   │  └────────────┘  │            │  │
│   │   │                  │          │                  │            │  │
│   │   └────────┬─────────┘          └────────┬─────────┘            │  │
│   │            │                             │                       │  │
│   └────────────┼─────────────────────────────┼───────────────────────┘  │
│                │                             │                          │
│                │ 租约续约                     │ 监听                    │
│                ▼                             ▼                          │
│   ┌─────────────────────────────────────────────────────────────────┐  │
│   │                         etcd 集群                                │  │
│   │                  (Leader 选举 & 服务发现)                         │  │
│   └─────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│                ▲ 查询元数据                                             │
│                │                                                        │
│   ┌────────────┴────────────────────────────────────────────────────┐  │
│   │                      vLLM 推理客户端                              │  │
│   └─────────────────────────────────────────────────────────────────┘  │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.2 核心组件

| 组件 | 位置 | 职责 |
|------|------|------|
| **OpLogManager** | Primary | 生成和管理操作日志 |
| **ReplicationService** | Primary | 推送 OpLog 到 Standby，处理核查请求 |
| **HotStandbyService** | Standby | 接收 OpLog，维护副本元数据 |
| **OpLogApplier** | Standby | 将 OpLog 条目应用到本地元数据存储 |
| **VerificationClient** | Standby | 定期验证数据一致性 |

---

## 3. 详细设计

### 3.1 OpLog（操作日志）设计

OpLog 是热备复制的基础。Primary 上的每个状态变更操作都会生成一个 OpLog 条目。

#### 3.1.1 OpLog 条目结构

```cpp
enum class OpType : uint8_t {
    PUT_START = 1,      // 对象分配开始
    PUT_END = 2,        // 对象写入完成
    PUT_REVOKE = 3,     // 对象分配撤销
    REMOVE = 4,         // 对象删除
    MOUNT_SEGMENT = 5,  // 存储段挂载
    UNMOUNT_SEGMENT = 6,// 存储段卸载
    EVICTION = 7,       // 对象驱逐
};

struct OpLogEntry {
    uint64_t sequence_id;       // 全局唯一，单调递增
    uint64_t timestamp_ms;      // Unix 时间戳（毫秒）
    OpType op_type;             // 操作类型
    std::string object_key;     // 目标对象 key
    std::string payload;        // 序列化的操作数据
    uint32_t checksum;          // payload 的 CRC32
    uint32_t prefix_hash;       // key 前缀的哈希（用于核查）
    
    // 序列化支持
    YLT_REFL(OpLogEntry, sequence_id, timestamp_ms, op_type,
             object_key, payload, checksum, prefix_hash);
};
```

#### 3.1.2 OpLog 管理器

```cpp
class OpLogManager {
public:
    // 追加新条目，返回分配的 sequence_id
    uint64_t Append(OpType type, const std::string& key,
                    const std::string& payload);
    
    // 获取指定序号之后的条目（用于同步）
    std::vector<OpLogEntry> GetEntriesSince(uint64_t seq_id, size_t limit = 1000);
    
    // 获取当前序列号
    uint64_t GetLastSequenceId() const;
    
    // 截断旧条目以释放内存
    void TruncateBefore(uint64_t seq_id);
    
    // 获取内存使用量
    size_t GetMemoryUsage() const;

private:
    std::deque<OpLogEntry> buffer_;
    uint64_t first_seq_id_ = 1;     // 缓冲区中的第一个 seq_id
    uint64_t last_seq_id_ = 0;      // 最后分配的 seq_id
    mutable std::shared_mutex mutex_;
    
    // 配置
    static constexpr size_t kMaxBufferEntries = 100000;
    static constexpr size_t kMaxBufferBytes = 256 * 1024 * 1024; // 256MB
};
```

#### 3.1.3 OpLog 生成点

在 MasterService 中以下位置生成 OpLog 条目：

| 操作 | OpType | Payload 内容 |
|------|--------|--------------|
| `PutStart()` | PUT_START | 分配的副本信息 |
| `PutEnd()` | PUT_END | 最终副本状态 |
| `PutRevoke()` | PUT_REVOKE | 空 |
| `Remove()` | REMOVE | 空 |
| `MountSegment()` | MOUNT_SEGMENT | 段信息 |
| `UnmountSegment()` | UNMOUNT_SEGMENT | 段名称 |
| 驱逐 | EVICTION | 被驱逐的对象 keys |

### 3.2 复制服务（Primary 端）

#### 3.2.1 架构

```
┌─────────────────────────────────────────────────────────────┐
│                    Primary Master                            │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────┐    ┌───────────────┐    ┌──────────────┐ │
│  │ MasterService│───►│ OpLogManager  │───►│ Replication  │ │
│  │              │    │               │    │ Service      │ │
│  │ (写操作)     │    │ (生成)        │    │ (广播)       │ │
│  └──────────────┘    └───────────────┘    └──────┬───────┘ │
│                                                   │         │
│  ┌──────────────────────────────────────────────┐│         │
│  │         核查处理器                            ││         │
│  │  (响应 Standby 的核查请求)                    ││         │
│  └──────────────────────────────────────────────┘│         │
│                                                   │         │
└───────────────────────────────────────────────────┼─────────┘
                                                    │
                    ┌───────────────────────────────┼───────────────────┐
                    │                               │                   │
                    ▼                               ▼                   ▼
            ┌──────────────┐               ┌──────────────┐    ┌──────────────┐
            │  Standby 1   │               │  Standby 2   │    │  Standby N   │
            └──────────────┘               └──────────────┘    └──────────────┘
```

#### 3.2.2 复制服务实现

```cpp
class ReplicationService {
public:
    explicit ReplicationService(OpLogManager& oplog_manager,
                               MasterService& master_service);
    
    // 注册新的 Standby 连接
    void RegisterStandby(const std::string& standby_id,
                        std::shared_ptr<ReplicationStream> stream);
    
    // 注销 Standby（断开连接时）
    void UnregisterStandby(const std::string& standby_id);
    
    // OpLogManager 追加新条目时调用
    void OnNewOpLog(const OpLogEntry& entry);
    
    // 处理来自 Standby 的核查请求
    VerificationResponse HandleVerification(const VerificationRequest& request);
    
    // 获取每个 Standby 的复制延迟
    std::map<std::string, uint64_t> GetReplicationLag() const;

private:
    void BroadcastEntry(const OpLogEntry& entry);
    void SendBatch(const std::string& standby_id,
                   const std::vector<OpLogEntry>& entries);
    
    OpLogManager& oplog_manager_;
    MasterService& master_service_;
    
    struct StandbyState {
        std::shared_ptr<ReplicationStream> stream;
        uint64_t acked_seq_id = 0;
        std::chrono::steady_clock::time_point last_ack_time;
    };
    std::unordered_map<std::string, StandbyState> standbys_;
    mutable std::shared_mutex mutex_;
    
    // 批处理配置
    static constexpr size_t kBatchSize = 100;
    static constexpr uint32_t kBatchTimeoutMs = 10;
};
```

#### 3.2.3 RPC 接口

```protobuf
service ReplicationService {
    // 流式 OpLog 同步
    rpc SyncOpLog(SyncOpLogRequest) returns (stream OpLogBatch);
    
    // 数据核查
    rpc Verify(VerificationRequest) returns (VerificationResponse);
    
    // 获取 Primary 状态（用于健康检查）
    rpc GetStatus(StatusRequest) returns (StatusResponse);
}

message SyncOpLogRequest {
    string standby_id = 1;
    uint64 start_seq_id = 2;  // 从此序号恢复
}

message OpLogBatch {
    repeated OpLogEntry entries = 1;
    uint64 primary_seq_id = 2;  // 当前 Primary 序列号
}

message OpLogEntry {
    uint64 sequence_id = 1;
    uint64 timestamp_ms = 2;
    uint32 op_type = 3;
    string object_key = 4;
    bytes payload = 5;
    uint32 checksum = 6;
    uint32 prefix_hash = 7;
}
```

### 3.3 热备服务（Standby 端）

#### 3.3.1 架构

```
┌─────────────────────────────────────────────────────────────┐
│                    Standby Master                            │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                  HotStandbyService                    │   │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────────┐  │   │
│  │  │ 复制客户端 │  │  OpLog     │  │ 核查客户端     │  │   │
│  │  │           │─►│  应用器    │  │               │  │   │
│  │  └────────────┘  └─────┬──────┘  └───────┬────────┘  │   │
│  │                        │                  │           │   │
│  │                        ▼                  │           │   │
│  │               ┌────────────────┐          │           │   │
│  │               │ 元数据存储     │◄─────────┘           │   │
│  │               │  (副本)       │   读取用于核查        │   │
│  │               └────────────────┘                      │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                              │
│                          │                                   │
│                          │ 提升时                            │
│                          ▼                                   │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              MasterService (激活)                     │   │
│  │         (使用 MetadataStore 作为初始状态)             │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

#### 3.3.2 热备服务实现

```cpp
class HotStandbyService {
public:
    explicit HotStandbyService(const HotStandbyConfig& config);
    
    // 连接到 Primary 并开始复制
    ErrorCode Start(const std::string& primary_address);
    
    // 停止复制
    void Stop();
    
    // 获取当前同步状态
    struct SyncStatus {
        uint64_t applied_seq_id;
        uint64_t primary_seq_id;
        uint64_t lag_entries;
        std::chrono::milliseconds lag_time;
        bool is_syncing;
    };
    SyncStatus GetSyncStatus() const;
    
    // 检查是否准备好提升（延迟在阈值内）
    bool IsReadyForPromotion() const;
    
    // 提升为 Primary（选举获胜后调用）
    std::unique_ptr<MasterService> Promote();

private:
    void ReplicationLoop();
    void VerificationLoop();
    void ApplyOpLogEntry(const OpLogEntry& entry);
    
    HotStandbyConfig config_;
    std::unique_ptr<MetadataStore> metadata_store_;
    std::unique_ptr<ReplicationClient> replication_client_;
    std::unique_ptr<VerificationClient> verification_client_;
    
    std::atomic<uint64_t> applied_seq_id_{0};
    std::atomic<uint64_t> primary_seq_id_{0};
    std::atomic<bool> running_{false};
    
    std::thread replication_thread_;
    std::thread verification_thread_;
};
```

### 3.4 核查机制

核查机制通过定期采样和校验和比较来确保 Primary 和 Standby 之间的数据一致性。

#### 3.4.1 核查流程

```
┌──────────────────────────────────────────────────────────────────────────┐
│                           核查流程                                        │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Standby                                              Primary            │
│    │                                                     │               │
│    │  1. 选择要核查的 shard（轮询 10%）                   │               │
│    │                                                     │               │
│    │  2. 从每个 shard 采样 key（最多 100 个）             │               │
│    │                                                     │               │
│    │  3. 计算核查条目:                                   │               │
│    │     prefix_hash = CRC32(key[0:8])                   │               │
│    │     checksum = CRC32(serialize(metadata))           │               │
│    │                                                     │               │
│    │  ─────────── VerifyRequest ──────────────────────►  │               │
│    │     {shard_ids, entries[], standby_seq_id}          │               │
│    │                                                     │               │
│    │                              4. 对每个 prefix_hash:  │               │
│    │                                 - 查找匹配的 keys    │               │
│    │                                 - 计算校验和         │               │
│    │                                 - 比较               │               │
│    │                                                     │               │
│    │  ◄─────────── VerifyResponse ─────────────────────  │               │
│    │     {status, mismatches[], primary_seq_id}          │               │
│    │                                                     │               │
│    │  5. 如果发现不匹配:                                  │               │
│    │     - 使用 correct_metadata 修复数据                │               │
│    │     - 记录警告日志                                  │               │
│    │                                                     │               │
│    │  6. 如果 NEED_FULL_SYNC:                            │               │
│    │     - 请求完整元数据转储                            │               │
│    │     - 重建本地存储                                  │               │
│    │                                                     │               │
└──────────────────────────────────────────────────────────────────────────┘
```

#### 3.4.2 核查数据结构

```cpp
struct VerificationEntry {
    uint32_t prefix_hash;  // CRC32(object_key.substr(0, 8))
    uint32_t checksum;     // CRC32(serialize(metadata_without_dynamic_fields))
};

struct VerificationRequest {
    std::vector<uint32_t> shard_ids;
    std::vector<VerificationEntry> entries;
    uint64_t standby_seq_id;
};

struct MismatchEntry {
    uint32_t prefix_hash;
    std::string object_key;
    std::string correct_metadata;  // 序列化的 ObjectMetadata
    enum Type { CHECKSUM_MISMATCH, KEY_NOT_FOUND, EXTRA_KEY };
    Type type;
};

struct VerificationResponse {
    enum Status { OK, MISMATCH, NEED_FULL_SYNC };
    Status status;
    std::vector<MismatchEntry> mismatches;
    uint64_t primary_seq_id;
};
```

#### 3.4.3 校验和计算

只有**静态字段**包含在校验和中以确保一致性：

```cpp
uint32_t CalculateMetadataChecksum(const ObjectMetadata& meta) {
    // 只包含静态字段
    std::string data;
    
    // 包含副本信息
    for (const auto& replica : meta.replicas) {
        data += std::to_string(static_cast<int>(replica.status()));
        if (replica.is_memory_replica()) {
            auto& desc = replica.get_memory_descriptor();
            for (const auto& buf : desc.buffer_descriptors) {
                data += buf.segment_name_;
                data += std::to_string(buf.address_);
                data += std::to_string(buf.size_);
            }
        }
    }
    
    // 包含大小
    data += std::to_string(meta.size);
    
    // 排除动态字段: lease_timeout, soft_pin_timeout
    
    return CRC32(data);
}
```

#### 3.4.4 核查配置

```cpp
struct VerificationConfig {
    uint32_t interval_sec = 30;           // 核查间隔
    float sample_ratio = 0.1;             // 每轮 10% 的 shard
    uint32_t max_entries_per_shard = 100; // 每个 shard 最多采样的 key 数
    uint32_t max_mismatches_for_repair = 10;  // 自动修复阈值
    uint64_t seq_diff_threshold = 1000;   // 触发 NEED_FULL_SYNC 的阈值
};
```

### 3.5 故障切换流程

#### 3.5.1 状态机

```
                    ┌─────────────────┐
                    │   INITIALIZING  │
                    └────────┬────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
              ▼              │              ▼
    ┌─────────────────┐     │    ┌─────────────────┐
    │    FOLLOWER     │     │    │    CANDIDATE    │
    │   (热备节点)    │◄────┴───►│    (选举中)     │
    └────────┬────────┘          └────────┬────────┘
             │                            │
             │ Leader 消失                │ 选举获胜
             │ + 准备好提升               │
             │                            │
             └──────────┬─────────────────┘
                        │
                        ▼
              ┌─────────────────┐
              │     LEADER      │
              │   (Primary)     │
              └─────────────────┘
```

#### 3.5.2 故障切换序列

```
时间线:
    T0          T1              T2              T3              T4
    │           │               │               │               │
    ▼           ▼               ▼               ▼               ▼
[Primary    [Primary        [租约         [Standby        [新 Leader
 运行中]     崩溃]          过期]          当选]          服务中]
    │                           │               │               │
    │◄── 正常运行 ────────────►│◄─── 5s ────►│◄─── <1s ────►│
    │                           │               │               │
    │   Standby 持续同步        │  etcd 检测   │ 选举 +        │
    │   OpLog                   │  到故障      │ 安全窗口      │
    │                           │               │               │

总 RTO: ~6-10 秒
- 租约 TTL: 5 秒
- 选举: < 1 秒
- 安全窗口: 5 秒（防止脑裂）
- 服务激活: < 1 秒
```

#### 3.5.3 提升流程

当 Standby 赢得选举时：

```cpp
// 在 MasterServiceSupervisor 中
void OnElectionWon() {
    // 1. 等待安全窗口（旧 leader 的租约 TTL）
    std::this_thread::sleep_for(std::chrono::seconds(ETCD_MASTER_VIEW_LEASE_TTL));
    
    // 2. 检查是否准备好提升
    if (!hot_standby_service_->IsReadyForPromotion()) {
        LOG(ERROR) << "Not ready for promotion, lag too high";
        // 放弃领导权并重试
        return;
    }
    
    // 3. 提升: 将元数据转移到 MasterService
    auto master_service = hot_standby_service_->Promote();
    
    // 4. 为新的 Standby 启动复制服务
    replication_service_ = std::make_unique<ReplicationService>(
        *oplog_manager_, *master_service);
    
    // 5. 启动 RPC 服务
    StartRpcServer(*master_service);
    
    // 6. 更新 etcd 中的 master 地址
    UpdateMasterView();
}
```

### 3.6 全量同步（初始化 & 恢复）

用于初始同步或核查检测到大量差异时：

```cpp
class FullSyncService {
public:
    // Primary 端: 流式传输所有元数据
    void StreamFullDump(FullDumpStream* stream);
    
    // Standby 端: 接收并应用完整转储
    ErrorCode ReceiveFullDump(const std::string& primary_addr);

private:
    // 分块传输以避免内存问题
    static constexpr size_t kChunkSize = 10000;  // 每块条目数
};
```

**全量同步协议：**

```
Standby                                   Primary
   │                                         │
   │──── FullSyncRequest ──────────────────►│
   │     {standby_id}                        │
   │                                         │
   │◄─── FullSyncResponse (块 1) ───────────│
   │     {entries[0..10000], has_more=true}  │
   │                                         │
   │◄─── FullSyncResponse (块 2) ───────────│
   │     {entries[10001..20000], has_more=true}│
   │                                         │
   │           ... 更多块 ...                │
   │                                         │
   │◄─── FullSyncResponse (最后) ───────────│
   │     {entries[N..M], has_more=false,     │
   │      final_seq_id=12345}                │
   │                                         │
   │──── 从 12345 开始 OpLog 同步 ─────────►│
   │                                         │
```

---

## 4. 配置

```cpp
struct HotStandbyConfig {
    // OpLog 设置
    size_t oplog_max_entries = 100000;
    size_t oplog_max_bytes = 256 * 1024 * 1024;  // 256MB
    
    // 复制设置
    uint32_t replication_batch_size = 100;
    uint32_t replication_batch_timeout_ms = 10;
    uint32_t replication_connect_timeout_ms = 5000;
    uint32_t replication_retry_interval_ms = 1000;
    
    // 核查设置
    uint32_t verify_interval_sec = 30;
    float verify_sample_ratio = 0.1;
    uint32_t verify_max_entries_per_shard = 100;
    uint32_t verify_max_mismatches_for_repair = 10;
    uint64_t verify_seq_diff_threshold = 1000;
    
    // 提升设置
    uint64_t max_lag_for_promotion_ms = 5000;
    uint64_t max_lag_entries_for_promotion = 100;
    
    // 网络设置
    std::string primary_address;  // Standby 连接用
    uint16_t replication_port = 12346;
};
```

---

## 5. 监控指标

### 5.1 关键指标

| 指标 | 描述 | 告警阈值 |
|------|------|----------|
| `oplog_sequence_id` | 当前 OpLog 序列号 | - |
| `oplog_buffer_size` | OpLog 缓冲区条目数 | > 80% 最大值 |
| `oplog_buffer_bytes` | OpLog 缓冲区内存使用 | > 80% 最大值 |
| `replication_lag_entries` | 落后 Primary 的条目数 | > 1000 |
| `replication_lag_ms` | 落后 Primary 的时间 | > 5000ms |
| `verification_mismatches` | 发现的不匹配数 | > 0 |
| `verification_duration_ms` | 核查轮次耗时 | > 10000ms |
| `standby_count` | 已连接的 Standby 数 | < 预期值 |

### 5.2 健康检查

```cpp
struct HealthStatus {
    bool is_leader;
    uint64_t current_seq_id;
    
    // 仅 Primary
    std::vector<StandbyHealth> standbys;
    
    // 仅 Standby
    uint64_t applied_seq_id;
    uint64_t lag_entries;
    std::chrono::milliseconds lag_time;
    bool verification_healthy;
};
```

---

## 6. 错误处理

### 6.1 复制错误

| 错误 | 处理方式 |
|------|----------|
| 连接丢失 | 指数退避重连 |
| 检测到序号间隙 | 请求缺失条目或全量同步 |
| 应用失败 | 记录错误，跳过条目，触发核查 |
| 校验和不匹配 | 拒绝条目，请求重传 |

### 6.2 核查错误

| 错误 | 处理方式 |
|------|----------|
| 少量不匹配（< 阈值） | 使用 Primary 数据自动修复 |
| 大量不匹配（>= 阈值） | 触发全量同步 |
| Primary 不可达 | 跳过本轮，下个间隔重试 |

### 6.3 提升错误

| 错误 | 处理方式 |
|------|----------|
| 延迟过高 | 拒绝提升，重试选举 |
| 元数据不一致 | 拒绝提升，触发全量同步 |
| etcd 更新失败 | 退避重试 |

---

## 7. 实现计划

### 阶段 1: 核心基础设施（2 周）
- [ ] OpLogEntry 数据结构
- [ ] OpLogManager 实现
- [ ] 序列化/反序列化

### 阶段 2: 复制服务（3 周）
- [ ] Primary 上的 ReplicationService
- [ ] Standby 上的 HotStandbyService
- [ ] OpLogApplier 实现
- [ ] gRPC 流式接口

### 阶段 3: 核查机制（2 周）
- [ ] VerificationClient 实现
- [ ] VerificationHandler 实现
- [ ] 自动修复逻辑
- [ ] 全量同步服务

### 阶段 4: 集成与测试（2 周）
- [ ] 与 MasterServiceSupervisor 集成
- [ ] 故障切换测试
- [ ] 性能测试
- [ ] 混沌测试

---

## 8. 附录

### 8.1 PlantUML 图

详细架构图请参见 `doc/zh/diagrams/hot-standby-only/`:
- `architecture.puml` - 整体架构
- `replication-flow.puml` - OpLog 复制流程
- `verification-flow.puml` - 核查机制
- `failover-sequence.puml` - 故障切换流程
- `state-machine.puml` - 节点状态机

### 8.2 相关文档

- RFC: Master Service HA（原始基于 etcd 的选举）
- RFC: 混合模式（热备 + 快照）








