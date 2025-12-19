# 热备模式 - 实施计划与任务拆解

## 文档信息

| 项目 | 内容 |
|------|------|
| 基于 | RFC: Master Service 热备模式 |
| 总工期 | 9 周 |
| 团队规模 | 2-3 名工程师 |
| 开始日期 | 待定 |

---

## 执行摘要

```
时间线概览:
第1-2周:   ████████░░░░░░░░░░  阶段1: 核心基础设施
第3-5周:   ░░░░░░░░████████████  阶段2: 复制服务  
第6-7周:   ░░░░░░░░░░░░░░░░████  阶段3: 核查机制
第8-9周:   ░░░░░░░░░░░░░░░░░░██  阶段4: 集成与测试

图例: █ = 活跃开发中
```

---

## 阶段 1: 核心基础设施 (第1-2周)

### 概述
构建所有其他组件依赖的基础 OpLog 系统。

### 任务 1.1: OpLog 数据结构
**工期:** 3 天  
**负责人:** 工程师 A  
**依赖:** 无

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 1.1.1 | 定义 `OpType` 枚举 (PUT_START, PUT_END, REMOVE 等) | 2h |
| 1.1.2 | 定义 `OpLogEntry` 结构体及所有字段 | 4h |
| 1.1.3 | 实现 CRC32 校验和计算 | 4h |
| 1.1.4 | 实现 prefix_hash 计算（前8字节） | 2h |
| 1.1.5 | 添加 YLT_REFL 序列化宏 | 4h |
| 1.1.6 | 编写数据结构单元测试 | 8h |

**交付物:**
- `mooncake-store/include/oplog.h`
- `mooncake-store/src/oplog.cpp`
- `mooncake-store/tests/oplog_test.cpp`

**验收标准:**
- [ ] 所有 OpType 已定义并文档化
- [ ] OpLogEntry 序列化/反序列化正确
- [ ] 校验和计算与预期值匹配
- [ ] 单元测试通过，覆盖率 >90%

---

### 任务 1.2: OpLog 管理器实现
**工期:** 4 天  
**负责人:** 工程师 A  
**依赖:** 任务 1.1

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 1.2.1 | 实现 `OpLogManager` 类骨架 | 4h |
| 1.2.2 | 实现 `Append()` 及序列号生成 | 8h |
| 1.2.3 | 实现 `GetEntriesSince()` 用于同步 | 6h |
| 1.2.4 | 实现 `TruncateBefore()` 用于内存管理 | 4h |
| 1.2.5 | 使用 shared_mutex 添加线程安全 | 4h |
| 1.2.6 | 实现缓冲区大小限制（条目数和字节数） | 4h |
| 1.2.7 | 编写单元测试 | 8h |

**交付物:**
- `mooncake-store/include/oplog_manager.h`
- `mooncake-store/src/oplog_manager.cpp`
- `mooncake-store/tests/oplog_manager_test.cpp`

**验收标准:**
- [ ] Append() 生成单调递增的序列号
- [ ] GetEntriesSince() 返回正确的条目
- [ ] 缓冲区遵守大小限制
- [ ] 并发访问下线程安全
- [ ] 单元测试通过

---

### 任务 1.3: OpLog 与 MasterService 集成
**工期:** 3 天  
**负责人:** 工程师 B  
**依赖:** 任务 1.2

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 1.3.1 | 在 MasterService 中添加 OpLogManager 成员 | 2h |
| 1.3.2 | 在 `PutStart()` 中生成 OpLog | 4h |
| 1.3.3 | 在 `PutEnd()` 中生成 OpLog | 3h |
| 1.3.4 | 在 `PutRevoke()` 中生成 OpLog | 2h |
| 1.3.5 | 在 `Remove()` 中生成 OpLog | 2h |
| 1.3.6 | 在 `MountSegment()` 中生成 OpLog | 3h |
| 1.3.7 | 在 `UnmountSegment()` 中生成 OpLog | 2h |
| 1.3.8 | 为驱逐事件生成 OpLog | 4h |
| 1.3.9 | 编写集成测试 | 8h |

