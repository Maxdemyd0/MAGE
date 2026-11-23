// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "scripting/ScriptHost.h"
#include "core/Engine.h"
#include "core/Log.h"
#include "physics/PhysicsWorld.h"

#include <string_view>

#ifdef MAGE_HAS_DOTNET
#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace mage {

using namespace interop;

// ============================================================================ Native API for C#
namespace {

Engine* s_engine = nullptr;

glm::vec3 ToGlm(const NVec3& v) { return glm::vec3(v.x, v.y, v.z); }
glm::quat ToGlm(const NQuat& q) { return glm::quat(q.w, q.x, q.y, q.z); }
NVec3 ToNative(const glm::vec3& v) { return NVec3{v.x, v.y, v.z}; }
NQuat ToNative(const glm::quat& q) { return NQuat{q.x, q.y, q.z, q.w}; }

void Native_Log(int32_t level, const char* message) {
    const std::string_view text = message ? message : "";
    switch (level) {
        case 0: Log::Trace("[C#] {}", text); break;
        case 2: Log::Warn("[C#] {}", text); break;
        case 3: Log::Error("[C#] {}", text); break;
        default: Log::Info("[C#] {}", text); break;
    }
}

void Native_GetEntityTransform(uint32_t entity, NVec3* outPosition, NQuat* outRotation) {
    glm::vec3 p(0.0f);
    glm::quat r(1.0f, 0.0f, 0.0f, 0.0f);
    s_engine->GetEntityTransform(entity, p, r);
    if (outPosition) *outPosition = ToNative(p);
    if (outRotation) *outRotation = ToNative(r);
}

void Native_SetEntityTransform(uint32_t entity, const NVec3* position, const NQuat* rotation) {
    glm::vec3 p(0.0f);
    glm::quat r(1.0f, 0.0f, 0.0f, 0.0f);
    s_engine->GetEntityTransform(entity, p, r);
    if (position) p = ToGlm(*position);
    if (rotation) r = glm::normalize(ToGlm(*rotation));
    s_engine->SetEntityTransform(entity, p, r);
}

void Native_ApplyVehicleInput(uint32_t entity, float throttle, float steering, float brake, float handBrake) {
    if (auto* v = s_engine->GetRegistry().TryGet<VehicleComponent>(entity)) {
        v->throttle = throttle;
        v->steering = steering;
        v->brake = brake;
        v->handBrake = handBrake;
    }
}

int32_t Native_GetVehicleState(uint32_t entity, NVehicleState* out) {
    const auto* v = s_engine->GetRegistry().TryGet<VehicleComponent>(entity);
    if (!v || !out) return 0;
    const VehicleTelemetry t = s_engine->GetPhysics().GetVehicleTelemetry(v->vehicle);
    *out = NVehicleState{t.speedKmh, t.forwardSpeed, t.engineRPM, t.gear, t.wheelsInContact};
    return 1;
}

int32_t Native_RaycastWorld(const NVec3* origin, const NVec3* direction, float maxDistance, uint32_t ignoreEntity,
                            NRaycastHit* outHit) {
    if (!origin || !direction) return 0;
    glm::vec3 dir = ToGlm(*direction);
    const float len = glm::length(dir);
    if (len < 1e-6f) return 0;
    dir /= len;

    BodyHandle ignore = InvalidBody;
    if (const auto* rb = s_engine->GetRegistry().TryGet<RigidBodyComponent>(ignoreEntity)) ignore = rb->body;

    RaycastHit hit;
    if (!s_engine->GetPhysics().Raycast(ToGlm(*origin), dir, maxDistance, hit, ignore)) return 0;
    if (outHit) {
        const auto entity = static_cast<Entity>(hit.userData);
        *outHit = NRaycastHit{ToNative(hit.point), ToNative(hit.normal), hit.distance,
                              s_engine->GetRegistry().Valid(entity) ? entity : NullEntity};
    }
    return 1;
}

uint32_t Native_SpawnEntity(const char* mesh, const NVec3* position, const NQuat* rotation, const NVec3* scale,
                            int32_t dynamic) {
    SpawnDesc d;
    d.name = mesh ? std::string("Spawned_") + mesh : "Spawned";
    d.mesh = mesh ? mesh : "cube";
    if (position) d.position = ToGlm(*position);
    if (rotation) d.rotation = glm::normalize(ToGlm(*rotation));
    if (scale) d.scale = ToGlm(*scale);
    d.albedo = glm::vec3(0.9f, 0.55f, 0.15f);
    d.roughness = 0.5f;
    d.physics = dynamic ? SpawnPhysics::Dynamic : SpawnPhysics::Static;
    return s_engine->SpawnPrimitive(d);
}

void Native_DestroyEntity(uint32_t entity) { s_engine->DestroyEntity(entity); }
int32_t Native_IsKeyDown(int32_t key) { return s_engine->GetInput().IsKeyDown(key) ? 1 : 0; }
int32_t Native_IsKeyPressed(int32_t key) { return s_engine->GetInput().IsKeyPressed(key) ? 1 : 0; }
int32_t Native_EntityExists(uint32_t entity) { return s_engine->GetRegistry().Valid(entity) ? 1 : 0; }

#ifdef MAGE_HAS_DOTNET

#if defined(_WIN32)
#define MAGE_STR(s) L##s
std::string Narrow(const char_t* s) {
    if (!s) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len > 0 ? len - 1 : 0), '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), len, nullptr, nullptr);
    return out;
}
void* LoadLib(const char_t* path) { return reinterpret_cast<void*>(::LoadLibraryW(path)); }
void* GetExport(void* lib, const char* name) {
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(lib), name));
}
#else
#define MAGE_STR(s) s
std::string Narrow(const char_t* s) { return s ? std::string(s) : std::string(); }
void* LoadLib(const char_t* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void* GetExport(void* lib, const char* name) { return dlsym(lib, name); }
#endif

void HOSTFXR_CALLTYPE HostErrorWriter(const char_t* message) { Log::Error("[hostfxr] {}", Narrow(message)); }

#endif // MAGE_HAS_DOTNET

} // namespace

