// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - embedded .NET CoreCLR scripting host.
//
// Boot sequence (see ScriptHost.cpp):
//   nethost:get_hostfxr_path -> hostfxr_initialize_for_runtime_config(Mage.Core.runtimeconfig.json)
//   -> hdt_load_assembly(Mage.Core.dll, GameScripts.dll) -> hdt_get_function_pointer(ScriptEngine.Initialize)
//   -> Initialize(&NativeBindings, &ManagedCallbacks)
//
// After that the engine drives C# entirely through unmanaged function pointers:
//   CreateScript(entity, "Namespace.Type") -> Entity.OnCreate()
//   OnUpdate(dt) / OnFixedUpdate(fixedDt) -> every live script instance

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/ECS.h"
#include "scripting/InteropTypes.h"

namespace mage {

class Engine;

class MAGE_API ScriptHost {
public:
    ScriptHost() = default;
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    // Returns false (and fills LastError) when .NET or the script assemblies are unavailable.
    bool Init(Engine& engine, const std::filesystem::path& scriptDirectory);
    void Shutdown();
    bool IsReady() const { return m_ready; }

    bool CreateInstance(Entity entity, const std::string& className);
    void DestroyInstance(Entity entity);
    void Update(float dt);
    void FixedUpdate(float fixedDt);

    const std::string& LastError() const { return m_lastError; }
    const std::filesystem::path& ScriptDirectory() const { return m_scriptDirectory; }
    uint32_t InstanceCount() const { return m_instanceCount; }

private:
    bool Fail(std::string message);

    interop::ManagedCallbacks m_callbacks{};
    std::filesystem::path m_scriptDirectory;
    std::string m_lastError;
    void* m_hostfxrLibrary = nullptr;
    uint32_t m_instanceCount = 0;
    bool m_ready = false;
};

} // namespace mage
