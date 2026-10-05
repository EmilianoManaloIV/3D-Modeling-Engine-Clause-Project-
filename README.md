# Modeler3D

A small 3D modeling program written in C++17 and OpenGL 3.3. It runs on Windows and Linux,
has **no third-party dependencies**, and builds into a single executable you can just run.

![Showcase scene: lights, emission, fire particles, hierarchy and parametric shapes](docs/showcase.png)

*Showcase scene, `Modeler3D --demo 1`. More screenshots are in [Screenshots](#screenshots) below.*

## Run it

| OS      | File                         | How                                              |
|---------|------------------------------|--------------------------------------------------|
| Windows | `dist/windows/Modeler3D.exe` | Double-click it. No installer, no DLLs.          |
| Linux   | `dist/linux/Modeler3D`       | `./Modeler3D` (needs X11 + an OpenGL 3.3 driver). |

You can also pass a file to open: `Modeler3D chair.m3d` or `Modeler3D model.obj`.

> The prebuilt Linux binary was built on Ubuntu 26.04, so it needs a recent glibc (2.43+).
> On older distributions, build it yourself with `./build_linux.sh`. It takes about 20 seconds.

## What it can do

**Modeling**
- **Parametric shapes**: cube, sphere, cylinder, cone/frustum, plane, torus, stairs, gear, pipe and
  spring. Every shape keeps its recipe, so you can change radius, segments, teeth, coils and so on at
  any time, plus a non-destructive modifier stack (subdivision levels, twist, taper). *Bake* turns the
  shape into a plain mesh; entering Edit mode bakes automatically (undo restores the recipe).
- **Object mode**: select (click, Shift+click, box), move / rotate / scale with axis locks and
  snapping, duplicate, delete, rename, and numeric location / rotation / scale.
- **Edit mode** (vertices): select, move / rotate / scale, **extrude** faces, delete, edit the
  median numerically. **Catmull-Clark subdivision** and **flip normals**.

**Hierarchy**
- Parent objects to each other (`Ctrl+P`, the last-clicked object becomes the parent) or clear the
  parent (`Alt+P`). Both keep each object where it is in the world.
- Children follow their parents: lights stuck to lamps, effects stuck to bones, groups under an
  **Empty**. The outliner shows the tree, and the viewport draws relationship lines.
- Deleting a parent hands its children to the grandparent. Duplicating keeps the links, and
  duplicating a whole rig remaps it to the copies.

**Lighting and materials**
- **Point, sun and spot lights** with color, intensity, range, cone angle and edge blend, plus a
  scene **ambient** color. Up to 8 lights are used in the **Lit** view.
- Materials have base color, **emission** (color + strength, glows in every view) and gloss.

**UVs**
- **Unwrap**: *Smart* (charts by face direction, packed without overlap), *Box*, *Planar*,
  *Cylindrical*, *Spherical*, *Per-face* (lightmap style). In Object mode it unwraps whole meshes;
  in Edit mode, only the selected faces.
- **Tools**: fit to 0-1, pack islands, rotate 90, flip U / V.
- A **UV editor** overlay shows the layout; drag selected faces in it to move their UVs.
- A **checker** view shows stretching and seams.
- UVs are stored per face corner, survive subdivision / extrude, and round-trip through `.m3d`
  and OBJ (`vt`).

**Bones and skinning**
- **Bones** are hierarchy objects: *Add Bone*, then *Extrude* to grow a chain from the tip.
- **Bind**: select the mesh and the root bone, then bind. The bind pose becomes the rest pose and
  weights are computed automatically (up to 4 bones per vertex).
- Pose by rotating bones (`R`). *Rest Pose* resets them.
- **Weights view** is a heat map of one bone's influence (select a bone, or pick it in
  Properties > Skin). In Edit mode you can **assign / remove** a weight on the selected vertices,
  **normalize**, or recompute automatic weights.

**Particles**
- **Emitters** with rate, lifetime, speed, spread cone, gravity, drag, start/end size and
  color, and spawn radius. *Glow* blends additively; *Smoke* is alpha-blended.
- They simulate live (Space plays / pauses) and are hierarchy objects, so you can parent fire to a
  torch or sparks to a bone.

