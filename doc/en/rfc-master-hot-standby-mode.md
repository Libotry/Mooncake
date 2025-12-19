# RFC: Master Service Metadata High Availability - Hot Standby Mode Design

## Document Information

| Item | Content |
|------|---------|
| Author | Mooncake Team |
| Status | Draft |
| Created | 2024-12 |
| Version | v1.0 |

---

## 1. Background

### 1.1 Problem Statement

Mooncake Master Service manages all metadata for KV Cache objects, including replica locations, allocation information, and lease management. Currently, when the Master Leader fails:

- **All metadata is lost** - No backup mechanism exists
- **Service completely unavailable** - Followers have no data to serve
- **Long recovery time** - Must wait for clients to re-register all data

### 1.2 Design Goals

| Goal | Target |
|------|--------|
| **RPO (Recovery Point Objective)** | < 1 second (near-zero data loss) |
| **RTO (Recovery Time Objective)** | < 10 seconds |
| **Data Consistency** | Strong consistency with verification |
| **Performance Impact** | < 5% latency increase on write path |

---

## 2. Architecture Overview

### 2.1 High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     Hot Standby Architecture                             │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│   ┌─────────────────────────────────────────────────────────────────┐  │
│   │                        Master Cluster                            │  │
│   │                                                                  │  │
│   │   ┌──────────────────┐          ┌──────────────────┐            │  │
│   │   │  Primary Master  │  OpLog   │  Standby Master  │            │  │
│   │   │    (Leader)      │ ──────►  │  (Hot Standby)   │            │  │
│   │   │                  │  Stream  │                  │            │  │
│   │   │  ┌────────────┐  │          │  ┌────────────┐  │            │  │
│   │   │  │ Metadata   │  │ Verify   │  │ Metadata   │  │            │  │
│   │   │  │ Store      │◄─┼──────────┼──│ Store      │  │            │  │
│   │   │  └────────────┘  │ Request  │  │ (Replica)  │  │            │  │
│   │   │                  │          │  └────────────┘  │            │  │
│   │   └────────┬─────────┘          └────────┬─────────┘            │  │
│   │            │                             │                       │  │
│   └────────────┼─────────────────────────────┼───────────────────────┘  │
│                │                             │                          │
│                │ Lease KeepAlive             │ Watch                    │
│                ▼                             ▼                          │
│   ┌─────────────────────────────────────────────────────────────────┐  │
│   │                         etcd Cluster                             │  │
│   │              (Leader Election & Service Discovery)               │  │
│   └─────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│                ▲ Query Metadata                                         │
│                │                                                        │
│   ┌────────────┴────────────────────────────────────────────────────┐  │
│   │                      vLLM Inference Clients                      │  │
│   └─────────────────────────────────────────────────────────────────┘  │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Architecture Diagram Description

The Hot Standby architecture consists of four main layers working together to provide high availability:

#### Layer 1: Master Cluster

The Master Cluster contains one **Primary Master** (Leader) and one or more **Standby Masters** (Hot Standbys).

**Primary Master (Leader)** is the active node that:
- **MasterService**: The core metadata management component that handles all client requests (Query, Put, Remove). It maintains the authoritative copy of all object metadata including replica locations, sizes, and lease information.
- **OpLogManager**: Generates and buffers Operation Log (OpLog) entries for every state-changing operation. Each entry contains a globally unique sequence ID, timestamp, operation type, object key, and payload with checksums.
- **ReplicationService**: Responsible for broadcasting OpLog entries to all connected Standby nodes via gRPC streaming. It tracks acknowledgments from each Standby to monitor replication lag.
- **VerificationHandler**: Responds to periodic verification requests from Standby nodes, comparing checksums to detect and help repair any data inconsistencies.

**Standby Master (Hot Standby)** is a passive replica that:
- **HotStandbyService**: The core service that manages the standby lifecycle, including connecting to Primary, coordinating replication and verification, and handling promotion when elected as new leader.
- **OpLogApplier**: Receives OpLog entries from Primary and applies them to the local MetadataStore in sequence order. Ensures the replica stays synchronized with Primary.
- **MetadataStore**: A complete replica of the Primary's metadata. This replica enables instant promotion without data loss when failover occurs.
- **VerificationClient**: Periodically samples local data, calculates checksums, and sends verification requests to Primary to detect any inconsistencies caused by bugs, network issues, or other anomalies.

