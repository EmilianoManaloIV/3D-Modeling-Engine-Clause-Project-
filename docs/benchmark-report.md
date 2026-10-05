# Modeler3D performance report

GPU: Intel(R) Iris(R) Xe Graphics, window 1728x1020, UI scale 3, vsync off, glFinish every frame.
Each scenario: 15 warm-up frames, then 90 measured frames.

## Scenarios

### 1. Default scene (baseline)

**6.50 ms/frame (154 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 2.339 | 36% |
| present (swap) | 1.600 | 25% |
| viewport render | 1.266 | 19% |
| selection outline | 0.483 | 7% |
| ui build (panels) | 0.440 | 7% |
| ui draw | 0.339 | 5% |
| path trace | 0.225 | 3% |
| ui build (overlays) | 0.065 | 1% |
| gizmos | 0.044 | 1% |
| input+tools | 0.021 | 0% |

Per frame: draw calls 15, triangles drawn 12, objects 2

GPU time for the viewport (GL timer queries): 0.944 ms/frame

### 2. 1000 objects in hierarchies, all selected

**24.20 ms/frame (41 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 16.631 | 69% |
| gpu finish | 5.699 | 24% |
| selection outline | 5.640 | 23% |
| gizmos | 1.899 | 8% |
| present (swap) | 0.754 | 3% |
| ui build (panels) | 0.424 | 2% |
| ui draw | 0.129 | 1% |
| path trace | 0.098 | 0% |
| ui build (overlays) | 0.078 | 0% |
| input+tools | 0.067 | 0% |

Per frame: draw calls 2013, triangles drawn 123888, objects 1000

GPU time for the viewport (GL timer queries): 4.919 ms/frame

### 3. Dense mesh idle (262k tris), selected

**10.39 ms/frame (96 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 9.085 | 87% |
| present (swap) | 0.607 | 6% |
| viewport render | 0.325 | 3% |
| ui build (panels) | 0.138 | 1% |
| selection outline | 0.101 | 1% |
| ui draw | 0.092 | 1% |
| path trace | 0.059 | 1% |
| ui build (overlays) | 0.021 | 0% |
| input+tools | 0.007 | 0% |

Per frame: draw calls 14, triangles drawn 261120, objects 1

GPU time for the viewport (GL timer queries): 6.874 ms/frame

### 4. Dense mesh idle (262k tris), not selected

**8.35 ms/frame (120 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 7.363 | 88% |
| present (swap) | 0.528 | 6% |
| viewport render | 0.196 | 2% |
| ui build (panels) | 0.085 | 1% |
| ui draw | 0.060 | 1% |
| path trace | 0.042 | 0% |
| ui build (overlays) | 0.016 | 0% |

Per frame: draw calls 12, triangles drawn 261120, objects 1

GPU time for the viewport (GL timer queries): 5.333 ms/frame

### 5. Dense mesh, edit-mode vertex drag every frame

**44.84 ms/frame (22 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 29.864 | 67% |
| mesh rebuild+upload | 18.783 | 42% |
| gpu finish | 11.427 | 25% |
| edit overlay | 10.854 | 24% |
| overlay upload | 1.967 | 4% |
| path trace | 0.815 | 2% |
| input+tools | 0.765 | 2% |
| ui build (overlays) | 0.697 | 2% |
| present (swap) | 0.512 | 1% |
| ui build (panels) | 0.390 | 1% |

Per frame: mesh uploads 1, uploaded vertices 132602, draw calls 14, triangles drawn 261120, objects 1

GPU time for the viewport (GL timer queries): 10.906 ms/frame

### 6. Skinned mesh idle (49k tris, 2 bones)

**3.20 ms/frame (312 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 2.261 | 71% |
| present (swap) | 0.437 | 14% |
| viewport render | 0.233 | 7% |
| ui build (panels) | 0.090 | 3% |
| ui draw | 0.061 | 2% |
| path trace | 0.052 | 2% |
| ui build (overlays) | 0.021 | 1% |
| gizmos | 0.015 | 0% |
| input+tools | 0.008 | 0% |

Per frame: draw calls 13, triangles drawn 49152, objects 3

GPU time for the viewport (GL timer queries): 1.102 ms/frame

### 7. Skinned mesh, posing a bone every frame

**8.45 ms/frame (118 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 5.542 | 66% |
| mesh rebuild+upload | 4.983 | 59% |
| gpu finish | 2.201 | 26% |
| present (swap) | 0.433 | 5% |
| skinning (CPU) | 0.308 | 4% |
| ui build (panels) | 0.087 | 1% |
| path trace | 0.067 | 1% |
| ui draw | 0.059 | 1% |
| ui build (overlays) | 0.019 | 0% |
| gizmos | 0.016 | 0% |

Per frame: mesh uploads 1, uploaded vertices 27237, draw calls 13, triangles drawn 49152, objects 3

GPU time for the viewport (GL timer queries): 1.358 ms/frame

### 8. Particles: 10 emitters, ~50k live

**5.83 ms/frame (171 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 3.621 | 62% |
| viewport render | 1.350 | 23% |
| particle draw | 1.129 | 19% |
| present (swap) | 0.439 | 8% |
| ui draw | 0.107 | 2% |
| particle sim | 0.093 | 2% |
| ui build (panels) | 0.078 | 1% |
| path trace | 0.075 | 1% |
| gizmos | 0.053 | 1% |
| ui build (overlays) | 0.014 | 0% |

Per frame: draw calls 13, objects 10

GPU time for the viewport (GL timer queries): 3.617 ms/frame

### 9. Lit view: dense mesh + 8 point lights

**14.30 ms/frame (70 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 13.083 | 91% |
| present (swap) | 0.500 | 3% |
| viewport render | 0.394 | 3% |
| ui build (panels) | 0.120 | 1% |
| selection outline | 0.095 | 1% |
| ui draw | 0.076 | 1% |
| path trace | 0.057 | 0% |
| gizmos | 0.036 | 0% |
| ui build (overlays) | 0.019 | 0% |
| particle draw | 0.017 | 0% |

Per frame: draw calls 16, triangles drawn 261120, objects 9

GPU time for the viewport (GL timer queries): 10.838 ms/frame

### 10. Picking: 50 click-selects per frame, 1000 objects

**29.53 ms/frame (34 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| picking (50 rays) | 17.851 | 60% |
| viewport render | 7.296 | 25% |
| gpu finish | 3.242 | 11% |
| gizmos | 1.274 | 4% |
| present (swap) | 0.505 | 2% |
| ui build (panels) | 0.183 | 1% |
| path trace | 0.158 | 1% |
| ui draw | 0.073 | 0% |
| ui build (overlays) | 0.015 | 0% |
| input+tools | 0.006 | 0% |

Per frame: draw calls 1012, triangles drawn 123888, objects 1000

GPU time for the viewport (GL timer queries): 2.523 ms/frame

### 11. Gizmo drag: rotating 200 objects with the Rotate handle

**6.28 ms/frame (159 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 2.821 | 45% |
| viewport render | 2.636 | 42% |
| selection outline | 0.856 | 14% |
| present (swap) | 0.398 | 6% |
| gizmos | 0.257 | 4% |
| ui build (panels) | 0.162 | 3% |
| ui draw | 0.073 | 1% |
| path trace | 0.066 | 1% |
| ui build (overlays) | 0.016 | 0% |
| input+tools | 0.008 | 0% |

Per frame: draw calls 413, triangles drawn 24732, objects 200

GPU time for the viewport (GL timer queries): 1.958 ms/frame

## One-shot operations

| Operation | ms |
|---|---:|
| Catmull-Clark (32k faces -> 131k quads) | 56.7 |
| buildRenderMesh, smooth (262k tris) | 22.8 |
| uniqueEdges (131k quads) | 43.4 |
| Smart UV unwrap (131k quads) | 89.0 |
| Pack UV islands (131k quads) | 72.8 |
| Raycast 100 rays vs 262k tris (brute force) | 254.9 |
| Picking BVH build (262k tris, once per edit) | 80.1 |
| Raycast 100 rays vs 262k tris (picking BVH) | 0.1 |
| Undo snapshot (scene with 131k-quad mesh) | 23.2 |
| Save .m3d (131k quads) | 142.3 |
| Load .m3d (131k quads) | 367.3 |
| Export OBJ (131k quads) | 233.7 |
| Import OBJ (131k quads) | 269.0 |
| Bind + automatic weights (24k quads, 20 bones) | 2.5 |
| CPU skinning x10 (24k quads, 20 bones) | 4.6 |
| Boolean difference (sphere 64x32 - cylinder 64) + clean-up | 560.1 |

## Multithreading (job system, 1 vs 12 threads)

Best of 3 runs each.

| Operation | 1 thread ms | 12 threads ms | speed-up |
|---|---:|---:|---:|
| CPU skinning x10 (24k quads, 20 bones) | 6.9 | 3.8 | 1.82x |
| Automatic weights (24k quads, 20 bones) | 8.0 | 1.8 | 4.54x |
| buildRenderMesh, smooth (262k tris) | 21.4 | 19.2 | 1.11x |
| Path-tracer scene build + BVH (262k tris) | 220.4 | 120.2 | 1.83x |

## Path tracing (320x200, 16 samples/pixel, 4644 triangles, max 4 bounces)

| Device | ms | Msamples/s |
|---|---:|---:|
| CPU, 1 thread | 2543 | 0.40 |
| CPU, 12 threads | 440 | 2.32 |
| GPU (Intel(R) Iris(R) Xe Graphics) | 81 | 12.70 |

GPU data uploaded for tracing: 0.88 MB (triangles, BVH, materials as RGBA32F textures) in 8.7 ms. GPU time per frame while tracing (timer queries): 16.7 ms of a 30 ms budget.
BVH of the 262k-triangle mesh: 157103 nodes.
