# Modeler3D performance report

GPU: Intel(R) Iris(R) Xe Graphics, window 1728x1020, UI scale 3, vsync off, glFinish every frame.
Each scenario: 15 warm-up frames, then 90 measured frames.

## Scenarios

### 1. Default scene (baseline)

**2.17 ms/frame (462 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 1.359 | 63% |
| present (swap) | 0.310 | 14% |
| viewport render | 0.249 | 12% |
| ui build (panels) | 0.101 | 5% |
| selection outline | 0.061 | 3% |
| ui draw | 0.047 | 2% |
| path trace | 0.043 | 2% |
| ui build (overlays) | 0.011 | 1% |
| gizmos | 0.008 | 0% |
| input+tools | 0.006 | 0% |

Per frame: draw calls 15, triangles drawn 12, objects 2

GPU time for the viewport (GL timer queries): 0.459 ms/frame

### 2. 1000 objects in hierarchies, all selected

**5.72 ms/frame (175 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 2.156 | 38% |
| viewport render | 2.127 | 37% |
| selection outline | 0.638 | 11% |
| present (swap) | 0.348 | 6% |
| particle sim | 0.223 | 4% |
| ui build (panels) | 0.220 | 4% |
| input+tools | 0.215 | 4% |
| ui build (overlays) | 0.213 | 4% |
| path trace | 0.046 | 1% |
| ui draw | 0.044 | 1% |

Per frame: draw calls 2013, triangles drawn 123888, objects 1000

GPU time for the viewport (GL timer queries): 1.866 ms/frame

### 3. Dense mesh idle (262k tris), selected

**3.67 ms/frame (272 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 2.629 | 72% |
| present (swap) | 0.440 | 12% |
| viewport render | 0.275 | 7% |
| ui build (panels) | 0.127 | 3% |
| selection outline | 0.075 | 2% |
| path trace | 0.067 | 2% |
| ui draw | 0.061 | 2% |
| ui build (overlays) | 0.016 | 0% |
| input+tools | 0.009 | 0% |
| particle sim | 0.006 | 0% |

Per frame: draw calls 14, triangles drawn 261120, objects 1

GPU time for the viewport (GL timer queries): 2.038 ms/frame

### 4. Dense mesh idle (262k tris), not selected

**2.61 ms/frame (383 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 1.722 | 66% |
| present (swap) | 0.369 | 14% |
| viewport render | 0.204 | 8% |
| ui build (panels) | 0.125 | 5% |
| path trace | 0.063 | 2% |
| ui draw | 0.058 | 2% |
| ui build (overlays) | 0.015 | 1% |
| input+tools | 0.006 | 0% |
| particle sim | 0.006 | 0% |

Per frame: draw calls 12, triangles drawn 261120, objects 1

GPU time for the viewport (GL timer queries): 1.411 ms/frame

### 5. Dense mesh, edit-mode vertex drag every frame

**23.23 ms/frame (43 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 11.490 | 49% |
| viewport render | 8.043 | 35% |
| mesh refresh+upload | 6.903 | 30% |
| input+tools | 0.959 | 4% |
| ui build (overlays) | 0.896 | 4% |
| edit overlay | 0.861 | 4% |
| present (swap) | 0.638 | 3% |
| ui build (panels) | 0.536 | 2% |
| overlay upload | 0.371 | 2% |
| ui draw | 0.111 | 0% |

Per frame: uploaded vertices 132602, draw calls 16, triangles drawn 261120, objects 1, mesh refreshes 1

GPU time for the viewport (GL timer queries): 12.001 ms/frame

### 6. Skinned mesh idle (49k tris, 2 bones)

**2.85 ms/frame (351 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 0.878 | 31% |
| present (swap) | 0.854 | 30% |
| viewport render | 0.435 | 15% |
| ui build (panels) | 0.267 | 9% |
| path trace | 0.136 | 5% |
| ui draw | 0.120 | 4% |
| ui build (overlays) | 0.043 | 1% |
| gizmos | 0.019 | 1% |
| input+tools | 0.015 | 1% |
| particle sim | 0.014 | 0% |

Per frame: draw calls 13, triangles drawn 49152, objects 3

GPU time for the viewport (GL timer queries): 0.571 ms/frame

### 7. Skinned mesh, posing a bone every frame

**4.90 ms/frame (204 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 2.577 | 53% |
| mesh refresh+upload | 1.961 | 40% |
| gpu finish | 1.117 | 23% |
| present (swap) | 0.670 | 14% |
| skinning (CPU) | 0.288 | 6% |
| ui build (panels) | 0.217 | 4% |
| path trace | 0.090 | 2% |
| ui draw | 0.089 | 2% |
| ui build (overlays) | 0.034 | 1% |
| input+tools | 0.017 | 0% |

Per frame: uploaded vertices 26846, draw calls 13, triangles drawn 49152, objects 3, mesh refreshes 1

GPU time for the viewport (GL timer queries): 0.709 ms/frame

### 8. Particles: 10 emitters, ~50k live

**3.56 ms/frame (281 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 1.464 | 41% |
| viewport render | 0.976 | 27% |
| particle draw | 0.690 | 19% |
| present (swap) | 0.581 | 16% |
| ui build (panels) | 0.172 | 5% |
| ui draw | 0.100 | 3% |
| particle sim | 0.086 | 2% |
| path trace | 0.081 | 2% |
| gizmos | 0.045 | 1% |
| ui build (overlays) | 0.027 | 1% |

Per frame: draw calls 13, objects 10

GPU time for the viewport (GL timer queries): 1.484 ms/frame

### 9. Lit view: dense mesh + 8 point lights

**5.80 ms/frame (172 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 4.019 | 69% |
| present (swap) | 0.655 | 11% |
| viewport render | 0.568 | 10% |
| ui build (panels) | 0.227 | 4% |
| selection outline | 0.132 | 2% |
| ui draw | 0.097 | 2% |
| path trace | 0.093 | 2% |
| gizmos | 0.045 | 1% |
| ui build (overlays) | 0.037 | 1% |
| particle draw | 0.021 | 0% |

Per frame: draw calls 16, triangles drawn 261120, objects 9

GPU time for the viewport (GL timer queries): 3.151 ms/frame

### 10. Picking: 50 click-selects per frame, 1000 objects

**33.89 ms/frame (30 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| picking (50 rays) | 23.484 | 69% |
| viewport render | 5.033 | 15% |
| gpu finish | 2.540 | 7% |
| present (swap) | 0.690 | 2% |
| ui build (overlays) | 0.382 | 1% |
| input+tools | 0.372 | 1% |
| ui build (panels) | 0.367 | 1% |
| particle sim | 0.348 | 1% |
| path trace | 0.204 | 1% |
| gizmos | 0.129 | 0% |

Per frame: draw calls 1012, triangles drawn 123888, objects 1000

GPU time for the viewport (GL timer queries): 2.340 ms/frame

### 11. Gizmo drag: rotating 200 objects with the Rotate handle

**6.57 ms/frame (152 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 2.772 | 42% |
| viewport render | 2.009 | 31% |
| present (swap) | 0.815 | 12% |
| selection outline | 0.625 | 10% |
| ui build (panels) | 0.310 | 5% |
| path trace | 0.128 | 2% |
| ui draw | 0.128 | 2% |
| particle sim | 0.112 | 2% |
| ui build (overlays) | 0.096 | 1% |
| gizmos | 0.055 | 1% |

Per frame: draw calls 413, triangles drawn 24732, objects 200

GPU time for the viewport (GL timer queries): 1.496 ms/frame

## One-shot operations

| Operation | ms |
|---|---:|
| Catmull-Clark (32k faces -> 131k quads) | 116.4 |
| buildRenderMesh, smooth (262k tris) | 42.2 |
| uniqueEdges (131k quads) | 66.5 |
| Smart UV unwrap (131k quads) | 122.5 |
| Pack UV islands (131k quads) | 139.5 |
| Raycast 100 rays vs 262k tris (brute force) | 661.1 |
| Picking BVH build (262k tris, once per edit) | 170.9 |
| Raycast 100 rays vs 262k tris (picking BVH) | 0.1 |
| Undo snapshot (scene with 131k-quad mesh) | 51.7 |
| Save .m3d (131k quads) | 104.6 |
| Load .m3d (131k quads) | 343.4 |
| Export OBJ (131k quads) | 624.9 |
| Import OBJ (131k quads) | 689.7 |
| Bind + automatic weights (24k quads, 20 bones) | 3.5 |
| CPU skinning x10 (24k quads, 20 bones) | 4.8 |
| Boolean difference (sphere 64x32 - cylinder 64) + clean-up | 987.3 |

## Multithreading (job system, 1 vs 12 threads)

Best of 3 runs each.

| Operation | 1 thread ms | 12 threads ms | speed-up |
|---|---:|---:|---:|
| CPU skinning x10 (24k quads, 20 bones) | 7.3 | 2.9 | 2.49x |
| Automatic weights (24k quads, 20 bones) | 10.3 | 1.8 | 5.63x |
| buildRenderMesh, smooth (262k tris) | 32.8 | 29.2 | 1.12x |
| Path-tracer scene build + BVH (262k tris) | 298.9 | 161.4 | 1.85x |

## Path tracing (320x200, 16 samples/pixel, 4644 triangles, max 4 bounces)

| Device | ms | Msamples/s |
|---|---:|---:|
| CPU, 1 thread | 3946 | 0.26 |
| CPU, 12 threads | 646 | 1.59 |
| GPU (Intel(R) Iris(R) Xe Graphics) | 61 | 16.80 |

GPU data uploaded for tracing: 0.88 MB (triangles, BVH, materials as RGBA32F textures) in 35.0 ms. GPU time per frame while tracing (timer queries): 17.7 ms of a 30 ms budget.
BVH of the 262k-triangle mesh: 157103 nodes.
