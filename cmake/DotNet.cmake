# C# mods (src/runtime/mod_dotnet.h): the .NET runtime, x86, bundled next to the menu as memenu\dotnet\, and
# Mirage.dll (managed/Mirage) built with the .NET SDK. The user chose .NET (LTS) over Mono for C# mods (2026-11-11).
#
#   <build>\dotnet\host\fxr\<v>\hostfxr.dll, shared\Microsoft.NETCore.App\<v>\...   the runtime (downloaded once)
#   <build>\dotnet\mirage\Mirage.dll + Mirage.runtimeconfig.json                   the API and host
#
# `memenu game-test` / `install-game` copy <build>\dotnet to the game's memenu\dotnet (only when it changed).
#
# The option also picks the scripting language (asked for 2026-11-11): ON, script mods and the in-game script editor
# use C# (.cs files compiled in the game); OFF, Lua, and no .NET is bundled or built. Lua stays in every build: the
# first-party mods are Lua.
option(MEMENU_DOTNET "C# scripting: bundle the .NET runtime, build Mirage.dll (needs the .NET SDK); OFF: Lua scripting" ON)
if(MEMENU_DOTNET AND MEMENU_GAME)
  target_compile_definitions(${PROJECT_NAME} PRIVATE MEMENU_DOTNET=1)
else()
  target_compile_definitions(${PROJECT_NAME} PRIVATE MEMENU_DOTNET=0)
  return()
endif()

# .NET 10 LTS (supported until 2028-11). Hash: builds.dotnet.microsoft.com release metadata (SHA-512 of the zip).
set(MEMENU_DOTNET_RUNTIME_VERSION 10.0.15)
set(MEMENU_DOTNET_RUNTIME_SHA512 184be357a27891e80d57737be8ef4749945ee958e6a3faf7d57b867fc6e1938db3d86c7b525b92393a7ffae3af5322a039410ce71b0a11475cd326fb2fbb903f)
set(_zip_name dotnet-runtime-${MEMENU_DOTNET_RUNTIME_VERSION}-win-x86.zip)
set(_zip_url https://builds.dotnet.microsoft.com/dotnet/Runtime/${MEMENU_DOTNET_RUNTIME_VERSION}/${_zip_name})
if(DEFINED ENV{CPM_SOURCE_CACHE})
  set(_cache $ENV{CPM_SOURCE_CACHE}/dotnet)
else()
  set(_cache ${CMAKE_BINARY_DIR}/_downloads)
endif()
set(_zip ${_cache}/${_zip_name})
set(MEMENU_DOTNET_DIR ${CMAKE_BINARY_DIR}/dotnet)
set(_stamp ${MEMENU_DOTNET_DIR}/runtime-${MEMENU_DOTNET_RUNTIME_VERSION}.stamp)

if(NOT EXISTS ${_stamp})
  if(NOT EXISTS ${_zip})
    message(STATUS ".NET runtime: downloading ${_zip_name}")
    file(DOWNLOAD ${_zip_url} ${_zip} EXPECTED_HASH SHA512=${MEMENU_DOTNET_RUNTIME_SHA512} STATUS _status TLS_VERIFY ON)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
      file(REMOVE ${_zip})
      message(FATAL_ERROR ".NET runtime: download failed (${_status}). Configure with -DMEMENU_DOTNET=OFF to build without C# mods.")
    endif()
  endif()
  file(REMOVE_RECURSE ${MEMENU_DOTNET_DIR}/host ${MEMENU_DOTNET_DIR}/shared)
  file(ARCHIVE_EXTRACT INPUT ${_zip} DESTINATION ${MEMENU_DOTNET_DIR} PATTERNS host/* shared/* LICENSE.txt ThirdPartyNotices.txt)
  file(TOUCH ${_stamp})
  message(STATUS ".NET runtime: ${MEMENU_DOTNET_RUNTIME_VERSION} (x86) in ${MEMENU_DOTNET_DIR}")
endif()

# Mirage.dll: built with the SDK whenever its sources change. The C# function tables are generated from api.h
# (tools/gen_csharp_api.py); the build fails when they are out of date rather than ship a mismatched table.
find_program(MEMENU_DOTNET_EXE dotnet REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
file(GLOB_RECURSE _managed_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/managed/*.cs ${CMAKE_SOURCE_DIR}/managed/*.csproj
     ${CMAKE_SOURCE_DIR}/managed/*.props)
set(_mirage_dll ${MEMENU_DOTNET_DIR}/mirage/Mirage.dll)
add_custom_command(OUTPUT ${_mirage_dll}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_csharp_api.py --check
        COMMAND ${MEMENU_DOTNET_EXE} build ${CMAKE_SOURCE_DIR}/managed/Mirage/Mirage.csproj -c Release --nologo -v quiet
                -o ${MEMENU_DOTNET_DIR}/mirage
        DEPENDS ${_managed_sources} ${CMAKE_SOURCE_DIR}/api/mirage/api.h ${CMAKE_SOURCE_DIR}/tools/gen_csharp_api.py
        COMMENT "Building Mirage.dll (C# mods' API, .NET ${MEMENU_DOTNET_RUNTIME_VERSION})"
        VERBATIM)
add_custom_target(mirage_managed DEPENDS ${_mirage_dll})
add_dependencies(${PROJECT_NAME} mirage_managed)
