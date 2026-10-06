# Modeler3D stress test

GPU: Intel(R) Iris(R) Xe Graphics, 12 CPU threads, window 1728x1020. Frame times include the GPU (glFinish every frame).

**Failures: 0** (invalid meshes, failed checks, problems found while fuzzing).

## A. Mesh size ladder

A UV sphere, Catmull-Clark subdivided once per level. *Idle* = the dense mesh selected in Object mode; *drag* = Edit mode with every vertex selected and moved each frame (what dragging a handle does). Frame times include the GPU (glFinish).

| Triangles | Build ms | First frame | Idle frame | Drag frame | Pick BVH | Click pick | Undo (1st / again) | Edge loop | Bevel loop | Loop cut | Save | Load | Path-trace BVH | Memory MB |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 3968 | 0.79 | 90.8 | 5.57 | 3.99 | 2.17 | 0.03 | 0.76 / 0.00 | 0.15 | 1.73 | 0.30 | 2.98 | 15.8 | 2.60 | 72 |
| 16128 | 4.05 | 5.56 | 4.49 | 5.56 | 8.65 | 0.03 | 1.41 / 0.01 | 0.38 | 3.12 | 1.32 | 12.9 | 23.8 | 14.9 | 77 |
| 64512 | 17.1 | 14.4 | 5.23 | 9.09 | 19.9 | 0.09 | 5.19 / 0.03 | 1.12 | 8.83 | 5.64 | 33.1 | 49.9 | 27.3 | 97 |
| 258048 | 52.5 | 37.4 | 5.89 | 26.0 | 69.4 | 0.29 | 18.4 / 0.06 | 4.16 | 28.8 | 15.7 | 75.3 | 172.9 | 143.7 | 177 |
| 1032192 | 408.7 | 307.2 | 9.61 | 169.8 | 715.5 | 2.47 | 182.9 / 0.43 | 41.9 | 208.3 | 103.1 | 551.8 | 917.4 | 882.4 | 490 |
| 4128768 | 1818 | 836.1 | 20.5 | 642.5 | 4751 | 20.7 | 1313 / 1.39 | 130.8 | 930.6 | 783.1 | - | - | - | 1686 |

Largest mesh still at 30+ fps idle: **4128768 triangles**; largest mesh whose vertices can all be dragged at 10+ fps: **258048 triangles**.

## B. Object count ladder

Cubes on a grid, each with its own mesh. *Rotate* = all of them rotated by the transform tool every frame.

| Objects | Create ms | First frame | Idle frame | All selected | Rotate all | Duplicate all | Undo it | Delete all | MB: objects / +drawn / end |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1000 | 7.00 | 332.4 | 10.8 | 14.7 | 16.7 | 10.6 | 9.78 | 2.97 | 6 / 3 / 668 |
| 5000 | 24.8 | 349.6 | 43.1 | 61.6 | 49.5 | 44.1 | 46.8 | 9.06 | 12 / 15 / 668 |
| 20000 | 98.9 | 1455 | 128.2 | 156.6 | 155.2 | 177.2 | 168.1 | 47.6 | 47 / 132 / 861 |
| 50000 | 234.9 | 2780 | 351.6 | 425.7 | 409.5 | 733.8 | 546.1 | 132.9 | 103 / 250 / 1096 |

Most objects still at 30+ fps idle: **1000**.

## C. Deep hierarchies

A chain of empties, each parented to the previous one, the deepest selected. Checks: world matrix of the deepest equals the batch computation, descendants of the root, parenting the root under the deepest is refused (cycle), deleting the root keeps the rest.

| Depth | Create ms | First frame | Idle frame | world(deepest) | All worlds | Descendants | Cycle refused | Transform roots | Delete root | Checks |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 1000 | 2.09 | 6.52 | 5.69 | 0.49 | 0.58 | 0.46 | 0.72 | 0.06 | 0.61 | pass |
| 10000 | 23.1 | 42.6 | 36.1 | 4.67 | 7.74 | 1.63 | 0.66 | 0.38 | 7.71 | pass |
| 50000 | 115.6 | 221.0 | 174.5 | 19.1 | 19.7 | 8.91 | 11.4 | 7.85 | 47.8 | pass |

## D. Tool fuzzing

6000 random operations on 10 kinds of mesh (closed, open, n-gons, a single triangle, degenerate faces, two separate pieces) with random selections and settings. After every operation the mesh is checked: indices in range, faces of 3+ distinct corners, UV / weight arrays in step, finite positions; and tools that should keep a closed mesh closed are checked for that.