#### Layer 2: Coordination Layer (etcd)

The **etcd Cluster** provides distributed coordination:
- **Leader Election**: Uses etcd's lease mechanism with a 5-second TTL. The Primary must continuously renew its lease; failure to do so triggers automatic leader election among Standbys.
- **Service Discovery**: Stores the current master's address so clients can discover and connect to the active Primary. Updated atomically during failover.

#### Layer 3: Data Flow

There are three primary data flows in the architecture:

1. **OpLog Replication Flow** (Primary → Standby):
   - Step 1: Client sends write request to MasterService
   - Step 2: MasterService processes request and calls OpLogManager.Append()
   - Step 3: OpLogManager creates entry with sequence_id and notifies ReplicationService
   - Step 4: ReplicationService pushes entry to all Standbys via gRPC stream
   - Step 5: Each Standby's OpLogApplier applies the entry to MetadataStore
   - Step 6: Standby sends ACK back to Primary

2. **Verification Flow** (Standby → Primary):
   - VerificationClient samples keys from local MetadataStore
   - Calculates prefix_hash (CRC32 of key prefix) and checksum (CRC32 of metadata)
   - Sends VerificationRequest to Primary's VerificationHandler
   - Primary compares with its data and returns mismatches
   - Standby auto-repairs minor inconsistencies

3. **Client Request Flow** (Client → Primary):
   - Clients query etcd for current master address
   - Send RPC requests (Query/Put/Remove) to Primary's MasterService
   - Primary processes requests and returns responses

#### Layer 4: Client Layer

The **vLLM Inference Cluster** consists of multiple vLLM instances that:
- Query metadata to locate KV Cache replicas
- Register new KV Cache objects via Put operations
- Remove expired or unused objects

### 2.3 Core Components Summary

| Component | Location | Responsibility |
|-----------|----------|----------------|
| **MasterService** | Primary | Handle all client RPCs, manage metadata |
| **OpLogManager** | Primary | Generate, buffer, and manage operation logs |
| **ReplicationService** | Primary | Push OpLog to Standbys, handle verification requests |
| **VerificationHandler** | Primary | Respond to Standby verification requests |
| **HotStandbyService** | Standby | Manage standby lifecycle and promotion |
| **OpLogApplier** | Standby | Apply OpLog entries to local metadata store |
| **MetadataStore** | Standby | Store replica of all metadata |
| **VerificationClient** | Standby | Periodically verify data consistency with Primary |

### 2.4 Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| **Asynchronous Replication** | Minimizes write latency impact while providing near-real-time sync (~10ms) |
| **Stream-based Push** | More efficient than polling; enables immediate propagation of changes |
| **Prefix Hash for Verification** | Reduces bandwidth (4 bytes vs variable-length key) while enabling fast lookup |
| **Checksum Excludes Dynamic Fields** | lease_timeout and soft_pin_timeout change frequently; excluding them ensures stable checksums |
| **Safety Window on Promotion** | Waiting for old lease TTL prevents split-brain scenarios |

---

## 3. Detailed Design

### 3.1 OpLog (Operation Log) Design

OpLog is the foundation of hot standby replication. Every state-changing operation on Primary generates an OpLog entry.

#### 3.1.1 OpLog Entry Structure

```cpp
enum class OpType : uint8_t {
    PUT_START = 1,      // Object allocation started
    PUT_END = 2,        // Object write completed
    PUT_REVOKE = 3,     // Object allocation revoked
    REMOVE = 4,         // Object removed
    MOUNT_SEGMENT = 5,  // Storage segment mounted
    UNMOUNT_SEGMENT = 6,// Storage segment unmounted
    EVICTION = 7,       // Object evicted
};

struct OpLogEntry {
    uint64_t sequence_id;       // Globally unique, monotonically increasing
    uint64_t timestamp_ms;      // Unix timestamp in milliseconds
    OpType op_type;             // Operation type
    std::string object_key;     // Target object key
    std::string payload;        // Serialized operation data
    uint32_t checksum;          // CRC32 of payload
    uint32_t prefix_hash;       // Hash of key prefix (for verification)
    
    // Serialization
    YLT_REFL(OpLogEntry, sequence_id, timestamp_ms, op_type,
             object_key, payload, checksum, prefix_hash);
};
```