**交付物:**
- 修改后的 `mooncake-store/src/master_service.cpp`
- `mooncake-store/tests/master_oplog_test.cpp`

**验收标准:**
- [ ] 每个写操作都生成 OpLog 条目
- [ ] OpLog 条目包含正确的 payload
- [ ] 写路径性能回退 <5%
- [ ] 集成测试通过

---

## 阶段 2: 复制服务 (第3-5周)

### 概述
实现 Primary 到 Standby 数据同步的复制基础设施。

### 任务 2.1: 复制 RPC 接口
**工期:** 3 天  
**负责人:** 工程师 A  
**依赖:** 阶段 1 完成

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 2.1.1 | 定义 `SyncOpLog` 流式 RPC | 4h |
| 2.1.2 | 定义 `Verify` RPC | 4h |
| 2.1.3 | 定义 `GetStatus` RPC | 2h |
| 2.1.4 | 定义 `FullSync` RPC 用于引导 | 4h |
| 2.1.5 | 生成 coro_rpc 存根 | 4h |
| 2.1.6 | 编写 RPC 接口测试 | 6h |

**交付物:**
- `mooncake-store/include/replication_rpc.h`
- `mooncake-store/src/replication_rpc.cpp`

**验收标准:**
- [ ] 所有 RPC 接口已定义且可编译
- [ ] 流式 RPC 用于 OpLog 同步正常工作
- [ ] 请求/响应序列化正确

---

### 任务 2.2: ReplicationService（Primary 端）
**工期:** 5 天  
**负责人:** 工程师 A  
**依赖:** 任务 2.1

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 2.2.1 | 实现 `ReplicationService` 类 | 4h |
| 2.2.2 | 实现 Standby 注册/注销 | 6h |
| 2.2.3 | 实现来自 OpLogManager 的 `OnNewOpLog()` 回调 | 4h |
| 2.2.4 | 实现批量广播逻辑 | 8h |
| 2.2.5 | 跟踪每个 Standby 的 ACK 状态 | 6h |
| 2.2.6 | 实现复制延迟监控 | 4h |
| 2.2.7 | 优雅处理 Standby 断连 | 4h |
| 2.2.8 | 编写单元测试 | 8h |

**交付物:**
- `mooncake-store/include/replication_service.h`
- `mooncake-store/src/replication_service.cpp`
- `mooncake-store/tests/replication_service_test.cpp`

**验收标准:**
- [ ] 多个 Standby 可同时连接
- [ ] OpLog 条目在 10ms 内广播
- [ ] 批次大小和超时可配置
- [ ] 复制延迟准确跟踪
- [ ] 优雅处理断连

---

### 任务 2.3: MetadataStore（Standby 端）
**工期:** 3 天  
**负责人:** 工程师 B  
**依赖:** 无（可并行开始）

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 2.3.1 | 从 MasterService 中提取元数据存储逻辑 | 8h |
| 2.3.2 | 创建独立的 `MetadataStore` 类 | 6h |
| 2.3.3 | 实现元数据的 CRUD 操作 | 4h |
| 2.3.4 | 添加用于核查的迭代器/采样接口 | 4h |
| 2.3.5 | 编写单元测试 | 6h |

**交付物:**
- `mooncake-store/include/metadata_store.h`
- `mooncake-store/src/metadata_store.cpp`
- `mooncake-store/tests/metadata_store_test.cpp`

**验收标准:**
- [ ] MetadataStore 与 MasterService 使用相同分片
- [ ] 所有 CRUD 操作正确工作
- [ ] 采样接口返回有代表性的数据
- [ ] 线程安全

---

