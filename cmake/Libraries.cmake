include(${CMAKE_CURRENT_LIST_DIR}/Versions.cmake)   # Pinned versions (fmt, spdlog, UnrealLib, ...)

# Libraries

# -------------------------------------------------------------------------------------------------

# Threads
find_package(Threads REQUIRED)

# ImGui
CPMAddPackage(
        NAME imgui
        GITHUB_REPOSITORY ocornut/imgui
        GIT_TAG ${IMGUI_VERSION}
        DOWNLOAD_ONLY YES
)
if(imgui_ADDED)
    add_library(imgui STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    )
    target_include_directories(imgui SYSTEM PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
    )
    # dwmapi: imgui_impl_win32 links it with #pragma comment(lib), which MinGW ignores
    target_link_libraries(imgui PUBLIC d3d9 dwmapi)
endif()

# Staged build, stages 0-2 (MIRAGE_BARE): only what that stage adds is downloaded and linked
if(MIRAGE_BARE)
    target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/imgui_demo.cpp)   # ImGui::ShowDemoWindow
    if(MIRAGE_STAGE GREATER_EQUAL MIRAGE_STAGE_LOGGING)
        set(_need_spdlog ON)
    endif()
    if(MIRAGE_STAGE GREATER_EQUAL MIRAGE_STAGE_THEMES)
        set(_need_themes ON)
    endif()
else()
    set(_need_spdlog ON)
    set(_need_themes ON)
endif()

# spdlog, on a separate fmt: UnrealLib links fmt::fmt too, and spdlog's bundled copy would define the same symbols
if(_need_spdlog)
CPMAddPackage(
        NAME fmt
        GITHUB_REPOSITORY fmtlib/fmt
        GIT_TAG ${FMT_VERSION}
        OPTIONS "FMT_INSTALL OFF" "FMT_TEST OFF" "FMT_DOC OFF" "FMT_MODULE OFF"
)
CPMAddPackage(
        NAME spdlog
        GITHUB_REPOSITORY gabime/spdlog
        GIT_TAG ${SPDLOG_VERSION}
        OPTIONS "SPDLOG_INSTALL OFF" "SPDLOG_FMT_EXTERNAL ON"
)

