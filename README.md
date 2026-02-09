# RealtimeDestruction Plugin Documentation

This is a summary of Docs/README.md for display on the GitHub repository page. For more detailed and up-to-date documentation, please refer to the Docs folder.

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
<img width="1280" height="760" alt="image" src="https://github.com/user-attachments/assets/2b461949-e332-4b8f-a96b-bdf7c2c7e5e0" />

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
<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/868ba0b2-a167-4847-bf25-eb8b3c281889" />
<img width="1920" height="1030" alt="image" src="https://github.com/user-attachments/assets/d6616425-5306-4539-9399-560fdb3ee73a" />

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

## License
This plugin is provided under Fab Standard License.

## Support
For technical support, bug reports, or feature requests: lazydeveloper24@gmail.com

---

This product was independently developed by us while participating in the Epic Project, a developer-support program of the KRAFTON JUNGLE GameTech Lab. All rights, title, and interest in and to the product are exclusively vested in us. Krafton, Inc. was not involved in its development and distribution and disclaims all representations and warranties, express or implied, and assumes no responsibility or liability for any consequences arising from the use of this product.
