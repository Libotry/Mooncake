# Master Service Hot Standby Hybrid Mode Architecture Diagrams

This directory contains PlantUML architecture diagrams for the Master Service Hot Standby Hybrid Mode design.

## File List

| File | Description |
|------|-------------|
| `hot-standby-architecture.puml` | Overall architecture diagram showing Primary/Standby component relationships |
| `hot-standby-verification.puml` | Data verification mechanism detailed sequence diagram |
| `hot-standby-failover.puml` | Failover detailed flow sequence diagram |
| `hot-standby-data-flow.puml` | OpLog data synchronization flow |
| `hot-standby-state-machine.puml` | Master node state machine |
| `hot-standby-comparison.puml` | Three modes comparison analysis diagram |
| `hot-standby-verification-data.puml` | Verification mechanism data structures explained |

## Viewing Methods

### Method 1: Online Preview

Visit [PlantUML Online Server](https://www.plantuml.com/plantuml/uml) and paste the `.puml` file contents to preview.

### Method 2: VS Code Extension

1. Install [PlantUML Extension](https://marketplace.visualstudio.com/items?itemName=jebbs.plantuml)
2. Open `.puml` file
3. Press `Alt + D` to preview

### Method 3: Command Line Generation

```bash
# Install PlantUML
brew install plantuml  # macOS
apt install plantuml   # Ubuntu

# Generate PNG
plantuml hot-standby-architecture.puml

# Generate SVG
plantuml -tsvg hot-standby-architecture.puml

# Batch generate
plantuml *.puml
```

### Method 4: Docker

```bash
docker run -v $(pwd):/data plantuml/plantuml -tpng /data/*.puml
```

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                Hot Standby Hybrid Mode Architecture              │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────┐     OpLog Stream     ┌─────────────────┐      │
│  │   Primary   │ ──────────────────── │   Standby 1     │      │
│  │   Master    │                      │  (Hot Standby)  │      │
│  │             │ ──────────────────── │                 │      │
│  │  - RPC Svc  │     Verification     │  - OpLog Apply  │      │
│  │  - OpLog Gen│ ◄─────────────────── │  - Verify Client│      │
│  │  - Verify   │                      │                 │      │
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
│  │                 (Periodic backup)                    │      │
│  └─────────────────────────────────────────────────────┘      │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Core Design Points

### 1. Real-time OpLog Synchronization
- Primary generates OpLog for each write operation
- Asynchronously pushed to all Standbys
- Supports batch transfer optimization

### 2. Verification Mechanism
- Standby periodically samples data
- Calculates `prefix_hash` + `checksum`
- Verifies consistency with Primary
- Auto-repairs minor inconsistencies

### 3. Hybrid Mode
- OpLog: Ensures low RPO
- Snapshot: Supports cold start recovery
- Combined: Balances reliability and recoverability

## Key Metrics

| Metric | Snapshot Only | Hot Standby | Hybrid Mode |
|--------|---------------|-------------|-------------|
| **RPO** | Minutes | Seconds/Zero | Seconds |
| **RTO** | Sec~Min | < 5 sec | < 5 sec |
| **Cold Start** | ✅ Yes | ❌ No | ✅ Yes |
| **Verification** | ❌ No | ✅ Yes | ✅ Yes |
| **Complexity** | Low | High | Medium-High |