**Unity-style transform tools**
- **Tools:** Hand / Move / Rotate / Scale / All (combined), on the palette at the left of the
  viewport or with `Q W E R Y`.
- **Handles you drag:**
  - Move: arrows (one axis), squares (two axes, on the camera-facing side), centre box (view plane).
  - Rotate: rings around each axis (only the front halves are drawn bright), an outer ring for the
    view axis, and trackball rotation from the inside.
  - Scale: cubes per axis and a centre box for uniform scale.
  - Hold `Ctrl` while dragging to snap.
- **Global / Local** orientation (`X`) and **Pivot / Center** handle position (`Z`), as in Unity. In
  Pivot mode, multiple objects rotate and scale around their own origins. The Scale handle always uses
  local axes.
- **Edit mode:** the same handles move vertices. Shift + drag on a Move handle **extrudes** the selected
  faces first (ProBuilder style).
- **Unity scene camera:**
  - Alt + left drag orbits, middle drag pans, Alt + right drag zooms, and the wheel zooms.
  - Hold the right button and use `W A S D Q E` to **fly**. It accelerates while held, Shift makes it
    faster, and scrolling while flying sets the fly speed.
  - Arrow keys move the camera.
  - The **scene gizmo** in the top-right corner works like Unity's: click an axis to look along it
    (switches to Iso), and click the centre or the Persp/Iso label to toggle projection.
  - `F`, the view buttons and **double-clicking an object in the hierarchy** frame it with an eased
    camera transition.
  - Field of view and fly speed are under *Mesh > Scene camera*.
- **Inspector:** shows Position / Rotation / Scale, with **Reset / Copy / Paste** (Paste goes to every
  selected object).
- **Scripting-style Transform API** in [src/transform.h](src/transform.h): `position / setPosition`,
  `rotation / setRotation`, `translate` and `rotate` in `Space::Self` or `Space::World`,
  `rotateAround`, `lookAt`, `forward / right / up`, `transformPoint / inverseTransformPoint`,
  `transformDirection`, `transformVector`, `lossyScale`, `reset`. The handles use it, and it is
  unit-tested.
- **Keymaps:** Unity (default) or Blender (G/R/S modal transforms). Choose in *File > Keymap*; the
  choice is saved in `Modeler3D.cfg` next to the executable.

**Everything else**
- **Undo / redo** (64 steps) covers every edit.
- **Save / load** `.m3d` scenes, and **export / import Wavefront OBJ** (+ `.mtl` with colors,
  emission and gloss). Exports bake transforms and the current pose.
- Shading modes **Studio / Lit / Checker / Weights** (`Z` cycles them).
- Also: perspective / ortho, front / right / top views, wireframe, grid, orientation gizmo,
  F12 PNG screenshots (compressed), an **F3 performance overlay**, and a warning before quitting with
  unsaved changes.

## Controls

**Unity keymap (default)**

| Input | Action |
|---|---|
| `Q` `W` `E` `R` `Y` | Hand / Move / Rotate / Scale / All tools |
| Drag a handle | Arrow = one axis, square = plane, ring = rotate, cube = scale. `Ctrl` snaps |
| Shift + drag a Move handle (Edit mode) | Extrude the selected faces, then move them |
| `X` / `Z` | Global-local axes / pivot-center |
| Alt + left drag, middle drag, Alt + right drag | Orbit, pan, zoom |
| Right drag + `W A S D Q E` | Fly the camera (accelerates; Shift = faster; scroll = fly speed) |
| Arrow keys | Move the camera |
| Scene gizmo (top right) | Click an axis: view along it (Iso); click the centre or label: Persp/Iso |
| Double-click in the hierarchy | Frame that object |
| Click / drag | Select / box select (Shift or Ctrl adds) |
| `Ctrl+D` / `Delete` | Duplicate / delete |
| `Ctrl+A` / `Ctrl+E` | Select all / extrude faces |
| `Shift+Z` | Cycle shading modes |

**Blender keymap**

| Input | Action |
|---|---|
| `G` / `R` / `S` | Move / rotate / scale. Then `X` `Y` `Z` locks an axis; click or Enter confirms, right click or Esc cancels |
| Right or middle drag | Orbit (Shift = pan); wheel zooms |
| `A` / `E` / `X` | Select all / extrude faces / delete |
| `Shift+D` | Duplicate |
| `Z` / `W` | Cycle shading / wireframe |