| Tool | Runs | Done | Refused | Invalid mesh | Closed mesh opened | Avg ms | Max ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| bevel edges | 255 | 224 | 31 | 0 | 0 | 1.20 | 28.2 |
| bevel verts | 287 | 287 | 0 | 0 | 0 | 0.74 | 31.9 |
| boolean diff | 257 | 113 | 144 | 0 | 0 | 12.5 | 1864 |
| bridge faces | 241 | 30 | 211 | 0 | 0 | 0.05 | 1.46 |
| bridge loops | 271 | 0 | 271 | 0 | 0 | 0.03 | 0.60 |
| catmull-clark | 290 | 290 | 0 | 0 | 0 | 0.23 | 5.28 |
| clean up | 286 | 286 | 0 | 0 | 0 | 1.29 | 25.8 |
| connect | 252 | 134 | 118 | 0 | 0 | 0.06 | 3.38 |
| delete faces | 269 | 269 | 0 | 0 | 0 | 0.01 | 0.53 |
| extrude edges | 303 | 303 | 0 | 0 | 0 | 0.04 | 1.77 |
| extrude faces | 273 | 273 | 0 | 0 | 0 | 0.08 | 0.91 |
| fill | 295 | 35 | 260 | 0 | 0 | 0.04 | 1.08 |
| flip | 287 | 287 | 0 | 0 | 0 | 0.00 | 0.04 |
| inset | 273 | 273 | 0 | 0 | 0 | 0.12 | 3.75 |
| loop cut | 263 | 263 | 0 | 0 | 0 | 0.07 | 1.27 |
| merge | 274 | 200 | 74 | 0 | 0 | 0.09 | 1.70 |
| poke | 287 | 287 | 0 | 0 | 0 | 0.07 | 1.38 |
| push through | 266 | 12 | 254 | 0 | 0 | 0.06 | 1.80 |
| subdivide | 281 | 281 | 0 | 0 | 0 | 0.23 | 9.78 |
| triangulate | 262 | 240 | 22 | 0 | 3 | 0.29 | 16.1 |
| tris to quads | 260 | 53 | 207 | 0 | 0 | 0.08 | 5.31 |
| unwrap | 268 | 268 | 0 | 0 | 0 | 0.07 | 1.20 |

Invalid meshes: **0**. Closed meshes opened by a tool that should keep them closed: **3**.
- triangulate on sphere: a closed mesh is no longer closed
- triangulate on sphere: a closed mesh is no longer closed
- triangulate on cylinder: a closed mesh is no longer closed

## E. Input fuzzing

3000 frames of random mouse moves / clicks / drags, wheel and trackpad gestures, key presses with random Ctrl / Shift / Alt, typed numbers and expressions (including 1/0, 0/0, 1e30, nan), plus 455 random commands from the command registry. After every frame: finite camera and transforms, valid meshes, a consistent mode.

Frame time: average 3.34 ms, 95th percentile 5.19 ms, worst 3920 ms. Scene at the end: 20 objects, 1552 triangles.

Problems found: **0**

### Undo / redo round trip

29 random commands (21 undo steps), all undone, all redone: back to the start **yes**, forward to the end **yes**.
Undo history memory with shared meshes: 0.9 MB.

## F. Layout extremes

Every window size at UI sizes 1, 3 and 6: all panels inside the window, the viewport keeps a size, the scene stays valid.

| Window | UI size | Frame ms | Result |
|---|---:|---:|---|
| 320x240 | 1 | 1.63 | ok |
| 320x240 | 3 | 1.36 | ok |
| 320x240 | 6 | 2.62 | ok |
| 640x480 | 1 | 0.57 | ok |
| 640x480 | 3 | 0.72 | ok |
| 640x480 | 6 | 0.78 | ok |
| 1024x600 | 1 | 1.07 | ok |
| 1024x600 | 3 | 1.26 | ok |
| 1024x600 | 6 | 0.65 | ok |
| 1920x1080 | 1 | 1.30 | ok |
| 1920x1080 | 3 | 1.08 | ok |
| 1920x1080 | 6 | 3.97 | ok |
| 3840x2160 | 1 | 1.77 | ok |
| 3840x2160 | 3 | 2.40 | ok |
| 3840x2160 | 6 | 6.11 | ok |

## G. Extreme values

| Case | ms | Result |
|---|---:|---|
| Frame an object at 1e6 | 0.00 | ok |
| Camera at the minimum distance | 0.00 | ok |
| Camera at the maximum distance | 0.00 | ok |
| Render data of a 20000-corner star polygon | 2601 | ok |
| Triangulate it (ear clipping) | 2654 | ok |
| Inset it | 27.5 | ok |
| Bevel 8 pole vertices of a sphere | 1.24 | ok |
