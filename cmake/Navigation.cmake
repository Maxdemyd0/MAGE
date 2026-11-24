# =============================================================================
#  Recast/Detour (AI navigation), compiled into MAGE_Engine
# =============================================================================
FetchContent_Declare(recast URL https://github.com/recastnavigation/recastnavigation/archive/refs/tags/v1.6.0.tar.gz DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(recast)
file(GLOB _recast_sources "${recast_SOURCE_DIR}/Recast/Source/*.cpp" "${recast_SOURCE_DIR}/Detour/Source/*.cpp" "${recast_SOURCE_DIR}/DetourCrowd/Source/*.cpp")
add_library(mage_recast OBJECT ${_recast_sources})
target_include_directories(mage_recast SYSTEM PUBLIC "${recast_SOURCE_DIR}/Recast/Include" "${recast_SOURCE_DIR}/Detour/Include" "${recast_SOURCE_DIR}/DetourCrowd/Include")
if(MSVC)
    target_compile_options(mage_recast PRIVATE /W0)
    target_compile_definitions(mage_recast PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(mage_recast PRIVATE -w)
endif()
set_target_properties(mage_recast PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "ThirdParty")
