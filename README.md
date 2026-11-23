# MAGE v1 - Maxdemyd0's Advanced Game Engine

[![License: GPL v3+](https://img.shields.io/badge/License-GPLv3%2B-blue.svg)](LICENSE)

An open-source 3D game engine written from scratch in **C++20**, with gameplay scripting in **C# (.NET 8+, CoreCLR)**.
It does not fork any existing engine. Third-party libraries are used only as building blocks: GLFW, GLM, Jolt Physics, Dear ImGui, nlohmann/json, and .NET hosting.

v1 is the **Core Foundation**: a playable vertical slice where a 4-wheel Jolt raycast vehicle drives from C# around a
checkerboard testbed, while a background thread streams procedural city sectors in and out around the car.

## Quick start

Requirements: CMake 3.24+, a C++20 compiler (MSVC 19.3x+ / Clang 15+ / GCC 12+), an OpenGL 4.1+ GPU, and the
.NET 8+ SDK (optional; without it the car falls back to a native C++ controller).

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64   # or: -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
./build/bin/Release/MAGE_Runtime.exe
./build/bin/Release/MAGE_Editor.exe
```

The first configure downloads all dependencies with `FetchContent`. The build also compiles the C# assemblies
(`dotnet build`) into `bin/<Config>/Scripts/` and copies `assets/` and `nethost.dll` next to the executables.

### Controls

| Key | Action |
| --- | --- |
| W / S (or arrow keys) | Throttle / brake and reverse |
| A / D | Steer (speed-sensitive, from the C# script) |
| Space | Handbrake |
| R | Reset the vehicle |
| E | Spawn a crate (C# `Entity.Spawn`) |
| F1 | Jolt physics wireframes |
| F2 | Switch between chase and free-fly camera (free-fly: hold RMB + WASD/QE) |
| RMB drag / mouse wheel | Orbit / zoom the chase camera |
| Tab | Show or hide the vehicle tuning panel (runtime) |
| F12 | Save a screenshot (`.bmp`) |

### Runtime flags

`--no-vsync`, `--no-scripts`, `--no-streaming`, `--autodrive` (demo autopilot that feeds synthetic key presses through
the real C++ -> C# -> Jolt path), `--quit-after <seconds>`, `--screenshot <file.bmp>`.

Automated smoke test:

```powershell
./build/bin/Release/MAGE_Runtime.exe --autodrive --quit-after 14 --screenshot smoke.bmp
```

## Architecture

```
MAGE_Engine (static lib)
 src/core/        Engine (60 Hz fixed step + interpolated variable-rate render, profiling timers),
                  sparse-set ECS, Window, Input, Camera (+ chase / fly controllers), Log, Profiler
 src/renderer/    OpenGL 4.x forward PBR renderer: GGX sun + hemispheric ambient, shadow map with PCF,
                  procedural sky + fog, ACES tonemap, frustum culling, debug lines, JPH::DebugRenderer bridge
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

MAGE is free and open-source software, licensed under the **GNU General Public License v3.0 or later**
([LICENSE](LICENSE), SPDX `GPL-3.0-or-later`).

* You may use, study, modify and redistribute MAGE.
* If you distribute MAGE, a modified version of it, or a program built on it (including games that link the engine),
  you must release the complete corresponding source under the GPL as well. The engine and its derivatives cannot be
  made closed source.
* Contributions are accepted under the same license with a DCO sign-off and no CLA (see [CONTRIBUTING.md](CONTRIBUTING.md)).
  Copyright stays with each contributor, so the project as a whole cannot be relicensed as closed source later.

Third-party dependencies keep their own permissive licenses. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
