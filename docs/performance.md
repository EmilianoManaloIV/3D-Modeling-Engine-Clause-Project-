# Performance testing

## How it was measured

`Modeler3D --benchmark report.md` runs scripted scenarios inside the real editor (real UI, real
renderer) and writes a report. Each scenario gets 15 warm-up frames and 90 measured frames, with vsync
off and `glFinish` every frame, so GPU time counts. After the scenarios it times a set of one-shot
operations. The per-section numbers come from the built-in profiler (`src/profiler.h`, `PROF_SCOPE`),
which you can also see live in the app with **F3**.

Machine: Intel Iris Xe (integrated GPU), 1728x1020 window, Windows 11. The full latest report is in
[benchmark-report.md](benchmark-report.md). Laptop timings vary by roughly ±10% between runs.

## Results

| Scenario | Before | After | Speed-up |
|---|---:|---:|---:|
| Default scene | 3.3 ms | 3.6 ms | (noise; outline pass added) |
| 1000 objects in hierarchies, all selected | **187.2 ms** | 12.3 ms | 15x |
| Dense mesh (262k tris), selected | 14.4 ms | 8.8 ms | 1.6x |
| Dense mesh (262k tris), not selected | n/a | 2.7 ms | |
| Dense mesh, vertex drag every frame | **189.4 ms** | 32.4 ms | 5.8x |
| Skinned mesh idle | 3.3 ms | 3.6 ms | (CPU skinning skipped, see below) |
| Skinned mesh, posing every frame | 17.4 ms | 6.1 ms | 2.8x |
| Particles, ~50k live | 5.2 ms | 5.6 ms | (GPU bound, unchanged) |
| Lit view, dense mesh, 8 lights | 15.0 ms | 11.4 ms | 1.3x |
| Picking: 50 click-selects per frame, 1000 objects | **285.2 ms** | 17.0 ms | 17x |
| Gizmo rotate of 200 objects | 6.5 ms | 4.9 ms | 1.3x |

| One-shot operation (131k-quad mesh unless noted) | Before | After |
|---|---:|---:|
| Save `.m3d` | **1303 ms** | 68 ms |
| Load `.m3d` | **4523 ms** | 127 ms |
| Export OBJ | **1941 ms** | 95 ms |
| Import OBJ | **4114 ms** | 131 ms |
| Smart UV unwrap | 183 ms | 40 ms |
| Pack UV islands | 207 ms | 39 ms |
| Build render data (262k tris, smooth) | 44 ms | 10 ms |
| Unique edges | 57 ms | 21 ms |
| Raycast 100 rays vs 262k tris | 186 ms | 119 ms |
| Catmull-Clark (32k to 131k faces) | 38 ms | 30 ms |
| Undo snapshot | 15 ms | 11 ms |
| F12 screenshot file size | 5.3 MB | ~0.1-0.3 MB |

## Bottlenecks found and what fixed them

1. **Outliner tree: O(n^3).**
   - **Problem:** each row searched every object for its children, and every parent lookup
     (`Scene::indexOf`) was a linear scan. This made 175 ms of the 187 ms frame with 1000 objects.
   - **Fix:** a self-validating id-to-index cache in `Scene` (O(1) lookups; checked on every hit and
     rebuilt when the object list changes), plus child lists built once per frame.
2. **Picking tested every triangle of every mesh.**
   - **Fix:** a ray vs. bounding-box test first (bounds cached per mesh version), and all world
     matrices computed in one pass (`Scene::computeWorlds`) instead of walking parent chains per
     object.
3. **Edge lists rebuilt twice per frame during vertex drags.**
   - **Problem:** `uniqueEdges` (a hash set, 57 ms) ran for the GPU wireframe and again for the
     edit overlay, although a drag never changes topology.
   - **Fix:**
     - Meshes now carry a *topology* stamp separate from the content stamp. `touchPositions()` bumps
       only the latter, so edge lists stay cached.
     - `uniqueEdges` is sort-based.
     - The edit overlay's vertex buffers persist and are re-uploaded only when vertices or the
       selection change.
4. **Non-indexed mesh upload.**
   - **Problem:** 3 vertices per triangle meant 786k vertices (28 MB) per upload for a 262k-triangle
     mesh.
   - **Fix:** `buildRenderMesh` produces an indexed mesh where smooth vertices are shared (split only
     at UV seams and hard edges): 132k vertices.
   - **Smooth normals:** they use a compressed adjacency table and a fast path for vertices whose
     faces are all within half the smoothing angle.
5. **Selection drawn as every edge.**
   - **Problem:** a selected 262k-triangle mesh drew 393k lines a frame (about 5 ms of GPU time;
     confirmed by the selected vs. not-selected scenarios).
   - **Fix:** a Unity-style screen-space outline. Selected meshes go into a mask texture, then an
     edge-detect pass runs only inside the selection's screen rectangle. Its cost no longer depends on
     mesh density.
6. **CPU skinning every frame even when nothing moved.**
   - **Fix:** the renderer now reports whether its cached copy matches the pose key. When it does,
     the editor skips evaluating (and skinning) the mesh entirely.
7. **Text file I/O: `fprintf` per number, `istringstream` per line.**
   - **Fix:** one buffer, `std::to_chars` / `std::from_chars`, one write or read. Loading is about
     35x faster.
8. **UV islands used a hash map of small vectors.**
   - **Fix:** sorted (edge, face) pairs and a flat union-find.
9. **Screenshots were stored uncompressed.**
   - **Fix:** the PNG writer now uses per-row filters (None/Sub/Up/Paeth) and its own DEFLATE encoder
     (LZ77 + fixed Huffman), which is about 27x smaller.

## Known remaining costs

- **Single raycast against a very dense mesh:** about 1.2 ms per ray at 262k triangles, because there
  is no BVH. Fine for clicks. Edit-mode vertex picking can do up to 32 occlusion rays on such a mesh.
- **Undo snapshots** copy the whole scene (11 ms with a 131k-quad mesh). A command/delta-based undo
  would remove this.
- **Particles and dense meshes are GPU-bound** on the integrated GPU. MSAA 4x and the 1728x1020
  viewport dominate.
