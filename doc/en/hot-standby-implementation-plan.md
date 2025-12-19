# Hot Standby Mode - Implementation Plan & Task Breakdown

## Document Information

| Item | Content |
|------|---------|
| Based On | RFC: Master Service Hot Standby Mode |
| Total Duration | 9 weeks |
| Team Size | 2-3 engineers |
| Start Date | TBD |

---

## Executive Summary

```
Timeline Overview:
Week 1-2:   ████████░░░░░░░░░░  Phase 1: Core Infrastructure
Week 3-5:   ░░░░░░░░████████████  Phase 2: Replication Service  
Week 6-7:   ░░░░░░░░░░░░░░░░████  Phase 3: Verification Mechanism
Week 8-9:   ░░░░░░░░░░░░░░░░░░██  Phase 4: Integration & Testing

Legend: █ = Active development
```

---

## Phase 1: Core Infrastructure (Week 1-2)

### Overview
Build the foundational OpLog system that all other components depend on.

### Task 1.1: OpLog Data Structures
**Duration:** 3 days  
**Owner:** Engineer A  
**Dependencies:** None

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 1.1.1 | Define `OpType` enum (PUT_START, PUT_END, REMOVE, etc.) | 2h |
| 1.1.2 | Define `OpLogEntry` struct with all fields | 4h |
| 1.1.3 | Implement CRC32 checksum calculation | 4h |
| 1.1.4 | Implement prefix_hash calculation (first 8 bytes) | 2h |
| 1.1.5 | Add YLT_REFL serialization macros | 4h |
| 1.1.6 | Write unit tests for data structures | 8h |

**Deliverables:**
- `mooncake-store/include/oplog.h`
- `mooncake-store/src/oplog.cpp`
- `mooncake-store/tests/oplog_test.cpp`

**Acceptance Criteria:**
- [ ] All OpTypes defined and documented
- [ ] OpLogEntry serialization/deserialization works correctly
- [ ] Checksum calculation matches expected values
- [ ] Unit tests pass with >90% coverage

---

### Task 1.2: OpLog Manager Implementation
**Duration:** 4 days  
**Owner:** Engineer A  
**Dependencies:** Task 1.1

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 1.2.1 | Implement `OpLogManager` class skeleton | 4h |
| 1.2.2 | Implement `Append()` with sequence ID generation | 8h |
| 1.2.3 | Implement `GetEntriesSince()` for sync | 6h |
| 1.2.4 | Implement `TruncateBefore()` for memory management | 4h |
| 1.2.5 | Add thread-safety with shared_mutex | 4h |
| 1.2.6 | Implement buffer size limits (entry count & bytes) | 4h |
| 1.2.7 | Write unit tests | 8h |

**Deliverables:**
- `mooncake-store/include/oplog_manager.h`
- `mooncake-store/src/oplog_manager.cpp`
- `mooncake-store/tests/oplog_manager_test.cpp`

**Acceptance Criteria:**
- [ ] Append() generates monotonically increasing sequence IDs
- [ ] GetEntriesSince() returns correct entries
- [ ] Buffer respects size limits
- [ ] Thread-safe under concurrent access
- [ ] Unit tests pass

---

### Task 1.3: OpLog Integration with MasterService
**Duration:** 3 days  
**Owner:** Engineer B  
**Dependencies:** Task 1.2

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 1.3.1 | Add OpLogManager member to MasterService | 2h |
| 1.3.2 | Generate OpLog in `PutStart()` | 4h |
| 1.3.3 | Generate OpLog in `PutEnd()` | 3h |
| 1.3.4 | Generate OpLog in `PutRevoke()` | 2h |
| 1.3.5 | Generate OpLog in `Remove()` | 2h |
| 1.3.6 | Generate OpLog in `MountSegment()` | 3h |
| 1.3.7 | Generate OpLog in `UnmountSegment()` | 2h |
| 1.3.8 | Generate OpLog for eviction events | 4h |
| 1.3.9 | Write integration tests | 8h |

