# Project orientation

Mirage is an in-process mod loader and development toolkit for the 32-bit Windows release of Mirror's Edge (2008, Unreal Engine 3). It grew from Mirror's Edge Menu and now ships a stable C ABI, generated engine SDK, first-party and third-party mods, and in-game management and editing tools. [Overview](../README.md)

## Vocabulary

| Term | Meaning |
| --- | --- |
| Loader | The injected `Mirage.dll` runtime. The optional `dinput8.dll` proxy bootstraps it. |
| Mod | One installed folder with an ID and `mod.toml`. It may contain native, Lua, managed, and data entry points. |
| Module | A toggleable or actionable menu item with callbacks and settings. A mod may register several modules and data-only mods can get a generated content module. |
| Cooked package | A UE3 asset or map read by the game, for example `.upk` or `.me1`. |
| Payload archive | An MPAF container for mod files. Its package contract is separate from UE3 asset serialization. |

The normal runtime target is `Mirage`. Source areas such as core, assets, rendering, and scripting are subsystem boundaries inside it. The suggested `MirageCore` targets appear in CMake examples. See [target inventory](modules.md).

Start with [architecture](architecture.md) for runtime work and [tooling](tooling.md) for CLI work. Use the [authoring guide](../docs/mod-authoring-ai.md) for mod recipes and the [file map](../docs/FILE-MAP.md) for additional navigation. The older AI guide lives at `docs/AI-GUIDE.md` or `docs/AI-GUIDE.md.old` depending on the checkout.