### 任务 2.4: OpLogApplier（Standby 端）
**工期:** 4 天  
**负责人:** 工程师 B  
**依赖:** 任务 2.3

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 2.4.1 | 实现 `OpLogApplier` 类 | 4h |
| 2.4.2 | 实现 `ApplyPutStart()` | 4h |
| 2.4.3 | 实现 `ApplyPutEnd()` | 3h |
| 2.4.4 | 实现 `ApplyPutRevoke()` | 2h |
| 2.4.5 | 实现 `ApplyRemove()` | 2h |
| 2.4.6 | 实现 `ApplyMountSegment()` | 3h |
| 2.4.7 | 实现 `ApplyUnmountSegment()` | 2h |
| 2.4.8 | 实现 `ApplyEviction()` | 3h |
| 2.4.9 | 处理乱序条目（不应发生） | 4h |
| 2.4.10 | 编写单元测试 | 8h |

**交付物:**
- `mooncake-store/include/oplog_applier.h`
- `mooncake-store/src/oplog_applier.cpp`
- `mooncake-store/tests/oplog_applier_test.cpp`

**验收标准:**
- [ ] 所有 OpType 正确应用到 MetadataStore
- [ ] 强制执行序列顺序
- [ ] 幂等（重复应用同一条目是安全的）
- [ ] 损坏条目的错误处理

---

### 任务 2.5: HotStandbyService（Standby 端）
**工期:** 5 天  
**负责人:** 工程师 B  
**依赖:** 任务 2.3, 2.4

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 2.5.1 | 实现 `HotStandbyService` 类骨架 | 4h |
| 2.5.2 | 实现 `Start()` - 连接到 Primary | 6h |
| 2.5.3 | 实现复制循环（接收和应用 OpLog） | 8h |
| 2.5.4 | 实现指数退避重连 | 4h |
| 2.5.5 | 实现 `GetSyncStatus()` | 4h |
| 2.5.6 | 实现 `IsReadyForPromotion()` | 4h |
| 2.5.7 | 实现 `Promote()` - 转移到 MasterService | 8h |
| 2.5.8 | 编写单元测试 | 8h |

**交付物:**
- `mooncake-store/include/hot_standby_service.h`
- `mooncake-store/src/hot_standby_service.cpp`
- `mooncake-store/tests/hot_standby_service_test.cpp`

**验收标准:**
- [ ] 连接到 Primary 并接收 OpLog 流
- [ ] 断连时自动重连
- [ ] 同步状态准确报告
- [ ] 提升时正确转移所有元数据

---

## 阶段 3: 核查机制 (第6-7周)

### 概述
实现数据一致性验证系统。

### 任务 3.1: 核查数据结构
**工期:** 2 天  
**负责人:** 工程师 A  
**依赖:** 阶段 2 完成

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 3.1.1 | 定义 `VerificationEntry` 结构体 | 2h |
| 3.1.2 | 定义 `VerificationRequest` 结构体 | 2h |
| 3.1.3 | 定义 `VerificationResponse` 结构体 | 2h |
| 3.1.4 | 定义 `MismatchEntry` 结构体 | 2h |
| 3.1.5 | 实现元数据校验和计算 | 6h |
| 3.1.6 | 编写单元测试 | 4h |

**交付物:**
- `mooncake-store/include/verification.h`
- `mooncake-store/src/verification.cpp`
- `mooncake-store/tests/verification_test.cpp`

**验收标准:**
- [ ] 校验和排除动态字段（lease_timeout, soft_pin_timeout）
- [ ] 相同元数据在 Primary 和 Standby 产生相同校验和
- [ ] 序列化正确工作

---

### 任务 3.2: VerificationHandler（Primary 端）
**工期:** 3 天  
**负责人:** 工程师 A  
**依赖:** 任务 3.1

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 3.2.1 | 实现 `VerificationHandler` 类 | 4h |
| 3.2.2 | 实现 prefix_hash 查找逻辑 | 6h |
| 3.2.3 | 实现校验和比较 | 4h |
| 3.2.4 | 生成带有正确数据的不匹配条目 | 4h |
| 3.2.5 | 处理 NEED_FULL_SYNC 条件 | 4h |
| 3.2.6 | 编写单元测试 | 6h |

**交付物:**
- `mooncake-store/include/verification_handler.h`
- `mooncake-store/src/verification_handler.cpp`
- `mooncake-store/tests/verification_handler_test.cpp`