**Deliverables:**
- Modified `mooncake-store/src/master_service.cpp`
- `mooncake-store/tests/master_oplog_test.cpp`

**Acceptance Criteria:**
- [ ] Every write operation generates an OpLog entry
- [ ] OpLog entries contain correct payload
- [ ] No performance regression >5% on write path
- [ ] Integration tests pass

---

## Phase 2: Replication Service (Week 3-5)

### Overview
Implement the replication infrastructure for Primary-to-Standby data synchronization.

### Task 2.1: Replication RPC Interface
**Duration:** 3 days  
**Owner:** Engineer A  
**Dependencies:** Phase 1 complete

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 2.1.1 | Define `SyncOpLog` streaming RPC | 4h |
| 2.1.2 | Define `Verify` RPC | 4h |
| 2.1.3 | Define `GetStatus` RPC | 2h |
| 2.1.4 | Define `FullSync` RPC for bootstrap | 4h |
| 2.1.5 | Generate coro_rpc stubs | 4h |
| 2.1.6 | Write RPC interface tests | 6h |

**Deliverables:**
- `mooncake-store/include/replication_rpc.h`
- `mooncake-store/src/replication_rpc.cpp`

**Acceptance Criteria:**
- [ ] All RPC interfaces defined and compilable
- [ ] Streaming RPC works for OpLog sync
- [ ] Request/Response serialization correct

---

### Task 2.2: ReplicationService (Primary Side)
**Duration:** 5 days  
**Owner:** Engineer A  
**Dependencies:** Task 2.1

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 2.2.1 | Implement `ReplicationService` class | 4h |
| 2.2.2 | Implement Standby registration/unregistration | 6h |
| 2.2.3 | Implement `OnNewOpLog()` callback from OpLogManager | 4h |
| 2.2.4 | Implement batch broadcasting logic | 8h |
| 2.2.5 | Track per-Standby ACK state | 6h |
| 2.2.6 | Implement replication lag monitoring | 4h |
| 2.2.7 | Handle Standby disconnection gracefully | 4h |
| 2.2.8 | Write unit tests | 8h |

**Deliverables:**
- `mooncake-store/include/replication_service.h`
- `mooncake-store/src/replication_service.cpp`
- `mooncake-store/tests/replication_service_test.cpp`

**Acceptance Criteria:**
- [ ] Multiple Standbys can connect simultaneously
- [ ] OpLog entries broadcast within 10ms
- [ ] Batch size and timeout configurable
- [ ] Replication lag accurately tracked
- [ ] Graceful handling of disconnections

---

### Task 2.3: MetadataStore (Standby Side)
**Duration:** 3 days  
**Owner:** Engineer B  
**Dependencies:** None (can start in parallel)

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 2.3.1 | Extract metadata storage logic from MasterService | 8h |
| 2.3.2 | Create standalone `MetadataStore` class | 6h |
| 2.3.3 | Implement CRUD operations for metadata | 4h |
| 2.3.4 | Add iterator/sampling interface for verification | 4h |
| 2.3.5 | Write unit tests | 6h |

**Deliverables:**
- `mooncake-store/include/metadata_store.h`
- `mooncake-store/src/metadata_store.cpp`
- `mooncake-store/tests/metadata_store_test.cpp`

**Acceptance Criteria:**
- [ ] MetadataStore has same sharding as MasterService
- [ ] All CRUD operations work correctly
- [ ] Sampling interface returns representative data
- [ ] Thread-safe

---

### Task 2.4: OpLogApplier (Standby Side)
**Duration:** 4 days  
**Owner:** Engineer B  
**Dependencies:** Task 2.3

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 2.4.1 | Implement `OpLogApplier` class | 4h |
| 2.4.2 | Implement `ApplyPutStart()` | 4h |
| 2.4.3 | Implement `ApplyPutEnd()` | 3h |
| 2.4.4 | Implement `ApplyPutRevoke()` | 2h |
| 2.4.5 | Implement `ApplyRemove()` | 2h |
| 2.4.6 | Implement `ApplyMountSegment()` | 3h |
| 2.4.7 | Implement `ApplyUnmountSegment()` | 2h |
| 2.4.8 | Implement `ApplyEviction()` | 3h |
| 2.4.9 | Handle out-of-order entries (should not happen) | 4h |
| 2.4.10 | Write unit tests | 8h |