**Both keymaps**

| Input | Action |
|---|---|
| `Tab` | Object / Edit mode |
| `Ctrl+P` / `Alt+P` | Parent to the active object / clear parent |
| `U` / `Space` / `F` | Smart unwrap / play-pause particles / frame selection |
| `1` `3` `7` (Ctrl = opposite), `5` | Front / right / top view, perspective-orthographic |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo |
| `Ctrl+S` / `Ctrl+O` | Save / load the file named in the File tab |
| `F3` / `F12` / `F1` | Performance overlay / PNG screenshot / help |

The left panel has tabs: **Create, Mesh, UV, Rig, FX, File**. To change a number in the right
panel, drag it sideways (hold Shift for fine steps) or click it and type a value.

## Screenshots

### Unity-style transform handles

![Move handle on the selected cube, the tool palette, and the Reset / Copy / Paste inspector](docs/transform-tools.png)

*Default scene: the Move handle (arrows, plane squares, centre box), the orange selection outline,
and the tool palette.*

### UV editor

![UV editor overlay showing the Smart-unwrapped layout of a gear, with the checker texture on the model](docs/uv-editor.png)

*`Modeler3D --demo 3`: Smart unwrap of a gear. The checker view on the model and the packed layout
in the UV editor (selected faces in orange).*

### Skinned mesh

![A column bent by a two-bone chain, coloured by the weights of the tip bone](docs/skinned-mesh.png)

*`Modeler3D --demo 4`: a column skinned to two bones and bent by posing the tip bone. The Weights
view shows that bone's influence (blue 0 to red 1).*

## Performance

See [docs/performance.md](docs/performance.md) for the benchmark method, before/after numbers and the
bottlenecks that were fixed. For example, 1000 objects went from 187 ms to 12 ms per frame, and
loading a 131k-quad scene from 4.5 s to 0.13 s. Run it yourself with `Modeler3D --benchmark report.md`.

## Building from source

**Windows:** run `build_windows.bat`. It uses CLion's bundled MinGW / CMake / Ninja if CLion is
installed, or any CMake + MinGW-w64 / Visual Studio on the PATH. The result is
`dist\windows\Modeler3D.exe`, statically linked so it only depends on DLLs that ship with Windows.
The project also opens directly in CLion (`CMakeLists.txt`); pick the `Modeler3D` target.

**Linux:** install a compiler, CMake and the X11/OpenGL headers
(`sudo apt install build-essential cmake libx11-dev libgl-dev` on Debian/Ubuntu), then run
`./build_linux.sh`. The result is `dist/linux/Modeler3D`.

**Tests:** configure with `-DMODELER_BUILD_TESTS=ON` and run `modeler_tests`. Its 983 checks cover:
- math and TRS decomposition
- primitives and all 10 parametric shapes (closed, consistently oriented, outward-facing)
- Catmull-Clark (including UVs and weights), extrude / delete
- every UV unwrap method and island packing
- hierarchy (world matrices, keep-world parenting, cycle refusal)
- skinning (rest pose is exact, bending moves the right vertices)
- particles, ray picking, and `.m3d` / OBJ round trips
- the Unity-style Transform API, the id cache, ray/box rejection and indexed render data

## How the code maps to the Engine Books

