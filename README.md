# Mirage

A mod loader for **Mirror's Edge** (2008, PC). Mirage loads mods with the game, gives them a live, reflected view of
the Unreal Engine 3 underneath, and keeps them isolated, hot-reloadable and undoable. Mods are written in **Lua**,
**C#** (.NET 10) or **C/C++**, or are plain data (textures, defaults, level layers, maps), and all of them use one
stable API.

It is built like [Geode](https://geode-sdk.org) is for Geometry Dash: a loader with a stable ABI and an in-game mod
manager, the engine's types as a generated SDK, a CLI for building and testing, and mods as folders with a manifest.

- **Objects by name:** read and write any property, call any function (`pawn.Location.Z = 500`,
  `mirage.call(pawn, "SetPhysics", {...})`), with layouts checked against the engine for all 13,481 of its types.
- **Hooks:** before, after or instead of any UnrealScript function; native detours chained between mods; code
  injected into the middle of the game's own bytecode; bytecode patches; natives of your own.
- **New classes:** UnrealScript (`.uc`) compiled in the game, or classes described from Lua and C#.
- **Content:** replacement textures (`content/textures/*.png`), default-property overrides, actors placed into levels
  (`levels/<map>.toml`), streamed sublevels, new maps in the level select (`maps/*.me1`, the packages they use in
  `packages/`), checkpoints placed from a mod (a `TdCheckpoint` subclass in a layer).
- **World access:** actors queried by class, tag and distance; handles that never point at the wrong object;
  events for actors coming and going, streaming and restarts.
- **Reversible changes:** writes made in a transaction are put back when the mod asks, unloads, reloads or crashes.
- **Safety:** every call into a mod is guarded; a crashing mod is named, stopped and left out next start; Lua runs in a
  sandbox with a memory limit and an instruction budget.

## Installing

1. Download **`mirage_installer.exe`** from the [latest release](https://github.com/connorfrancey/mirage/releases/latest).
2. Run it and accept the administrator prompt (the game lives in Program Files). If Windows says "Windows protected
   your PC", click **More info → Run anyway**: the installer isn't code-signed.
3. It finds Mirror's Edge by itself. Click **Install**, then start the game. Press **INSERT** to open Mirage's window
   (the mod manager and the mods' settings).

The installer puts a small loader (`dinput8.dll`) next to `MirrorsEdge.exe` and Mirage in a `mirage` folder beside
it; the game's own files are never modified. Mods go in `mirage\mods\<id>\`. To start the game without Mirage once,
create an empty file called `disabled` in the `mirage` folder. Uninstall from Windows' Installed apps.

Coming from **Mirror's Edge Menu** (the project Mirage grew out of): the first start copies your own mods, your level
layers and the Collision View's caches from the menu's `memenu` folder into `mirage` (once; the menu's folder is not
changed).

## Writing a mod

[docs/getting-started.md](docs/getting-started.md) takes one from nothing to the game:

```
uv run mirage new my-mod --lang lua      # or cs (a C# script), native (C++)
uv run mirage mod install my-mod         # checked, built if native, copied into the game
uv run mirage mod test my-mod            # its own tests (mirage.test), run in the game
```

Every function a mod can call is in [docs/api-reference.md](docs/api-reference.md), generated from
`loader/include/mirage/api.h`.

A mod is a folder in `mirage\mods\` with a `mod.toml`:

```toml
id = "low-gravity"
version = "1.0.0"
name = "Low Gravity"
mirage = "^0.1"          # the loader versions it works with

[settings.scale]
type = "float"
name = "Gravity scale"
module = "Low Gravity"
default = 0.5
min = 0.1
max = 1.0
```

and its code in `main.lua` (or `main.cs`, or a DLL built against `loader/include/mirage/api.h`):

```lua
local m = mirage.module { name = "Low Gravity", tab = "Mods" }
local Scale = mirage.setting("scale")
local change

mirage.on("tick", function()
    local world = mirage.world()
    if m:enabled() and world and not change then
        change = mirage.transaction("low gravity", function()
            world.WorldGravityZ = world.WorldGravityZ * Scale:get()
        end)
    elseif not m:enabled() and change then
        mirage.revert(change)          -- (unloading the mod would put it back too)
        change = nil
    end
end)
```

Save the file while the game runs and the mod reloads. [`examples/mods/`](examples/mods) has a mod of each kind
(native C++, Lua, C#, a C# script, a `.uc` class, a texture, a script patch) and
[`examples/mods/README.md`](examples/mods/README.md) is the guide to the API. The full C ABI is
[`loader/include/mirage/api.h`](loader/include/mirage/api.h) (append-only, frozen by `tests/test_abi.cpp`); C++ mods
use its wrapper `mirage.hpp`, C# mods `managed/Mirage`, and the engine's types are in the generated SDK (`sdk/`).

## Repository layout

| Folder | What |
|---|---|
| `loader/include/mirage/` | The public API: `api.h` (the C ABI), `mirage.hpp` (C++ wrapper), `sdk.hpp`, `imgui.hpp` |
| `loader/src/` | The loader: the runtime (`runtime/`: mods, Lua, .NET, reflection, hooks, injection, classes, content, layers, maps, changes, world events), the game layer (`game/`), the input service (`tas/engine`, `tas/input_monitor`), the in-game UI and mod manager, the self-test |
| `loader/proxy/` | `dinput8.dll`: sits next to the game and loads `mirage\Mirage.dll` |
| `sdk/` | The Mirror's Edge SDK, generated from the game's packages (`tools/sdkgen`; never edited by hand) |
| `managed/` | `Mirage.dll` for C# mods (.NET 10, x86) |
| `mods/` | Mirage's first-party mods, built into the loader: `mirage-levels` (level select, sublevels, restart), `mirage-collision` (Collision View), `mirage-grapple` (Grappling Hook) |
| `examples/mods/` | Example mods and the mod author's guide |
| `installer/`, `launcher/` | `mirage_installer.exe`; `mirage_launcher.exe` (injects into a running game) |
| `tests/` | Unit tests (GoogleTest), the self-test's test mods, the sources of test maps |
| `tools/` | The `mirage` CLI (`tools/mirage_tools`), the SDK generator, the C# binding generator, the test host |
| `cmake/` | Dependencies (`Libraries.cmake`, `Versions.cmake`), features, embedding of the first-party mods |
| `docs/` | The plan and status (`mirage-plan.md`), C# mods, classes from mods, memory, infrastructure |

## Building

You need Visual Studio with "Desktop development with C++", CMake 4.2+, Ninja and [uv](https://docs.astral.sh/uv/)
(`setup.cmd dev` installs what is missing). Everything is 32-bit like the game; the tool sets up the MSVC environment
itself.

```
uv run mirage build                 # debug Mirage.dll (-c release for release) -> build/Mirage.dll
uv run mirage errors                # build everything, errors grouped by file
uv run mirage test                  # unit tests, no game needed
uv run mirage game-test             # the in-game self-test in the real game (installs, runs, closes it)
uv run mirage install-game          # build and install into the game
uv run mirage sdk                   # regenerate sdk/ from the game's packages
uv run mirage package               # the installer, in dist/
```

The loader builds against [UnrealLib](https://github.com/connorfrancey/UnrealLib) (packages, decompiler, bytecode
encoder, UnrealScript compiler); `-D CPM_UnrealLib_SOURCE=<a local checkout>` builds against yours.

## Testing

- **Unit tests** (`tests/`): the manifest and resolver, semver, the ABI layout, settings, caches, memory, detours,
  textures, UnrealLib, the installer on a fake game folder. No game needed.
- **`game-test`**: starts the game (windowed, minimized), loads the Prologue and runs the self-test
  (`loader/src/selftest.cpp`): layouts and calls against the live engine, hooks, mods of every kind and their
  lifecycle (crashes, hot reload), the UI layer, C# mods, content and textures, script patches and injection, natives,
  classes, level additions, maps (including one built with the game's own editor), world access, reversible changes,
  the crash reporter. Movement is driven by key presses through the engine's input. Checks wait on state, never on
  time alone, and the game closes by itself with every step's result.

## License

MIT (see `LICENSE`).