**Deliverables:**
- `mooncake-store/include/oplog_applier.h`
- `mooncake-store/src/oplog_applier.cpp`
- `mooncake-store/tests/oplog_applier_test.cpp`

**Acceptance Criteria:**
- [ ] All OpTypes correctly applied to MetadataStore
- [ ] Sequence order enforced
- [ ] Idempotent (re-applying same entry is safe)
- [ ] Error handling for corrupted entries

---

### Task 2.5: HotStandbyService (Standby Side)
**Duration:** 5 days  
**Owner:** Engineer B  
**Dependencies:** Tasks 2.3, 2.4

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 2.5.1 | Implement `HotStandbyService` class skeleton | 4h |
| 2.5.2 | Implement `Start()` - connect to Primary | 6h |
| 2.5.3 | Implement replication loop (receive & apply OpLog) | 8h |
| 2.5.4 | Implement reconnection with exponential backoff | 4h |
| 2.5.5 | Implement `GetSyncStatus()` | 4h |
| 2.5.6 | Implement `IsReadyForPromotion()` | 4h |
| 2.5.7 | Implement `Promote()` - transfer to MasterService | 8h |
| 2.5.8 | Write unit tests | 8h |

**Deliverables:**
- `mooncake-store/include/hot_standby_service.h`
- `mooncake-store/src/hot_standby_service.cpp`
- `mooncake-store/tests/hot_standby_service_test.cpp`

**Acceptance Criteria:**
- [ ] Connects to Primary and receives OpLog stream
- [ ] Reconnects automatically on disconnection
- [ ] Sync status accurately reported
- [ ] Promotion transfers all metadata correctly

---

## Phase 3: Verification Mechanism (Week 6-7)

### Overview
Implement the data consistency verification system.

### Task 3.1: Verification Data Structures
**Duration:** 2 days  
**Owner:** Engineer A  
**Dependencies:** Phase 2 complete

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 3.1.1 | Define `VerificationEntry` struct | 2h |
| 3.1.2 | Define `VerificationRequest` struct | 2h |
| 3.1.3 | Define `VerificationResponse` struct | 2h |
| 3.1.4 | Define `MismatchEntry` struct | 2h |
| 3.1.5 | Implement metadata checksum calculation | 6h |
| 3.1.6 | Write unit tests | 4h |

**Deliverables:**
- `mooncake-store/include/verification.h`
- `mooncake-store/src/verification.cpp`
- `mooncake-store/tests/verification_test.cpp`

**Acceptance Criteria:**
- [ ] Checksum excludes dynamic fields (lease_timeout, soft_pin_timeout)
- [ ] Same metadata produces same checksum on Primary and Standby
- [ ] Serialization works correctly

---

### Task 3.2: VerificationHandler (Primary Side)
**Duration:** 3 days  
**Owner:** Engineer A  
**Dependencies:** Task 3.1

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 3.2.1 | Implement `VerificationHandler` class | 4h |
| 3.2.2 | Implement prefix_hash lookup logic | 6h |
| 3.2.3 | Implement checksum comparison | 4h |
| 3.2.4 | Generate mismatch entries with correct data | 4h |
| 3.2.5 | Handle NEED_FULL_SYNC condition | 4h |
| 3.2.6 | Write unit tests | 6h |

**Deliverables:**
- `mooncake-store/include/verification_handler.h`
- `mooncake-store/src/verification_handler.cpp`
- `mooncake-store/tests/verification_handler_test.cpp`

