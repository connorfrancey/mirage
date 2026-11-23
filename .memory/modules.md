# Component map

## Build targets

| Target or output | Responsibility and source |
| --- | --- |
| `Mirage` → `Mirage.dll` | Injected runtime and menu. Follow target setup and source selection from the root build. [CMake](../CMakeLists.txt) |
| `mirage_loader` → `dinput8.dll` | Proxy bootstrap. [CMake](../loader/proxy/CMakeLists.txt) |
| `mirage_installer`, `mirage_launcher` | Disk installation and running-process launch or injection. [Installer](../installer/CMakeLists.txt), [launcher](../launcher/CMakeLists.txt) |
| `mirage::sdk` | Static library of generated UE3 types and layouts. [SDK build](../sdk/CMakeLists.txt) |
| `mirage::mpaf` | Offline payload archive reader and writer. [Archive build](../archive/CMakeLists.txt) |
| `mirage_sdkgen`, `mirage_index`, `mirage_assets` | SDK, object index, and asset catalog or preview helpers. Check feature and working-tree availability. [SDK tools](../tools/sdkgen/CMakeLists.txt) |
| `mirage_testhost`, `mirage_crashdump` | Standalone D3D9 harness and out-of-process dump helper. [Test host](../tools/testhost/CMakeLists.txt), [dump helper](../loader/crashdump/CMakeLists.txt) |

The active CMake worktree factors target setup into `cmake/project/`. It has a `mirage_library` helper and example calls for a proposed Core/UI/Script split. Inspect actual targets before referring to MirageCore, MirageAssets, or MirageDX as linkable libraries. [Root CMake](../CMakeLists.txt)

## Change routing

- Menu item or control → `core/module.h`, `core/setting.h`; built-in registration uses `MEM_MODULE`. Compile-time feature wiring belongs in [Features.cmake](../cmake/Features.cmake).
- Runtime mods and API → [mods.cpp](../loader/src/runtime/mods.cpp), [public API](../loader/include/mirage/api.h), then the area-specific `runtime/mod_*` bridge.
- In-game editing → level, Script, Kismet, Animation and Object Inspector features in [features/misc](../loader/src/features/misc). Follow their calls into the corresponding runtime service for persistence and engine mutations.
- First-party mod → [mods](../mods) and [FirstPartyMods.cmake](../cmake/FirstPartyMods.cmake). Examples are author-facing; tests use fixtures under [tests/mods](../tests/mods).
