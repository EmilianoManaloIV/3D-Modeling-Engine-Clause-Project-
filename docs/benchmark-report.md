# Modeler3D performance report

GPU: Intel(R) Iris(R) Xe Graphics, window 1728x1020, UI scale 3, vsync off, glFinish every frame.
Each scenario: 15 warm-up frames, then 90 measured frames.

## Scenarios

### 1. Default scene (baseline)

**3.67 ms/frame (273 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 2.848 | 78% |
| present (swap) | 0.392 | 11% |
| viewport render | 0.223 | 6% |
| ui build (panels) | 0.068 | 2% |
| selection outline | 0.056 | 2% |
| ui draw | 0.049 | 1% |
| path trace | 0.030 | 1% |
| ui build (overlays) | 0.011 | 0% |
| gizmos | 0.010 | 0% |

Per frame: draw calls 15, triangles drawn 12, objects 2

GPU time for the viewport (GL timer queries): 0.974 ms/frame

### 2. 1000 objects in hierarchies, all selected

**13.24 ms/frame (76 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 6.454 | 49% |
| gpu finish | 5.896 | 45% |
| selection outline | 2.448 | 18% |
| gizmos | 0.622 | 5% |
| present (swap) | 0.378 | 3% |
| ui build (panels) | 0.203 | 2% |
| ui draw | 0.053 | 0% |
| path trace | 0.053 | 0% |
| ui build (overlays) | 0.029 | 0% |
| input+tools | 0.025 | 0% |

Per frame: draw calls 2013, triangles drawn 123888, objects 1000

GPU time for the viewport (GL timer queries): 4.841 ms/frame

### 3. Dense mesh idle (262k tris), selected

**9.38 ms/frame (107 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 8.591 | 92% |
| present (swap) | 0.385 | 4% |
| viewport render | 0.198 | 2% |
| ui build (panels) | 0.069 | 1% |
| selection outline | 0.048 | 1% |
| ui draw | 0.047 | 1% |
| path trace | 0.025 | 0% |
| ui build (overlays) | 0.011 | 0% |
| input+tools | 0.006 | 0% |

Per frame: draw calls 14, triangles drawn 261120, objects 1

GPU time for the viewport (GL timer queries): 6.053 ms/frame

### 4. Dense mesh idle (262k tris), not selected

**7.04 ms/frame (142 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 6.306 | 90% |
| present (swap) | 0.392 | 6% |
| viewport render | 0.163 | 2% |
| ui build (panels) | 0.054 | 1% |
| ui draw | 0.050 | 1% |
| path trace | 0.028 | 0% |
| ui build (overlays) | 0.008 | 0% |

Per frame: draw calls 12, triangles drawn 261120, objects 1

GPU time for the viewport (GL timer queries): 3.743 ms/frame

### 5. Dense mesh, edit-mode vertex drag every frame

**32.47 ms/frame (31 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 18.788 | 58% |
| mesh rebuild+upload | 12.352 | 38% |
| gpu finish | 11.404 | 35% |
| edit overlay | 6.268 | 19% |
| overlay upload | 1.659 | 5% |
| input+tools | 0.458 | 1% |
| ui build (overlays) | 0.440 | 1% |
| present (swap) | 0.427 | 1% |
| path trace | 0.373 | 1% |
| ui build (panels) | 0.260 | 1% |

Per frame: mesh uploads 1, uploaded vertices 132602, draw calls 14, triangles drawn 261120, objects 1

GPU time for the viewport (GL timer queries): 9.704 ms/frame

### 6. Skinned mesh idle (49k tris, 2 bones)

**2.34 ms/frame (428 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 1.696 | 73% |
| present (swap) | 0.302 | 13% |
| viewport render | 0.161 | 7% |
| ui build (panels) | 0.061 | 3% |
| ui draw | 0.043 | 2% |
| path trace | 0.027 | 1% |
| ui build (overlays) | 0.012 | 1% |
| gizmos | 0.011 | 0% |

Per frame: draw calls 13, triangles drawn 49152, objects 3

GPU time for the viewport (GL timer queries): 0.647 ms/frame

### 7. Skinned mesh, posing a bone every frame

**6.31 ms/frame (159 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 3.831 | 61% |
| mesh rebuild+upload | 3.448 | 55% |
| gpu finish | 1.957 | 31% |
| present (swap) | 0.307 | 5% |
| skinning (CPU) | 0.194 | 3% |
| ui build (panels) | 0.064 | 1% |
| path trace | 0.051 | 1% |
| ui draw | 0.046 | 1% |
| gizmos | 0.012 | 0% |
| ui build (overlays) | 0.012 | 0% |

Per frame: mesh uploads 1, uploaded vertices 27237, draw calls 13, triangles drawn 49152, objects 3

GPU time for the viewport (GL timer queries): 1.131 ms/frame

### 8. Particles: 10 emitters, ~50k live

**5.40 ms/frame (185 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 3.854 | 71% |
| viewport render | 0.889 | 16% |
| particle draw | 0.738 | 14% |
| present (swap) | 0.338 | 6% |
| ui draw | 0.089 | 2% |
| particle sim | 0.066 | 1% |
| ui build (panels) | 0.060 | 1% |
| path trace | 0.051 | 1% |
| gizmos | 0.026 | 0% |
| ui build (overlays) | 0.009 | 0% |

Per frame: draw calls 13, objects 10

GPU time for the viewport (GL timer queries): 3.733 ms/frame

### 9. Lit view: dense mesh + 8 point lights

**12.90 ms/frame (78 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 11.798 | 91% |
| present (swap) | 0.557 | 4% |
| viewport render | 0.307 | 2% |
| ui build (panels) | 0.085 | 1% |
| selection outline | 0.072 | 1% |
| ui draw | 0.056 | 0% |
| path trace | 0.031 | 0% |
| gizmos | 0.026 | 0% |
| ui build (overlays) | 0.016 | 0% |
| particle draw | 0.012 | 0% |

Per frame: draw calls 16, triangles drawn 261120, objects 9

GPU time for the viewport (GL timer queries): 9.038 ms/frame

### 10. Picking: 50 click-selects per frame, 1000 objects

**18.74 ms/frame (53 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| picking (50 rays) | 10.680 | 57% |
| viewport render | 4.093 | 22% |
| gpu finish | 3.141 | 17% |
| gizmos | 0.738 | 4% |
| present (swap) | 0.382 | 2% |
| path trace | 0.125 | 1% |
| ui build (panels) | 0.114 | 1% |
| ui draw | 0.052 | 0% |
| ui build (overlays) | 0.010 | 0% |

Per frame: draw calls 1012, triangles drawn 123888, objects 1000

GPU time for the viewport (GL timer queries): 2.342 ms/frame

### 11. Gizmo drag: rotating 200 objects with the Rotate handle

**5.38 ms/frame (186 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 3.051 | 57% |
| viewport render | 1.648 | 31% |
| selection outline | 0.606 | 11% |
| present (swap) | 0.348 | 6% |
| gizmos | 0.155 | 3% |
| ui build (panels) | 0.114 | 2% |
| ui draw | 0.062 | 1% |
| path trace | 0.056 | 1% |
| ui build (overlays) | 0.011 | 0% |
| input+tools | 0.006 | 0% |

Per frame: draw calls 413, triangles drawn 24732, objects 200

GPU time for the viewport (GL timer queries): 1.823 ms/frame

## One-shot operations

| Operation | ms |
|---|---:|
| Catmull-Clark (32k faces -> 131k quads) | 34.0 |
| buildRenderMesh, smooth (262k tris) | 13.0 |
| uniqueEdges (131k quads) | 23.2 |
| Smart UV unwrap (131k quads) | 42.5 |
| Pack UV islands (131k quads) | 41.3 |
| Raycast 100 rays vs 262k tris | 144.7 |
| Undo snapshot (scene with 131k-quad mesh) | 14.5 |
| Save .m3d (131k quads) | 195.5 |
| Load .m3d (131k quads) | 150.7 |
| Export OBJ (131k quads) | 734.5 |
| Import OBJ (131k quads) | 655.4 |
| Bind + automatic weights (24k quads, 20 bones) | 1.2 |
| CPU skinning x10 (24k quads, 20 bones) | 1.9 |
| Boolean difference (sphere 64x32 - cylinder 64) + clean-up | 222.6 |

## Multithreading (job system, 1 vs 12 threads)

Best of 3 runs each.

| Operation | 1 thread ms | 12 threads ms | speed-up |
|---|---:|---:|---:|
| CPU skinning x10 (24k quads, 20 bones) | 2.9 | 2.0 | 1.44x |
| Automatic weights (24k quads, 20 bones) | 4.3 | 1.2 | 3.48x |
| buildRenderMesh, smooth (262k tris) | 11.3 | 8.9 | 1.27x |
| Path-tracer scene build + BVH (262k tris) | 108.3 | 60.8 | 1.78x |

## Path tracing (320x200, 16 samples/pixel, 4644 triangles, max 4 bounces)

| Device | ms | Msamples/s |
|---|---:|---:|
| CPU, 1 thread | 1134 | 0.90 |
| CPU, 12 threads | 225 | 4.55 |
| GPU (Intel(R) Iris(R) Xe Graphics) | 31 | 32.76 |

GPU data uploaded for tracing: 0.56 MB (triangles, BVH, materials as RGBA32F textures) in 3.4 ms. GPU time per frame while tracing (timer queries): 9.1 ms of a 30 ms budget.
BVH of the 262k-triangle mesh: 157103 nodes.
