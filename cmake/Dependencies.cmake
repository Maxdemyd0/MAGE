# =============================================================================
#  Third-party dependencies (all fetched at configure time via FetchContent)
# =============================================================================
include(FetchContent)
# Everything ends up inside MAGE_Engine.so/.dll, so static deps must be position independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(FETCHCONTENT_QUIET OFF)

# ---- GLFW (windowing & input) ------------------------------------------------
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# ---- GLM (math) --------------------------------------------------------------
set(GLM_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# ---- nlohmann/json (scene & sector serialization) ----------------------------
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# ---- Jolt Physics ------------------------------------------------------------
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION    OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS              OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS             OFF CACHE BOOL "" FORCE)
# Keep exceptions/RTTI on: otherwise Jolt publishes _HAS_EXCEPTIONS=0 to every consumer.
set(CPP_EXCEPTIONS_ENABLED          ON  CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED                ON  CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE ON CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE   OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL                  OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS               OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD              OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST         OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES                  OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER                   OFF CACHE BOOL "" FORCE)
FetchContent_Declare(JoltPhysics
    URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.3.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR Build)

# ---- Dear ImGui (docking branch) ---------------------------------------------
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b-docking.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(glfw glm nlohmann_json JoltPhysics imgui)

# ImGui ships no CMake project: compile it (+ GLFW/OpenGL3 backends) as an OBJECT library
# that is linked *into* MAGE_Engine. In shared builds ImGui is exported from MAGE_Engine.dll,
# so the engine and the applications share one ImGui context.
add_library(mage_imgui OBJECT
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(mage_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(mage_imgui PRIVATE glfw)
target_compile_definitions(mage_imgui PUBLIC GLFW_INCLUDE_NONE)

foreach(dep glfw Jolt mage_imgui)
    if(TARGET ${dep})
        set_target_properties(${dep} PROPERTIES FOLDER "ThirdParty")
    endif()
endforeach()
