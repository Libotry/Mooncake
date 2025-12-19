# Master Service 热备混合模式架构图

本目录包含 Master Service 热备混合模式设计的 PlantUML 架构图。

## 文件列表

| 文件 | 描述 |
|------|------|
| `hot-standby-architecture.puml` | 整体架构图，展示 Primary/Standby 组件关系 |
| `hot-standby-sequences.puml` | 数据核查机制详细时序图 |
| `hot-standby-failover.puml` | 故障切换详细流程时序图 |
| `hot-standby-data-flow.puml` | OpLog 数据同步流程 |
| `hot-standby-state-machine.puml` | Master 节点状态机 |
| `hot-standby-comparison.puml` | 三种模式对比分析图 |
| `hot-standby-verification-data.puml` | 核查机制数据结构详解 |

## 查看方式

### 方式一：在线预览

访问 [PlantUML Online Server](https://www.plantuml.com/plantuml/uml)，粘贴 `.puml` 文件内容即可预览。

### 方式二：VS Code 插件

1. 安装 [PlantUML 插件](https://marketplace.visualstudio.com/items?itemName=jebbs.plantuml)
2. 打开 `.puml` 文件
3. 按 `Alt + D` 预览

### 方式三：命令行生成

```bash
# 安装 PlantUML
brew install plantuml  # macOS
apt install plantuml   # Ubuntu

# 生成 PNG
plantuml hot-standby-architecture.puml

# 生成 SVG
plantuml -tsvg hot-standby-architecture.puml

# 批量生成
plantuml *.puml
```

### 方式四：Docker

```bash
docker run -v $(pwd):/data plantuml/plantuml -tpng /data/*.puml
```

## 架构概览

```
┌─────────────────────────────────────────────────────────────────┐
│                    热备混合模式架构                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────┐     OpLog Stream     ┌─────────────────┐      │
│  │   Primary   │ ──────────────────── │   Standby 1     │      │
│  │   Master    │                      │  (Hot Standby)  │      │
│  │             │ ──────────────────── │                 │      │
│  │  - RPC服务  │     Verification     │  - OpLog应用    │      │
│  │  - OpLog生成│ ◄─────────────────── │  - 核查客户端   │      │
│  │  - 核查响应 │                      │                 │      │
│  └──────┬──────┘                      └────────┬────────┘      │
│         │                                      │               │
│         │ Lease                                │ Watch         │
│         ▼                                      ▼               │
│  ┌─────────────────────────────────────────────────────┐      │
│  │                       etcd                           │      │
│  │              (Leader Election & Discovery)           │      │
│  └─────────────────────────────────────────────────────┘      │
│                                                                 │
│  ┌─────────────────────────────────────────────────────┐      │
│  │                  Snapshot Storage                    │      │
│  │                   (定期快照备份)                      │      │
│  └─────────────────────────────────────────────────────┘      │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 核心设计要点

### 1. OpLog 实时同步
- Primary 每次写操作生成 OpLog
- 异步推送到所有 Standby
- 支持批量传输优化

### 2. 核查机制
- Standby 定期采样数据
- 计算 `prefix_hash` + `checksum`
- 向 Primary 验证一致性
- 自动修复小量不一致

### 3. 混合模式
- OpLog: 保证低 RPO
- 快照: 支持冷启动恢复
- 两者结合: 兼顾可靠性与可恢复性