**验收标准:**
- [ ] 正确识别不匹配
- [ ] 返回用于修复的正确数据
- [ ] 检测大的 seq_id 差距

---

### 任务 3.3: VerificationClient（Standby 端）
**工期:** 4 天  
**负责人:** 工程师 B  
**依赖:** 任务 3.1

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 3.3.1 | 实现 `VerificationClient` 类 | 4h |
| 3.3.2 | 实现 shard 采样逻辑（轮询 10%） | 6h |
| 3.3.3 | 实现 shard 内 key 采样（最多 100 个） | 4h |
| 3.3.4 | 实现核查请求构建 | 4h |
| 3.3.5 | 实现少量不匹配的自动修复 | 6h |
| 3.3.6 | 实现大量不匹配的全量同步触发 | 4h |
| 3.3.7 | 实现定期核查循环 | 4h |
| 3.3.8 | 编写单元测试 | 8h |

**交付物:**
- `mooncake-store/include/verification_client.h`
- `mooncake-store/src/verification_client.cpp`
- `mooncake-store/tests/verification_client_test.cpp`

**验收标准:**
- [ ] 完整核查周期在 5 分钟内完成
- [ ] 自动修复少量不一致
- [ ] 需要时触发全量同步
- [ ] 间隔和采样率可配置

---

### 任务 3.4: 全量同步服务
**工期:** 3 天  
**负责人:** 工程师 A  
**依赖:** 任务 3.2, 3.3

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 3.4.1 | 在 Primary 上实现 `FullSyncService` | 6h |
| 3.4.2 | 实现分块元数据流式传输 | 6h |
| 3.4.3 | 在 Standby 上实现 `ReceiveFullDump()` | 6h |
| 3.4.4 | 处理检查点序列号 | 4h |
| 3.4.5 | 编写集成测试 | 6h |

**交付物:**
- `mooncake-store/include/full_sync_service.h`
- `mooncake-store/src/full_sync_service.cpp`
- `mooncake-store/tests/full_sync_test.cpp`

**验收标准:**
- [ ] 100 万条目的全量同步在 60 秒内完成
- [ ] 分块传输处理大数据集
- [ ] Standby 从正确位置恢复 OpLog 同步

---

## 阶段 4: 集成与测试 (第8-9周)

### 概述
集成所有组件并进行全面测试。

### 任务 4.1: MasterServiceSupervisor 集成
**工期:** 4 天  
**负责人:** 工程师 A + B  
**依赖:** 阶段 1-3 完成

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 4.1.1 | 修改 Supervisor 以检测 Leader/Follower 模式 | 6h |
| 4.1.2 | 成为 Leader 时启动 ReplicationService | 4h |
| 4.1.3 | 成为 Follower 时启动 HotStandbyService | 4h |
| 4.1.4 | 实现提升流程（Follower→Leader） | 8h |
| 4.1.5 | 实现降级流程（Leader→Follower） | 6h |
| 4.1.6 | 添加安全窗口逻辑 | 4h |
| 4.1.7 | 编写集成测试 | 8h |

**交付物:**
- 修改后的 `mooncake-store/src/ha_helper.cpp`
- `mooncake-store/tests/supervisor_integration_test.cpp`

**验收标准:**
- [ ] Leader/Follower 无缝转换
- [ ] 安全窗口防止脑裂
- [ ] 故障切换期间保留所有元数据

---

### 任务 4.2: 指标与监控
**工期:** 2 天  
**负责人:** 工程师 B  
**依赖:** 任务 4.1

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 4.2.1 | 添加 OpLog 指标（sequence_id, buffer_size） | 4h |
| 4.2.2 | 添加复制延迟指标 | 4h |
| 4.2.3 | 添加核查指标 | 4h |
| 4.2.4 | 实现健康检查端点 | 4h |

**交付物:**
- 通过现有指标系统暴露的指标
- 健康检查端点文档

**验收标准:**
- [ ] 所有关键指标已暴露
- [ ] 告警阈值已文档化

---

