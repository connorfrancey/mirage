# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

An internal mod menu, trainer and TAS tool for Mirror's Edge (2008, PC, 32-bit UE3), becoming **Mirage**, a mod
loader (branch `mirage`). The README's "Developing" section is the long-form reference; `doc/mirage-plan.md` has the
Mirage steps, milestones and their status.

## Commands

Everything goes through the `memenu` tool (`tools/memenu_tools/`), which sets up the 32-bit MSVC environment itself:

```
uv run memenu build                 # debug medge_menu.dll (-c release); DLL lands in build/medge_menu.dll
uv run memenu errors                # build everything, errors grouped by file
uv run memenu test                  # unit tests (GoogleTest via CTest), no game needed
uv run memenu test -R Replay        # only tests matching a regex
uv run memenu game-test             # the in-game self-test (src/selftest.cpp) in the real game
uv run memenu game-test --no-build --only "Mirage maps,Mirage time trials"   # selected steps
uv run memenu features              # feature switches (cmake/Features.cmake)
uv run memenu sdk                   # regenerate the Mirage SDK (sdk/) from the game's packages
```

- Never run a bare `cmake --build`: without the MSVC environment the cache picks up clang and the configure breaks.
- Build folders are `cmake-build-x86-<config>`; feature toggles (`--enable/--disable/--only`) are remembered per folder.
- `game-test` installs the build into the game, starts it windowed and minimized (`--visible` to opt out), and closes
  it. The game must not run elevated. Results: `<game>\Binaries\memenu\logs\memenu.log`. The user is often at the PC:
  say before a run that the game will start. Unit tests can't prove game features; only `game-test` can.
- The debug build uses a local UnrealLib checkout (`-D CPM_UnrealLib_SOURCE=...`, see `cmake/Versions.cmake` and
  memory notes); `cmake/Libraries.cmake` / `Versions.cmake` often hold the user's own uncommitted pins.

## Architecture

- **Threads.** The *render thread* is the D3D9 EndScene hook (ImGui, `OnRender`, every control). The *game thread*
  is the engine tick (`OnTick`, `game::RunOnGameThread`). UObjects are touched only on the game thread.
- **Modules** (`src/core/module.h`) self-register with `MEM_MODULE(T)`; there is no central list. Each feature file
  is also listed in `cmake/Features.cmake` (`memenu_feature(... STAGE n FILES ...)`). Controls (`src/core/setting.h`)
  are module members, saved by name to `MedgeMenu.json`. `OnDisable` must undo everything (it also runs on unload).
- **Game layer** (`src/game/`): `game::Pawn()/Controller()/World()` (null during loads; check every time), level
  loading (`levels::`), object lookups, `game::FindPattern` + MinHook for engine hooks (patterns, never fixed
  addresses).
- **SDK** (`sdk/`, generated, namespace `Classes`): never edit it; change `tools/sdkgen` and run `memenu sdk`.
  `AActor::SetLocation()` doesn't move the player (write `Location`); functions with a native index must go through
  `mirage::call` (`src/runtime/call.h`), which also lays out 4-byte UBOOL params correctly.
- **Mirage runtime** (`src/runtime/`): mods are folders in `Binaries\memenu\mods` with `mod.toml` plus a native DLL,
  `main.lua`, C# (.NET 10 x86 via hostfxr), and/or content (`content/`, `levels/`, `maps/`, `routes/`). The C ABI is
  `api/mirage/api.h`, frozen by `tests/test_abi.cpp`: only append to `MirageApi`, and mirror it in the Lua binding
  (`mod_lua.cpp`) and C# (`managed/Mirage`). First-party features live in `mods/` as mods and are embedded in the DLL.
  UnrealLib (packages, decompiler, UnrealScript compiler) builds classes from `.uc` source in the game
  (`runtime/classes`, `class_source`).
- **TAS** (`src/tas/`): virtual clock and input injection on the game thread; `.merp` replay format documented in
  `src/tas/replay.cpp`; `tas::Macro` builds macros in code (used by self-tests). Playbacks are verified frame by frame.
- **Self-test** (`src/selftest.cpp`): one function per step, listed in `FeatureList()`. Test mods live in
  `tests/mods/` and are deployed by `tools/memenu_tools/example_mods.py`; game packages they need are copied from the
  player's install at deploy time (never commit game content). Checks must not pass by time or gravity alone; wait on
  state (`WaitFor`, `WaitPlayerReady`), not tick counts, and add no deliberate stalls (runs must stay fast).

## Conventions

- Commit each verified milestone on the feature branch with `git commit -- <paths>` (the user keeps their own files
  staged); push only when asked.
- Comments and docs are plain prose sentences describing what the code does and why, in the existing style.
- New libraries must earn their place: measure first (see `doc/LIBRARIES.md`).
