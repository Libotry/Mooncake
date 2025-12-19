# Mooncake Store 功能概览

## 1. 核心数据操作

### 1.1 基本 CRUD 操作

| 操作 | 说明 | 接口 |
|------|------|------|
| **Put** | 写入对象（支持多副本） | `Client::Put()` |
| **Get** | 读取对象 | `Client::Get()` |
| **Remove** | 删除单个对象 | `Client::Remove()` |
| **RemoveByRegex** | 按正则表达式批量删除 | `Client::RemoveByRegex()` |
| **RemoveAll** | 删除所有对象 | `Client::RemoveAll()` |

### 1.2 查询操作

| 操作 | 说明 | 接口 |
|------|------|------|
| **ExistKey** | 检查对象是否存在 | `Client::IsExist()` |
| **BatchExistKey** | 批量检查存在性 | `Client::BatchIsExist()` |
| **Query** | 查询对象元数据（不传输数据） | `Client::Query()` |
| **BatchQuery** | 批量查询元数据 | `Client::BatchQuery()` |
| **QueryByRegex** | 正则表达式查询 | `Client::QueryByRegex()` |
| **GetAllKeys** | 获取所有键 | `MasterService::GetAllKeys()` |

### 1.3 批量操作

| 操作 | 说明 | 接口 |
|------|------|------|
| **BatchPut** | 批量写入 | `Client::BatchPut()` |
| **BatchGet** | 批量读取 | `Client::BatchGet()` |
| **BatchPutStart** | 批量开始 Put | `MasterClient::BatchPutStart()` |
| **BatchPutEnd** | 批量结束 Put | `MasterClient::BatchPutEnd()` |
| **BatchGetReplicaList** | 批量获取副本列表 | `MasterClient::BatchGetReplicaList()` |

---

## 2. Segment 管理

### 2.1 Segment 生命周期

| 操作 | 说明 | 接口 |
|------|------|------|
| **MountSegment** | 注册 Segment 到 Master | `MasterService::MountSegment()` |
| **ReMountSegment** | 重新注册 Segment（客户端重连时） | `MasterService::ReMountSegment()` |
| **UnmountSegment** | 注销 Segment | `MasterService::UnmountSegment()` |
| **GetAllSegments** | 获取所有 Segment 列表 | `MasterService::GetAllSegments()` |
| **QuerySegments** | 查询 Segment 容量和使用情况 | `MasterService::QuerySegments()` |

### 2.2 Segment 信息

- **容量查询**：获取 Segment 的总容量和已使用容量
- **状态管理**：跟踪 Segment 的挂载/卸载状态
- **自动清理**：Segment 卸载时自动清理相关对象的元数据

---

## 3. 副本管理

### 3.1 副本配置

```cpp
struct ReplicateConfig {
    size_t replica_num{1};              // 副本数量
    bool with_soft_pin{false};         // 是否启用软固定
    std::string preferred_segment{};    // 首选 Segment
    bool prefer_alloc_in_same_node{false}; // 是否优先在同一节点分配
};
```

### 3.2 副本特性

- **多副本支持**：同一对象可存储多个副本
- **Slice 级分布**：保证同一对象的每个 slice 在不同 Segment
- **尽力而为分配**：空间不足时分配尽可能多的副本
- **副本状态管理**：INITIALIZED → PROCESSING → COMPLETE

---

## 4. Lease 租约机制

### 4.1 Lease 功能

| 功能 | 说明 |
|------|------|
| **GrantLease** | 授予对象租约，防止被驱逐 |
| **Lease 过期检查** | 定期检查租约是否过期 |
| **Lease TTL** | 可配置的租约生存时间 |
| **Lease 续期** | 通过 PutEnd 自动续期 |

### 4.2 Lease 配置

- `default_kv_lease_ttl`：默认租约 TTL
- 对象在 Lease 期间不会被驱逐
- Lease 过期后对象可被驱逐

---

## 5. Soft Pin 软固定机制

### 5.1 Soft Pin 功能

| 功能 | 说明 |
|------|------|
| **软固定对象** | 标记对象为软固定，降低被驱逐优先级 |
| **Soft Pin TTL** | 软固定的生存时间 |
| **可配置驱逐** | 可配置是否允许驱逐软固定对象 |

### 5.2 使用场景

- 保护重要但非关键的数据
- 在内存紧张时仍可被驱逐
- 比普通对象有更高的保留优先级

---

## 6. 驱逐策略（Eviction）

### 6.1 驱逐机制

| 特性 | 说明 |
|------|------|
| **自动驱逐** | 定期检查内存使用率，自动触发驱逐 |
| **LRU 策略** | 优先驱逐 Lease 过期时间最早的对象 |
| **两阶段驱逐** | 先驱逐无 Soft Pin 对象，再考虑 Soft Pin 对象 |
| **可配置比例** | 支持配置驱逐比例和水位线 |

### 6.2 驱逐配置