#### 3.1.2 OpLog Manager

```cpp
class OpLogManager {
public:
    // Append new entry, returns assigned sequence_id
    uint64_t Append(OpType type, const std::string& key,
                    const std::string& payload);
    
    // Get entries since given sequence (for sync)
    std::vector<OpLogEntry> GetEntriesSince(uint64_t seq_id, size_t limit = 1000);
    
    // Get current sequence ID
    uint64_t GetLastSequenceId() const;
    
    // Truncate old entries to free memory
    void TruncateBefore(uint64_t seq_id);
    
    // Get memory usage
    size_t GetMemoryUsage() const;

private:
    std::deque<OpLogEntry> buffer_;
    uint64_t first_seq_id_ = 1;     // First seq_id in buffer
    uint64_t last_seq_id_ = 0;      // Last assigned seq_id
    mutable std::shared_mutex mutex_;
    
    // Configuration
    static constexpr size_t kMaxBufferEntries = 100000;
    static constexpr size_t kMaxBufferBytes = 256 * 1024 * 1024; // 256MB
};
```

#### 3.1.3 OpLog Generation Points

OpLog entries are generated at these points in MasterService:

| Operation | OpType | Payload Content |
|-----------|--------|-----------------|
| `PutStart()` | PUT_START | Allocated replicas info |
| `PutEnd()` | PUT_END | Final replica status |
| `PutRevoke()` | PUT_REVOKE | Empty |
| `Remove()` | REMOVE | Empty |
| `MountSegment()` | MOUNT_SEGMENT | Segment info |
| `UnmountSegment()` | UNMOUNT_SEGMENT | Segment name |
| Eviction | EVICTION | Evicted object keys |

### 3.2 Replication Service (Primary Side)

#### 3.2.1 Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Primary Master                            │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────┐    ┌───────────────┐    ┌──────────────┐ │
│  │ MasterService│───►│ OpLogManager  │───►│ Replication  │ │
│  │              │    │               │    │ Service      │ │
│  │ (Write Ops)  │    │ (Generate)    │    │ (Broadcast)  │ │
│  └──────────────┘    └───────────────┘    └──────┬───────┘ │
│                                                   │         │
│  ┌──────────────────────────────────────────────┐│         │
│  │         Verification Handler                  ││         │
│  │  (Respond to Standby verification requests)   ││         │
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

#### 3.2.2 Replication Service Implementation

```cpp
class ReplicationService {
public:
    explicit ReplicationService(OpLogManager& oplog_manager,
                               MasterService& master_service);
    
    // Register a new Standby connection
    void RegisterStandby(const std::string& standby_id,
                        std::shared_ptr<ReplicationStream> stream);
    
    // Unregister Standby (on disconnect)
    void UnregisterStandby(const std::string& standby_id);
    
    // Called by OpLogManager when new entry is appended
    void OnNewOpLog(const OpLogEntry& entry);
    
    // Handle verification request from Standby
    VerificationResponse HandleVerification(const VerificationRequest& request);
    
    // Get replication lag for each Standby
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
    
    // Batching configuration
    static constexpr size_t kBatchSize = 100;
    static constexpr uint32_t kBatchTimeoutMs = 10;
};
```

#### 3.2.3 RPC Interface

```protobuf
service ReplicationService {
    // Streaming OpLog synchronization
    rpc SyncOpLog(SyncOpLogRequest) returns (stream OpLogBatch);
    
    // Data verification
    rpc Verify(VerificationRequest) returns (VerificationResponse);
    
    // Get Primary status (for health check)
    rpc GetStatus(StatusRequest) returns (StatusResponse);
}

message SyncOpLogRequest {
    string standby_id = 1;
    uint64 start_seq_id = 2;  // Resume from this sequence
}

message OpLogBatch {
    repeated OpLogEntry entries = 1;
    uint64 primary_seq_id = 2;  // Current Primary sequence ID
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

### 3.3 Hot Standby Service (Standby Side)

#### 3.3.1 Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Standby Master                            │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────────────────────────────────────────────┐   │
│  │                  HotStandbyService                    │   │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────────┐  │   │
│  │  │ Replication│  │  OpLog     │  │ Verification   │  │   │
│  │  │ Client     │─►│  Applier   │  │ Client         │  │   │
│  │  └────────────┘  └─────┬──────┘  └───────┬────────┘  │   │
│  │                        │                  │           │   │
│  │                        ▼                  │           │   │
│  │               ┌────────────────┐          │           │   │
│  │               │ MetadataStore  │◄─────────┘           │   │
│  │               │  (Replica)     │   Read for verify    │   │
│  │               └────────────────┘                      │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                              │
│                          │                                   │
│                          │ On promotion                      │
│                          ▼                                   │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              MasterService (Activated)                │   │
│  │         (Uses MetadataStore as initial state)         │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

#### 3.3.2 Hot Standby Service Implementation

```cpp
class HotStandbyService {
public:
    explicit HotStandbyService(const HotStandbyConfig& config);
    
