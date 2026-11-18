# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**Mirage** is a mod loader for Mirror's Edge (2008, PC, 32-bit UE3), structured like Geode: a loader with a stable C
ABI and an in-game mod manager, a generated engine SDK, a CLI, and mods as folders with a manifest. It grew out of
Mirror's Edge Menu (memenu); the menu's features (TAS, trainer, editors, console) stayed there. The README is the
overview; `docs/mirage-plan.md` has the plan's steps, milestones and their status.
`docs/AI-GUIDE.md` (concepts, rules, recipes) and `docs/FILE-MAP.md` (`python tools/file_map.py`) help assistants.

## Commands

Everything goes through the `mirage` tool (`tools/mirage_tools/`), which sets up the 32-bit MSVC environment itself:

```
uv run mirage build                 # debug Mirage.dll (-c release); DLL lands in build/Mirage.dll
uv run mirage errors                # build everything, errors grouped by file
uv run mirage test                  # unit tests (GoogleTest via CTest), no game needed
uv run mirage test -R Manifest      # only tests matching a regex
uv run mirage game-test             # the in-game self-test (loader/src/selftest.cpp) in the real game
uv run mirage game-test --no-build --only "Mirage maps,Mirage world access"   # selected steps
uv run mirage install-game          # build and install into the game
uv run mirage features              # feature switches (cmake/Features.cmake)
uv run mirage sdk                   # regenerate the SDK (sdk/) from the game's packages
```

- Never run a bare `cmake --build`: without the MSVC environment the cache picks up clang and the configure breaks.
- Build folders are `cmake-build-x86-<config>`; feature toggles (`--enable/--disable/--only`) are remembered per folder.
- `game-test` installs the build into the game, starts it windowed and minimized (`--visible` to opt out), and closes
  it. The game must not run elevated. Results: `<game>\Binaries\mirage\logs\mirage.log`. The user is often at the PC:
  say before a run that the game will start. Unit tests can't prove game features; only `game-test` can.
- Mirage and memenu both install a `dinput8.dll` into the game; whichever was installed last is the one that runs.
- The debug build uses a local UnrealLib checkout (`-D CPM_UnrealLib_SOURCE=...`, see `cmake/Versions.cmake` and
  memory notes); `cmake/Libraries.cmake` / `Versions.cmake` often hold the user's own uncommitted pins.

## Layout

- `loader/include/mirage/`: the public API (`api.h`, the C ABI; `mirage.hpp`, `sdk.hpp`, `imgui.hpp`).
- `loader/src/`: the loader (`runtime/`, `game/`, `core/`, the UI and mod manager, `selftest.cpp`).
- `loader/proxy/`: `dinput8.dll`, which loads `<Binaries>\mirage\Mirage.dll` once the game window is up.
- `mods/`: first-party mods embedded in the DLL (`mirage-levels`, `mirage-collision`).
- `examples/mods/`, `tests/`, `tools/` (CLI, sdkgen, C# binding generator, test host), `managed/`, `installer/`,
  `launcher/`, `docs/`.

## Architecture

- **Threads.** The *render thread* is the D3D9 EndScene hook (ImGui, `OnRender`, every control). The *game thread*
  is the engine tick (`OnTick`, `game::RunOnGameThread`). UObjects are touched only on the game thread.
- **Modules** (`loader/src/core/module.h`) self-register with `MEM_MODULE(T)`; there is no central list. Each feature
  file is also listed in `cmake/Features.cmake` (`mirage_feature(... STAGE n FILES ...)`). Controls
  (`loader/src/core/setting.h`) are module members, saved by name to `Mirage.json`. `OnDisable` must undo everything
  (it also runs on unload).
- **Game layer** (`loader/src/game/`): `game::Pawn()/Controller()/World()` (null during loads; check every time),
  level loading (`levels::`), object lookups, `game::FindPattern` + MirageHook (`core/hooking.h`) for engine hooks
  (patterns, never fixed addresses).
- **SDK** (`sdk/`, generated, namespace `Classes`): never edit it; change `tools/sdkgen` and run `mirage sdk`.
  `AActor::SetLocation()` doesn't move the player (write `Location`); functions with a native index must go through
  `mirage::call` (`loader/src/runtime/call.h`), which also lays out 4-byte UBOOL params correctly.
- **Runtime** (`loader/src/runtime/`): mods are folders in `Binaries\mirage\mods` with `mod.toml` plus a native DLL,
  `main.lua`, C# (.NET 10 x86 via hostfxr), and/or content (`content/`, `levels/`, `maps/`). The C ABI is frozen by
  `tests/test_abi.cpp`: only append to `MirageApi`, and mirror it in the Lua binding (`mod_lua.cpp`) and C#
  (`managed/Mirage`). UnrealLib (packages, decompiler, UnrealScript compiler) builds classes from `.uc` source in the
  game (`runtime/classes`, `class_source`). `firstparty.cpp` migrates a memenu install's user data once.
- **Input service** (`loader/src/tas/engine`, `input_monitor`): key injection on the game thread, all that is left of
  the TAS; the self-test presses keys with `KeyEvent`/`HoldKey`.
- **Self-test** (`loader/src/selftest.cpp`): one function per step, listed in `FeatureList()`. Test mods live in
  `tests/mods/` and are deployed by `tools/mirage_tools/example_mods.py`; game packages they need are copied from the
  player's install at deploy time (never commit game content). Checks must not pass by time or gravity alone; wait on
  state (`WaitFor`, `WaitPlayerReady`), not tick counts, and add no deliberate stalls (runs must stay fast).

## Conventions

- Work on `main` (or a feature branch); commit each verified milestone with `git commit -- <paths>` (the user keeps
  their own files staged); push only when asked.
- Comments and docs are plain prose sentences describing what the code does and why, in the existing style.
- New libraries must earn their place: measure first (see `docs/LIBRARIES.md`).
