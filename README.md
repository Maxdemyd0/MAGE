# MAGE v1 - Maxdemyd0's Advanced Game Engine

[![License: LGPL v3+](https://img.shields.io/badge/License-LGPLv3%2B-blue.svg)](LICENSE)

An open-source 3D game engine written from scratch in **C++20**, with gameplay scripting in **C# (.NET 8+, CoreCLR)**.
It does not fork any existing engine. Third-party libraries are used only as building blocks: GLFW, GLM, Jolt Physics, Dear ImGui, nlohmann/json, and .NET hosting.

v1 is the **Core Foundation**: a playable vertical slice where a 4-wheel Jolt raycast vehicle drives from C# around a
checkerboard testbed, while a background thread streams procedural city sectors in and out around the car.
On GPUs with hardware ray tracing, the scene is **path traced in real time**: Vulkan ray queries run on the RT cores,
with an SVGF-style denoiser. Other GPUs fall back to the OpenGL PBR rasterizer.

## Quick start

Requirements: CMake 3.24+, a C++20 compiler (MSVC 19.3x+ / Clang 15+ / GCC 12+), an OpenGL 4.1+ GPU, and the
.NET 8+ SDK (optional; without it the car falls back to a native C++ controller). Path tracing additionally needs a
Vulkan 1.2 GPU with `VK_KHR_ray_query` (NVIDIA RTX 20-series / AMD RX 6000 / Intel Arc or newer). No Vulkan SDK is
required, because the headers, volk, VMA and glslang are fetched and built automatically.

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64   # or: -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
./build/bin/Release/MAGE_Runtime.exe
./build/bin/Release/MAGE_Editor.exe
```

The first configure downloads all dependencies with `FetchContent`. The build also compiles the C# assemblies
(`dotnet build`) into `bin/<Config>/Scripts/` and copies `assets/` and `nethost.dll` next to the executables.
`MAGE_Engine` is built as a shared library by default. The executables load `MAGE_Engine.dll` from their own folder.
Pass `-DMAGE_BUILD_SHARED=OFF` to link it statically instead.

### Controls

| Key | Action |
| --- | --- |
| W / S (or arrow keys) | Throttle / brake and reverse |
| A / D | Steer (speed-sensitive, from the C# script) |
| Space | Handbrake |
| R | Reset the vehicle |
| E | Spawn a crate (C# `Entity.Spawn`) |
| F1 | Jolt physics wireframes |
| F3 | Switch between hardware path tracing and the raster renderer |
| F2 | Switch between chase and free-fly camera (free-fly: hold RMB + WASD/QE) |
| RMB drag / mouse wheel | Orbit / zoom the chase camera |
| Tab | Show or hide the vehicle tuning panel (runtime) |
| F12 | Save a screenshot (`.bmp`) |

### Runtime flags

`--no-vsync`, `--max-fps <n>` (default 240), `--raster`, `--no-scripts`, `--no-streaming`, `--autodrive` (demo autopilot that feeds synthetic key presses through
the real C++ -> C# -> Jolt path), `--quit-after <seconds>`, `--screenshot <file.bmp>`.

Automated smoke test:

```powershell
./build/bin/Release/MAGE_Runtime.exe --autodrive --quit-after 14 --screenshot smoke.bmp
```

## Architecture

```
MAGE_Engine (shared library: MAGE_Engine.dll / .so; exports the MAGE_API classes and Dear ImGui)
 src/core/        Engine (60 Hz fixed step + interpolated variable-rate render, profiling timers),
                  sparse-set ECS, Window, Input, Camera (+ chase / fly controllers), Log, Profiler
 src/renderer/    OpenGL 4.x forward PBR renderer: GGX sun + hemispheric ambient, shadow map with PCF,
                  procedural sky + fog, ACES tonemap, frustum culling, debug lines, JPH::DebugRenderer bridge
 src/raytracing/  Vulkan hardware path tracer (headless device, BLAS per mesh, TLAS rebuilt per frame,
                  2 frames in flight, result displayed through OpenGL). Shaders live in shaders/*.comp
 src/physics/     PhysicsWorld over JPH::PhysicsSystem: layers STATIC_WORLD / DYNAMIC_PROP / VEHICLE / RAGDOLL,
                  raycasts, CreateRaycastVehicle() (VehicleConstraint + WheeledVehicleController, AWD, anti-roll bars)
 src/streaming/   SectorStreamer: std::jthread worker loads sector_<x>_<z>.json (or generates a city block),
                  main thread integrates a bounded number per frame and unloads with hysteresis
 src/scripting/   ScriptHost: nethost -> hostfxr -> CoreCLR; native API table <-> managed callbacks
 src/scene/       Testbed: the vertical-slice scene shared by runtime and editor
MAGE_Runtime      src/main.cpp            game window + HUD + live tuning sliders
MAGE_Editor       src/editor/EditorMain.cpp  docking editor: viewport, hierarchy, inspector, physics, profiler...
scripting/Mage.Core     C# SDK (Entity, Vector3, Quaternion, Input, VehicleController, Physics, Log, InternalCalls)
scripting/GameScripts   TestVehicleScript.cs
assets/sectors          hand-authored sector JSON (other sectors are generated procedurally)
```

### Frame

```
poll input
while accumulator >= 1/60:     C# OnFixedUpdate -> vehicle inputs -> Jolt step (job system) -> capture poses
interpolate render transforms  (alpha = accumulator / dt)
C# OnUpdate -> camera -> sector streaming -> deferred entity destruction
render: cull -> shadow pass -> PBR pass -> sky -> debug lines -> ImGui -> swap
```

### Path tracer

`Renderer` hands its draw list to `PathTracer` (`src/raytracing/`). Each frame goes through these steps:

1. **TLAS rebuild** over per-mesh BLASes, with one instance per draw command. Material data goes in a parallel buffer.
2. **`pathtrace.comp`**: ray queries (`VK_KHR_ray_query`) trace up to N bounces. Diffuse is Lambert and specular is GGX
   (sampled from the visible normal distribution, VNDF). The sun is a finite-size disc sampled directly at every
   bounce (next-event estimation), which gives soft shadows. The sky acts as an environment light, emissive surfaces
   emit light, and Russian roulette plus a firefly clamp keep noise in check. Output is noise-free albedo/emission
   plus the lighting with albedo divided out (demodulated), which is easier to denoise.
3. **`temporal.comp`**: reprojects last frame's history and validates it with position/normal checks. While the camera
   and scene are still (within `staticTolerance`), accumulation is unbounded and converges to the reference image.
4. **`atrous.comp`** (×5): 3×3 edge-aware à-trous wavelet filter, guided by normals, depth and the variance of each
   pixel's brightness.
5. **`composite.comp`**: multiplies the albedo back in, adds emission and fog, then ACES tonemapping. The result goes
   to an OpenGL texture.

The CPU never blocks on the GPU. Two frames are kept in flight, and the screen shows the newest finished frame.
On an RTX 4070 Ti SUPER the testbed (about 740 instances) traces at 1600×900 in about 2.5 ms of GPU time.
All settings are live in the **Path Tracer** panel. Shaders are compiled to SPIR-V at build time by glslang into
`bin/<Config>/shaders/`.

### C++ <-> C# interop

`src/scripting/InteropTypes.h` and `scripting/Mage.Core/InternalCalls.cs` define the same ABI:

* **Native -> managed bootstrap:** `hdt_get_function_pointer` resolves
  `Mage.Interop.ScriptEngine.Initialize` (an `[UnmanagedCallersOnly]` method). The engine passes it a `NativeBindings`
  table and gets `ManagedCallbacks` back (`CreateScript`, `DestroyScript`, `OnUpdate`, `OnFixedUpdate`, `Shutdown`).
* **Managed -> native:** the table holds raw function pointers (`delegate* unmanaged<...>`) for
  `GetEntityTransform`, `SetEntityTransform`, `ApplyVehicleInput`, `GetVehicleState`, `RaycastWorld`, `SpawnEntity`,
  `DestroyEntity`, `IsKeyDown`, `IsKeyPressed`, `EntityExists` and `Log`.
* Both sides check the struct size and the `kInteropVersion` number at startup. C++ `static_assert`s pin the layout.

To add a gameplay script, derive from `Mage.Entity` in `scripting/GameScripts`, then attach it from C++ with
`registry.Add<ScriptComponent>(e).className = "GameScripts.MyScript";` (or pass the name to `Engine::SpawnVehicle`).

### Sector JSON

```json
{ "sector": [1, 0],
  "entities": [ { "name": "Tower", "mesh": "cube", "position": [-30, 15, 34], "rotation": [0, 0, 0],
                  "scale": [10, 30, 10], "color": [0.5, 0.52, 0.56], "metallic": 0.3, "roughness": 0.5,
                  "physics": "static", "mass": 0 } ] }
```

Positions are relative to the sector centre. The default sector size is 96 m. `physics` is one of `static`, `dynamic` or `none`.
The available meshes are `cube`, `sphere`, `cylinder`, `cone` and `plane`.

## License

MAGE is free and open-source software, licensed under the **GNU Lesser General Public License v3.0 or later**
(SPDX `LGPL-3.0-or-later`). The LGPL is a set of additional permissions on top of the GPL: see [LICENSE](LICENSE)
(LGPLv3) together with [COPYING](COPYING) (GPLv3).

* **The engine stays open.** If you distribute MAGE or a modified version of it, including the C++ engine and the
  `Mage.Core` C# SDK, you must publish the source of those changes under the LGPL. Nobody can turn MAGE into a
  closed-source fork.
* **Your game can be closed source.** Games and tools that only *use* MAGE can ship under any license, including
  proprietary or commercial ones, provided you:
  * include the LGPL/GPL texts and a notice that MAGE is used,
  * keep MAGE replaceable, so a user can swap in a modified build. MAGE does this for you by default:
    `MAGE_Engine` is a shared library (`MAGE_Engine.dll` / `libMAGE_Engine.so`), so shipping that DLL unmodified
    next to your game is enough. With `-DMAGE_BUILD_SHARED=OFF` (static linking), you must also ship your game's
    object files or another way to relink,
  * provide (or offer) the source of the MAGE version you shipped.
  C# gameplay scripts that reference `Mage.Core.dll` are separate assemblies and can use any license.
* Contributions are accepted under the same license with a DCO sign-off and no CLA (see [CONTRIBUTING.md](CONTRIBUTING.md)).
  Copyright stays with each contributor, so the project as a whole cannot be relicensed as closed source later.

Third-party dependencies keep their own permissive licenses. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