- `eviction_ratio`：驱逐比例（默认 5%）
- `eviction_high_watermark_ratio`：高水位线（默认 95%）
- `allow_evict_soft_pinned_objects`：是否允许驱逐软固定对象

### 6.3 驱逐流程

```
内存使用率 > 高水位线
    ↓
触发批量驱逐
    ↓
第一轮：驱逐无 Soft Pin 且 Lease 过期的对象
    ↓
第二轮：如果未达到目标，驱逐 Soft Pin 对象（如果允许）
    ↓
更新元数据，释放内存
```

---

## 7. Put 操作管理

### 7.1 Put 操作状态

| 状态 | 说明 |
|------|------|
| **PutStart** | 开始 Put，分配空间 |
| **PutEnd** | 完成 Put，标记为 COMPLETE |
| **PutRevoke** | 撤销 Put，释放已分配空间 |

### 7.2 PutRevoke 使用场景

- 写入失败时撤销已分配的空间
- 磁盘存储失败时撤销磁盘副本
- 批量操作中部分失败时的回滚

---

## 8. 持久化支持

### 8.1 磁盘存储

| 功能 | 说明 |
|------|------|
| **Disk Replica** | 支持磁盘副本存储 |
| **异步持久化** | Put 时异步写入 SSD |
| **持久化回退** | Get 时如果内存中不存在，从 SSD 加载 |
| **文件管理** | 自动管理持久化文件路径 |

### 8.2 持久化配置

- `root_fs_dir`：持久化根目录
- `cluster_id`：集群 ID（用于目录隔离）
- 支持多级目录结构（基于 key hash）

---

## 9. 高可用（HA）支持

### 9.1 HA 功能

| 功能 | 说明 |
|------|------|
| **Leader 选举** | 基于 etcd 的 Leader 选举 |
| **故障检测** | 通过 Lease TTL 检测 Leader 故障 |
| **自动切换** | Leader 故障时自动选举新 Leader |
| **客户端重连** | 客户端自动发现新 Leader |

### 9.2 HA 模式

- **默认模式**：单 Master，简单但存在单点故障
- **HA 模式**：多 Master 集群，通过 etcd 协调

---

## 10. 客户端监控

### 10.1 监控功能

| 功能 | 说明 |
|------|------|
| **Ping 心跳** | 客户端定期发送心跳 |
| **健康检查** | Master 监控客户端健康状态 |
| **自动清理** | 客户端故障时自动清理相关元数据 |
| **重连支持** | 客户端恢复后自动重新注册 |

### 10.2 监控机制

- `client_live_ttl_sec`：客户端存活 TTL
- 定期检查客户端 Ping 时间
- 超时后标记客户端为失效

---

## 11. 内存管理

### 11.1 本地内存管理

| 功能 | 说明 | 接口 |
|------|------|------|
| **RegisterLocalMemory** | 注册本地内存到 Transfer Engine | `Client::RegisterLocalMemory()` |
| **UnregisterLocalMemory** | 注销本地内存 | `Client::unregisterLocalMemory()` |

### 11.2 内存分配器

- **CachelibBufferAllocator**：基于 Facebook CacheLib
- **OffsetBufferAllocator**：基于偏移量的分配器
- **弱引用机制**：通过 `weak_ptr` 自动检测失效

---

## 12. 传输优化

### 12.1 传输策略

| 策略 | 说明 | 使用场景 |
|------|------|----------|
| **LOCAL_MEMCPY** | 本地内存拷贝 | 源和目标在同一进程 |
| **TRANSFER_ENGINE** | 远程传输 | 跨进程/跨节点传输 |
| **FILE_READ** | 文件读取 | 从 SSD 读取 |

### 12.2 传输协议

- **RDMA**：零拷贝，低延迟，高带宽
- **TCP**：标准网络协议，兼容性好
- **自动选择**：根据环境自动选择最佳协议

### 12.3 传输优化

- **条带化传输**：大对象分片并行传输
- **多网卡聚合**：充分利用多网卡带宽
- **批量传输**：支持批量操作优化

---

## 13. 元数据管理

### 13.1 元数据结构

```cpp
struct ObjectMetadata {
    uint64_t size;                      // 对象大小
    std::vector<Replica> replicas;      // 副本列表
    std::chrono::steady_clock::time_point lease_timeout;  // 租约过期时间
    std::optional<std::chrono::steady_clock::time_point> soft_pin_timeout;  // 软固定过期时间
};
```

### 13.2 元数据操作

- **分片存储**：元数据按 key 分片存储，提高并发性能
- **版本管理**：支持对象版本号（用于一致性检查）
- **状态跟踪**：跟踪 Replica 状态（INITIALIZED → COMPLETE）

---

## 14. 指标和监控

### 14.1 性能指标

| 指标类型 | 说明 |
|---------|------|
| **RPC 指标** | PutStart、PutEnd、GetReplicaList 的延迟和成功率 |
| **传输指标** | 传输延迟、带宽利用率 |
| **驱逐指标** | 驱逐次数、释放的内存大小 |
| **客户端指标** | 活跃客户端数、Segment 容量使用率 |

