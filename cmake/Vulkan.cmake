# =============================================================================
#  Vulkan hardware ray tracing (path tracer)
#
#  Needs no Vulkan SDK: the headers, volk (function loader), VMA (allocator) and
#  glslang (GLSL -> SPIR-V compiler, built from source) are fetched here. At runtime
#  only the GPU driver's vulkan-1.dll / libvulkan.so.1 is required.
#
#  Produces:
#    MAGE_HAS_VULKAN_RT   - ON when enabled
#    mage_vulkan          - INTERFACE target (include dirs + defines)
#    MAGE_Shaders         - custom target compiling shaders/*.comp -> <bin>/shaders/*.spv
# =============================================================================
option(MAGE_ENABLE_VULKAN_RT "Build the Vulkan ray-traced path tracer" ON)
set(MAGE_HAS_VULKAN_RT OFF)
if(NOT MAGE_ENABLE_VULKAN_RT)
    return()
endif()

# SOURCE_SUBDIR _none: only download, we wire the sources up ourselves.
FetchContent_Declare(vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR _none)
FetchContent_Declare(volk
    URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR _none)
FetchContent_Declare(vma
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.1.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR _none)

# glslang: only the standalone compiler, no optimizer / HLSL / tests / install.
set(ENABLE_OPT               OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL              OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER       OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES  ON  CACHE BOOL "" FORCE)
set(ENABLE_CTEST             OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS            OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL   OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL           OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
    URL https://github.com/KhronosGroup/glslang/archive/refs/tags/15.0.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# glslang must not inherit the shared-library default meant for MAGE.
set(_mage_saved_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(vulkan_headers volk vma glslang)
set(BUILD_SHARED_LIBS ${_mage_saved_shared})
foreach(t glslang glslang-standalone SPIRV GenericCodeGen MachineIndependent OSDependent glslang-default-resource-limits)
    if(TARGET ${t})
        set_target_properties(${t} PROPERTIES FOLDER "ThirdParty/glslang")
    endif()
endforeach()

add_library(mage_vulkan INTERFACE)
target_include_directories(mage_vulkan INTERFACE
    "${vulkan_headers_SOURCE_DIR}/include"
    "${volk_SOURCE_DIR}"
    "${vma_SOURCE_DIR}/include")
target_compile_definitions(mage_vulkan INTERFACE VK_NO_PROTOTYPES MAGE_HAS_VULKAN_RT=1
    $<$<PLATFORM_ID:Windows>:VK_USE_PLATFORM_WIN32_KHR>)
set(MAGE_VOLK_SOURCE "${volk_SOURCE_DIR}/volk.c")

# ---- Shaders -----------------------------------------------------------------
set(MAGE_SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders")
file(GLOB MAGE_SHADER_SOURCES CONFIGURE_DEPENDS "${MAGE_SHADER_DIR}/*.comp")
file(GLOB MAGE_SHADER_INCLUDES CONFIGURE_DEPENDS "${MAGE_SHADER_DIR}/*.glsl")
set(MAGE_SPIRV_OUTPUTS "")
foreach(src ${MAGE_SHADER_SOURCES})
    get_filename_component(name "${src}" NAME)
    set(out "${MAGE_BIN_DIR}/shaders/${name}.spv")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MAGE_BIN_DIR}/shaders"
        COMMAND $<TARGET_FILE:glslang-standalone> -V --target-env vulkan1.2 --quiet
                "-I${MAGE_SHADER_DIR}" -o "${out}" "${src}"
        DEPENDS "${src}" ${MAGE_SHADER_INCLUDES} glslang-standalone
        COMMENT "GLSL -> SPIR-V: ${name}"
        VERBATIM)
    list(APPEND MAGE_SPIRV_OUTPUTS "${out}")
endforeach()
add_custom_target(MAGE_Shaders ALL DEPENDS ${MAGE_SPIRV_OUTPUTS} SOURCES ${MAGE_SHADER_SOURCES} ${MAGE_SHADER_INCLUDES})
set_target_properties(MAGE_Shaders PROPERTIES FOLDER "Shaders")

set(MAGE_HAS_VULKAN_RT ON)
list(LENGTH MAGE_SHADER_SOURCES _n)
message(STATUS "MAGE: Vulkan ray tracing enabled (${_n} compute shaders)")
