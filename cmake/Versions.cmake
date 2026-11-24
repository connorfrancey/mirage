
# ==============================================================================
# Mirage - Third-Party Dependency Versions
# ==============================================================================
#
# Centralized dependency versions, tags, and commit hashes.
# Referenced by CPMAddPackage() declarations throughout the project.
#
# ==============================================================================


# ==============================================================================
# Core Dependencies
# ==============================================================================

# YAML
set(RAPIDYAML_VERSION v0.16.0)

# Serialization
set(GLAZE_VERSION v9.1.0)

# Formatting
set(FMT_VERSION           12.2.0)

# Logging
set(SPDLOG_VERSION        v1.17.0)

# JSON
set(RAPIDJSON_VERSION v1.1.0)

# TOML
set(TOML11_VERSION        v4.4.0)

# Serialization
set(BITSERY_VERSION       v5.2.5)

# Compression
set(ZSTD_VERSION          v1.5.7)

# Parsing
set(FASTFLOAT_VERSION     v8.3.3)


# ==============================================================================
# Unreal Engine / Scripting
# ==============================================================================

# UnrealLib
# Branch: m4-m5-properties-reflection-bytecode
# Includes the UnrealScript compiler and package writers.
set(UNREALLIB_GIT_TAG     8f1c5e9a10b3e8782a0d83a59ac1e389ae638d84)

# Lua scripting runtime
set(LUA_VERSION           v5.4.8)
set(SOL2_VERSION v3.3.0)


# ==============================================================================
# Mirage Framework
# ==============================================================================

# MirageHook - Mirage's native x86 hooking library
# All game hooks are routed through this library.
set(MIRAGEHOOK_GIT_COMMIT 64d8d303375fede272210cd1db24b5dd9a9ac499)

# mmultiplayer (legacy)
set(MMULTIPLAYER_GIT_TAG  7b1eb25ddc0fc1ce498d024e7b2f9d1980b9eca3)


# ==============================================================================
# ImGui
# ==============================================================================

# Core
set(IMGUI_VERSION         v1.92.9b-docking)

# File browser
set(IMGUIFILEBROWSER_GIT_COMMIT
        47a1884524d6a3df5d99f70ecc82e1ed0ccd26cf)

# File dialog
set(IMGUIFILEDIALOG_VERSION v0.6.8)

# Code editor
set(IMGUICOLORTEXTINPUT_GIT_COMMIT a07eb7fa4d1c097e202950d75a41e583f5a28d2b)   # goossens' fork, 2026-11-04

# Icons: IconFontCppHeaders names the codepoints of Font Awesome 6, whose solid font is embedded (cmake/Fonts.cmake)
set(ICONFONTCPPHEADERS_GIT_COMMIT 210b5a399a64270674560d633638952d1e8d804d)
set(FONTAWESOME_VERSION 6.7.2)
set(FONTAWESOME_SOLID_SHA256 af19d135d3a935b3ebfbd80320716ffe1202052c5f68dc2c5f1abc57005ac605)

# Models: assimp, in the offline model tool only (tools/model: `mirage model`), never in Mirage.dll
set(ASSIMP_VERSION v6.0.5)

# Reflection: ImGui inputs drawn for a struct (Sven-vh/ImReflect: no releases, a commit)
set(IMREFLECT_GIT_COMMIT  c4a07dba3badc78285a5cfdf324ab3f8412c3d50)

# Markdown
set(IMGUI_MARKDOWN_GIT_COMMIT
        68fb679b1ab314940589445b36934682cd3e8027)

set(IMGUI_RICHMD_VERSION  v1.0.0)

# Node editor
set(IMGUI_NODEEDITOR_VERSION v0.9.4)

# Gizmos
set(IMGUIZMO_VERSION     1.10)

# Plotting
set(IMPLOT_VERSION       v1.1)

# Curves
set(IMCURVE_GIT_COMMIT   8cc8dc3a971593c6ce5aa8f418776545b2dbc3bb)

# Flame graph
set(IMGUI_FLAMEGRAPH_GIT_COMMIT
        aae0bd9665d1379c511d0b8a64d3752301a327a2)

# ImGui Club
set(IMGUI_CLUB_GIT_COMMIT
        b083116866e05e0d4bccb2c7f28d94a76ee4965e)


# ==============================================================================
# Containers / Data Structures
# ==============================================================================

set(UNORDERED_DENSE_VERSION v5.3.3)
set(ATOMICQUEUE_VERSION     v1.9.4)


# ==============================================================================
# Reflection / Type Utilities
# ==============================================================================

set(MAGICENUM_VERSION      v0.9.8)
set(NAMEOF_VERSION         v0.11.0)


# ==============================================================================
# Memory Management
# ==============================================================================

set(MIMALLOC_VERSION       v3.6.1)


# ==============================================================================
# Concurrency
# ==============================================================================

# MirageHook's pinned copy; share this dependency where possible.
set(READERWRITERQUEUE_VERSION v1.0.6)


# ==============================================================================
# System / Platform Utilities
# ==============================================================================

set(LIBUV_VERSION          v1.53.0)
set(GHC_FILESYSTEM_VERSION v1.5.16)
set(SPAN_LITE_VERSION      v0.11.0)


# ==============================================================================
# Unicode / Text Processing
# ==============================================================================

set(SIMDUTF_VERSION        v9.3.3)


# ==============================================================================
# Graphics / Mathematics
# ==============================================================================

set(DIRECTXMATH_VERSION    jun2026)


# ==============================================================================
# Diagnostics / Profiling
# ==============================================================================

set(CPPTRACE_VERSION       v1.0.4)

# Tracy client and profiler must use matching versions.
set(TRACY_VERSION          v0.14.1)


# ==============================================================================
# Testing / Benchmarking
# ==============================================================================

set(GOOGLETEST_VERSION     v1.17.0)
set(GOOGLEBENCH_VERSION   v1.10.0)


# ==============================================================================
# Build Tools
# ==============================================================================

set(FORMATDOTCMAKE_VERSION 1.8.2)
set(CPM_LICENSES_VERSION   0.0.7)


# ==============================================================================
# Single-Header / Vendored Dependencies
# ==============================================================================

# stb (stb_image, stb_dxt)
set(STB_GIT_COMMIT         4e9b368bfc071e97206ea5c6adba68dd0cbf8897)


# ==============================================================================
# Legacy / Transitional Dependencies
# ==============================================================================



# ==============================================================================
# Dependency Notes
# ==============================================================================

# fmt:
#   UnrealLib's pinned version. spdlog should use this copy through
#   SPDLOG_FMT_EXTERNAL.

# Tracy:
#   The Tracy profiler GUI must match the client version.

# ReaderWriterQueue:
#   MirageHook already pins this dependency. Avoid duplicate copies.
