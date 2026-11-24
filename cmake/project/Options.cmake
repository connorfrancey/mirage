# ======================================================================================================================
# Options
# ======================================================================================================================

# Optional targets: by default only Mirage.dll is built
option(MIRAGE_BUILD_TESTS     "Build the GoogleTest unit tests (mirage_tests)" OFF)
option(MIRAGE_BUILD_INSTALLER "Build the dinput8.dll loader and mirage_installer.exe" OFF)
option(MIRAGE_BUILD_LAUNCHER  "Build mirage_launcher.exe (injects the DLL into a running game)" OFF)
option(MIRAGE_BUILD_TESTHOST  "Build mirage_testhost.exe (loads the DLL into a D3D9 test app: mirage verbose-build)" OFF)
option(MIRAGE_UNREALLIB       "Build UnrealLib's tests and status tool (unreallib_tests). The DLL links UnrealLib whenever it has the game layer" ${MIRAGE_BUILD_TESTS})
option(MIRAGE_BUILD_SDKGEN    "Build mirage_sdkgen.exe (writes the Mirage SDK from the game's packages: mirage sdk)" OFF)
option(MIRAGE_BUILD_APIGEN    "Build the reviewed MirageAPIGen Lua bindings into the game runtime" OFF)
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
