**Store 内存管理（流程说明）**

概述：
- 内存由 Store 端的 `BufferAllocator`（例如 `CachelibBufferAllocator` 或 `OffsetBufferAllocator`）分配并拥有。
- 客户端通过向 Master 发起 `PutStart` 请求获得副本描述符（`Replica::Descriptor`），并把数据写入到这些 descriptor 指定的地址。
- Master 持有元数据（`ObjectMetadata` / `Replica`），当 Master 删除对应的 `Replica` 或 `ObjectMetadata` 时，`AllocatedBuffer` 会析构并调用 allocator 的 `deallocate`，由 allocator 在本地释放内存。

流程语言描述（逐步）
1. 客户端计算 object key（例如 `prefix_hash`），准备数据分片（slices）。
2. 客户端调用 RPC：`PutStart(key, slice_lengths, config)`。
   - Master 调用 `AllocationStrategy::Allocate`，使用 `SegmentManager` 提供的 `allocators` 列表为每个 slice 分配 `AllocatedBuffer`。
   - allocator 返回 `std::unique_ptr<AllocatedBuffer>`，Master 把其封装为 `Replica`（状态 `PROCESSING`），插入 `metadata_shards_`。
   - Master 返回 `Replica::Descriptor` 给客户端，包含 `transport_endpoint`, `buffer_address`, `size`。
3. 客户端根据 descriptor 选择传输策略：
   - 若为本地：使用本地 `memcpy` 把数据写到 `buffer_address`。
   - 否则：通过 `TransferEngine` 提交远端写请求（openSegment + submitTransfer）。
4. 写入成功后，客户端调用 `PutEnd(key, ReplicaType::MEMORY)`。
   - Master 标记相应 replica 为 `COMPLETE`，并调用 `GrantLease`（设置 lease/soft-pin）。
5. 其它客户端可通过 `GetReplicaList(key)` 获取 descriptors 并读取；Master 会在读取时延长 lease，保证短期内不被回收。
6. 当系统触发回收（例如 `BatchEvict`），Master 在其元数据中删除对应 `Replica` 或 `ObjectMetadata`：
   - `Replica` 包含 `unique_ptr<AllocatedBuffer>`，析构后会调用 `AllocatedBuffer::~AllocatedBuffer()`。
   - `AllocatedBuffer::~AllocatedBuffer()` 通过 `allocator_.lock()` 拿到 `shared_ptr<BufferAllocatorBase>`，并调用 `allocator->deallocate(this)`；allocator 完成底层内存回收与度量更新。

关键责任与边界：
- 写入：由客户端发起写请求，实际写入目标是 Store 的内存（allocator 分配的区域）。
- 所有权与释放：内存所有权在 allocator（Store）；Master 通过删除元数据间接触发释放。
- 协议保障：客户端必须通过 Master 的 lease/heartbeat 协议在读写期间保持 lease，否则可能被驱逐并导致访问已释放内存的风险。

推荐观测与改进点：
- 在 `BatchEvict` 与 `EraseReplica` 处记录被释放对象的 key/size/segment，便于追查问题。
- 在 `TransferSubmitter` 写入前验证 descriptor（endpoint/size），写失败时自动重试 `GetReplicaList`。
- 若需要更强保证，可实现 Master -> Client 的 `NotifyEvicted(key)` 回调协议（需权衡复杂度）。

更多细节请参见 `docs/diagrams/store_memory_flow.puml` 与 `docs/diagrams/store_memory_seq.puml`。
