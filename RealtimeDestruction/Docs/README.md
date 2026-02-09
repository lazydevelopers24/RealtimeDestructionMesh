# RealtimeDestruction Plugin Documentation

---

## Overview
RealtimeDestruction delivers production-ready mesh destruction for Unreal Engine. Shoot and blast through meshes in real-time using optimized boolean operations running on background threads. Key features include adaptive batch processing, structural integrity with grid cell approximation, automatic mesh simplification, and full multiplayer replication.
Ideal for placing destructible cover or obstacles such as pillars, walls, crates, and furniture throughout your levels. This solution is a perfect fit for FPS games, destruction simulators, or any project that demands high-performance, dynamic mesh deformation.

## User Guide
- Online documentation: https://lazydevelopers24.github.io/RealtimeDestructibleMesh/
- Youtube: https://www.youtube.com/@LazyDevelopers-y5k
- Changelog: Docs/CHANGELOG.md (included in project)

## Features

### Real-time Boolean Mesh Operations
- Dynamic mesh subtraction using optimized boolean algorithms
- Support for multiple tool shapes (Sphere, Cylinder)
- Automatic mesh simplification to maintain performance
- Hole filling and edge welding for clean geometry

### Multi-threaded Processing
- Asynchronous boolean operations on background threads
- Slot-based worker system for parallel chunk processing
- Adaptive batch sizing based on frame budget
- Thread-safe queue management (MPSC queues)

### Chunk-based Mesh Management
- Support for multi-chunk destructible meshes
- Independent processing pipeline per chunk
- Generation tracking to prevent race conditions
- Automatic collision updates with configurable delays

### Structural Integrity System
- Grid cell-based structural analysis
- Anchor point connectivity detection
- Automatic mesh island detection and removal
- Subcell subdivision for fine-grained destruction detection
- Supercell grouping for accelerated connectivity analysis

### Network Replication
- Client-server destruction synchronization
- Optimized RPC calls for multiplayer games
- Server-authoritative destruction validation
- Standalone and dedicated server support

## Installation

### Requirements
- Unreal Engine 5.7 or later
- Windows
- C++ project (for full customization)

### Installation Steps
1. Download the plugin package
2. Extract to your project's `Plugins` folder
3. Create folder structure: `Plugins/RealtimeDestruction/`
4. Regenerate project files
5. Enable the plugin in Edit > Plugins > RealtimeDestruction
6. Restart Unreal Editor

## Usage

### Basic Setup
1. Add `URealtimeDestructibleMeshComponent` to your actor
2. Assign the source mesh to be destructible
3. Configure destruction settings in the Details panel
4. Call destruction functions via Blueprint or C++

### Blueprint Integration
```cpp
// Trigger destruction at impact point
DestructibleMeshComponent->RequestDestruction(ImpactLocation, ToolMesh, bIsPenetration);
```

### C++ Integration
```cpp
#include "Components/RealtimeDestructibleMeshComponent.h"

// Setup destruction request
FRealtimeDestructionRequest Request;
Request.ToolCenterWorld = ImpactLocation;
Request.ToolShape = EDestructionToolShape::Sphere;
Request.ToolMeshPtr = ToolMeshSharedPtr;

// Execute destruction
DestructibleComponent->EnqueueDestructionOp(MoveTemp(Request));
```

### Configuration Options

| Property | Description | Default |
|----------|-------------|---------|
| bEnableMultiWorkers | Enable multi-threaded processing | true |
| bEnableSupercell | Enable large-scale cell grouping | false |
| bEnableSubcell | Enable fine-grained cell division | false |
| FrameBudgetMs | Target frame time budget (ms) | 8.0 |

## Technical Details

### Architecture
```
RealtimeDestruction/
├── Source/
│   ├── RealtimeDestruction/
│   │   ├── Public/
│   │   │   ├── Actors/
│   │   │   ├── BooleanProcessor/
│   │   │   ├── Components/
│   │   │   ├── Data/
│   │   │   ├── Debug/
│   │   │   ├── Settings/
│   │   │   ├── StructuralIntegrity/
│   │   │   ├── Subsystems/
│   │   │   └── Testing/
│   │   └── Private/
│   │       └── (mirrors Public structure)
│   └── RealtimeDestructionEditor/
│       ├── Public/
│       │   └── AnchorMode/
│       └── Private/
│           └── AnchorMode/
├── Content/
│   └── Decals/
├── Config/
├── Docs/
└── RealtimeDestruction.uplugin
```

### Core Classes

| Class | Description |
|-------|-------------|
| `URealtimeDestructibleMeshComponent` | Main component for destructible meshes |
| `FRealtimeBooleanProcessor` | Handles async boolean operations |
| `URDMThreadManagerSubsystem` | Manages worker thread allocation |

### Processing Pipeline
1. **Enqueue**: Destruction request added to priority queue
2. **Gather**: Batch operations per chunk
3. **Union**: Combine tool meshes (background thread)
4. **Subtract**: Boolean subtraction from target mesh
5. **Simplify**: Reduce triangle count if needed
6. **Apply**: Update mesh on Game Thread

### Performance Optimization
- Adaptive union count based on operation cost
- Automatic mesh simplification triggers
- Frame budget monitoring and throttling
- Worker count tuning based on throughput

## Dependencies
- GeometryScriptingCore (Unreal Engine built-in)
- DynamicMesh (Unreal Engine built-in)
- ProceduralMeshComponent (optional, for debris)

## Troubleshooting

### Common Issues

**Destruction not visible**
- Ensure mesh has valid collision
- Check that chunk index is valid
- Verify tool mesh is not empty

**Performance drops during destruction**
- Reduce MaxBatchSize
- Enable multi-worker mode
- Increase simplification frequency

**Crash on destruction**
- Verify mesh has valid triangles
- Check for NaN values in tool transform
- Ensure component is not being destroyed

**Network desync**
- Enable server-authoritative mode
- Check RPC call frequency
- Verify client has replicated mesh state

## Version History

### Version 1.0.0
- Initial release
- Real-time boolean mesh destruction
- Multi-threaded processing pipeline
- Structural integrity system
- Network replication support
- Automatic mesh simplification

## License
This plugin is provided under Fab Standard License.

## Support
For technical support, bug reports, or feature requests: lazydeveloper24@gmail.com

---
*RealtimeDestruction Plugin - High-Performance Mesh Destruction for Unreal Engine*
