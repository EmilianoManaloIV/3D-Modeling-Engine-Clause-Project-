# Modeler3D performance report

GPU: llvmpipe (LLVM 20.1.2, 256 bits), window 1360x850, UI scale 2, vsync off, glFinish every frame.
Each scenario: 15 warm-up frames, then 90 measured frames.

## Scenarios

### 1. Default scene (baseline)

**10.32 ms/frame (97 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 5.402 | 52% |
| selection outline | 5.144 | 50% |
| gpu finish | 3.501 | 34% |
| present (swap) | 0.653 | 6% |
| ui draw | 0.557 | 5% |
| ui build (panels) | 0.049 | 0% |
| path trace | 0.038 | 0% |
| ui build (overlays) | 0.010 | 0% |
| gizmos | 0.006 | 0% |

Per frame: draw calls 16, triangles drawn 12, objects 2

GPU time for the viewport (GL timer queries): 1.940 ms/frame

### 2. 1000 objects in hierarchies, all selected

**50.77 ms/frame (20 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 35.905 | 71% |
| selection outline | 13.898 | 27% |
| gpu finish | 12.332 | 24% |
| present (swap) | 1.062 | 2% |
| ui draw | 0.548 | 1% |
| gizmos | 0.480 | 1% |
| ui build (panels) | 0.296 | 1% |
| path trace | 0.046 | 0% |
| ui build (overlays) | 0.035 | 0% |
| input+tools | 0.031 | 0% |

Per frame: draw calls 2014, triangles drawn 123888, objects 1000

GPU time for the viewport (GL timer queries): 24.298 ms/frame

### 3. Dense mesh idle (262k tris), selected

**104.65 ms/frame (10 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 93.845 | 90% |
| selection outline | 33.741 | 32% |
| gpu finish | 5.541 | 5% |
| ui draw | 3.873 | 4% |
| present (swap) | 1.076 | 1% |
| ui build (panels) | 0.085 | 0% |
| path trace | 0.043 | 0% |
| ui build (overlays) | 0.013 | 0% |
| input+tools | 0.009 | 0% |

Per frame: draw calls 15, triangles drawn 261120, objects 1

GPU time for the viewport (GL timer queries): 46.788 ms/frame

### 4. Dense mesh idle (262k tris), not selected

**57.60 ms/frame (17 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 34.720 | 60% |
| gpu finish | 20.927 | 36% |
| present (swap) | 1.094 | 2% |
| ui draw | 0.381 | 1% |
| ui build (panels) | 0.078 | 0% |
| ui build (overlays) | 0.011 | 0% |
| input+tools | 0.007 | 0% |

Per frame: draw calls 13, triangles drawn 261120, objects 1

GPU time for the viewport (GL timer queries): 19.623 ms/frame

### 5. Dense mesh, edit-mode vertex drag every frame

**181.97 ms/frame (5 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 147.840 | 81% |
| edit overlay | 94.264 | 52% |
| gpu finish | 29.372 | 16% |
| mesh rebuild+upload | 16.848 | 9% |
| edit overlay (patch) | 4.527 | 2% |
| overlay upload | 2.695 | 1% |
| ui draw | 1.179 | 1% |
| present (swap) | 1.167 | 1% |
| ui build (overlays) | 0.675 | 0% |
| input+tools | 0.640 | 0% |

Per frame: mesh uploads 1, uploaded vertices 132602, draw calls 15, triangles drawn 261120, objects 1

GPU time for the viewport (GL timer queries): 118.903 ms/frame

### 6. Skinned mesh idle (49k tris, 2 bones)

**18.79 ms/frame (53 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 11.166 | 59% |
| viewport render | 5.716 | 30% |
| present (swap) | 0.954 | 5% |
| ui draw | 0.626 | 3% |
| ui build (panels) | 0.063 | 0% |
| path trace | 0.056 | 0% |
| ui build (overlays) | 0.010 | 0% |
| gizmos | 0.007 | 0% |
| input+tools | 0.006 | 0% |

Per frame: draw calls 14, triangles drawn 49152, objects 3

GPU time for the viewport (GL timer queries): 4.366 ms/frame

### 7. Skinned mesh, posing a bone every frame

**25.34 ms/frame (39 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 13.623 | 54% |
| viewport render | 9.853 | 39% |
| mesh rebuild+upload | 3.750 | 15% |
| present (swap) | 1.040 | 4% |
| ui draw | 0.494 | 2% |
| skinning (CPU) | 0.405 | 2% |
| ui build (panels) | 0.072 | 0% |
| path trace | 0.052 | 0% |
| ui build (overlays) | 0.011 | 0% |
| gizmos | 0.008 | 0% |

Per frame: mesh uploads 1, uploaded vertices 27237, draw calls 14, triangles drawn 49152, objects 3

GPU time for the viewport (GL timer queries): 4.499 ms/frame

### 8. Particles: 10 emitters, ~50k live

**17.19 ms/frame (58 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 12.113 | 70% |
| viewport render | 3.265 | 19% |
| particle draw | 2.943 | 17% |
| present (swap) | 0.885 | 5% |
| ui draw | 0.444 | 3% |
| particle sim | 0.159 | 1% |
| path trace | 0.058 | 0% |
| ui build (panels) | 0.057 | 0% |
| gizmos | 0.018 | 0% |
| ui build (overlays) | 0.008 | 0% |

Per frame: draw calls 14, objects 10

GPU time for the viewport (GL timer queries): 2.548 ms/frame

### 9. Lit view: dense mesh + 8 point lights

**102.90 ms/frame (10 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 92.868 | 90% |
| selection outline | 41.363 | 40% |
| gpu finish | 6.168 | 6% |
| ui draw | 2.347 | 2% |
| present (swap) | 1.140 | 1% |
| ui build (panels) | 0.094 | 0% |
| path trace | 0.051 | 0% |
| particle draw | 0.033 | 0% |
| gizmos | 0.032 | 0% |
| ui build (overlays) | 0.015 | 0% |

Per frame: draw calls 17, triangles drawn 261120, objects 9

GPU time for the viewport (GL timer queries): 47.913 ms/frame

### 10. Picking: 50 click-selects per frame, 1000 objects

**37.14 ms/frame (27 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 18.380 | 49% |
| picking (50 rays) | 8.217 | 22% |
| gpu finish | 8.203 | 22% |
| present (swap) | 1.042 | 3% |
| gizmos | 0.486 | 1% |
| ui draw | 0.402 | 1% |
| ui build (panels) | 0.221 | 1% |
| selection outline | 0.179 | 0% |
| ui build (overlays) | 0.013 | 0% |
| input+tools | 0.008 | 0% |

Per frame: draw calls 1013, triangles drawn 123888, objects 1000

GPU time for the viewport (GL timer queries): 3.591 ms/frame

### 11. Gizmo drag: rotating 200 objects with the Rotate handle

**22.80 ms/frame (44 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 12.483 | 55% |
| gpu finish | 8.278 | 36% |
| selection outline | 7.867 | 35% |
| present (swap) | 0.973 | 4% |
| ui draw | 0.647 | 3% |
| ui build (panels) | 0.137 | 1% |
| gizmos | 0.115 | 1% |
| path trace | 0.042 | 0% |
| ui build (overlays) | 0.009 | 0% |
| input+tools | 0.008 | 0% |

Per frame: draw calls 414, triangles drawn 24732, objects 200

GPU time for the viewport (GL timer queries): 7.798 ms/frame

### 12. Stress: 1M-triangle mesh (524k quads), selected

**232.50 ms/frame (4 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 222.503 | 96% |
| selection outline | 97.051 | 42% |
| gpu finish | 8.028 | 3% |
| present (swap) | 1.097 | 0% |
| ui draw | 0.515 | 0% |
| ui build (panels) | 0.079 | 0% |
| path trace | 0.046 | 0% |
| ui build (overlays) | 0.033 | 0% |
| input+tools | 0.010 | 0% |

Per frame: draw calls 15, triangles drawn 1044480, objects 1

GPU time for the viewport (GL timer queries): 201.239 ms/frame

### 13. Stress: 1M-triangle mesh, edit mode, dragging 10% of the vertices

**551.92 ms/frame (2 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 480.483 | 87% |
| edit overlay | 272.031 | 49% |
| mesh rebuild+upload | 78.909 | 14% |
| gpu finish | 66.092 | 12% |
| overlay upload | 12.223 | 2% |
| edit overlay (patch) | 5.049 | 1% |
| present (swap) | 1.190 | 0% |
| ui build (overlays) | 0.825 | 0% |
| input+tools | 0.803 | 0% |
| ui build (panels) | 0.795 | 0% |

Per frame: mesh uploads 1, uploaded vertices 529461, draw calls 15, triangles drawn 1044480, objects 1

GPU time for the viewport (GL timer queries): 434.855 ms/frame

### 14. Stress: 5000 objects in hierarchies (outliner + picking)

**77.21 ms/frame (13 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 58.264 | 75% |
| picking (10 rays) | 9.182 | 12% |
| gpu finish | 4.806 | 6% |
| gizmos | 2.276 | 3% |
| ui build (panels) | 1.133 | 1% |
| present (swap) | 1.097 | 1% |
| ui draw | 0.549 | 1% |
| selection outline | 0.155 | 0% |
| path trace | 0.054 | 0% |
| particle draw | 0.035 | 0% |

Per frame: draw calls 5013, triangles drawn 619932, objects 5000

GPU time for the viewport (GL timer queries): 47.184 ms/frame

### 15. Stress: re-running a bevel every frame (adjusting its width), 131k quads

**314.42 ms/frame (3 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 187.318 | 60% |
| edit overlay | 132.871 | 42% |
| mesh tool | 105.638 | 34% |
| edit overlay (rebuild) | 19.384 | 6% |
| gpu finish | 18.458 | 6% |
| mesh rebuild+upload | 18.119 | 6% |
| overlay upload | 3.249 | 1% |
| present (swap) | 1.219 | 0% |
| ui draw | 0.751 | 0% |
| ui build (panels) | 0.231 | 0% |

Per frame: mesh uploads 1, uploaded vertices 136185, draw calls 16, triangles drawn 268288, objects 1

GPU time for the viewport (GL timer queries): 146.049 ms/frame

### 16. Stress: an undo step every frame, 10 dense meshes in the scene

**102.87 ms/frame (10 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 97.417 | 95% |
| selection outline | 14.382 | 14% |
| gpu finish | 3.384 | 3% |
| present (swap) | 1.111 | 1% |
| ui draw | 0.551 | 1% |
| ui build (panels) | 0.088 | 0% |
| path trace | 0.047 | 0% |
| ui build (overlays) | 0.037 | 0% |
| undo snapshot | 0.023 | 0% |
| input+tools | 0.008 | 0% |

Per frame: draw calls 24, triangles drawn 650240, objects 10

GPU time for the viewport (GL timer queries): 73.050 ms/frame

### 17. Stress: command search typing + workspace switching every frame

**27.13 ms/frame (37 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 20.172 | 74% |
| selection outline | 18.887 | 70% |
| gpu finish | 4.759 | 18% |
| present (swap) | 0.975 | 4% |
| ui draw | 0.855 | 3% |
| ui build (panels) | 0.087 | 0% |
| particle draw | 0.071 | 0% |
| ui build (overlays) | 0.058 | 0% |
| path trace | 0.041 | 0% |
| gizmos | 0.021 | 0% |

Per frame: draw calls 24, triangles drawn 4644, objects 12

GPU time for the viewport (GL timer queries): 2.243 ms/frame

## One-shot operations

| Operation | ms |
|---|---:|
| Catmull-Clark (32k faces -> 131k quads) | 36.4 |
| buildRenderMesh, smooth (262k tris) | 15.3 |
| uniqueEdges (131k quads) | 33.7 |
| Smart UV unwrap (131k quads) | 57.0 |
| Pack UV islands (131k quads) | 54.4 |
| Raycast 100 rays vs 262k tris (brute force) | 180.7 |
| Picking BVH build (262k tris, once per edit) | 58.5 |
| Raycast 100 rays vs 262k tris (picking BVH) | 0.1 |
| Edge loop select (131k quads) | 19.1 |
| Split 10k edges | 70.6 |
| Loop cut, one ring (131k quads) | 36.8 |
| Bevel 10k edges | 332.7 |
| Inset 10k faces (individual) | 25.6 |
| Push in the top cap (8.7k faces) | 93.9 |
| Punch a hole through the sphere (131k quads) | 219.2 |
| Bridge two 131k-quad spheres (facing caps) | 60.4 |
| Clean up (weld + T-junctions), 131k quads | 251.6 |
| Undo snapshot after editing 1 of 10 dense meshes (1.3M quads total) | 12.9 |
| Undo snapshot (scene with 131k-quad mesh) | 10.9 |
| Save .m3d (131k quads) | 205.8 |
| Load .m3d (131k quads) | 93.4 |
| Export OBJ (131k quads) | 252.6 |
| Import OBJ (131k quads) | 190.1 |
| Bind + automatic weights (24k quads, 20 bones) | 2.4 |
| CPU skinning x10 (24k quads, 20 bones) | 4.1 |
| Boolean difference (sphere 64x32 - cylinder 64) + clean-up | 317.8 |

## Multithreading (job system, 1 vs 4 threads)

Best of 3 runs each.

| Operation | 1 thread ms | 4 threads ms | speed-up |
|---|---:|---:|---:|
| CPU skinning x10 (24k quads, 20 bones) | 6.2 | 2.7 | 2.29x |
| Automatic weights (24k quads, 20 bones) | 8.0 | 2.2 | 3.74x |
| buildRenderMesh, smooth (262k tris) | 12.4 | 10.7 | 1.16x |
| Path-tracer scene build + BVH (262k tris) | 148.8 | 97.3 | 1.53x |

## Path tracing (320x200, 16 samples/pixel, 4644 triangles, max 4 bounces)

| Device | ms | Msamples/s |
|---|---:|---:|
| CPU, 1 thread | 2027 | 0.51 |
| CPU, 4 threads | 591 | 1.73 |
| GPU (llvmpipe (LLVM 20.1.2, 256 bits)) | 2234 | 0.46 |

GPU data uploaded for tracing: 0.88 MB (triangles, BVH, materials as RGBA32F textures) in 1.2 ms. GPU time per frame while tracing (timer queries): 35.5 ms of a 30 ms budget.
BVH of the 262k-triangle mesh: 157103 nodes.