    // Connect to Primary and start replication
    ErrorCode Start(const std::string& primary_address);
    
    // Stop replication
    void Stop();
    
    // Get current sync status
    struct SyncStatus {
        uint64_t applied_seq_id;
        uint64_t primary_seq_id;
        uint64_t lag_entries;
        std::chrono::milliseconds lag_time;
        bool is_syncing;
    };
    SyncStatus GetSyncStatus() const;
    
    // Check if ready for promotion (lag within threshold)
    bool IsReadyForPromotion() const;
    
    // Promote to Primary (called after winning election)
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

#### 3.3.3 OpLog Applier

```cpp
class OpLogApplier {
public:
    explicit OpLogApplier(MetadataStore& store);
    
    // Apply a single OpLog entry
    ErrorCode Apply(const OpLogEntry& entry);
    
    // Apply a batch of entries (must be in order)
    ErrorCode ApplyBatch(const std::vector<OpLogEntry>& entries);

private:
    ErrorCode ApplyPutStart(const OpLogEntry& entry);
    ErrorCode ApplyPutEnd(const OpLogEntry& entry);
    ErrorCode ApplyPutRevoke(const OpLogEntry& entry);
    ErrorCode ApplyRemove(const OpLogEntry& entry);
    ErrorCode ApplyMountSegment(const OpLogEntry& entry);
    ErrorCode ApplyUnmountSegment(const OpLogEntry& entry);
    ErrorCode ApplyEviction(const OpLogEntry& entry);
    
    MetadataStore& store_;
};
```

### 3.4 Verification Mechanism

The verification mechanism ensures data consistency between Primary and Standby through periodic sampling and checksum comparison.

#### 3.4.1 Verification Flow

```
┌──────────────────────────────────────────────────────────────────────────┐
│                        Verification Flow                                  │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  Standby                                              Primary            │
│    │                                                     │               │
│    │  1. Select shards to verify (round-robin 10%)       │               │
│    │                                                     │               │
│    │  2. Sample keys from each shard (max 100)           │               │
│    │                                                     │               │
│    │  3. Calculate verification entries:                 │               │
│    │     prefix_hash = CRC32(key[0:8])                   │               │
│    │     checksum = CRC32(serialize(metadata))           │               │
│    │                                                     │               │
│    │  ─────────── VerifyRequest ──────────────────────►  │               │
│    │     {shard_ids, entries[], standby_seq_id}          │               │
│    │                                                     │               │
│    │                              4. For each prefix_hash:│               │
│    │                                 - Find matching keys │               │
│    │                                 - Calculate checksum │               │
│    │                                 - Compare            │               │
│    │                                                     │               │
│    │  ◄─────────── VerifyResponse ─────────────────────  │               │
│    │     {status, mismatches[], primary_seq_id}          │               │
│    │                                                     │               │
│    │  5. If mismatches found:                            │               │
│    │     - Repair data using correct_metadata            │               │
│    │     - Log warning                                   │               │
│    │                                                     │               │
│    │  6. If NEED_FULL_SYNC:                              │               │
│    │     - Request full metadata dump                    │               │
│    │     - Rebuild local store                           │               │
│    │                                                     │               │
└──────────────────────────────────────────────────────────────────────────┘
```

#### 3.4.2 Verification Data Structures

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
    std::string correct_metadata;  // Serialized ObjectMetadata
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

#### 3.4.3 Checksum Calculation

Only **static fields** are included in checksum to ensure consistency:

```cpp
uint32_t CalculateMetadataChecksum(const ObjectMetadata& meta) {
    // Only include static fields
    std::string data;
    
    // Include replicas
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
    
    // Include size
    data += std::to_string(meta.size);
    
    // Exclude dynamic fields: lease_timeout, soft_pin_timeout
    
    return CRC32(data);
}
```

#### 3.4.4 Verification Configuration

```cpp
struct VerificationConfig {
    uint32_t interval_sec = 30;           // Verification interval
    float sample_ratio = 0.1;             // 10% of shards per round
    uint32_t max_entries_per_shard = 100; // Max keys to sample per shard
    uint32_t max_mismatches_for_repair = 10;  // Auto-repair threshold
    uint64_t seq_diff_threshold = 1000;   // Trigger NEED_FULL_SYNC
};
```

### 3.5 Failover Process

#### 3.5.1 State Machine

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
    │  (Hot Standby)  │◄────┴───►│   (Electing)    │
    └────────┬────────┘          └────────┬────────┘
             │                            │
             │ Leader disappeared         │ Election won
             │ + Ready for promotion      │
             │                            │
             └──────────┬─────────────────┘
                        │
                        ▼
              ┌─────────────────┐
              │     LEADER      │
              │   (Primary)     │
              └─────────────────┘
```

#### 3.5.2 Failover Sequence

```
Timeline:
    T0          T1              T2              T3              T4
    │           │               │               │               │
    ▼           ▼               ▼               ▼               ▼
[Primary    [Primary        [Lease         [Standby        [New Leader
 Running]    Crashes]        Expires]       Elected]        Serving]
    │                           │               │               │
    │◄── Normal Operation ────►│◄─── 5s ────►│◄─── <1s ────►│
    │                           │               │               │
    │   Standbys syncing        │  etcd detects │ Election +    │
    │   OpLog continuously      │  failure      │ Safety window │
    │                           │               │               │

Total RTO: ~6-10 seconds
- Lease TTL: 5 seconds
- Election: < 1 second
- Safety window: 5 seconds (to prevent split-brain)
- Service activation: < 1 second
```

#### 3.5.3 Promotion Process

When a Standby wins the election:

```cpp
// In MasterServiceSupervisor
void OnElectionWon() {
    // 1. Wait for safety window (old leader's lease TTL)
    std::this_thread::sleep_for(std::chrono::seconds(ETCD_MASTER_VIEW_LEASE_TTL));
    
    // 2. Check if ready for promotion
    if (!hot_standby_service_->IsReadyForPromotion()) {
        LOG(ERROR) << "Not ready for promotion, lag too high";
        // Give up leadership and retry
        return;
    }
    
    // 3. Promote: transfer metadata to MasterService
    auto master_service = hot_standby_service_->Promote();
    
    // 4. Start replication service for new Standbys
    replication_service_ = std::make_unique<ReplicationService>(
        *oplog_manager_, *master_service);
    
    // 5. Start RPC service
    StartRpcServer(*master_service);
    
    // 6. Update etcd with new master address
    UpdateMasterView();
}
```

### 3.6 Full Sync (Bootstrap & Recovery)

For initial sync or when verification detects large divergence:

```cpp
class FullSyncService {
public:
    // Primary side: Stream all metadata
    void StreamFullDump(FullDumpStream* stream);
    