| Topic | Code | Reference |
|---|---|---|
| Vectors, matrices, TRS transforms / decomposition, Euler angles | `src/math3d.h` | *Fundamentals of Computer Graphics* 5e ch. 2, 6, 7; *Game Engine Architecture* Vol. I ch. 5 |
| Camera, perspective / orthographic projection | `Camera` in `editor.cpp` | FoCG ch. 8 |
| Ray picking (unproject + ray/triangle) | `Editor::viewRay`, `raycastMesh`, `pickObject` | FoCG ch. 4 |
| Point / sun / spot lights, Blinn-Phong, ambient, emission | shaders in `renderer.cpp` | FoCG ch. 5; GEA Vol. II ch. 12 (12.4 shading equation, 12.5 lighting with rasterization) |
| GPU pipeline, vertex buffers, per-object mesh cache, point sprites | `renderer.cpp` | FoCG ch. 9, 17; GEA Vol. II sec. 11.4 |
| UV mapping, unwrapping, checker texture | `uv.cpp`, `renderer.cpp` | FoCG ch. 11 (texture mapping) |
| Indexed polygon meshes, subdivision, extrude | `mesh.cpp` | FoCG sec. 12.1 |
| Transform hierarchy / scene graph | `Scene::world`, `setParent` in `scene.cpp` | FoCG sec. 12.2; GEA Vol. II sec. 13.2 |
| Skeletons, poses, linear-blend skinning, matrix palette | `skin.cpp` | GEA Vol. II sec. 13.2-13.5 |
| Particle systems | `particles.cpp` | FoCG sec. 16.7; GEA Vol. II sec. 11.6, 14.4 (integration) |
| Parametric / procedural shapes, data-driven recipes | `parametric.cpp` | FoCG sec. 16.6; GEA Vol. II sec. 16.3 |
| Scene / object model, world editor | `scene.cpp`, `editor*.cpp` | GEA Vol. II ch. 16 (game world editor) |
| Unity-style Transform API, handles (ray-plane dragging) | `transform.cpp`, `editor_gizmo.cpp` | FoCG ch. 4, 7; GEA Vol. I sec. 5.3 |
| Profiler, benchmark, F3 overlay | `profiler.cpp`, `editor_bench.cpp` | GEA Vol. I sec. 2.3, ch. 10 |
| Platform layer (Win32/WGL, X11/GLX) | `platform_*.cpp` | GEA Vol. I sec. 1.5 |
| Input: per-frame pressed / held / released state | `platform.h` | GEA Vol. I ch. 9 (Human Interface Devices) |
| Main loop, startup / shutdown order, frame timing | `main.cpp` | GEA Vol. I sec. 6.1, ch. 8 |
| Immediate-mode UI, in-tool menus, debug overlays | `ui.cpp`, `editor_ui.cpp` | GEA Vol. I ch. 10; GEA Vol. II sec. 12.8 |
| Asset I/O (scene + OBJ) | `scene.cpp` | GEA Vol. I ch. 7 |

## Project layout

```
CMakeLists.txt        build definition (Windows: WIN32 GUI exe, static runtime; Linux: X11 + libGL)
build_windows.bat     one-click Windows build -> dist/windows/
build_linux.sh        one-command Linux build -> dist/linux/
src/
  main.cpp            entry point + main loop
  platform.h          window/input abstraction; platform_win32.cpp, platform_x11.cpp
  gl.h / gl.cpp       minimal OpenGL 3.3 function loader (no GLAD/GLEW)
  math3d.h            vector/matrix math
  mesh.*              mesh data (+UVs, bone weights), primitives, Catmull-Clark, extrude, ray casting
  uv.*                UV unwrapping, island packing, UV tools
  parametric.*        parametric shapes and the modifier stack
  scene.*             objects, hierarchy, lights, picking, .m3d and .obj file I/O
  skin.*              bones, binding, automatic weights, skinning
  particles.*         particle simulation
  renderer.*          shaders, lights, GPU buffers, drawing
  ui.*, font.*        immediate-mode GUI + built-in bitmap font
  editor.*            the application: frame loop, input, transform tool, undo
  editor_actions.cpp  commands (create, mesh/UV/rig tools, hierarchy, files, sample scenes)
  editor_ui.cpp       panels, outliner, properties, UV editor, dialogs
  editor_render.cpp   viewport drawing, selection outline, icons, particles
  editor_gizmo.cpp    Unity-style Move / Rotate / Scale handles, keymap settings
  editor_bench.cpp    --benchmark scenarios and report
  transform.*         Unity-style Transform API (world get/set, Translate, Rotate, LookAt...)
  profiler.*          scoped CPU profiler (F3 overlay, benchmark)
  image_io.*          PNG writer with its own DEFLATE compressor
docs/                 screenshots, performance.md, benchmark-report.md
tests/tests.cpp       unit tests
```

Sample scenes for a quick tour: `Modeler3D --demo 1` (showcase), `--demo 3` (UVs), `--demo 4` (rig).
