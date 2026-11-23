cmake_minimum_required(VERSION 4.2)

option(MIRAGE_DOCS_ONLY "Configure documentation without the game toolchain or dependencies" OFF)
option(MIRAGE_BUILD_DOCS "Enable Mirage documentation targets" OFF)
set(_mirage_languages C CXX)
if(MIRAGE_DOCS_ONLY)
  set(_mirage_languages NONE)
endif()
project(Mirage
        DESCRIPTION "Mirror's Edge Runtime and Gameplay Extensions (MIRAGE)"
        LANGUAGES ${_mirage_languages})

set(MIRAGE_VERSION "0.1.0-dev")   # Shown by the installer, stamped into the loader

if(MIRAGE_BUILD_DOCS OR MIRAGE_DOCS_ONLY)
  include(cmake/Doxygen.cmake)
  mirage_add_documentation(VERSION "${MIRAGE_VERSION}")
endif()
if(MIRAGE_DOCS_ONLY)
  return()
endif()

include(cmake/project/Platform.cmake)   # 32-bit Windows guards, static CRT
include(cmake/project/Options.cmake)    # MIRAGE_BUILD_*, MIRAGE_MINIMAL and the other switches
include(cmake/project/Settings.cmake)   # C++ standard: before the dependencies so they inherit it

# ccache: before CPM and every target, so the dependencies are cached too
include(cmake/Ccache.cmake)

# Use CPM.cmake
include(cmake/CPM.cmake)

# -------------------------------------------------------------------------------------------------
# Features: one option per module (MIRAGE_FEATURE_<NAME>), per category and a master switch, or the staged build's
# stage (cmake/Features.cmake). Sets MIRAGE_FEATURE_SOURCES, MIRAGE_GAME, MIRAGE_BARE and MIRAGE_STAGE.
include(cmake/Features.cmake)

include(cmake/project/Sources.cmake)          # SOURCE_FILES / HEADER_FILES
include(cmake/project/Target.cmake)           # The Mirage DLL, its includes and definitions
include(cmake/project/CompilerOptions.cmake)
include(cmake/project/Output.cmake)           # Copy of the DLL to <repo>/build/
include(cmake/project/Library.cmake)          # mirage_library(): parts of the loader as their own libraries

# -------------------------------------------------------------------------------------------------
# Libraries

include(cmake/Libraries.cmake)
include(cmake/RapidHash.cmake)
include(cmake/DotNet.cmake)

if(MIRAGE_BUILD_APIGEN)
  if(NOT MIRAGE_GAME)
    message(FATAL_ERROR "MIRAGE_BUILD_APIGEN requires the Mirage game runtime")
  endif()
  include(cmake/generation/ApiGen.cmake)
endif()

# -------------------------------------------------------------------------------------------------
# The loader's libraries (cmake/project/Library.cmake), linked into Mirage.dll. None yet: each is one call, e.g.
#   mirage_library(MirageCore FOLDERS core util LIBS spdlog::spdlog glaze::glaze rapidhash::rapidhash)
#   mirage_library(MirageUI FOLDERS imgui editor DEPENDS MirageCore LIBS imgui)
#   mirage_library(MirageScript FOLDERS runtime/lua DEPENDS MirageCore LIBS lua::lua)

# The first-party mods (mods/): native ones built as their own DLLs (cmake/FirstPartyMods.cmake), then every mod's
# files embedded in the DLL, written to the mods folder at startup (runtime/firstparty.h)
if(MIRAGE_GAME)
  include(cmake/FirstPartyMods.cmake)
  if(MSVC)
    enable_language(RC)
  endif()
  mirage_native_mods(${PROJECT_NAME} _firstparty_bins)
  include(cmake/generation/EmbedSymbols.cmake)
  include(cmake/generation/EmbedCrashdump.cmake)
  include(cmake/generation/EmbedModsCommand.cmake)
endif()

# The menu's font built into the DLL (hooks.cpp loads it): after the libraries, it comes from imgui's sources
include(cmake/Fonts.cmake)
mirage_embed_font(_font_cpp)
target_sources(${PROJECT_NAME} PRIVATE ${_font_cpp} ${_font_cpp_rc})
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

if(NOT MIRAGE_BARE)
  add_subdirectory(archive)
endif()
include(cmake/targets/Benchmarks.cmake)