**Acceptance Criteria:**
- [ ] Correctly identifies mismatches
- [ ] Returns correct data for repair
- [ ] Detects large seq_id gaps

---

### Task 3.3: VerificationClient (Standby Side)
**Duration:** 4 days  
**Owner:** Engineer B  
**Dependencies:** Task 3.1

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 3.3.1 | Implement `VerificationClient` class | 4h |
| 3.3.2 | Implement shard sampling logic (round-robin 10%) | 6h |
| 3.3.3 | Implement key sampling within shard (max 100) | 4h |
| 3.3.4 | Implement verification request building | 4h |
| 3.3.5 | Implement auto-repair for minor mismatches | 6h |
| 3.3.6 | Implement full sync trigger for major mismatches | 4h |
| 3.3.7 | Implement periodic verification loop | 4h |
| 3.3.8 | Write unit tests | 8h |

**Deliverables:**
- `mooncake-store/include/verification_client.h`
- `mooncake-store/src/verification_client.cpp`
- `mooncake-store/tests/verification_client_test.cpp`

**Acceptance Criteria:**
- [ ] Full verification cycle completes in 5 minutes
- [ ] Auto-repairs minor inconsistencies
- [ ] Triggers full sync when needed
- [ ] Configurable interval and sample ratio

---

### Task 3.4: Full Sync Service
**Duration:** 3 days  
**Owner:** Engineer A  
**Dependencies:** Tasks 3.2, 3.3

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 3.4.1 | Implement `FullSyncService` on Primary | 6h |
| 3.4.2 | Implement chunked metadata streaming | 6h |
| 3.4.3 | Implement `ReceiveFullDump()` on Standby | 6h |
| 3.4.4 | Handle checkpoint sequence ID | 4h |
| 3.4.5 | Write integration tests | 6h |

**Deliverables:**
- `mooncake-store/include/full_sync_service.h`
- `mooncake-store/src/full_sync_service.cpp`
- `mooncake-store/tests/full_sync_test.cpp`

**Acceptance Criteria:**
- [ ] Full sync completes for 1M entries in <60 seconds
- [ ] Chunked transfer handles large datasets
- [ ] Standby resumes OpLog sync from correct point

---

## Phase 4: Integration & Testing (Week 8-9)

### Overview
Integrate all components and perform comprehensive testing.

### Task 4.1: MasterServiceSupervisor Integration
**Duration:** 4 days  
**Owner:** Engineer A + B  
**Dependencies:** Phases 1-3 complete

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 4.1.1 | Modify Supervisor to detect Leader/Follower mode | 6h |
| 4.1.2 | Start ReplicationService when becoming Leader | 4h |
| 4.1.3 | Start HotStandbyService when becoming Follower | 4h |
| 4.1.4 | Implement promotion flow (Follower→Leader) | 8h |
| 4.1.5 | Implement demotion flow (Leader→Follower) | 6h |
| 4.1.6 | Add safety window logic | 4h |
| 4.1.7 | Write integration tests | 8h |

**Deliverables:**
- Modified `mooncake-store/src/ha_helper.cpp`
- `mooncake-store/tests/supervisor_integration_test.cpp`

**Acceptance Criteria:**
- [ ] Seamless Leader/Follower transitions
- [ ] Safety window prevents split-brain
- [ ] All metadata preserved during failover

---

### Task 4.2: Metrics & Monitoring
**Duration:** 2 days  
**Owner:** Engineer B  
**Dependencies:** Task 4.1

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 4.2.1 | Add OpLog metrics (sequence_id, buffer_size) | 4h |
| 4.2.2 | Add replication lag metrics | 4h |
| 4.2.3 | Add verification metrics | 4h |
| 4.2.4 | Implement health check endpoint | 4h |

**Deliverables:**
- Metrics exposed via existing metrics system
- Health check endpoint documentation

**Acceptance Criteria:**
- [ ] All key metrics exposed
- [ ] Alerting thresholds documented

---

