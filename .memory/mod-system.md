# Mod discovery and lifecycle

| Stage | Owner |
| --- | --- |
| Locate folders, parse TOML, resolve compatible load order | `FindInstalledMods`, `Parse`, `Load`, and the dependency resolver. [Manifest](../loader/src/runtime/manifest.cpp), [resolver](../loader/src/runtime/resolve.cpp) |
| Publish modules and API bindings | `LoadPhase` and `LoadCode` own native, Lua, managed, and content setup. Active runtime state lives in `MirageMod`. [Lifecycle](../loader/src/runtime/mods.cpp), [record](../loader/src/runtime/mod_internal.h) |
| Track changes | `mod_watch` collects folder notifications and falls back to scans. Game-thread `ReloadChanged` settles writes and calls `Reload`. [Watcher](../loader/src/runtime/mod_watch.cpp), [lifecycle](../loader/src/runtime/mods.cpp) |
| Stop and undo | Disable modules, drain guarded callbacks, remove owned hooks and subscriptions, restore content and tracked changes, then unload code. [Lifecycle](../loader/src/runtime/mods.cpp), [transactions](../loader/src/runtime/changes.h) |

Discovery normally reads `mirage/mods`; the in-game harness also adds `selftest-mods`. First-party payloads come from the build, with startup installation and legacy data migration owned by `firstparty`. User action overrides live in the mod's root `config.toml`; menu control state is saved by the loader's config service. [First-party installation](../loader/src/runtime/firstparty.cpp), [menu config](../loader/src/config.cpp)

The committed baseline uses inline action and setting declarations and legacy entry-point fields. An in-progress worktree also supports external TOML declarations and `[entrypoints]`. Check [session status](sessions/2026-11-22-baseline.md) and current sources before relying on either branch of the authoring workflow.

For archive contents and distribution scope see [package format](package-format.md).
