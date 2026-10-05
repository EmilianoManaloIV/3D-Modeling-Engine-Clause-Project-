# Modeler3D performance report

GPU: Intel(R) Iris(R) Xe Graphics, window 1728x1020, UI scale 3, vsync off, glFinish every frame.
Each scenario: 15 warm-up frames, then 90 measured frames.

## Scenarios

### 1. Default scene (baseline)

**3.59 ms/frame (278 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 2.387 | 66% |
| viewport render | 0.598 | 17% |
| ui draw | 0.344 | 10% |
| present (swap) | 0.120 | 3% |
| ui build (panels) | 0.098 | 3% |
| selection outline | 0.020 | 1% |
| gizmos | 0.013 | 0% |
| ui build (overlays) | 0.007 | 0% |

Per frame: draw calls 15, triangles drawn 12, objects 2

### 2. 1000 objects in hierarchies, all selected

**12.30 ms/frame (81 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 6.199 | 50% |
| gpu finish | 5.580 | 45% |
| selection outline | 1.402 | 11% |
| gizmos | 0.768 | 6% |
| ui build (panels) | 0.195 | 2% |
| ui draw | 0.085 | 1% |
| present (swap) | 0.071 | 1% |
| input+tools | 0.029 | 0% |
| ui build (overlays) | 0.027 | 0% |
| particle sim | 0.012 | 0% |

Per frame: draw calls 2013, triangles drawn 123888, objects 1000

### 3. Dense mesh idle (262k tris), selected

**8.81 ms/frame (113 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 7.927 | 90% |
| viewport render | 0.486 | 6% |
| ui draw | 0.201 | 2% |
| present (swap) | 0.089 | 1% |
| ui build (panels) | 0.080 | 1% |
| selection outline | 0.011 | 0% |

Per frame: draw calls 14, triangles drawn 261120, objects 1

### 4. Dense mesh idle (262k tris), not selected

**2.74 ms/frame (366 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 2.159 | 79% |
| viewport render | 0.372 | 14% |
| ui draw | 0.083 | 3% |
| present (swap) | 0.063 | 2% |
| ui build (panels) | 0.040 | 1% |

Per frame: draw calls 12, triangles drawn 261120, objects 1

### 5. Dense mesh, edit-mode vertex drag every frame

**32.43 ms/frame (31 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 17.858 | 55% |
| gpu finish | 12.863 | 40% |
| mesh rebuild+upload | 11.264 | 35% |
| edit overlay | 6.439 | 20% |
| overlay upload | 1.886 | 6% |
| ui build (overlays) | 0.457 | 1% |
| input+tools | 0.445 | 1% |
| ui build (panels) | 0.271 | 1% |
| ui draw | 0.111 | 0% |
| present (swap) | 0.099 | 0% |

Per frame: mesh uploads 1, uploaded vertices 132602, draw calls 14, triangles drawn 261120, objects 1

### 6. Skinned mesh idle (49k tris, 2 bones)

**3.59 ms/frame (278 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 2.781 | 77% |
| viewport render | 0.499 | 14% |
| ui draw | 0.134 | 4% |
| present (swap) | 0.081 | 2% |
| ui build (panels) | 0.070 | 2% |
| gizmos | 0.011 | 0% |

Per frame: draw calls 13, triangles drawn 49152, objects 3

### 7. Skinned mesh, posing a bone every frame

**6.14 ms/frame (163 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| viewport render | 3.642 | 59% |
| mesh rebuild+upload | 3.188 | 52% |
| gpu finish | 2.191 | 36% |
| skinning (CPU) | 0.286 | 5% |
| ui draw | 0.127 | 2% |
| present (swap) | 0.075 | 1% |
| ui build (panels) | 0.071 | 1% |
| gizmos | 0.013 | 0% |
| ui build (overlays) | 0.006 | 0% |

Per frame: mesh uploads 1, uploaded vertices 27237, draw calls 13, triangles drawn 49152, objects 3

### 8. Particles: 10 emitters, ~50k live

**5.64 ms/frame (177 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 3.775 | 67% |
| viewport render | 1.414 | 25% |
| particle draw | 0.901 | 16% |
| ui draw | 0.203 | 4% |
| particle sim | 0.080 | 1% |
| present (swap) | 0.075 | 1% |
| ui build (panels) | 0.064 | 1% |
| gizmos | 0.037 | 1% |

Per frame: draw calls 13, objects 10

### 9. Lit view: dense mesh + 8 point lights

**11.36 ms/frame (88 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 10.383 | 91% |
| viewport render | 0.686 | 6% |
| ui draw | 0.099 | 1% |
| ui build (panels) | 0.080 | 1% |
| present (swap) | 0.075 | 1% |
| gizmos | 0.024 | 0% |
| selection outline | 0.014 | 0% |
| ui build (overlays) | 0.005 | 0% |

Per frame: draw calls 16, triangles drawn 261120, objects 9

### 10. Picking: 50 click-selects per frame, 1000 objects

**17.00 ms/frame (59 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| picking (50 rays) | 9.325 | 55% |
| viewport render | 4.009 | 24% |
| gpu finish | 3.315 | 20% |
| gizmos | 0.657 | 4% |
| ui build (panels) | 0.115 | 1% |
| present (swap) | 0.066 | 0% |
| ui draw | 0.061 | 0% |

Per frame: draw calls 1012, triangles drawn 123888, objects 1000

### 11. Gizmo drag: rotating 200 objects with the Rotate handle

**4.89 ms/frame (205 fps)** over 90 frames

| Section | ms/frame | share |
|---|---:|---:|
| gpu finish | 2.988 | 61% |
| viewport render | 1.605 | 33% |
| selection outline | 0.259 | 5% |
| gizmos | 0.148 | 3% |
| ui build (panels) | 0.095 | 2% |
| ui draw | 0.079 | 2% |
| present (swap) | 0.062 | 1% |

Per frame: draw calls 413, triangles drawn 24732, objects 200

## One-shot operations

| Operation | ms |
|---|---:|
| Catmull-Clark (32k faces -> 131k quads) | 29.9 |
| buildRenderMesh, smooth (262k tris) | 9.6 |
| uniqueEdges (131k quads) | 20.8 |
| Smart UV unwrap (131k quads) | 39.6 |
| Pack UV islands (131k quads) | 39.0 |
| Raycast 100 rays vs 262k tris | 118.7 |
| Undo snapshot (scene with 131k-quad mesh) | 11.4 |
| Save .m3d (131k quads) | 68.0 |
| Load .m3d (131k quads) | 127.2 |
| Export OBJ (131k quads) | 95.1 |
| Import OBJ (131k quads) | 130.8 |
| Bind + automatic weights (24k quads, 20 bones) | 2.8 |
| CPU skinning x10 (24k quads, 20 bones) | 3.3 |