### Task 4.3: Failover Testing
**Duration:** 3 days  
**Owner:** Engineer A  
**Dependencies:** Task 4.1

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 4.3.1 | Test normal failover (clean shutdown) | 4h |
| 4.3.2 | Test crash failover (kill -9) | 4h |
| 4.3.3 | Test network partition scenarios | 6h |
| 4.3.4 | Measure RTO and RPO | 4h |
| 4.3.5 | Document test results | 4h |

**Deliverables:**
- `mooncake-store/tests/e2e/failover_test.cpp`
- Test result report

**Acceptance Criteria:**
- [ ] RTO < 10 seconds
- [ ] RPO < 1 second
- [ ] No split-brain in any scenario

---

### Task 4.4: Performance Testing
**Duration:** 2 days  
**Owner:** Engineer B  
**Dependencies:** Task 4.1

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 4.4.1 | Benchmark write latency with replication | 4h |
| 4.4.2 | Benchmark replication throughput | 4h |
| 4.4.3 | Benchmark verification overhead | 4h |
| 4.4.4 | Compare with baseline (no replication) | 4h |

**Deliverables:**
- Performance benchmark results
- Optimization recommendations

**Acceptance Criteria:**
- [ ] Write latency increase < 5%
- [ ] Can handle 10K writes/sec with replication
- [ ] Verification does not impact normal operations

---

### Task 4.5: Chaos Testing
**Duration:** 3 days  
**Owner:** Engineer A + B  
**Dependencies:** Tasks 4.3, 4.4

| Sub-task | Description | Est. Hours |
|----------|-------------|------------|
| 4.5.1 | Random Primary kills | 4h |
| 4.5.2 | Random Standby kills | 4h |
| 4.5.3 | Network latency injection | 4h |
| 4.5.4 | Packet loss simulation | 4h |
| 4.5.5 | Combined chaos scenarios | 6h |
| 4.5.6 | Long-running stability test (24h) | 8h |

**Deliverables:**
- `mooncake-store/tests/e2e/chaos_hot_standby_test.cpp`
- Chaos test report

**Acceptance Criteria:**
- [ ] System recovers from all chaos scenarios
- [ ] No data loss or corruption
- [ ] 24h stability test passes

---

## Summary

### Timeline

```
Week    1    2    3    4    5    6    7    8    9
        ├────┼────┼────┼────┼────┼────┼────┼────┤
Phase 1 ████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░
Phase 2 ░░░░░░░░████████████████░░░░░░░░░░░░░░░░
Phase 3 ░░░░░░░░░░░░░░░░░░░░░░░░████████░░░░░░░░
Phase 4 ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░████████
```

### Resource Allocation

| Engineer | Phase 1 | Phase 2 | Phase 3 | Phase 4 |
|----------|---------|---------|---------|---------|
| Engineer A | OpLog, OpLogManager | RPC, ReplicationService | Verification Handler, Full Sync | Integration, Failover Test |
| Engineer B | Integration | MetadataStore, OpLogApplier, HotStandby | VerificationClient | Metrics, Perf Test, Chaos |

### Deliverables Summary

| Phase | New Files | Modified Files |
|-------|-----------|----------------|
| Phase 1 | 6 | 2 |
| Phase 2 | 10 | 1 |
| Phase 3 | 8 | 0 |
| Phase 4 | 4 | 2 |
| **Total** | **28** | **5** |

### Risk Mitigation

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Replication adds unacceptable latency | Medium | High | Async replication, batching optimization |
| Split-brain during failover | Low | Critical | Safety window, etcd fencing |
| Full sync too slow | Medium | Medium | Chunked transfer, parallel processing |
| Verification finds too many mismatches | Low | Medium | Root cause analysis, bug fixes |

### Definition of Done

- [ ] All unit tests pass (>90% coverage)
- [ ] All integration tests pass
- [ ] Performance benchmarks meet targets
- [ ] Chaos tests pass
- [ ] Documentation complete
- [ ] Code reviewed and merged








