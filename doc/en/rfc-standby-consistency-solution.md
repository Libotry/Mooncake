# Standby Master Data Consistency Solution Design

## Solution Overview

This solution aims to solve the data consistency problem between Standby Master and Primary Master in Hot Standby mode, specifically:
1. **Delete Event Synchronization**: Ensure explicit delete operations are correctly applied on Standby
2. **Lease Renewal Synchronization**: Ensure Standby can detect lease renewals to avoid false evictions
3. **Eviction Strategy Consistency**: Standby independently executes eviction, consistent with Primary
4. **Cleanup on Standby-to-Primary Promotion**: Execute one-time cleanup when promoting to Primary to ensure data consistency

---

## Core Design Principles

### 1. Delete Events Written to etcd

- **Explicit Delete Events**: `Remove`, `RemoveByRegex`, `RemoveAll`, `PutRevoke` (leading to full object deletion), `UnmountSegment` (leading to full object deletion)
- **No Eviction Events**: Automatic evictions due to lease expiration are NOT written to etcd
- **Reason**: Eviction events are extremely frequent (up to 130,000 per second), writing to etcd would create a performance bottleneck

### 2. Batch Lease Renewal Synchronization

- **Periodic Scanning**: Primary periodically scans (e.g., every 1 second) which objects have had their leases renewed
- **Batch Write to etcd**: Batch write renewal information to etcd to reduce write frequency
- **Standby Synchronization**: Standby obtains renewal information via Watch etcd and updates local metadata

### 3. Standby Independently Executes Eviction

- **Based on Local Metadata**: Standby independently determines eviction based on local metadata's `lease_timeout`
- **Consistent with Primary**: Uses the same eviction logic and strategy
- **No Dependency on Primary**: Does not require Primary to synchronize eviction events

### 4. One-Time Cleanup on Standby-to-Primary Promotion

- **Trigger**: When Standby is promoted to Primary (`PromoteToLeader`)
- **Cleanup Scope**: All metadata with expired leases
- **Purpose**: Ensure new Primary's data state is consistent with old Primary

---

## Detailed Design

### 1. Delete Event Synchronization Mechanism

#### 1.1 Delete Event Types

```cpp
enum class DeleteEventType {
    REMOVE,              // Explicit Remove operation
    REMOVE_BY_REGEX,     // RemoveByRegex operation
    REMOVE_ALL,          // RemoveAll operation
    PUT_REVOKE,          // PutRevoke leading to full object deletion
    UNMOUNT_SEGMENT      // UnmountSegment leading to full object deletion
};

struct DeleteEvent {
    DeleteEventType type;
    std::string key;                    // Object key (may be empty for REMOVE_BY_REGEX and REMOVE_ALL)
    std::string regex_pattern;          // Regex pattern (only for REMOVE_BY_REGEX)
    std::chrono::steady_clock::time_point timestamp;
    uint64_t version;                   // Event version number (for deduplication)
};
```

#### 1.2 Primary Side: Write Delete Events

```cpp
class MasterService {
private:
    void WriteDeleteEventToEtcd(const DeleteEvent& event) {
        std::string etcd_key = BuildDeleteEventKey(event.key, event.version);
        std::string etcd_value = SerializeDeleteEvent(event);
        
        // Write to etcd with TTL (e.g., 60 seconds)
        EtcdHelper::PutWithTTL(etcd_key, etcd_value, 60);
    }
    
public:
    auto Remove(const std::string& key) -> tl::expected<void, ErrorCode> {
        // ... Execute deletion logic ...
        
        // Write Delete event to etcd
        DeleteEvent event;
        event.type = DeleteEventType::REMOVE;
        event.key = key;
        event.timestamp = std::chrono::steady_clock::now();
        event.version = GetNextEventVersion();
        WriteDeleteEventToEtcd(event);
        
        return {};
    }
    
    auto RemoveByRegex(const std::string& regex_pattern) -> ... {
        // ... Execute deletion logic ...
        
        // Write Delete event for each matched object
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
        // ... Execute deletion logic ...
        
        // Write Delete event for each deleted object
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

#### 1.3 Standby Side: Receive Delete Events

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
        
        // Apply Delete event to local metadata
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

### 2. Batch Lease Renewal Synchronization Mechanism

#### 2.1 Data Structure Extension

```cpp
struct ObjectMetadata {
    std::vector<Replica> replicas;
    size_t size;
    std::chrono::steady_clock::time_point lease_timeout;
    std::optional<std::chrono::steady_clock::time_point> soft_pin_timeout;
    
    // New: Mark if lease has been renewed in this cycle
    bool lease_renewed_since_last_sync{false};
    std::chrono::steady_clock::time_point last_lease_renew_time;
};
```

#### 2.2 Primary Side: Batch Scan and Write

```cpp
class LeaseRenewalSyncService {
private:
    std::thread sync_thread_;
    std::atomic<bool> running_{false};
    static constexpr uint64_t kSyncIntervalMs = 1000;  // Scan every 1 second
    
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
        