### 14.2 指标接口

- `MasterMetricManager`：Master 端指标管理
- `ClientMetric`：Client 端指标收集
- 支持 Prometheus 格式导出

---

## 15. 正则表达式支持

### 15.1 正则表达式操作

| 操作 | 说明 | 接口 |
|------|------|------|
| **QueryByRegex** | 正则表达式查询对象 | `Client::QueryByRegex()` |
| **GetReplicaListByRegex** | 正则表达式获取副本列表 | `MasterService::GetReplicaListByRegex()` |
| **RemoveByRegex** | 正则表达式批量删除 | `Client::RemoveByRegex()` |

### 15.2 使用场景

- 批量管理：按模式批量操作对象
- 数据清理：清理特定模式的数据
- 查询分析：查找匹配特定模式的对象

---

## 16. 错误处理和容错

### 16.1 错误类型

| 错误码 | 说明 |
|--------|------|
| `OBJECT_NOT_FOUND` | 对象不存在 |
| `OBJECT_ALREADY_EXISTS` | 对象已存在 |
| `NO_AVAILABLE_HANDLE` | 无可用空间 |
| `LEASE_EXPIRED` | 租约过期 |
| `REPLICA_IS_NOT_READY` | 副本未就绪 |
| `TRANSFER_FAIL` | 传输失败 |

### 16.2 容错机制

- **自动重试**：传输失败时自动重试其他副本
- **优雅降级**：空间不足时分配部分副本
- **失效检测**：自动检测并清理失效的 Replica

---

## 17. 配置管理

### 17.1 Master 配置

```cpp
struct MasterServiceConfig {
    uint64_t default_kv_lease_ttl;              // 默认租约 TTL
    uint64_t default_kv_soft_pin_ttl;          // 默认软固定 TTL
    bool allow_evict_soft_pinned_objects;      // 是否允许驱逐软固定对象
    double eviction_ratio;                      // 驱逐比例
    double eviction_high_watermark_ratio;       // 高水位线
    std::string root_fs_dir;                    // 持久化根目录
    std::string cluster_id;                     // 集群 ID
};
```

### 17.2 Client 配置

- `local_hostname`：本地主机名
- `metadata_connstring`：元数据服务连接字符串
- `protocol`：传输协议（rdma/tcp）
- `master_server_entry`：Master 服务地址

---

## 18. 集成支持

### 18.1 LLM 推理系统集成

- **vLLM**：支持 prefill-decode disaggregation
- **SGLang**：支持 Hierarchical KV Caching
- **LMCache**：KV Cache 管理增强

### 18.2 Python 绑定

- 提供 Python API
- 支持与 Python 应用集成
- 支持 NumPy 数组直接传输

---

## 19. 特殊功能

### 19.1 条带化存储

- 大对象自动分片
- 每个 slice 存储在不同 Segment
- 并行传输提高性能

### 19.2 多网卡支持

- 自动检测可用网卡
- 负载均衡到多网卡
- 聚合带宽利用

### 19.3 动态资源管理

- 支持动态添加/删除节点
- 自动发现新 Segment
- 弹性扩容/缩容

---

## 20. 功能总结表

| 功能类别 | 功能数量 | 主要功能 |
|---------|---------|---------|
| **数据操作** | 8+ | Put, Get, Remove, Query, ExistKey 等 |
| **批量操作** | 5+ | BatchPut, BatchGet, BatchQuery 等 |
| **Segment 管理** | 5 | Mount, Unmount, Query 等 |
| **副本管理** | - | 多副本、副本配置、状态管理 |
| **租约机制** | - | Lease 授予、过期检查、续期 |
| **软固定** | - | Soft Pin、可配置驱逐 |
| **驱逐策略** | - | 自动驱逐、LRU、两阶段驱逐 |
| **持久化** | - | SSD 存储、异步持久化、回退加载 |
| **高可用** | - | Leader 选举、故障切换 |
| **监控** | - | 心跳、健康检查、指标收集 |
| **传输优化** | - | RDMA/TCP、条带化、多网卡 |
| **正则表达式** | 3 | QueryByRegex, RemoveByRegex 等 |

---

## 总结

Mooncake Store 除了基本的 Put/Get/传输功能外，还提供了：

1. **丰富的查询功能**：ExistKey、Query、正则表达式查询
2. **批量操作支持**：提高操作效率
3. **完善的资源管理**：Segment 管理、内存管理
4. **智能的缓存策略**：Lease、Soft Pin、驱逐策略
5. **持久化支持**：SSD 存储、自动回退
6. **高可用保障**：Leader 选举、故障切换
7. **完善的监控**：指标收集、健康检查
8. **传输优化**：多协议、多网卡、条带化

这些功能共同构成了一个完整的分布式 KV Cache 存储引擎，专为 LLM 推理场景优化。







