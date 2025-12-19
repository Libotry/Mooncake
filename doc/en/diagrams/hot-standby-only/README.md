# Hot Standby Mode Architecture Diagrams

This directory contains PlantUML diagrams for the **pure Hot Standby Mode** (without snapshot).

## Diagrams

| File | Description |
|------|-------------|
| `architecture.puml` | Overall architecture showing Primary, Standbys, and their components |
| `replication-flow.puml` | OpLog generation and replication flow |
| `verification-flow.puml` | Data consistency verification mechanism |
| `failover-sequence.puml` | Detailed failover sequence with timing |
| `state-machine.puml` | Master node state machine |

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                     Hot Standby Architecture                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌───────────────────┐        ┌───────────────────┐            │
│  │   Primary Master  │ OpLog  │  Standby Master   │            │
│  │                   │ ─────► │                   │            │
│  │  ┌─────────────┐  │        │  ┌─────────────┐  │            │
│  │  │  Metadata   │  │ Verify │  │  Metadata   │  │            │
│  │  │  Store      │ ◄┼────────┼─ │  Store      │  │            │
│  │  └─────────────┘  │        │  │  (Replica)  │  │            │
│  │                   │        │  └─────────────┘  │            │
│  └─────────┬─────────┘        └─────────┬─────────┘            │
│            │                            │                       │
│            │ Lease                      │ Watch                 │
│            ▼                            ▼                       │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                         etcd                             │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Key Design Points

### 1. OpLog-based Replication
- Every write operation generates an OpLog entry
- OpLog is streamed to all Standbys in real-time
- Standbys apply OpLog to maintain replica metadata

### 2. Verification Mechanism
- Standby periodically samples data and calculates checksums
- Sends `prefix_hash` + `checksum` to Primary for verification
- Auto-repairs minor inconsistencies
- Triggers full sync for major divergence

### 3. Failover Process
- etcd lease expiration triggers leader election
- Safety window prevents split-brain
- Hot data enables instant promotion

## Rendering

```bash
# Generate PNG
plantuml *.puml

# Generate SVG
plantuml -tsvg *.puml
```

Or use VS Code PlantUML extension: `Alt + D` to preview.

## Key Metrics

| Metric | Target |
|--------|--------|
| **RPO** | < 1 second |
| **RTO** | < 10 seconds |
| **Replication Lag** | < 100 entries |
| **Verification Interval** | 30 seconds |








