# =============================================================================
#  .NET hosting (nethost / hostfxr) + C# script build
#
#  Produces:
#    MAGE_HAS_DOTNET  - ON when nethost was found and scripting is enabled
#    mage_nethost     - imported shared library target (nethost.h / nethost.dll)
#    MAGE_Scripts     - custom target that builds Mage.Core.dll + GameScripts.dll
# =============================================================================
set(MAGE_HAS_DOTNET OFF)
if(NOT MAGE_ENABLE_DOTNET)
    return()
endif()

find_program(DOTNET_EXECUTABLE dotnet HINTS "$ENV{DOTNET_ROOT}" "C:/Program Files/dotnet")
if(NOT DOTNET_EXECUTABLE)
    message(WARNING "MAGE: 'dotnet' not found - C# scripting disabled (native vehicle controller will be used).")
    return()
endif()

# Runtime identifier for the nethost pack.
if(WIN32)
    set(MAGE_DOTNET_RID "win-x64")
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(MAGE_DOTNET_RID "osx-arm64")
    else()
        set(MAGE_DOTNET_RID "osx-x64")
    endif()
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(MAGE_DOTNET_RID "linux-arm64")
    else()
        set(MAGE_DOTNET_RID "linux-x64")
    endif()
endif()

# 1) Look for the AppHost pack that ships with the installed SDK.
file(REAL_PATH "${DOTNET_EXECUTABLE}" _dotnet_real)
get_filename_component(MAGE_DOTNET_ROOT "${_dotnet_real}" DIRECTORY)
file(GLOB _nethost_headers
    "${MAGE_DOTNET_ROOT}/packs/Microsoft.NETCore.App.Host.${MAGE_DOTNET_RID}/*/runtimes/${MAGE_DOTNET_RID}/native/nethost.h")
set(MAGE_NETHOST_DIR "")
if(_nethost_headers)
    list(SORT _nethost_headers COMPARE NATURAL ORDER DESCENDING)
    list(GET _nethost_headers 0 _nethost_header)
    get_filename_component(MAGE_NETHOST_DIR "${_nethost_header}" DIRECTORY)
endif()

# 2) Fallback: download the AppHost NuGet package (it is a plain zip).
if(NOT MAGE_NETHOST_DIR)
    message(STATUS "MAGE: nethost pack not found locally, downloading from NuGet")
    FetchContent_Declare(dotnet_apphost
        URL "https://www.nuget.org/api/v2/package/Microsoft.NETCore.App.Host.${MAGE_DOTNET_RID}/8.0.11"
        DOWNLOAD_NAME apphost.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(dotnet_apphost)
    set(MAGE_NETHOST_DIR "${dotnet_apphost_SOURCE_DIR}/runtimes/${MAGE_DOTNET_RID}/native")
endif()

if(WIN32)
    set(_nethost_lib "${MAGE_NETHOST_DIR}/nethost.lib")
    set(_nethost_dll "${MAGE_NETHOST_DIR}/nethost.dll")
elseif(APPLE)
    set(_nethost_lib "${MAGE_NETHOST_DIR}/libnethost.dylib")
    set(_nethost_dll "${_nethost_lib}")
else()
    set(_nethost_lib "${MAGE_NETHOST_DIR}/libnethost.so")
    set(_nethost_dll "${_nethost_lib}")
endif()

if(NOT EXISTS "${MAGE_NETHOST_DIR}/nethost.h" OR NOT EXISTS "${_nethost_dll}")
    message(WARNING "MAGE: nethost not usable in '${MAGE_NETHOST_DIR}' - C# scripting disabled.")
    return()
endif()

add_library(mage_nethost SHARED IMPORTED GLOBAL)
set_target_properties(mage_nethost PROPERTIES
    IMPORTED_LOCATION "${_nethost_dll}"
    INTERFACE_INCLUDE_DIRECTORIES "${MAGE_NETHOST_DIR}")
if(WIN32)
    set_target_properties(mage_nethost PROPERTIES IMPORTED_IMPLIB "${_nethost_lib}")
endif()

set(MAGE_HAS_DOTNET ON)
message(STATUS "MAGE: nethost found in ${MAGE_NETHOST_DIR}")

# Build the C# SDK and game scripts straight into <bin>/Scripts.
set(_cs_config "$<IF:$<CONFIG:Debug>,Debug,Release>")
add_custom_target(MAGE_Scripts ALL
    COMMAND "${DOTNET_EXECUTABLE}" build "${CMAKE_SOURCE_DIR}/scripting/Mage.Core/Mage.Core.csproj"
            -c ${_cs_config} -o "${MAGE_BIN_DIR}/Scripts" --nologo -v:minimal
    COMMAND "${DOTNET_EXECUTABLE}" build "${CMAKE_SOURCE_DIR}/scripting/GameScripts/GameScripts.csproj"
            -c ${_cs_config} -o "${MAGE_BIN_DIR}/Scripts" --nologo -v:minimal
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/scripting"
    COMMENT "Building C# scripting assemblies (Mage.Core.dll, GameScripts.dll)"
    SOURCES
        "${CMAKE_SOURCE_DIR}/scripting/Mage.Core/Mage.Core.csproj"
        "${CMAKE_SOURCE_DIR}/scripting/GameScripts/GameScripts.csproj"
    VERBATIM)
set_target_properties(MAGE_Scripts PROPERTIES FOLDER "Scripting")
