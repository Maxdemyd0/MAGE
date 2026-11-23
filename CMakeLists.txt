# =============================================================================
#  MAGE v1 - Maxdemyd0's Advanced Game Engine
#  Root build script: MAGE_Engine (static core) + MAGE_Runtime + MAGE_Editor
# =============================================================================
cmake_minimum_required(VERSION 3.24)

# Several third-party projects still declare very old cmake_minimum_required()
# versions, which CMake 4.x rejects. This keeps them configurable.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(MAGE VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MAGE_ENABLE_DOTNET "Embed the .NET CoreCLR runtime for C# scripting" ON)
option(MAGE_BUILD_SHARED "Build MAGE_Engine as a shared library (DLL/.so) - recommended for LGPL compliance" ON)

# Dynamic CRT everywhere (Jolt defaults to the static CRT, which we override).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# All executables land in build/bin/<Config>/ next to their assets and scripts.
set(MAGE_BIN_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${MAGE_BIN_DIR}")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Dependencies)
include(DotNet)

# -----------------------------------------------------------------------------
#  MAGE_Engine - the core engine library (shared by default)
# -----------------------------------------------------------------------------
set(MAGE_ENGINE_SOURCES
    src/core/Export.h
    src/core/Log.h                  src/core/Log.cpp
    src/core/Profiler.h
    src/core/Window.h               src/core/Window.cpp
    src/core/Input.h                src/core/Input.cpp
    src/core/Camera.h               src/core/Camera.cpp
    src/core/ECS.h                  src/core/ECS.cpp
    src/core/Engine.h               src/core/Engine.cpp
    src/renderer/GL.h               src/renderer/GL.cpp
    src/renderer/Mesh.h             src/renderer/Mesh.cpp
    src/renderer/Shader.h           src/renderer/Shader.cpp
    src/renderer/Renderer.h         src/renderer/Renderer.cpp
    src/renderer/PhysicsDebugRenderer.h src/renderer/PhysicsDebugRenderer.cpp
    src/physics/PhysicsWorld.h      src/physics/PhysicsWorld.cpp
    src/streaming/SectorStreamer.h  src/streaming/SectorStreamer.cpp
    src/scripting/InteropTypes.h
    src/scripting/ScriptHost.h      src/scripting/ScriptHost.cpp
    src/scene/Testbed.h             src/scene/Testbed.cpp
)

if(MAGE_BUILD_SHARED)
    add_library(MAGE_Engine SHARED ${MAGE_ENGINE_SOURCES} $<TARGET_OBJECTS:mage_imgui>)
    set_target_properties(MAGE_Engine PROPERTIES
        DEFINE_SYMBOL MAGE_ENGINE_BUILD
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
    target_compile_definitions(MAGE_Engine INTERFACE MAGE_ENGINE_SHARED)
    if(WIN32)
        set(_imgui_export "__declspec(dllexport)")
        set(_imgui_import "__declspec(dllimport)")
    else()
        set(_imgui_export "__attribute__((visibility(\"default\")))")
        set(_imgui_import "${_imgui_export}")
    endif()
    target_compile_definitions(mage_imgui PRIVATE "IMGUI_API=${_imgui_export}")
    target_compile_definitions(MAGE_Engine PRIVATE "IMGUI_API=${_imgui_export}" INTERFACE "IMGUI_API=${_imgui_import}")
else()
    add_library(MAGE_Engine STATIC ${MAGE_ENGINE_SOURCES} $<TARGET_OBJECTS:mage_imgui>)
endif()

target_include_directories(MAGE_Engine PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
    $<TARGET_PROPERTY:mage_imgui,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:glfw,INTERFACE_INCLUDE_DIRECTORIES>)   # GLFW key codes for apps
target_compile_definitions(MAGE_Engine
    PUBLIC
        GLFW_INCLUDE_NONE
        GLM_ENABLE_EXPERIMENTAL
        GLM_FORCE_RADIANS
    PRIVATE
        MAGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        $<$<PLATFORM_ID:Windows>:NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS>
)
target_link_libraries(MAGE_Engine
    PUBLIC  glm::glm
    PRIVATE glfw Jolt nlohmann_json::nlohmann_json
)
if(MAGE_HAS_DOTNET)
    target_compile_definitions(MAGE_Engine PRIVATE MAGE_HAS_DOTNET=1)
    target_link_libraries(MAGE_Engine PRIVATE mage_nethost)
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(MAGE_Engine PRIVATE ${CMAKE_DL_LIBS} pthread)
endif()

if(MSVC)
    target_compile_options(MAGE_Engine PRIVATE /W3 /permissive- /utf-8 /Zc:__cplusplus /MP)
    # Exported classes hold STL members; safe because every module shares the same /MD CRT.
    target_compile_options(MAGE_Engine PUBLIC /wd4251 /wd4275)
else()
    target_compile_options(MAGE_Engine PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# -----------------------------------------------------------------------------
#  Executables
# -----------------------------------------------------------------------------
function(mage_add_executable target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE MAGE_Engine)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /permissive- /utf-8 /Zc:__cplusplus)
        set_property(TARGET ${target} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>")
    endif()
    if(UNIX)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN")
    endif()

    # Ship sector JSON + other assets next to the binary.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:${target}>/assets"
        VERBATIM)

    if(MAGE_HAS_DOTNET)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:mage_nethost>" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
        if(TARGET MAGE_Scripts)
            add_dependencies(${target} MAGE_Scripts)
        endif()
    endif()
endfunction()

mage_add_executable(MAGE_Runtime src/main.cpp)
mage_add_executable(MAGE_Editor  src/editor/EditorMain.cpp)

if(MSVC)
    set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT MAGE_Runtime)
endif()

message(STATUS "MAGE: shared engine ${MAGE_BUILD_SHARED}, C# scripting ${MAGE_HAS_DOTNET}  (binaries -> ${CMAKE_BINARY_DIR}/bin/<Config>)")
