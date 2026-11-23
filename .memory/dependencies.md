# Dependency boundaries

[CPM.cmake](../cmake/CPM.cmake) bootstraps dependency acquisition. [Versions.cmake](../cmake/Versions.cmake) holds pins and [Libraries.cmake](../cmake/Libraries.cmake) selects and links packages. Verify active options and target links before assuming a declared package is available in a minimal build.

| Area | Dependency and boundary |
| --- | --- |
| Engine | UnrealLib reads and rewrites UE3 packages, supplies bytecode and UnrealScript tooling, and supports SDK generation. MirageHook implements native detour mechanics. |
| UI and platform | ImGui with Win32/DX9 backends and UI extensions, DirectXMath, Windows D3D9. First-party native mods have a public-API-only library allowlist. |
| Data and runtime | toml11 for manifests and mod data, Glaze for JSON paths in the active refactor, Lua for embedded scripts, stb for asset decoding. Check call sites for current JSON types. |
| Diagnostics | spdlog/fmt, zstd for compressed log archives, optional Tracy. |
| Archive | xxHash for standardized XXH3-64 and simdutf for UTF-8 validation. Runtime hashing is a separate concern. |
| Development | GoogleTest/CTest and Google Benchmark. Python dependencies are in `pyproject.toml` and `uv.lock`. Managed mods use .NET 10 x86 through hostfxr. |

The active Glaze refactor selects C++23 for the loader. The standalone MPAF target asks for C++20. Preserve local CPM overrides and dependency pins. Benchmark and justify a new library before adding it. [Root build](../CMakeLists.txt), [archive build](../archive/CMakeLists.txt), [.NET provisioning](../cmake/DotNet.cmake)
