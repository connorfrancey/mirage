cmake_minimum_required(VERSION 4.2)
project(Mirage LANGUAGES C CXX)
set(MIRAGE_VERSION "0.1.0-dev")   # Shown by the installer, stamped into the loader

# -------------------------------------------------------------------------------------------------
# Platform Guards (Mirror's Edge is a 32-bit Windows process)
if(NOT WIN32)
  message(FATAL_ERROR "${PROJECT_NAME} is Windows-only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Mirror's Edge is 32-bit. Use an x86 toolchain/preset (cmake -A Win32 / x86-debug).")
endif()

# -------------------------------------------------------------------------------------------------
# Global Settings (must come before dependencies so they inherit them)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT

# Optional targets: by default only Mirage.dll is built
option(MIRAGE_BUILD_TESTS     "Build the GoogleTest unit tests (mirage_tests)" OFF)
option(MIRAGE_BUILD_INSTALLER "Build the dinput8.dll loader and mirage_installer.exe" OFF)
option(MIRAGE_BUILD_LAUNCHER  "Build mirage_launcher.exe (injects the DLL into a running game)" OFF)
option(MIRAGE_BUILD_TESTHOST  "Build mirage_testhost.exe (loads the DLL into a D3D9 test app: mirage verbose-build)" OFF)
option(MIRAGE_UNREALLIB       "Build UnrealLib's tests and status tool (unreallib_tests). The DLL links UnrealLib whenever it has the game layer" ${MIRAGE_BUILD_TESTS})
option(MIRAGE_BUILD_SDKGEN    "Build mirage_sdkgen.exe (writes the Mirage SDK from the game's packages: mirage sdk)" OFF)
# Developer mode (`mirage --dev`): the DLL switches its stored developer mode on at every start, which shows the Test
# module (in-game self-tests). A normal build keeps what the menu stored (Misc > Developer Mode).
option(MIRAGE_DEV_MODE        "Developer build: developer mode on at every start (Test module)" OFF)
option(MIRAGE_BUILD_BENCHMARKS "Build the Google Benchmark benchmarks (mirage_bench: mirage bench)" OFF)
# Dev tooling packages (CPMLicenses, Format.cmake): not needed to build the DLL
option(MIRAGE_DEV_TOOLS       "Fetch CPMLicenses.cmake and Format.cmake" OFF)
# ccache (https://ccache.dev): reuses compiled objects across rebuilds, clean builds and build folders. Needs ccache
# on PATH (or CCACHE_PROGRAM). With MSVC, debug info goes into each object (/Z7) instead of a shared compiler PDB
# (/Zi), which ccache can't cache; the linker still writes the DLL's PDB, so debugging is the same.
option(MIRAGE_CCACHE          "Compile through ccache (needs ccache installed)" OFF)
# Performance optimizations (loader/src/core/optimizations.h): caches, cached object/name lookups, the scheduler's intervals,
# skipping and budget. Optional: OFF (`mirage build --no-optimizations`) does the work every time, nothing else changes.
option(MIRAGE_OPTIMIZATIONS   "Build the performance optimizations (caches, scheduler intervals/budget)" ON)
# Tracy profiler (`mirage build --tracy`): zones on the scheduler, every module's OnTick/OnRender and the menu's frame.
# The client only collects while the Tracy profiler is connected (on demand) and only listens on localhost.
option(MIRAGE_TRACY           "Build with the Tracy profiler client (connect with the Tracy profiler, same version)" OFF)
# Staged build (`mirage minimal --stage N`, presets x86-minimal-*): stage 0 is only the D3D9 hook and ImGui's demo
# window (links only MirageHook, ImGui and d3d9); each stage adds back a layer or a group of features, least likely to
# break the game first, up to the whole trainer. The stages are listed in cmake/Features.cmake.
option(MIRAGE_MINIMAL         "Staged build: only what MIRAGE_MINIMAL_STAGE adds back (ignores the feature options)" OFF)
set(MIRAGE_MINIMAL_STAGE 0 CACHE STRING "Staged build: how far to go (0 = D3D9 hook + ImGui demo; see cmake/Features.cmake)")

# CMake 4 dropped support for cmake_minimum_required < 3.5; this lets older dependencies configure
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# ccache: before CPM and every target, so the dependencies are cached too
include(cmake/Ccache.cmake)

# Use CPM.cmake
include(cmake/CPM.cmake)

# -------------------------------------------------------------------------------------------------
# Features: one option per module (MIRAGE_FEATURE_<NAME>), per category and a master switch, or the staged build's
# stage (cmake/Features.cmake). Sets MIRAGE_FEATURE_SOURCES, MIRAGE_GAME, MIRAGE_BARE and MIRAGE_STAGE.
include(cmake/Features.cmake)

