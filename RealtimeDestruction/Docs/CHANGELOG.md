# Changelog

## v1.1
February 9, 2026

### New Features

**Auto-Setup from Parent StaticMesh**
- Added `TryAutoSetupFromParentStaticMesh()` to automatically initialize component from parent StaticMesh

**GridCell Data Preservation on Blueprint Reconstruction**
- `FGridCellLayout` and `CachedRDMScale` saved as instance data to prevent loss during Blueprint reconstruction

**Mesh Data Caching for Runtime Builds**
- Added `CachedVertices`/`CachedIndices` to `FGridCellLayout` for Grid Cell regeneration in packaged builds
- 3-tier voxelization strategy: Cached data -> MeshDescription -> Bounding box fallback

### Improvements

**SubCell System Enhancement**
- Integrated SubCell into GridCellBuilder voxelization pipeline with `MarkIntersectingSubCellsAlive()`
- Extracted `VoxelizeTriangle()` / `VoxelizeFromArrays()` for centralized SubCell processing
- Added SubCell debug visualization (`bShowSubCellDebug`)

**Decal Size Editor**
- Added `DecalSizeEditorWindow` / `DecalSizeEditorViewport` for visual decal size editing and `DecalMaterialDataAssetDetails` custom Details panel

**Anchor Placement Improvements**
- Smart anchor placement: auto-detects target mesh bounds and camera position for optimal placement
- Explicit Anchor cleanup on AnchorEditMode exit; removed `RemoveAllAnchors()` in favor of per-type removal methods

**Lightweight Cell Collection Methods**
- `CollectToolMeshOverlappingCells()` - Collects cells overlapping with ToolMesh without boolean operations
- `CollectCellsOverlappingMesh()` - SAT-based triangle-AABB intersection testing
- `BuildSmoothedToolMesh()` - Integrated GreedyMesh + FillHoles + HC Laplacian Smoothing

### Performance Optimization
**Debris Parameter Tuning**
- Adjusted `DebrisExpandRatio` (1.2→1.5) and `DebrisScaleRatio` (0.9→0.7) to expand debris removal range and reduce rendering cost.

**Mesh Smoothing Parameter Range Adjustments**
- More conservative smoothing for improved visual quality and stability.

### API Changes

**Backward Compatible (Default Parameter Additions)**
- `RemoveTrianglesForDetachedCells()` - Added `OutToolMeshOverlappingCellIds` output parameter
- `FGridCellBuilder::BuildFromStaticMesh()` - Added `OutSubCellStates` parameter
- `TriangleIntersectsAABB()` - Changed visibility from private to public

**UI Category Changes**
- Smoothing-related properties moved from "StructuralIntegrity" to "Advanced|Debris"

---