// ============================================================================ ScriptHost
ScriptHost::~ScriptHost() { Shutdown(); }

bool ScriptHost::Fail(std::string message) {
    m_lastError = std::move(message);
    m_ready = false;
    return false;
}

bool ScriptHost::Init(Engine& engine, const std::filesystem::path& scriptDirectory) {
    s_engine = &engine;
    m_scriptDirectory = scriptDirectory;

#ifndef MAGE_HAS_DOTNET
    return Fail("engine was built without .NET support (MAGE_ENABLE_DOTNET=OFF or nethost missing)");
#else
    namespace fs = std::filesystem;
    const fs::path coreDll = scriptDirectory / "Mage.Core.dll";
    const fs::path gameDll = scriptDirectory / "GameScripts.dll";
    const fs::path runtimeConfig = scriptDirectory / "Mage.Core.runtimeconfig.json";
    std::error_code ec;
    if (!fs::exists(coreDll, ec) || !fs::exists(runtimeConfig, ec))
        return Fail("Mage.Core.dll / Mage.Core.runtimeconfig.json not found in " + scriptDirectory.string());

    // 1) Locate and load hostfxr through nethost.
    char_t hostfxrPath[4096];
    size_t hostfxrPathSize = sizeof(hostfxrPath) / sizeof(char_t);
    if (const int rc = get_hostfxr_path(hostfxrPath, &hostfxrPathSize, nullptr); rc != 0)
        return Fail(std::format("get_hostfxr_path failed (0x{:08X}) - is the .NET 8+ runtime installed?", static_cast<uint32_t>(rc)));

    m_hostfxrLibrary = LoadLib(hostfxrPath);
    if (!m_hostfxrLibrary) return Fail("could not load hostfxr from " + Narrow(hostfxrPath));

    auto initFn = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(GetExport(m_hostfxrLibrary, "hostfxr_initialize_for_runtime_config"));
    auto getDelegateFn = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(GetExport(m_hostfxrLibrary, "hostfxr_get_runtime_delegate"));
    auto closeFn = reinterpret_cast<hostfxr_close_fn>(GetExport(m_hostfxrLibrary, "hostfxr_close"));
    auto setErrorWriterFn = reinterpret_cast<hostfxr_set_error_writer_fn>(GetExport(m_hostfxrLibrary, "hostfxr_set_error_writer"));
    if (!initFn || !getDelegateFn || !closeFn) return Fail("hostfxr exports missing");
    if (setErrorWriterFn) setErrorWriterFn(HostErrorWriter);

    // 2) Boot the CoreCLR runtime described by Mage.Core.runtimeconfig.json.
    hostfxr_handle context = nullptr;
    const int32_t initRc = initFn(runtimeConfig.c_str(), nullptr, &context);
    if (initRc < 0 || context == nullptr) {
        if (context) closeFn(context);
        return Fail(std::format("hostfxr_initialize_for_runtime_config failed (0x{:08X})", static_cast<uint32_t>(initRc)));
    }

    load_assembly_fn loadAssembly = nullptr;
    get_function_pointer_fn getFunctionPointer = nullptr;
    int32_t rc = getDelegateFn(context, hdt_load_assembly, reinterpret_cast<void**>(&loadAssembly));
    if (rc == 0) rc = getDelegateFn(context, hdt_get_function_pointer, reinterpret_cast<void**>(&getFunctionPointer));
    closeFn(context);
    if (rc != 0 || !loadAssembly || !getFunctionPointer)
        return Fail(std::format("hostfxr_get_runtime_delegate failed (0x{:08X}) - requires .NET 8+", static_cast<uint32_t>(rc)));

    // 3) Load the SDK + game assemblies into the default load context.
    if ((rc = loadAssembly(coreDll.c_str(), nullptr, nullptr)) != 0)
        return Fail(std::format("failed to load Mage.Core.dll (0x{:08X})", static_cast<uint32_t>(rc)));
    if (fs::exists(gameDll, ec)) {
        if ((rc = loadAssembly(gameDll.c_str(), nullptr, nullptr)) != 0)
            return Fail(std::format("failed to load GameScripts.dll (0x{:08X})", static_cast<uint32_t>(rc)));
    } else {
        Log::Warn("GameScripts.dll not found in {} - only Mage.Core is loaded", scriptDirectory.string());
    }

    // 4) Resolve the managed entry point ([UnmanagedCallersOnly] static method).
    FnManagedInitialize managedInit = nullptr;
    rc = getFunctionPointer(MAGE_STR("Mage.Interop.ScriptEngine, Mage.Core"), MAGE_STR("Initialize"),
                            UNMANAGEDCALLERSONLY_METHOD, nullptr, nullptr, reinterpret_cast<void**>(&managedInit));
    if (rc != 0 || !managedInit)
        return Fail(std::format("could not resolve Mage.Interop.ScriptEngine.Initialize (0x{:08X})", static_cast<uint32_t>(rc)));

    // 5) Hand C# the native API and receive its lifecycle callbacks.
    NativeBindings bindings{};
    bindings.structSize = sizeof(NativeBindings);
    bindings.version = kInteropVersion;
    bindings.Log = &Native_Log;
    bindings.GetEntityTransform = &Native_GetEntityTransform;
    bindings.SetEntityTransform = &Native_SetEntityTransform;
    bindings.ApplyVehicleInput = &Native_ApplyVehicleInput;
    bindings.GetVehicleState = &Native_GetVehicleState;
    bindings.RaycastWorld = &Native_RaycastWorld;
    bindings.SpawnEntity = &Native_SpawnEntity;
    bindings.DestroyEntity = &Native_DestroyEntity;
    bindings.IsKeyDown = &Native_IsKeyDown;
    bindings.IsKeyPressed = &Native_IsKeyPressed;
    bindings.EntityExists = &Native_EntityExists;

    m_callbacks = {};
    const int32_t initResult = managedInit(&bindings, &m_callbacks);
    if (initResult != 0)
        return Fail(std::format("ScriptEngine.Initialize returned {} (interop layout/version mismatch?)", initResult));
    if (!m_callbacks.CreateScript || !m_callbacks.DestroyScript || !m_callbacks.OnUpdate || !m_callbacks.OnFixedUpdate)
        return Fail("ScriptEngine.Initialize did not provide all callbacks");

    m_ready = true;
    Log::Info(".NET scripting ready (hostfxr: {})", Narrow(hostfxrPath));
    return true;
#endif
}

void ScriptHost::Shutdown() {
    if (m_ready && m_callbacks.Shutdown) m_callbacks.Shutdown();
    m_ready = false;
    m_callbacks = {};
    m_instanceCount = 0;
    // The CoreCLR runtime cannot be unloaded from a process; hostfxr stays resident.
}

bool ScriptHost::CreateInstance(Entity entity, const std::string& className) {
    if (!m_ready) return false;
    const bool ok = m_callbacks.CreateScript(entity, className.c_str()) != 0;
    if (ok) ++m_instanceCount;
    else Log::Error("Failed to create C# script '{}' for entity {}", className, entity);
    return ok;
}

void ScriptHost::DestroyInstance(Entity entity) {
    if (!m_ready) return;
    m_callbacks.DestroyScript(entity);
    if (m_instanceCount > 0) --m_instanceCount;
}

void ScriptHost::Update(float dt) {
    if (m_ready) m_callbacks.OnUpdate(dt);
}

void ScriptHost::FixedUpdate(float fixedDt) {
    if (m_ready) m_callbacks.OnFixedUpdate(fixedDt);
}

} // namespace mage