        // Scan all metadata to find objects renewed in this cycle
        for (auto& shard : master_service_->metadata_shards_) {
            MutexLocker lock(&shard.mutex);
            
            for (auto& [key, metadata] : shard.metadata) {
                // Check if lease has been renewed in this cycle
                if (metadata.lease_renewed_since_last_sync) {
                    LeaseRenewalEvent event;
                    event.key = key;
                    event.lease_timeout = metadata.lease_timeout;
                    event.soft_pin_timeout = metadata.soft_pin_timeout;
                    event.timestamp = metadata.last_lease_renew_time;
                    events.push_back(event);
                    
                    // Reset flag
                    metadata.lease_renewed_since_last_sync = false;
                }
            }
        }
        
        // Batch write to etcd
        if (!events.empty()) {
            BatchWriteToEtcd(events);
        }
    }
    
    void BatchWriteToEtcd(const std::vector<LeaseRenewalEvent>& events) {
        for (const auto& event : events) {
            std::string etcd_key = BuildLeaseRenewKey(event.key);
            std::string etcd_value = SerializeLeaseRenewal(event);
            
            // Write to etcd (with TTL, e.g., 60 seconds)
            EtcdHelper::PutWithTTL(etcd_key, etcd_value, 60);
        }
    }
};

// Modify GrantLease method
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
    
    // Mark for synchronization
    last_lease_renew_time = now;
    lease_renewed_since_last_sync = true;
}
```

#### 2.3 Standby Side: Receive Lease Renewal Events

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
        
        // Update local metadata
        auto& metadata = GetMetadata(renewal.key);
        if (metadata.Exists()) {
            metadata.lease_timeout = renewal.lease_timeout;
            metadata.soft_pin_timeout = renewal.soft_pin_timeout;
        }
    }
};
```

---

### 3. Standby Independently Executes Eviction

#### 3.1 Standby Eviction Logic

