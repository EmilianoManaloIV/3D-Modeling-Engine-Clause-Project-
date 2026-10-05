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

## Multithreading and GPU utilisation

**CPU: job system.** `src/jobs.*` is a thread pool (one worker per hardware thread, minus the
calling thread) with `parallelFor` (fork/join; the caller helps, and its chunks go to the front of the
queue) and `TaskGroup`s for background work such as path-tracing tiles. The benchmark runs each
operation on 1 thread and then on all 12 threads (best of 3):

| Operation | 1 thread | 12 threads | Speed-up |
|---|---:|---:|---:|
| CPU path tracing, 320x200 x 16 spp | 1134 ms | 225 ms | **5.0x** |
| Path-tracer scene build + BVH (262k tris) | 108 ms | 61 ms | 1.8x |
| Automatic bone weights (24k quads, 20 bones) | 4.3 ms | 1.2 ms | 3.5x |
| CPU skinning x10 (24k quads) | 2.9 ms | 2.0 ms | 1.4x |
| Render data, smooth (262k tris) | 11.3 ms | 8.9 ms | 1.3x |

- Path tracing scales best because tiles are independent. The test laptop has 4 performance and 8
  efficiency cores, so 12 threads do not mean 12x.
- The BVH build is serial for the top levels and then builds independent subtrees in parallel.
- Render data is only partly parallel (the face normals). The rest is a sequential indexing pass and
  is memory-bound.

**GPU: verifying that it is used.** OpenGL timer queries (`GL_TIME_ELAPSED`, read back
asynchronously a few frames later so they never stall) measure the GPU time of the viewport and of
path tracing. They are shown in the F3 overlay and in the benchmark report:

| Scenario | Frame | GPU time (viewport) |
|---|---:|---:|
| Default scene | 3.7 ms | 1.0 ms |
| Dense mesh (262k tris), selected | 9.4 ms | 6.1 ms |
| Dense mesh, vertex drag every frame | 32.5 ms | 9.7 ms (rest is CPU: mesh rebuild 12 ms) |
| Particles, ~50k live | 5.4 ms | 3.7 ms |
| Lit view, dense mesh, 8 lights | 12.9 ms | 9.0 ms (GPU-bound) |

- In the preview, the GPU is busy most of the frame only in the heavy shading scenes (dense mesh,
  8 lights). In the others it idles for most of the frame, so the GPU is not the bottleneck.
- For **path tracing**, the GPU tracer sizes its work to a per-frame GPU-time budget measured by the
  same queries. On the Iris Xe it traced **32.8 Msamples/s against 4.6 on all 12 CPU threads (7.2x)**.
- With the default 12 ms budget at 60 fps, the F3 overlay shows the GPU about 80% busy with tracing
  while the UI stays responsive.
- Scene data for the GPU (0.56 MB for the showcase) uploads in about 3-10 ms.
- The status bar and the Render tab warn when OpenGL is a software renderer (e.g. llvmpipe under WSL).
  In that case the "GPU" tracer runs on the CPU: under WSLg it reached 0.36 Msamples/s against 3.2
  for the native CPU tracer. So rendering defaults to the CPU there.
- On Windows laptops with hybrid graphics, the executable exports `NvOptimusEnablement` and
  `AmdPowerXpressRequestHighPerformance`, so the driver gives it the discrete GPU.

**A regression caught by the benchmark.** The first version of the concave-face support in
`buildRenderMesh` called the general convexity test, which allocates, for every quad. That took render
data from 10 to 18 ms at 262k triangles, and the vertex-drag scenario from 32 to 40+ ms. The check now
reuses the face normal and allocates nothing, which restored both numbers.

File I/O timings (save / load / OBJ) vary up to 4x between runs on this laptop (disk cache and
antivirus scanning). The rest of the report is stable to about ±10%.

## Round 6: materials, textures, edit tools and hardware ray tracing

The new features add work in hot paths (texture lookups, a full metallic-roughness BRDF, glass,
transparent shadows), so each one was measured before and after. During these runs the laptop was
heavily loaded by background applications: even the default scene took 6.5 ms instead of 3.6 ms, and
Catmull-Clark took 57-87 ms instead of 34. So the comparisons below are **ratios measured in the
same run, back to back**, not against the older tables.