    // Standby side: Receive and apply full dump
    ErrorCode ReceiveFullDump(const std::string& primary_addr);

private:
    // Chunked transfer to avoid memory issues
    static constexpr size_t kChunkSize = 10000;  // entries per chunk
};
```

**Full Sync Protocol:**

```
Standby                                   Primary
   │                                         │
   │──── FullSyncRequest ──────────────────►│
   │     {standby_id}                        │
   │                                         │
   │◄─── FullSyncResponse (chunk 1) ────────│
   │     {entries[0..10000], has_more=true}  │
   │                                         │
   │◄─── FullSyncResponse (chunk 2) ────────│
   │     {entries[10001..20000], has_more=true}│
   │                                         │
   │           ... more chunks ...           │
   │                                         │
   │◄─── FullSyncResponse (final) ──────────│
   │     {entries[N..M], has_more=false,     │
   │      final_seq_id=12345}                │
   │                                         │
   │──── Start OpLog sync from 12345 ──────►│
   │                                         │
```

---

## 4. Configuration

```cpp
struct HotStandbyConfig {
    // OpLog settings
    size_t oplog_max_entries = 100000;
    size_t oplog_max_bytes = 256 * 1024 * 1024;  // 256MB
    
    // Replication settings
    uint32_t replication_batch_size = 100;
    uint32_t replication_batch_timeout_ms = 10;
    uint32_t replication_connect_timeout_ms = 5000;
    uint32_t replication_retry_interval_ms = 1000;
    
