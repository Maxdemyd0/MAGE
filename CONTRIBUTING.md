# Contributing to MAGE

Thanks for helping build MAGE. This document covers how to contribute.

## License of contributions

MAGE is licensed under **LGPL-3.0-or-later**. When you submit a contribution, you license it under the same terms
(inbound = outbound). There is **no CLA** and copyright is **not** assigned to anyone. Every contributor keeps the
copyright on their own work. Because ownership stays spread across contributors, nobody can relicense the project
as closed source later.

### Developer Certificate of Origin

Sign off every commit (`git commit -s`). The sign-off certifies that you have the right to submit the work under the
project license, as defined by the [Developer Certificate of Origin 1.1](https://developercertificate.org/):

```
Signed-off-by: Your Name <you@example.com>
```

## Workflow

1. Fork the repository and create a feature branch.
2. Build and run the smoke test before opening a pull request:
   ```powershell
   cmake -S . -B build && cmake --build build --config Release --parallel
   ./build/bin/Release/MAGE_Runtime.exe --autodrive --quit-after 14
   ```
3. New source files must start with the SPDX header:
   ```cpp
   // SPDX-License-Identifier: LGPL-3.0-or-later
   // Copyright (C) <year> <your name> and MAGE contributors
   ```
4. If you change the C++ <-> C# ABI, update `src/scripting/InteropTypes.h` and
   `scripting/Mage.Core/InternalCalls.cs` in the same commit, and bump `kInteropVersion` / `InteropVersion`.
5. Only add new third-party dependencies if their license is LGPL-3.0-compatible. List each one in
   `THIRD_PARTY_NOTICES.md`.

## Style

* C++20. Follow the existing style: 4-space indent, `PascalCase` types and functions, `m_` member prefix.
* Keep Jolt, GL and .NET types out of public headers where practical.
* Mark public engine classes and free functions with `MAGE_API` (from `core/Export.h`). Without it they are not
  exported from `MAGE_Engine.dll`. Don't put mutable `static` data in header-only code, because each DLL/EXE would
  get its own copy.
* Do not let exceptions cross the managed/native boundary.