| Change | Before | After | Speed-up |
|---|---:|---:|---:|
| Edit-mode picking: 100 rays vs a 262k-triangle mesh | 254.9 ms (brute force) | 0.1 ms (picking BVH) | ~2500x |
| JPEG decode, Huffman: 9-bit lookahead table (best of 6) | 318 ms | 275 ms | 1.16x |
| JPEG decode, IDCT: fixed-point separable (jidctint) instead of float cosine sums | 537 ms | 187 ms | 2.9x |
| Shadow rays through scenes with see-through objects: opaque any-hit test first | nearest-hit walk for every shadow ray | early-out on the first opaque hit | restores the pre-transparency speed |

- **Picking BVH.** Edge, face and vertex picking need occlusion tests (a hidden edge must not be
  picked), and object picking used to raycast every triangle. Each mesh now keeps a BVH
  (`MeshAccel` in `bvh.h`) keyed on its geometry version. It is built once per edit (80 ms at 262k
  triangles, only when that mesh is picked) and used by object picking for meshes with 512+
  triangles. This removes the "1.2 ms per ray" cost listed under the remaining costs before.
- **JPEG decoder.** Textures are often multi-megapixel JPEGs, so decode time is load time. The first
  version decoded Huffman codes bit by bit and evaluated the IDCT as float cosine sums. The decoder
  now resolves codes of up to 9 bits with one table lookup, skips the IDCT for flat (DC-only) blocks,
  and uses the integer IDCT from the IJG library. Output differs from the float version by at most
  3 levels out of 255, and the tests compare against reference images. Texture sets are decoded in
  parallel on the job system (`TextureCache::preload`).
- **Transparent shadows.** Glass and alpha surfaces let light through, so shadow rays must find every
  surface on the way instead of any one. Doing that for every shadow ray made scenes slower even
  when nothing was transparent. All three tracers (CPU, GLSL, DXR) now first ask "is there an opaque
  triangle in the way?", which ends at the first hit, and walk the see-through surfaces only when the
  scene has any. In the DXR tracer the opaque and see-through triangles are separate BLAS geometries,
  so the hardware skips the shader call for opaque hits.
- **Texture sampling on the CPU** decodes sRGB through a 256-entry table instead of three `pow` calls
  per lookup, and Fresnel uses multiplies instead of `pow(x, 5)`.
- **GLSL tracer** data textures share one row width passed as a uniform, instead of a `textureSize`
  query for every fetch.
- **Mesh editing.** The concave-face check in `buildRenderMesh` uses an allocation-free
  cross-product test.

**The cost of the new material model.** A full GGX metallic-roughness BRDF with textures, glass and
alpha costs more per sample than the old Blinn-Phong-style model: about 15% on the CPU tracer and
about 45% on the GLSL tracer, measured on the same scene back to back. The latest run (above
background load) traced 2.32 Msamples/s on 12 CPU threads and 12.7 Msamples/s on the Iris Xe; see
[benchmark-report.md](benchmark-report.md).

**Hardware ray tracing (RTX device).** The *RTX* device uses DXR 1.1 inline ray queries in a compute
shader, so ray/triangle and BVH traversal run on RT cores. The Iris Xe in the test laptop has no DXR
support, so the DXR path was validated on Microsoft's WARP software device. Its image matches the CPU
tracer (mean 162.9 vs 162.8, mean absolute difference 1.4/255 with glass and alpha in the scene), but
WARP runs on the CPU, so **no RT-core speed numbers were measured**. On RTX-class GPUs, hardware
traversal is typically several times faster than the GLSL tracer's BVH walk in a fragment shader.
The tracer submits work without blocking (fences, adaptive rows per submit) and reads back about
10 times a second, so the UI stays responsive.

## Known remaining costs

- **Picking BVH rebuilds** after every edit of a dense mesh (80 ms at 262k triangles), on the first
  pick after the edit. A refit instead of a rebuild would cut that.
- **Texture uploads** are synchronous: the first frame after loading a large texture set waits for
  mipmap generation.
- **Live render restarts** rebuild the BVH and re-upload the scene on every change (e.g. each frame
  of a drag). That takes a few ms for normal scenes and about 70 ms at 262k triangles.
- **Undo snapshots** copy the whole scene (11 ms with a 131k-quad mesh). A command/delta-based undo
  would remove this.
- **Particles and dense meshes are GPU-bound** on the integrated GPU. MSAA 4x and the 1728x1020
  viewport dominate.
