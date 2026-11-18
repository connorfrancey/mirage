# Mirage for AI assistants

A guide for coding assistants, written for smaller local models (Qwen and similar) as much as for large ones: short
sections, the facts that are not obvious from the code, and recipes for the common tasks. Read this, then
[FILE-MAP.md](FILE-MAP.md) to find files, then the header of the file you change. `AGENTS.md` / `CLAUDE.md` have the
commands; [mirage-plan.md](mirage-plan.md) has the plan (its steps are also called stages).

## 1. What Mirage is, in ten lines

- A **mod loader** for **Mirror's Edge (2008, PC)**: Unreal Engine 3, a **32-bit** Windows game. Mirage is a DLL
  (`Mirage.dll`) loaded into the game by a small proxy `dinput8.dll` (`loader/proxy/`).
- It loads **mods**: folders in `<game>\Binaries\mirage\mods\`, each with a `mod.toml` and any of: a native DLL,
  `main.lua`, `main.cs` (C#), UnrealScript `.uc` classes, and data (`levels/`, `kismet/`, `maps/`, `packages/`,
  `content/`, `routes/`).
- Mods reach the game only through the **mod API**: a C function table, `MirageApi` in `loader/include/mirage/api.h`.
  Lua (`runtime/mod_lua.cpp`) and C# (`managed/Mirage`) are bindings over the same table.
- The game's own types come from a **generated SDK** (`sdk/`, namespace `Classes`): C++ structs that match the game's
  memory. Never edit `sdk/`; change `tools/sdkgen` and run `uv run mirage sdk`.
- There is an **in-game UI** (ImGui over D3D9): the mod manager, modules (switches and settings per feature).
- **Tests**: unit tests (GoogleTest, no game) and an **in-game self-test** (`loader/src/selftest.cpp`) that starts the
  real game and checks features there.
- The **plan** (`docs/mirage-plan.md`) is a numbered list of steps; each has Scope, Tests, Milestone and a `**Status**`
  paragraph saying what is done. `/stage` (Claude Code) reports the current one.

## 2. Words used everywhere

| Word | Meaning |
|---|---|
| UObject, UClass, AActor | The engine's objects. Every actor is a UObject. `Classes::APawn` etc. in the SDK. |
| Pawn, Controller | The player's body (`game::Pawn()`) and its controller (`game::Controller()`). Null while a level loads. |
| Game thread | The engine's tick. **Only here may UObjects be read or written.** |
| Render thread | The D3D9 `EndScene` hook: ImGui, `on_render`, every UI control. Never touch UObjects here. |
| ProcessEvent | The engine function that calls an UnrealScript function by its UFunction. Hooks sit on it. |
| Native | A script function implemented in C++ in the engine (has a native index). Call it through `mirage::call`. |
| Kismet | UE3's visual scripting: sequences of ops (events, actions, conditions, variables) linked together. |
| Layer | Data a mod adds to a level: `levels/<map>.toml` (actors) or `kismet/<map>.toml` (Kismet edits). |
| Checkpoint restart | The game respawns the player at the active checkpoint. Spawned actors go, the level's Kismet stays. |
| Module | A switchable feature with settings (`core/module.h`), shown in a tab; mods register their own. |
| Step / stage | A numbered section of the plan. |

## 3. Directory map (the parts you will touch)

- `loader/include/mirage/api.h`: the C ABI. **Frozen**: only append fields to `MirageApi` (tests/test_abi.cpp checks).
- `loader/src/runtime/`: the mod runtime. `mods.cpp` (loading, the gates into mod code), `mod_game.cpp` (the API's game
  functions, event dispatch), `mod_lua.cpp` (the Lua binding, `mirage.*`), `layers.cpp` (level layers),
  `kismet*.cpp` (Kismet read, layers, trace), `classes.cpp` / `class_source.cpp` (`.uc` compiled in the game).
- `loader/src/game/`: the game layer: `game::Pawn()`, `World()`, levels, object lookups, `FindPattern`.
- `loader/src/core/`: modules, settings, hooking, caches, events, the scheduler.
- `loader/src/features/misc/mod_manager.cpp`: the Mod Manager window.
- `loader/src/selftest.cpp`: every in-game test step; the list is `FeatureList()` near the end of the file.
- `tests/`: unit tests (`test_*.cpp`, listed in `tests/CMakeLists.txt`) and `tests/mods/` (mods the self-test uses).
- `mods/`: first-party mods embedded in the DLL. `examples/mods/`: example mods for authors.
- `tools/mirage_tools/`: the `mirage` CLI (Python, run with `uv run mirage ...`).

## 4. How things flow

**A mod loads.** `runtime/mods.cpp` finds folders, reads `mod.toml` (`manifest.cpp`), resolves order and
dependencies (`resolve.cpp`), then loads the native DLL (`mirage_mod_load(api, mod)`), `main.lua`, or `main.cs`.

**The game calls a mod.** Every call into a mod's code passes one of two gates in `runtime/mods.cpp`:
`detail::Enter` (hooks, events, natives, triggers, moves) and `ModModule::Call` (a module's `on_tick`, `on_render`,
...). Both run the call under SEH (`Guarded`): a crash stops only that mod and records it in `crashes.toml`. Both
time the call for the profiler (`runtime/mod_profile.h`).

**Events.** The game layer reports ticks, level loads, deaths, actors added/removed; `mod_game.cpp`'s `Dispatch*`
functions send them to subscribed mods (`MIRAGE_EVENT_*` in api.h; `mirage.on("tick", fn)` in Lua).

**Layers.** Each tick `layers::Tick()` / `kismet_layers::Tick()` compare each mod's `levels/<map>.toml` /
`kismet/<map>.toml` (by file time) with what is applied, and spawn, apply or undo. A changed file re-applies.

**A `.uc` class.** A layer naming `classes/X.uc` makes `classes::BuildSource` compile it on a worker thread with
UnrealLib into its own package, then the engine loads it. Until it is built the entry is "pending".

## 5. Rules that are easy to break

1. **UObjects on the game thread only.** From the render thread use `game::RunOnGameThread(fn)`; the self-test uses
   `OnGameThread([&] { ... })`.
2. **Check `game::Pawn()` / `World()` for null every time**; they are null during loads.
3. **Never edit `sdk/`.** Regenerate it.
4. **Natives through `mirage::call`** (`runtime/call.h`): it lays out 4-byte UBOOL parameters correctly. Calling a
   native SDK function directly can pass bools wrong.
5. **`AActor::SetLocation()` does not move the player**: write `pawn->Location`.
6. **Engine hooks by pattern** (`game::FindPattern`), never fixed addresses.
7. **ABI**: append to `MirageApi` only; mirror a new function in Lua (`mod_lua.cpp`) and C# (`managed/Mirage`).
8. **`OnDisable` undoes everything** a module did (it also runs on unload).
9. **UnrealScript in mods**: each `.uc` file is its **own package**. A class can't name another mod class
   (`class'MyOther'` fails); find it by name (`A.IsA('MyOther')`). Qualify game classes in literals:
   `class'Engine.UIInteraction'`, `class'TdGame.SeqAct_TdSupersMessage'`. `"text" @ SomeObject` doesn't compile; use
   `SomeObject.Name`.
10. **Names must exist**: a layer can only set a `name` property to a name the engine already has (a compiled `.uc`
    that uses the name literal adds it). Layers can't set string properties.
11. **Don't run a bare `cmake --build`** (wrong compiler). Use `uv run mirage build` / `errors`.
12. **The self-test starts the real game.** Say so before running it; it must not run elevated.
13. Self-test checks wait on **state** (`WaitFor`, `WaitPlayerReady`), never on time alone; no deliberate sleeps.
14. Files are CRLF in git; editing tools that write LF are fine (git converts), but don't mass-rewrite line endings.

## 6. Recipes

**Build and check.**

```powershell
uv run mirage errors                         # build, errors grouped by file
uv run mirage test                           # all unit tests
uv run mirage test -R ModProfile             # some
uv run mirage game-test --only "Mirage mission"          # one in-game step (starts the game)
uv run mirage game-test --no-build                        # all in-game steps, current build
```

In-game results: `<game>\Binaries\mirage\selftest-result.json` (steps, problems, notes) and
`<game>\Binaries\mirage\logs\mirage.log`. `<game>` is `C:\Program Files (x86)\Steam\steamapps\common\mirrors edge`.

**Add an in-game self-test step.**
1. Write `void TestSomething()` in `loader/src/selftest.cpp`: `Step& s = Begin("Mirage something"); Probe p(s);`,
   checks with `p.Problem("why")`, facts with `s.notes.push_back(...)`, end with `End(s);`.
2. Add `{"Mirage something", &TestSomething, false},` to `FeatureList()`.
3. If it needs a mod, add a folder under `tests/mods/<id>/` (`mod.toml` with `id` equal to the folder name). It is
   deployed automatically: Lua (`main.lua`), C# (`main.cs`), or content (`levels/`, `maps/`, `routes/`, `kismet/`).
4. Talk to a test mod through shares: the self-test `ShareSet(&self, "key", "value")` with `self.id = "selftest"`; the
   mod reads `mirage.shared("selftest", "key")` and answers with `mirage.share("key", value)`.

**Add a unit test.** `tests/test_<name>.cpp` with GoogleTest; add it and any game-free source it needs to the list in
`tests/CMakeLists.txt`. Code that touches the game can't be unit tested: split the pure part into its own file
(like `runtime/mod_profile.cpp`).

**Add a Lua function.** Declare `static int MyFn(lua_State* L);` in the `LuaMod` class and write
`int LuaMod::MyFn(lua_State* L)` in `runtime/mod_lua.cpp` next to similar ones, with a
`// mirage.my_fn(args): what it returns` comment, and add `{"my_fn", MyFn}` to the `fns[]` table in the same file. If
it needs a new API entry, append it to `MirageApi` (api.h), implement it in `mod_game.cpp`, and check
`MIRAGE_API_HAS(self.api_, my_fn)` before calling it.