    // Verification settings
    uint32_t verify_interval_sec = 30;
    float verify_sample_ratio = 0.1;
    uint32_t verify_max_entries_per_shard = 100;
    uint32_t verify_max_mismatches_for_repair = 10;
    uint64_t verify_seq_diff_threshold = 1000;
    
    // Promotion settings
    uint64_t max_lag_for_promotion_ms = 5000;
    uint64_t max_lag_entries_for_promotion = 100;
    
    // Network settings
    std::string primary_address;  // For Standby to connect
    uint16_t replication_port = 12346;
};
```

---

## 5. Metrics & Monitoring

### 5.1 Key Metrics

| Metric | Description | Alert Threshold |
|--------|-------------|-----------------|
| `oplog_sequence_id` | Current OpLog sequence | - |
| `oplog_buffer_size` | OpLog buffer entry count | > 80% of max |
| `oplog_buffer_bytes` | OpLog buffer memory usage | > 80% of max |
| `replication_lag_entries` | Entries behind Primary | > 1000 |
| `replication_lag_ms` | Time behind Primary | > 5000ms |
| `verification_mismatches` | Mismatches found | > 0 |
| `verification_duration_ms` | Verification round time | > 10000ms |
| `standby_count` | Connected Standbys | < expected |

### 5.2 Health Checks

```cpp
struct HealthStatus {
    bool is_leader;
    uint64_t current_seq_id;
    
    // Primary only
    std::vector<StandbyHealth> standbys;
    
    // Standby only
    uint64_t applied_seq_id;
    uint64_t lag_entries;
    std::chrono::milliseconds lag_time;
    bool verification_healthy;
};
```

---

## 6. Error Handling

### 6.1 Replication Errors

| Error | Handling |
|-------|----------|
| Connection lost | Reconnect with exponential backoff |
| Seq gap detected | Request missing entries or full sync |
| Apply failure | Log error, skip entry, trigger verification |
| Checksum mismatch | Reject entry, request retransmit |

### 6.2 Verification Errors

| Error | Handling |
|-------|----------|
| Minor mismatches (< threshold) | Auto-repair using Primary data |
| Major mismatches (>= threshold) | Trigger full sync |
| Primary unreachable | Skip round, retry next interval |

### 6.3 Promotion Errors

| Error | Handling |
|-------|----------|
| Lag too high | Decline promotion, retry election |
| Metadata inconsistent | Decline promotion, trigger full sync |
| etcd update failed | Retry with backoff |

---

## 7. Implementation Plan

### Phase 1: Core Infrastructure (2 weeks)
- [ ] OpLogEntry data structure
- [ ] OpLogManager implementation
- [ ] Serialization/deserialization

### Phase 2: Replication Service (3 weeks)
- [ ] ReplicationService on Primary
- [ ] HotStandbyService on Standby
- [ ] OpLogApplier implementation
- [ ] gRPC streaming interface

### Phase 3: Verification Mechanism (2 weeks)
- [ ] VerificationClient implementation
- [ ] VerificationHandler implementation
- [ ] Auto-repair logic
- [ ] Full sync service

### Phase 4: Integration & Testing (2 weeks)
- [ ] Integrate with MasterServiceSupervisor
- [ ] Failover testing
- [ ] Performance testing
- [ ] Chaos testing

---

## 8. Appendix

### 8.1 PlantUML Diagrams

See `doc/en/diagrams/hot-standby-only/` for detailed architecture diagrams:
- `architecture.puml` - Overall architecture
- `replication-flow.puml` - OpLog replication flow
- `verification-flow.puml` - Verification mechanism
- `failover-sequence.puml` - Failover process
- `state-machine.puml` - Node state machine

### 8.2 Related Documents

- RFC: Master Service HA (original etcd-based election)
- RFC: Hybrid Mode (Hot Standby + Snapshot)