# -------------------------------------------------------------------------------------------------
# Find sources and headers
file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/loader/src/*.c
  ${CMAKE_SOURCE_DIR}/loader/src/*.cpp)
file(GLOB_RECURSE HEADER_FILES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/loader/src/*.h
  ${CMAKE_SOURCE_DIR}/loader/src/*.hpp)

# Feature modules (loader/src/features/<category>/), the TAS engine and the game layer come from cmake/Features.cmake.
# Only the always-built files stay from the glob: core, menu, hooks, config, themes and features/tabs.cpp.
set(_feature_files ${SOURCE_FILES})
list(FILTER _feature_files INCLUDE REGEX "/src/features/[^/]+/")
list(FILTER SOURCE_FILES EXCLUDE REGEX "/src/(features/[^/]+|tas|game|render|speedrun|runtime)/")
foreach(file IN LISTS _feature_files)   # A new module file has to be listed there, or it would silently not build
  if(NOT file IN_LIST MIRAGE_ALL_FEATURE_SOURCES)
    message(FATAL_ERROR "${file} is not listed in cmake/Features.cmake: add a mirage_feature() for it")
  endif()
endforeach()
list(APPEND SOURCE_FILES ${MIRAGE_FEATURE_SOURCES})
if(MIRAGE_GAME)   # The Mirage runtime (loader/src/runtime/): UnrealLib and Lua, linked with the game layer
  file(GLOB_RECURSE _runtime_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/loader/src/runtime/*.cpp)
  list(APPEND SOURCE_FILES ${_runtime_sources})
  # The first-party mods (mods/): embedded in the DLL, written to the mods folder at startup (runtime/firstparty.h)
  file(GLOB_RECURSE _firstparty_files CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/mods/*)
  set(_firstparty_cpp ${CMAKE_BINARY_DIR}/generated/firstparty_mods.cpp)   # Its command: after the libraries, below
endif()
if(NOT MIRAGE_GAME)   # Keep the IDE tree to what is built
  list(FILTER HEADER_FILES EXCLUDE REGEX "/src/(tas|game|runtime)/")
endif()
if(MIRAGE_BARE)    # Staged build before the menu: hooks.cpp + loader/src/minimal/ (its own DllMain and gui::)
  set(SOURCE_FILES ${PROJECT_SOURCE_DIR}/loader/src/hooks.cpp ${PROJECT_SOURCE_DIR}/loader/src/core/hooking.cpp
      ${PROJECT_SOURCE_DIR}/loader/src/core/clock.cpp
      ${PROJECT_SOURCE_DIR}/loader/src/minimal/minimal.cpp)
  set(HEADER_FILES ${PROJECT_SOURCE_DIR}/loader/src/hooks.h ${PROJECT_SOURCE_DIR}/loader/src/core/hooking.h ${PROJECT_SOURCE_DIR}/loader/src/gui.h ${PROJECT_SOURCE_DIR}/loader/src/paths.h)
  if(MIRAGE_STAGE GREATER_EQUAL MIRAGE_STAGE_LOGGING)
    list(APPEND SOURCE_FILES ${PROJECT_SOURCE_DIR}/loader/src/log.cpp)
    list(APPEND HEADER_FILES ${PROJECT_SOURCE_DIR}/loader/src/log.h)
  endif()
  if(MIRAGE_STAGE GREATER_EQUAL MIRAGE_STAGE_THEMES)
    list(APPEND SOURCE_FILES ${PROJECT_SOURCE_DIR}/loader/src/theme.cpp ${PROJECT_SOURCE_DIR}/loader/src/theme_backend.cpp)
    list(APPEND HEADER_FILES ${PROJECT_SOURCE_DIR}/loader/src/theme.h ${PROJECT_SOURCE_DIR}/loader/src/theme_backend.h)
  endif()
else()
  list(FILTER SOURCE_FILES EXCLUDE REGEX "/src/minimal/")
endif()

# Injected mod => DLL, not an executable
add_library(${PROJECT_NAME} SHARED ${HEADER_FILES} ${SOURCE_FILES})
if(MIRAGE_GAME)
  target_sources(${PROJECT_NAME} PRIVATE ${_firstparty_cpp})
endif()
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

# build_info.cpp: a new build identifier (UUID, commit, build time) on every build, logged at startup
include(cmake/BuildInfo.cmake)
mirage_add_build_info(${PROJECT_NAME})

source_group(TREE ${PROJECT_SOURCE_DIR}/loader/src FILES ${HEADER_FILES} ${SOURCE_FILES})

target_include_directories(${PROJECT_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/loader/src)
target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
  MIRAGE_GAME=$<BOOL:${MIRAGE_GAME}>
  MIRAGE_OPTIMIZATIONS=$<BOOL:${MIRAGE_OPTIMIZATIONS}>
  MIRAGE_DEV_MODE=$<BOOL:${MIRAGE_DEV_MODE}>
  MIRAGE_TRACY=$<AND:$<BOOL:${MIRAGE_TRACY}>,$<NOT:$<BOOL:${MIRAGE_BARE}>>>   # Staged stages 0-2 never link it
  MIRAGE_MINIMAL=$<BOOL:${MIRAGE_BARE}>     # The code's "minimal": no menu, loader/src/minimal/ instead (stages 0-2)
  MIRAGE_MINIMAL_STAGE=$<IF:$<BOOL:${MIRAGE_BARE}>,${MIRAGE_STAGE},0>)
foreach(feature IN LISTS MIRAGE_ENABLED_FEATURES)    # MIRAGE_FEATURE_<NAME>=0/1 in the code
  string(TOUPPER "${feature}" _name)
  target_compile_definitions(${PROJECT_NAME} PRIVATE MIRAGE_FEATURE_${_name}=1)
endforeach()
foreach(feature IN LISTS MIRAGE_DISABLED_FEATURES)
  string(TOUPPER "${feature}" _name)
  target_compile_definitions(${PROJECT_NAME} PRIVATE MIRAGE_FEATURE_${_name}=0)
endforeach()

if(MSVC)
target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive- /utf-8)
else()
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
target_link_options(${PROJECT_NAME} PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Copy the finished DLL to <repo>/build/ so it's in one fixed place regardless of preset/config
option(MIRAGE_COPY_TO_BUILD_DIR "Copy Mirage.dll to <repo>/build/ after each build" ON)
if(MIRAGE_MINIMAL)   # Don't overwrite the full build's DLL in build/
  set(_mirage_output_default "${PROJECT_SOURCE_DIR}/build/minimal")
else()
  set(_mirage_output_default "${PROJECT_SOURCE_DIR}/build")
endif()
set(MIRAGE_BUILD_OUTPUT_DIR "${_mirage_output_default}" CACHE PATH "Where MIRAGE_COPY_TO_BUILD_DIR copies the DLL")
if(MIRAGE_COPY_TO_BUILD_DIR)
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MIRAGE_BUILD_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${PROJECT_NAME}> "${MIRAGE_BUILD_OUTPUT_DIR}"
    COMMENT "Copying ${PROJECT_NAME}.dll to ${MIRAGE_BUILD_OUTPUT_DIR}")
endif()

# -------------------------------------------------------------------------------------------------
# Libraries

include(cmake/Libraries.cmake)
include(cmake/XxHash.cmake)
include(cmake/DotNet.cmake)

# The first-party mods (mods/): native ones built as their own DLLs (cmake/FirstPartyMods.cmake), then every mod's
# files embedded in the DLL, written to the mods folder at startup (runtime/firstparty.h)
if(MIRAGE_GAME)
  include(cmake/FirstPartyMods.cmake)
  if(MSVC)
    enable_language(RC)
  endif()
  mirage_native_mods(${PROJECT_NAME} _firstparty_bins)
  get_property(_native_mods GLOBAL PROPERTY MIRAGE_NATIVE_MODS)
  set(_native_dlls "")
  foreach(_id IN LISTS _native_mods)
    list(APPEND _native_dlls ${MIRAGE_NATIVE_MODS_DIR}/${_id}/${_id}.dll)
  endforeach()
  string(REPLACE ";" "," _bins_arg "${_firstparty_bins}")
  add_custom_command(OUTPUT ${_firstparty_cpp}
          COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR}/mods -DOUT=${_firstparty_cpp} "-DBINS=${_bins_arg}"
                  -P ${CMAKE_SOURCE_DIR}/cmake/EmbedMods.cmake
          DEPENDS ${_firstparty_files} ${CMAKE_SOURCE_DIR}/cmake/EmbedMods.cmake
          COMMENT "Embedding the first-party mods (mods/)")
endif()

# The menu's font built into the DLL (hooks.cpp loads it): after the libraries, it comes from imgui's sources
include(cmake/Fonts.cmake)
mirage_embed_font(_font_cpp)
target_sources(${PROJECT_NAME} PRIVATE ${_font_cpp})
source_group("Generated" FILES ${_font_cpp})

# -------------------------------------------------------------------------------------------------
# Loader (dinput8.dll proxy next to the game exe) and its installer: the normal way to use the menu
if(MIRAGE_BUILD_INSTALLER)
  add_subdirectory(loader/proxy loader)
  add_subdirectory(installer)
endif()

# Launcher (injects the DLL into a running game without installing anything)
if(MIRAGE_BUILD_LAUNCHER)
  add_subdirectory(launcher)
endif()

# Test host (loads the DLL into a small D3D9 app instead of the game, and drives it: `mirage verbose-build`)
if(MIRAGE_BUILD_TESTHOST)
  add_subdirectory(tools/testhost)
endif()

# -------------------------------------------------------------------------------------------------
# Tests (GoogleTest)
if(MIRAGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# -------------------------------------------------------------------------------------------------
# SDK generator (Mirage, step 3): a host tool, run at development time, not part of the DLL
if(MIRAGE_BUILD_SDKGEN)
  add_subdirectory(tools/sdkgen)
endif()

# -------------------------------------------------------------------------------------------------
# Benchmarks (Google Benchmark)
if(MIRAGE_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()