### 任务 4.3: 故障切换测试
**工期:** 3 天  
**负责人:** 工程师 A  
**依赖:** 任务 4.1

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 4.3.1 | 测试正常故障切换（干净关闭） | 4h |
| 4.3.2 | 测试崩溃故障切换（kill -9） | 4h |
| 4.3.3 | 测试网络分区场景 | 6h |
| 4.3.4 | 测量 RTO 和 RPO | 4h |
| 4.3.5 | 记录测试结果 | 4h |

**交付物:**
- `mooncake-store/tests/e2e/failover_test.cpp`
- 测试结果报告

**验收标准:**
- [ ] RTO < 10 秒
- [ ] RPO < 1 秒
- [ ] 任何场景下都没有脑裂

---

### 任务 4.4: 性能测试
**工期:** 2 天  
**负责人:** 工程师 B  
**依赖:** 任务 4.1

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 4.4.1 | 基准测试带复制的写延迟 | 4h |
| 4.4.2 | 基准测试复制吞吐量 | 4h |
| 4.4.3 | 基准测试核查开销 | 4h |
| 4.4.4 | 与基线（无复制）比较 | 4h |

**交付物:**
- 性能基准测试结果
- 优化建议

**验收标准:**
- [ ] 写延迟增加 < 5%
- [ ] 可处理带复制的 10K 写/秒
- [ ] 核查不影响正常操作

---

### 任务 4.5: 混沌测试
**工期:** 3 天  
**负责人:** 工程师 A + B  
**依赖:** 任务 4.3, 4.4

| 子任务 | 描述 | 预估工时 |
|--------|------|----------|
| 4.5.1 | 随机 Primary 杀死 | 4h |
| 4.5.2 | 随机 Standby 杀死 | 4h |
| 4.5.3 | 网络延迟注入 | 4h |
| 4.5.4 | 丢包模拟 | 4h |
| 4.5.5 | 组合混沌场景 | 6h |
| 4.5.6 | 长时间稳定性测试（24小时） | 8h |

**交付物:**
- `mooncake-store/tests/e2e/chaos_hot_standby_test.cpp`
- 混沌测试报告

**验收标准:**
- [ ] 系统从所有混沌场景恢复
- [ ] 无数据丢失或损坏
- [ ] 24 小时稳定性测试通过

---

## 总结

### 时间线

```
周次    1    2    3    4    5    6    7    8    9
        ├────┼────┼────┼────┼────┼────┼────┼────┤
阶段 1  ████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░
阶段 2  ░░░░░░░░████████████████░░░░░░░░░░░░░░░░
阶段 3  ░░░░░░░░░░░░░░░░░░░░░░░░████████░░░░░░░░
阶段 4  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░████████
```

### 资源分配

| 工程师 | 阶段 1 | 阶段 2 | 阶段 3 | 阶段 4 |
|--------|--------|--------|--------|--------|
| 工程师 A | OpLog, OpLogManager | RPC, ReplicationService | 核查处理器, 全量同步 | 集成, 故障切换测试 |
| 工程师 B | 集成 | MetadataStore, OpLogApplier, HotStandby | 核查客户端 | 指标, 性能测试, 混沌 |

### 交付物汇总

| 阶段 | 新文件 | 修改文件 |
|------|--------|----------|
| 阶段 1 | 6 | 2 |
| 阶段 2 | 10 | 1 |
| 阶段 3 | 8 | 0 |
| 阶段 4 | 4 | 2 |
| **总计** | **28** | **5** |

### 风险缓解

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|----------|
| 复制增加不可接受的延迟 | 中 | 高 | 异步复制，批处理优化 |
| 故障切换期间脑裂 | 低 | 严重 | 安全窗口，etcd 隔离 |
| 全量同步太慢 | 中 | 中 | 分块传输，并行处理 |
| 核查发现太多不匹配 | 低 | 中 | 根因分析，bug 修复 |

### 完成定义

- [ ] 所有单元测试通过（覆盖率 >90%）
- [ ] 所有集成测试通过
- [ ] 性能基准测试达标
- [ ] 混沌测试通过
- [ ] 文档完成
- [ ] 代码审查并合入