# zstd: compresses the previous session's log into logs/.old/ (log.cpp) and the TAS's memory snapshots
# (tas/snapshot.cpp). Built by hand: zstd's own CMake project is large and sets its own runtime-library
# flags. The target keeps its old name; it has the decompressor too.
# gh: https://github.com/facebook/zstd
CPMAddPackage(
        NAME zstd
        GITHUB_REPOSITORY facebook/zstd
        GIT_TAG ${ZSTD_VERSION}
        DOWNLOAD_ONLY YES
)
if(zstd_ADDED)
    file(GLOB zstd_SOURCES CONFIGURE_DEPENDS
            ${zstd_SOURCE_DIR}/lib/common/*.c
            ${zstd_SOURCE_DIR}/lib/compress/*.c
            ${zstd_SOURCE_DIR}/lib/decompress/*.c)
    add_library(zstd_compress STATIC ${zstd_SOURCES})
    target_include_directories(zstd_compress SYSTEM PUBLIC ${zstd_SOURCE_DIR}/lib)
    target_compile_definitions(zstd_compress PRIVATE ZSTD_DISABLE_ASM XXH_NAMESPACE=ZSTD_)
endif()
endif()


# MirageHook: Mirage's own x86 hooking library (inline, mid-function, vtable, IAT hooks, transactions, SIMD signature
# scanning); every hook goes through it (loader/src/core/hooking.h). Added after fmt/spdlog so it builds on the same
# copies; its other pins (magic_enum, fast_float, unordered_dense) match Versions.cmake, and CPM keeps one copy of each.
# The staged build's stages without logging compile its logging out. A local checkout can stand in with
# -D CPM_MirageHook_SOURCE=<path>.
if(_need_spdlog)
    set(_miragehook_logging ON)
else()
    set(_miragehook_logging OFF)
endif()
CPMAddPackage(
        NAME MirageHook
        GITHUB_REPOSITORY connorfrancey/miragehook
        GIT_TAG ${MIRAGEHOOK_GIT_COMMIT}
        OPTIONS
            "MIRAGEHOOK_BUILD_TESTS OFF"
            "MIRAGEHOOK_BUILD_EXAMPLES OFF"
            "MIRAGEHOOK_BUILD_BENCHMARKS OFF"
            "MIRAGEHOOK_BUILD_DOCS OFF"
            "MIRAGEHOOK_LOGGING ${_miragehook_logging}"
)

if(_need_themes)

# nlohmann/json: config file + theme parsing (release tarball is much smaller than cloning the repo)
CPMAddPackage(
        NAME nlohmann_json
        VERSION ${NLOHMANN_JSON_VERSION}
        URL https://github.com/nlohmann/json/releases/download/v${NLOHMANN_JSON_VERSION}/json.tar.xz
        OPTIONS "JSON_Install OFF"
)

# imjson: ImGui themes from JSON (https://github.com/moleium/imjson)
# The repo has no library target we can use, so build its sources ourselves.
# Pin GIT_TAG to a commit hash once you have a version that works for you.
CPMAddPackage(
        NAME imjson
        GITHUB_REPOSITORY moleium/imjson
        GIT_TAG ${IMJSON_COMMIT}
        DOWNLOAD_ONLY YES
)
if(imjson_ADDED)
    file(GLOB_RECURSE imjson_SOURCES CONFIGURE_DEPENDS
            ${imjson_SOURCE_DIR}/include/*.cpp
            ${imjson_SOURCE_DIR}/src/*.cpp)
    if(NOT imjson_SOURCES)
        message(FATAL_ERROR "imjson: no .cpp found under ${imjson_SOURCE_DIR}/include or /src - "
                "check where imjson.cpp lives in the repo and adjust the glob in cmake/Libraries.cmake")
    endif()
    add_library(imjson STATIC ${imjson_SOURCES})
    # imjson includes <imgui/imgui.h>, but the imgui target exposes its root (<imgui.h>): forward it
    file(WRITE ${CMAKE_BINARY_DIR}/imjson_compat/imgui/imgui.h "#pragma once\n#include \"${imgui_SOURCE_DIR}/imgui.h\"\n")
    target_include_directories(imjson PUBLIC ${imjson_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/imjson_compat)
    target_link_libraries(imjson PUBLIC imgui nlohmann_json::nlohmann_json)
endif()
endif()

if(MIRAGE_BARE)
    target_link_libraries(${PROJECT_NAME} PRIVATE imgui MirageHook::MirageHook d3d9)
    if(_need_spdlog)
        target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog zstd_compress)
    endif()
    if(_need_themes)
        target_link_libraries(${PROJECT_NAME} PRIVATE imjson nlohmann_json::nlohmann_json shell32)
    endif()
    return()
endif()

# magic_enum: enumerator names at compile time, for the file-format words that are exactly an enum's names
# (loader/src/core/enum_names.h: mod.toml's phase and setting types, a replay's start kind and camera ease). Header only.
CPMAddPackage(
        NAME magic_enum
        GITHUB_REPOSITORY Neargye/magic_enum
        GIT_TAG ${MAGICENUM_VERSION}
        OPTIONS
            "MAGIC_ENUM_OPT_BUILD_EXAMPLES OFF"
            "MAGIC_ENUM_OPT_BUILD_TESTS OFF"
            "MAGIC_ENUM_OPT_INSTALL OFF"
)
target_link_libraries(${PROJECT_NAME} PRIVATE magic_enum::magic_enum)

# fast_float: Fast and exact implementation of the C++ from_chars functions for number types: 4x to 10x faster than strtod, part of GCC 12, MySQL, DuckDB, Chromium, Redis and WebKit/Safari
CPMAddPackage(
        NAME fast_float
        GITHUB_REPOSITORY fastfloat/fast_float
        GIT_TAG ${FASTFLOAT_VERSION}
        OPTIONS
            "FASTFLOAT_TEST OFF"
)
target_link_libraries(${PROJECT_NAME} PRIVATE FastFloat::fast_float)   # util/float_util.h: numbers from text


# unordered_dense: open-addressing hash maps (values in one vector) where lookups are the hot part: the mesh builder's
# edge map (render/mesh.cpp: 2x faster builds). Not the TAS page tables: measured, no gain. Header only. Unlike std::, an
# insert or erase moves the other entries: no reference into the map may be kept across one.
CPMAddPackage(
        NAME unordered_dense
        GITHUB_REPOSITORY martinus/unordered_dense
        GIT_TAG ${UNORDERED_DENSE_VERSION}
        DOWNLOAD_ONLY YES
)
if(NOT TARGET unordered_dense::unordered_dense)   # MirageHook may have fetched it first (then _ADDED is off)
    add_library(unordered_dense INTERFACE)
    target_include_directories(unordered_dense SYSTEM INTERFACE ${unordered_dense_SOURCE_DIR}/include)
    add_library(unordered_dense::unordered_dense ALIAS unordered_dense)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE unordered_dense::unordered_dense)

# imgui-filebrowser: the TAS module's Open / Save file browser, drawn with ImGui inside the menu (Windows' own dialog
# opened behind the fullscreen game and took the cursor). Header only; the repo has no tags, so a commit is pinned.
if("tas" IN_LIST MIRAGE_ENABLED_FEATURES)
CPMAddPackage(
        NAME imgui_filebrowser
        GITHUB_REPOSITORY AirGuanZ/imgui-filebrowser
        GIT_TAG ${IMGUIFILEBROWSER_GIT_COMMIT}
        DOWNLOAD_ONLY YES
)
if(imgui_filebrowser_ADDED)
    add_library(imgui_filebrowser INTERFACE)
    target_include_directories(imgui_filebrowser SYSTEM INTERFACE ${imgui_filebrowser_SOURCE_DIR})
    target_link_libraries(imgui_filebrowser INTERFACE imgui)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE imgui_filebrowser)
endif()

# Tracy: profiler client, only with MIRAGE_TRACY (loader/src/core/profile.h). Set up for a DLL injected into the game:
#   TRACY_DELAYED_INIT, TRACY_MANUAL_LIFETIME  started and stopped by the menu (prof::Start/Stop), not by static
#                                               constructors: those run in DllMain under the loader lock, and the
#                                               profiler's threads must be gone before the DLL unloads
#   TRACY_ON_DEMAND                             nothing is collected until the profiler connects
#   TRACY_ONLY_LOCALHOST                        the game doesn't listen on the network
#   TRACY_NO_CRASH_HANDLER                      the menu's own crash logger (diag.cpp) stays the one in charge
#   TRACY_NO_SYSTEM_TRACING                     no ETW (needs admin, and traces the whole system)
#   TRACY_NO_SAMPLING                           no call stack sampling (needs admin, 64-bit only)
if(MIRAGE_TRACY)
CPMAddPackage(
        NAME tracy
        GITHUB_REPOSITORY wolfpld/tracy
        GIT_TAG ${TRACY_VERSION}
        SYSTEM YES
        OPTIONS
            "TRACY_ENABLE ON"
            "TRACY_DELAYED_INIT ON"
            "TRACY_MANUAL_LIFETIME ON"
            "TRACY_ON_DEMAND ON"
            "TRACY_ONLY_LOCALHOST ON"
            "TRACY_NO_CRASH_HANDLER ON"
            "TRACY_NO_SYSTEM_TRACING ON"
            "TRACY_NO_SAMPLING ON"
            "TRACY_STATIC ON"
)
target_link_libraries(${PROJECT_NAME} PRIVATE Tracy::TracyClient)
endif()

# Direct3D 9 comes from the Windows SDK (d3d9.h / d3d9.lib). Nothing uses D3DX, so the legacy DirectX SDK
# (June 2010) is not needed, and players don't need the d3dx9_43.dll runtime either.

# Mirage SDK (sdk/): the game's classes, structs and functions, generated by mirage_sdkgen (tools/sdkgen, `mirage sdk`)
# from the game's packages and the engine's own layouts. Every member offset and size is a static_assert, so a wrong
# layout fails the build. Target mirage_sdk (alias mirage::sdk), header "sdk/SDK.hpp". Only the game layer uses it.
if(MIRAGE_GAME AND NOT TARGET mirage_sdk)
  add_subdirectory(${CMAKE_SOURCE_DIR}/sdk ${CMAKE_BINARY_DIR}/sdk)
endif()

# The old SDK (the connorfrancey/mmultiplayer fork's Client/sdk), downloaded only for the SDK generator's report of
# what it had wrong (build/sdk-reference/old-sdk-report.md); nothing builds it.
if(MIRAGE_BUILD_SDKGEN)
CPMAddPackage(
        NAME mmultiplayer
        GITHUB_REPOSITORY connorfrancey/mmultiplayer
        GIT_TAG ${MMULTIPLAYER_COMMIT}
        DOWNLOAD_ONLY YES
)
endif()

# UnrealLib: reads Unreal Engine 3 packages (.u/.upk/.me1), a C++ port of EliotVU/Unreal-Library. The Mirage runtime
# (loader/src/runtime/) reads the game's script packages with it inside the game, so every build with the game layer links it;
# MIRAGE_UNREALLIB adds its tests and status tool. Reuses the fmt/spdlog above and adds zlib.
if(MIRAGE_UNREALLIB OR MIRAGE_GAME OR MIRAGE_BUILD_SDKGEN)
CPMAddPackage(
        NAME UnrealLib
        GITHUB_REPOSITORY connorfrancey/UnrealLib
        GIT_TAG ${UNREALLIB_GIT_TAG}
        OPTIONS
            "UNREALLIB_BUILD_TESTS OFF"
            "UNREALLIB_BUILD_TOOLS OFF"
            "UNREALLIB_BUILD_EXAMPLES OFF"
)
endif()

# toml11 (https://github.com/ToruNiina/toml11): TOML for Mirage's own files: mod manifests (mod.toml, read by the
# runtime and the unit tests) and the SDK generator's configuration (tools/sdkgen/sdkgen.toml). Header only.
if(MIRAGE_GAME OR MIRAGE_BUILD_SDKGEN OR MIRAGE_BUILD_TESTS)
CPMAddPackage(
        NAME toml11
        GITHUB_REPOSITORY ToruNiina/toml11
        GIT_TAG ${TOML_VERSION}
        OPTIONS
            "TOML11_INSTALL OFF"
            "TOML11_BUILD_EXAMPLES OFF"
            "TOML11_BUILD_TESTS OFF"
)
endif()

# Lua 5.4: the language of Mirage mods (loader/src/runtime/lua_vm.cpp). Built from the sources as a static library, compiled as
# C++: a Lua error is then a C++ exception, so it unwinds the C++ frames between the error and the pcall (destructors
# run) instead of longjmp-ing over them. Without the standalone interpreter/compiler and the amalgamation/test files.
if(MIRAGE_GAME)
  CPMAddPackage(
      NAME lua
      GITHUB_REPOSITORY lua/lua
      GIT_TAG ${LUA_VERSION}
      DOWNLOAD_ONLY YES
  )
  if(lua_ADDED)
    file(GLOB _lua_sources CONFIGURE_DEPENDS ${lua_SOURCE_DIR}/*.c)
    list(FILTER _lua_sources EXCLUDE REGEX "/(lua|luac|onelua|ltests)[.]c$")
    set_source_files_properties(${_lua_sources} PROPERTIES LANGUAGE CXX)
    add_library(lua_static STATIC ${_lua_sources})
    target_include_directories(lua_static SYSTEM PUBLIC ${lua_SOURCE_DIR})
    if(MSVC)
      target_compile_options(lua_static PRIVATE /TP /W0)     # C sources as C++; Lua's own code, not ours to warn about
    endif()
    add_library(lua::lua ALIAS lua_static)
  endif()

  # stb: stb_image (PNG / TGA / BMP / JPEG decoding) and stb_dxt (DXT1/5 blocks) for a mod's replacement textures
  # (loader/src/runtime/texture_files.cpp; the implementations are compiled once in loader/src/runtime/stb_impl.cpp). Header only.
  CPMAddPackage(
      NAME stb
      GITHUB_REPOSITORY nothings/stb
      GIT_TAG ${STB_COMMIT}
      DOWNLOAD_ONLY YES
  )
  if(stb_ADDED AND NOT TARGET stb_headers)
    add_library(stb_headers INTERFACE)
    target_include_directories(stb_headers SYSTEM INTERFACE ${stb_SOURCE_DIR})
  endif()

endif()

# GoogleTest (only needed for the unit tests)
if(MIRAGE_BUILD_TESTS)
  CPMAddPackage(
      NAME googletest
      GITHUB_REPOSITORY google/googletest
      GIT_TAG ${GOOGLETEST_VERSION}
      OPTIONS
          "INSTALL_GTEST OFF"
          "BUILD_GMOCK OFF"
          "gtest_force_shared_crt OFF"   # Static CRT, like the rest of the project (CMAKE_MSVC_RUNTIME_LIBRARY)
  )
endif()

# Google Benchmark (only needed for the benchmarks: bench/, mirage bench)
if(MIRAGE_BUILD_BENCHMARKS)
  CPMAddPackage(
      NAME benchmark
      GITHUB_REPOSITORY google/benchmark
      GIT_TAG ${GOOGLEBENCH_VERSION}
      OPTIONS
          "BENCHMARK_ENABLE_TESTING OFF"
          "BENCHMARK_ENABLE_GTEST_TESTS OFF"
          "BENCHMARK_ENABLE_INSTALL OFF"
          "BENCHMARK_INSTALL_DOCS OFF"
          "BENCHMARK_ENABLE_WERROR OFF"
          "BENCHMARK_ENABLE_LIBPFM OFF"
  )
endif()

if(MIRAGE_DEV_TOOLS)
# CPM Licenses
CPMAddPackage(
        NAME CPMLicenses.cmake
        GITHUB_REPOSITORY cpm-cmake/CPMLicenses.cmake
        VERSION ${CPM_LICENSES_VERSION}
)

# Format.cmake
# GitHub Repository: https://github.com/TheLartians/Format.cmake
CPMAddPackage(
        NAME Format.cmake
        VERSION ${FORMATDOTCMAKE_VERSION}
        GITHUB_REPOSITORY TheLartians/Format.cmake
        OPTIONS
            "FORMAT_SKIP_CMAKE NO"  # Set to yes skip cmake formatting
            "FORMAT_SKIP_CLANG NO"  # Set to yes skip clang formatting
            "CMAKE_FORMAT_EXCLUDE cmake/CPM.cmake"  # path to exclude (optional, supports regular expressions)
            "CMAKE_FORMAT_EXTRA_ARGS -c /path/to/cmake-format.{yaml,json,py}" # extra arguments for cmake_format (optional)
)
endif()
# -------------------------------------------------------------------------------------------------
# Utility and ImGui libraries for the full build (not the staged build's bare stages)

# readerwriterqueue: a lock-free single-producer, single-consumer queue. Header only; MirageHook fetches the same version.
CPMAddPackage(
        NAME readerwriterqueue
        GITHUB_REPOSITORY cameron314/readerwriterqueue
        GIT_TAG ${READERWRITERQUEUE_VERSION}
        DOWNLOAD_ONLY YES
)
if(NOT TARGET readerwriterqueue)
    add_library(readerwriterqueue INTERFACE)
    target_include_directories(readerwriterqueue SYSTEM INTERFACE ${readerwriterqueue_SOURCE_DIR})
endif()

# simdutf: Unicode validation and transcoding (UTF-8/16/32, Latin-1) with SIMD
CPMAddPackage(
        NAME simdutf
        GITHUB_REPOSITORY simdutf/simdutf
        GIT_TAG ${SIMDUTF_VERSION}
        OPTIONS
            "SIMDUTF_TESTS OFF"
            "SIMDUTF_BENCHMARKS OFF"
            "SIMDUTF_TOOLS OFF"
            "SIMDUTF_ICONV OFF"
            "BUILD_TESTING OFF"
)

# libuv: an event loop for sockets, pipes, timers, file watching and child processes
CPMAddPackage(
        NAME libuv
        GITHUB_REPOSITORY libuv/libuv
        GIT_TAG ${LIBUV_VERSION}
        OPTIONS
            "LIBUV_BUILD_SHARED OFF"
            "LIBUV_BUILD_TESTS OFF"
            "LIBUV_BUILD_BENCH OFF"
            "BUILD_TESTING OFF"
)

# cpptrace: stack traces with symbols (dbghelp on Windows)
CPMAddPackage(
        NAME cpptrace
        GITHUB_REPOSITORY jeremy-rifkin/cpptrace
        GIT_TAG ${CPPTRACE_VERSION}
        OPTIONS
            "CPPTRACE_BUILD_SHARED OFF"
            "BUILD_SHARED_LIBS OFF"
)

# ghc::filesystem: std::filesystem's interface as a header (paths UTF-8 on every platform)
CPMAddPackage(
        NAME ghc_filesystem
        GITHUB_REPOSITORY gulrak/filesystem
        GIT_TAG ${GHC_FILESYSTEM_VERSION}
        DOWNLOAD_ONLY YES
)
if(NOT TARGET ghc_filesystem)
    add_library(ghc_filesystem INTERFACE)
    target_include_directories(ghc_filesystem SYSTEM INTERFACE ${ghc_filesystem_SOURCE_DIR}/include)
endif()

# DirectXMath: SIMD vector and matrix math (D3D's conventions). Header only.
CPMAddPackage(
        NAME DirectXMath
        GITHUB_REPOSITORY microsoft/DirectXMath
        GIT_TAG ${DIRECTXMATH_VERSION}
        DOWNLOAD_ONLY YES
)
if(NOT TARGET directxmath)
    add_library(directxmath INTERFACE)
    target_include_directories(directxmath SYSTEM INTERFACE ${DirectXMath_SOURCE_DIR}/Inc)
endif()

# bitsery: binary serialization, header only
CPMAddPackage(
        NAME bitsery
        GITHUB_REPOSITORY fraillt/bitsery
        GIT_TAG ${BITSERY_VERSION}
        DOWNLOAD_ONLY YES
)
if(NOT TARGET bitsery)
    add_library(bitsery INTERFACE)
    target_include_directories(bitsery SYSTEM INTERFACE ${bitsery_SOURCE_DIR}/include)
endif()

# span-lite: nonstd::span, header only
CPMAddPackage(
        NAME span-lite
        GITHUB_REPOSITORY martinmoene/span-lite
        GIT_TAG ${SPAN_LITE_VERSION}
        DOWNLOAD_ONLY YES
)
if(NOT TARGET span_lite)
    add_library(span_lite INTERFACE)
    target_include_directories(span_lite SYSTEM INTERFACE ${span-lite_SOURCE_DIR}/include)
endif()

# ImGuiColorTextEdit: a code editor widget with syntax highlighting (no CMake project: built here against our ImGui)
CPMAddPackage(
        NAME ImGuiColorTextEdit
        GITHUB_REPOSITORY BalazsJako/ImGuiColorTextEdit
        GIT_TAG ${IMGUICOLORTEXTINPUT_GIT_COMMIT}
        DOWNLOAD_ONLY YES
)
if(ImGuiColorTextEdit_ADDED)
    add_library(imgui_color_text_edit STATIC ${ImGuiColorTextEdit_SOURCE_DIR}/TextEditor.cpp)
    target_include_directories(imgui_color_text_edit SYSTEM PUBLIC ${ImGuiColorTextEdit_SOURCE_DIR})
    target_link_libraries(imgui_color_text_edit PUBLIC imgui)
endif()

# imgui_markdown: Markdown drawn with ImGui. Header only (the implementation is in the header).
CPMAddPackage(
        NAME imgui_markdown
        GITHUB_REPOSITORY enkisoftware/imgui_markdown
        GIT_TAG ${IMGUI_MARKDOWN_GIT_COMMIT}
        DOWNLOAD_ONLY YES
)
if(imgui_markdown_ADDED)
    add_library(imgui_markdown INTERFACE)
    target_include_directories(imgui_markdown SYSTEM INTERFACE ${imgui_markdown_SOURCE_DIR})
    target_link_libraries(imgui_markdown INTERFACE imgui)
endif()

# imgui-flame-graph: a flame graph widget (profiling)
CPMAddPackage(
        NAME imgui_flame_graph
        GITHUB_REPOSITORY bwrsandman/imgui-flame-graph
        GIT_TAG ${IMGUI_FLAMEGRAPH_GIT_COMMIT}
        DOWNLOAD_ONLY YES
)
if(imgui_flame_graph_ADDED)
    add_library(imgui_flame_graph STATIC ${imgui_flame_graph_SOURCE_DIR}/imgui_widget_flamegraph.cpp)
    target_include_directories(imgui_flame_graph SYSTEM PUBLIC ${imgui_flame_graph_SOURCE_DIR})
    target_link_libraries(imgui_flame_graph PUBLIC imgui)
    target_compile_definitions(imgui_flame_graph PRIVATE IMGUI_DEFINE_MATH_OPERATORS)   # It uses ImVec2's + and -
endif()

# imgui_club: ImGui extras by its author; the memory editor among them. Header only.
CPMAddPackage(
        NAME imgui_club
        GITHUB_REPOSITORY ocornut/imgui_club
        GIT_TAG ${IMGUI_CLUB_GIT_COMMIT}
        DOWNLOAD_ONLY YES
)
if(imgui_club_ADDED)
    add_library(imgui_club INTERFACE)
    target_include_directories(imgui_club SYSTEM INTERFACE ${imgui_club_SOURCE_DIR})
    target_link_libraries(imgui_club INTERFACE imgui)
endif()

# -------------------------------------------------------------------------------------------------

target_link_libraries(${PROJECT_NAME} PRIVATE
        imgui imjson MirageHook::MirageHook d3d9 shell32 spdlog::spdlog zstd_compress nlohmann_json::nlohmann_json winmm Threads::Threads
        readerwriterqueue simdutf::simdutf uv_a cpptrace::cpptrace ghc_filesystem directxmath bitsery span_lite
        imgui_color_text_edit imgui_markdown imgui_flame_graph imgui_club)
if(MIRAGE_GAME)
  target_link_libraries(${PROJECT_NAME} PRIVATE mirage::sdk UnrealLib::Linker UnrealLib::Assets UnrealLib::Writer UnrealLib::Compiler UnrealLib::Decompiler lua::lua toml11::toml11 stb_headers
          Zydis dbghelp)   # Zydis (built for MirageHook) and dbghelp: the crash handler's disassembly and symbols
  target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/loader/include)   # mirage/api.h: the mod ABI
endif()

# -------------------------------------------------------------------------------------------------