```cpp
class HotStandbyService {
private:
    std::thread eviction_thread_;
    std::atomic<bool> eviction_running_{false};
    static constexpr uint64_t kEvictionThreadSleepMs = 10;  // 10ms
    
    void EvictionThreadFunc() {
        while (eviction_running_) {
            // Execute eviction logic (same as Primary)
            BatchEvict();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kEvictionThreadSleepMs));
        }
    }
    
    void BatchEvict() {
        // Use the same eviction logic as Primary
        // Determine eviction based on local metadata's lease_timeout
        auto now = std::chrono::steady_clock::now();
        
        for (auto& shard : metadata_shards_) {
            MutexLocker lock(&shard.mutex);
            
            for (auto it = shard.metadata.begin(); 
                 it != shard.metadata.end();) {
                // Check if lease has expired
                if (it->second.IsLeaseExpired(now)) {
                    // Execute eviction (same logic as Primary)
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

#### 3.2 Eviction Strategy Consistency

- **Same Judgment Condition**: `IsLeaseExpired(now)`
- **Same Eviction Logic**: `EraseReplica(ReplicaType::MEMORY)`
- **Same Soft Pin Handling**: Consider `IsSoftPinned(now)` and `allow_evict_soft_pinned_objects_`

---

### 4. One-Time Cleanup on Standby-to-Primary Promotion

#### 4.1 PromoteToLeader Implementation

```cpp
class HotStandbyService {
public:
    void PromoteToLeader() {
        // 1. Stop Standby services
        StopStandbyServices();
        
        // 2. Execute one-time cleanup: evict all metadata with expired leases
        CleanupExpiredMetadata();
        
        // 3. Convert Standby's metadata to Primary's MasterService
        PromoteMetadataToMasterService();
        
        // 4. Start Primary services
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
                // Check if lease has expired
                if (it->second.IsLeaseExpired(now)) {
                    // Statistics
                    total_freed_size += 
                        it->second.size * it->second.GetMemReplicaCount();
                    evicted_count++;
                    
                    // Execute eviction
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

## Data Flow Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                    Primary Master                           │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────┐      ┌──────────────┐                    │
│  │ Delete Event │─────▶│   etcd       │                    │
│  │   (immediate)│      │              │                    │
│  └──────────────┘      └──────────────┘                    │
│                                                              │
│  ┌──────────────┐      ┌──────────────┐                    │
│  │ Lease Renewal│─────▶│ Lease Renewal│                    │
│  │   (mark)      │      │ Sync Service │                    │
│  └──────────────┘      └──────┬───────┘                    │
│                                 │                            │
│                                 │ Batch scan (1s)            │
│                                 ▼                            │
│                          ┌──────────────┐                    │
│                          │   etcd       │                    │
│                          └──────────────┘                    │
│                                                              │
│  ┌──────────────┐                                           │
│  │   Eviction   │  (NOT written to etcd)                   │
│  │ (independent) │                                           │
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
│         │ Apply Delete event                                 │
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
│         │ Update lease_timeout                               │
│         ▼                                                    │
│  ┌──────────────┐                                           │
│  │   Metadata   │                                           │
│  └──────┬───────┘                                           │
│         │                                                    │
│         │ Judge based on lease_timeout                       │
│         ▼                                                    │
│  ┌──────────────┐                                           │
│  │   Eviction   │  (independent, same logic as Primary)      │
│  │ (independent) │                                           │
│  └──────────────┘                                           │
│                                                              │
│  ┌──────────────┐                                           │
│  │ PromoteTo    │  (one-time cleanup on promotion)         │
│  │   Leader     │                                           │
│  └──────────────┘                                           │
└─────────────────────────────────────────────────────────────┘
```

---

## Key Design Decisions

### 1. Why Write Delete Events to etcd, But Not Eviction Events?

**Reasons**:
- **Delete Events Are Infrequent**: Explicit delete operations are far less frequent than eviction events
- **Eviction Events Are Extremely Frequent**: Up to 130,000 per second, writing to etcd would create a performance bottleneck
- **Standby Can Independently Judge**: Standby can independently determine eviction based on local metadata's `lease_timeout`

### 2. Why Batch Synchronize Lease Renewals?

**Reasons**:
- **Reduce etcd Write Frequency**: From < 15,000 per second to < 1,000 per second
- **Improve Efficiency**: Batch writes are more efficient than individual writes
- **Tolerate Delay**: 1 second delay has minimal impact on lease renewals

### 3. Why Should Standby Independently Execute Eviction?

**Reasons**:
- **Performance Consideration**: No need to synchronize large numbers of eviction events
- **Logic Consistency**: Use the same eviction logic to ensure consistency
- **Real-time**: Standby can execute eviction in real-time without waiting for Primary synchronization

### 4. Why Execute One-Time Cleanup on Standby-to-Primary Promotion?

**Reasons**:
- **Data Consistency**: Ensure new Primary's data state is consistent with old Primary
- **Clean Expired Data**: Clean all metadata with expired leases
- **Avoid Misuse**: Prevent new Primary from returning expired but uncleaned metadata

---

## Implementation Steps

### Phase 1: Delete Event Synchronization

1. Define `DeleteEvent` data structure
2. Write Delete events to etcd in `Remove`, `RemoveByRegex`, `RemoveAll` operations
3. Implement Delete event Watcher on Standby side
4. Apply Delete events to Standby's metadata

### Phase 2: Batch Lease Renewal Synchronization

1. Add `lease_renewed_since_last_sync` flag to `ObjectMetadata`
2. Modify `GrantLease` method to mark renewals
3. Implement `LeaseRenewalSyncService` for periodic scanning and batch writing
4. Implement Lease Renewal Watcher on Standby side
5. Update Standby's metadata `lease_timeout`

### Phase 3: Standby Independently Executes Eviction

1. Implement eviction thread in `HotStandbyService`
2. Use the same eviction logic as Primary
3. Determine eviction based on local metadata's `lease_timeout`

### Phase 4: One-Time Cleanup on Standby-to-Primary Promotion

1. Implement `CleanupExpiredMetadata` in `PromoteToLeader`
2. Clean all metadata with expired leases
3. Record cleanup statistics

---

## Performance Considerations

### 1. etcd Write Frequency

| Event Type | Frequency | Write Method |
|-----------|-----------|--------------|
| **Delete Events** | < 1,000/sec | Immediate write |
| **Lease Renewals** | < 15,000/sec | Batch write (< 1,000/sec) |
| **Eviction Events** | 130,000/sec | **NOT written** |

### 2. etcd Capacity Management

- **TTL Mechanism**: All events have TTL (e.g., 60 seconds) for automatic cleanup
- **Periodic Cleanup**: Optional, periodically clean expired events

### 3. Network Overhead

- **Batch Writes**: Reduce network round trips
- **Watch Mechanism**: Use etcd Watch to reduce polling overhead

---

## Fault Tolerance and Consistency Guarantees

### 1. Delete Event Loss

- **Impact**: Standby may retain deleted objects
- **Mitigation**: Execute one-time cleanup on promotion, clean all expired objects
- **Eventual Consistency**: Guarantee eventual consistency through periodic cleanup

### 2. Lease Renewal Delay

- **Impact**: Standby may falsely evict renewed objects
- **Mitigation**:
  - Shorten scan period (e.g., 100ms)
  - Immediate write for critical renewals (lease remaining time < 1 second)

### 3. Clock Desynchronization

- **Impact**: Clock desynchronization between Standby and Primary may cause inconsistent judgments
- **Mitigation**: Use NTP to synchronize clocks, or use relative time

---

## Summary

This solution ensures data consistency between Standby Master and Primary Master through the following mechanisms:

1. ✅ **Delete Event Synchronization**: Explicit delete operations written to etcd, Standby synchronously applies
2. ✅ **Batch Lease Renewal Synchronization**: Periodically batch synchronize lease renewals to avoid false evictions
3. ✅ **Standby Independently Executes Eviction**: Independently judge based on local metadata, consistent with Primary logic
4. ✅ **One-Time Cleanup on Promotion**: Clean all expired metadata to ensure data consistency

This solution achieves a good balance between **performance**, **consistency**, and **implementation complexity**.