**Add something to a level from data (no code).** `levels/<map>.toml` in the mod:

```toml
[[actor]]
name = "Pad"
class = "classes/MyPad.uc"        # or a game class, "Engine.PointLight"
location = [19309.0, -3009.0, 9017.0]
rotation = [0, 16384, 0]          # pitch, yaw, roll in Unreal units (65536 = 360 degrees)
[actor.properties]
Strength = 1800.0
```

Kismet edits: `kismet/<map>.toml` with `[[op]]`, `[[link]]`, `[[unlink]]`, `[[set]]` (format in
`runtime/kismet_layers.h`).

**Read the game's UnrealScript.** Decompile with UnrealLib (see the memory note or ask the user for the path):
`UnrealScriptDecompiler.exe "<CookedPC>\TdGame.u" -s <CookedPC> -o <out> -q`.

**Finish a plan step.** Update its `**Status (date)**` paragraph in `docs/mirage-plan.md` (what was done, how it was
verified, what is left), and commit with `git commit -- <paths>` (only your files: the user keeps their own files
staged). Every step also finishes one major feature of the in-game level editor (the plan's "The in-game level
editor, a feature every stage").

## 7. Where to look first for a task

| Task | Start in |
|---|---|
| A mod API function misbehaves | `runtime/mod_game.cpp` (C side), `runtime/mod_lua.cpp` (Lua side) |
| A mod doesn't load | `runtime/mods.cpp`, `runtime/manifest.cpp`, `runtime/resolve.cpp`, the log |
| Something about levels / actors placed by mods | `runtime/layers.cpp` |
| Kismet | `runtime/kismet.cpp`, `runtime/kismet_layers.cpp` |
| UnrealScript classes in mods | `runtime/classes.cpp`, `runtime/class_source.cpp`, `docs/mod-classes.md` |
| Player, movement | `game/player_state.*`, `runtime/moves.cpp` |
| A crash | `<game>\Binaries\mirage\crashlogs\`, `runtime/crash_reporter.cpp` |
| The mod manager window | `features/misc/mod_manager.cpp` |

## 8. Keeping these docs current

`docs/FILE-MAP.md` is generated from the first comment of each header: after adding or renaming headers, run
`python tools/file_map.py` from the repository root.
Update this guide when a rule in section 5 changes.
