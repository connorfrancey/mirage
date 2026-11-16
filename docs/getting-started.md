# Getting started: your first Mirage mod

A Mirage mod is a folder with a `mod.toml` and what it runs: a Lua file, C# files, a native DLL, or content (textures,
level additions, maps). This page takes one from nothing to running in the game. You need Mirror's Edge (PC) with
Mirage installed (`mirage install-game`, or the installer), and this repository with its `mirage` tool
(`uv run mirage ...`).

## 1. Make it

```
uv run mirage new my-mod --lang lua
```

`--lang` picks the template:

| | What it is | Build |
|---|---|---|
| `lua` | `main.lua`: no compiler, saved files reload in the game | none |
| `cs` | `main.cs`, a C# script: compiled by the game when the mod loads | none (no .NET SDK needed) |
| `native` | C++ with its own CMake project, against the mod API headers only | `mirage mod build` (MSVC or clang-cl, x86) |

The folder has a `mod.toml` (its id, version, the Mirage versions it is for, its settings) and one module: a switch in
the menu's Mods column, with a setting from `mod.toml`, one made in code, and a tick that reads the player.

## 2. Install it

```
uv run mirage mod install my-mod
```

It is checked the way the runtime reads it (`mirage mod check` on its own), built when it is native, and copied into
the game's `Binaries\mirage\mods\my-mod`. Settings the game saved there (`config.toml`) are kept across installs.

Start the game, press INSERT for the menu, and switch your module on in the Mods column. The mod manager (Misc > Mod
Manager) lists it with its version, what it registered and its log lines.

## 3. Change it

With `hot_reload = true` in `mod.toml` (the templates have it), a change reloads the mod while the game runs:

- Lua and C# scripts: edit the file in the installed folder, or edit yours and run `mirage mod install` again.
- Native: `mirage mod install` rebuilds it; the runtime runs a copy of the DLL, so the new one replaces it.

The mod's log lines are in `Binaries\mirage\logs\mirage.log`, prefixed with `[mod my-mod]`.

## 4. Share it

```
uv run mirage mod package my-mod
```

makes `dist/mods/my-mod-<version>.zip`: the mod as it is installed. Unzipped into another player's `Binaries\mirage\mods`,
it works the same.

## What a mod can do

- **The API**: [api-reference.md](api-reference.md), generated from `loader/include/mirage/api.h` (`mirage docs api`).
  Lua has it as `mirage.*`, C++ as the `mirage::` wrappers (`mirage/mirage.hpp`), C# as `Mod`, `Module` and
  `GameObject`.
- **The game's classes**: the generated SDK (`sdk/`) with the decompiled UnrealScript next to each class header: the
  reference for property paths (`pawn.Location.Z`) and the functions a mod can hook or call.
- **Examples**: `examples/mods/` (hello in each language, a script patch, a retexture, a bounce pad written in
  UnrealScript) and the self-test's own mods in `tests/mods/` (hooks, world access, classes, level additions).
- **Input actions and moves**: `[actions.<name>] key = "Q"` in `mod.toml` gives the mod an `action` event when the
  game gets that key (a TAS's replayed input too); `move_register` (Lua `mirage.move`) adds a move of the mod's own
  that the game runs like its own ones (`tests/mods/double-jump` is a double jump in a page of Lua).
- **More**: [csharp-mods.md](csharp-mods.md) (C# in depth), [mod-classes.md](mod-classes.md) (new UnrealScript classes).

## When it goes wrong

- A crash in your mod's code stops your mod, names it in the log and the mod manager, and it isn't loaded at the next
  start until you allow it again there. A crash of the game shows the crash window: what happened, where, the likely
  cause, both call stacks; "Copy the report" gives it all.
- `mirage mod check` says what the runtime would refuse in `mod.toml` before you start the game.
