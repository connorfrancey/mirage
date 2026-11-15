# Mirage: a gameplay modding framework for Mirror's Edge

> **Repository split (2026-11-14).** Mirage now lives in its own repository, with only the loader core: the loader,
> mod manager, API, SDK, CLI, examples, tests and the first-party mods `mirage-levels` and `mirage-collision`. The
> menu's features named below (TAS, trainer, editors, Level Editor, console, log viewer, the other `memenu-*` mods)
> stayed in Mirror's Edge Menu; where this plan mentions them, it is history or work for that project.

Mirage turns the Mirror's Edge menu into a framework for changing and creating gameplay: a mod loader in the spirit of
Fabric (a loader plus hooks, everything else in mods) and Geode (a native runtime with a stable API, an in-game mod
manager and tooling), whose purpose is what mods do with it. A mod should be able to inspect the live engine, change
the player, movement, animation and physics, edit the levels' Kismet, place and wire interactive objects, and build
new mechanics and whole experiences, then debug, test, record and replay them in the game. The menu's own features are
the first mods built on it.

UnrealLib (`C:\dev\cpp\lib\UnrealLib`, the C++ UELib port) is the foundation for everything that has to understand
the game's packages and script. Mods are written in **Lua** (no compiler, hot reload, sandboxed), **C#** (.NET,
scripts compiled in the game or built assemblies, hot reload; see `docs/csharp-mods.md`) or as native DLLs (for what
the others are too slow or too limited for); all use the same API.

## How to read this plan

Each step ends in a milestone with a check that proves it; a milestone is committed when its check passes. Claims are
marked by where they come from:

- **Verified** (in the code, or by a self-test in the game) and **measured** (a number from a benchmark or self-test,
  with where it was measured) are facts.
- **Target** is a number this plan sets for work not done yet: a target until something measures it.
- **Unverified** and **to investigate** are what the plan expects of the engine but nobody has checked yet. Work that
  depends on them starts with the investigation, and the plan changes if the engine says otherwise.
- **Proposed** marks an architecture or API shape that is a plan, not code.

Steps 1–13 keep their numbers: the code's comments refer to them ("step 6", "step 13"). Steps from 14 on were
re-sequenced on 2026-11-13 (see [Old and new step numbers](#old-and-new-step-numbers)).

## Vision

A developer should be able to:

- inspect the game's live engine state: objects, actors, the player, the level's Kismet;
- modify the existing gameplay systems: movement, animation, physics, triggers, AI;
- create new movement mechanics, gameplay objects and Kismet logic, and extend existing levels or build new ones;
- do it from Lua, C# or C++, mostly inside the running game, with visual editors where text is the wrong tool;
- debug, test, record and replay the result, with the TAS verifying it frame by frame;
- share it as an ordinary Mirage mod that turns off cleanly.

## Design philosophy

- **Gameplay first.** The order of work is: reach the live game state; inspect and change the objects that are
  already there; change the player, movement, animation and physics; understand and edit Kismet; create new gameplay
  objects and mechanics; give all of it a visual editor; let mods assemble whole experiences. Ecosystem features
  (distribution, profiles, multiplayer, long-term ABI policy) come after gameplay modding is useful.
- **Infrastructure in step with the features that need it.** A gameplay step builds the smallest infrastructure it
  needs (a change log before editing tools, handles before world queries) and no more. Infrastructure for a future
  that hasn't arrived waits.
- **Vertical slices.** A gameplay capability lands with its runtime, its API (C, Lua, C#), its editor workspace and an
  end-to-end self-test in the game at the same time, not as separate layers months apart.
- **The engine decides.** Mirror's Edge is UE3 (2008): Kismet, TdMove states, AnimTrees and PhysX 2 are what exist.
  Nothing here assumes UE4/5 Blueprints, components or physics. Each system starts by surveying what the game really
  has (classes, functions, natives, where reflection stops) before an API is designed over it.
- **The rules that cost crashes so far stay rules:** no work under the loader lock; UObjects touched only on the game
  thread; the rendering thread is not the game thread; the 62 fps cap; the TAS's memory ownership (see the project
  memory notes); everything goes through the API (appended, never changed); every capability has an in-game self-test;
  nothing breaks the TAS's determinism.
- **Every step names its cost** (frame time, load time, address space), measured in the self-test, because the game is
  a 32-bit process and every capability competes for the same 2 GB.

## Current implementation state

Verified on 2026-11-13 against the repository and the self-tests.

| Step | Status | What exists |
|------|--------|-------------|
| 1. Groundwork | Done (tree split replaced by step 9's mods) | C++20, UnrealLib and Lua 5.4 (as C++) in the DLL; packages read in the game |
| 2. Live engine model | Done | Layouts of all 13,481 types and 33,987 properties match the engine; `mirage::call` for natives and UBOOLs |
| 3. SDK generator | Done | `sdk/` (2,354 classes, 515 structs, 11,310 functions), `mirage sdk`; decompiled script and LuaLS stubs |
| 4. Mod format, ABI v1 | Done | `loader/include/mirage/api.h`: 137 functions in `MirageApi` (frozen by `tests/test_abi.cpp`), `mirage.hpp`, `mod.toml` |
| 5. Lifecycle | Done, one gap | Resolver, phases, crash isolation, hot reload; crashes on a mod's own thread attributed but not tested |
| 6. Hooks and events | Done | Script function hooks (0.7-0.8 ns unhooked), native detours, 7 events, objects by path from Lua/C++/C# |
| 7. Content mods | Defaults and textures done | Sounds, meshes and T3D map edits not done |
| 8. Script patching | Done | Byte-identical encoder over 13,770 scripts; live patching |
| 9. Menu as mods | Mostly done | 10 first-party mods, UI layer (`MirageUi`), mod manager; the runtime's own tools stay in the runtime |
| 10. Tooling and distribution | Not started | Split into steps 35 and 40 |
| 11. Injection, natives | Done | Injection points in bytecode, mod natives |
| 12. Classes, compiler | Done | `.uc` compiled in the game, classes described from Lua/C#, lifetime |
| 13. Levels | Done | Level layers, sublevels, mod maps, routes, Level Editor, a map built with the 2008 editor |

What the code already has for gameplay work (verified):

- **Objects:** `object_find`, `object_get` / `object_set` by dotted path with arrays (`runtime/reflect`),
  `objects_of_class`, `object_path`, `object_keep`; every handle except kept ones is weak, checked against its GObjects
  slot on use (`game/object_ref.h`, `game/object_query`). There is no generation counter: a slot reused by a new object
  of the same address is the case a handle can't tell yet.
- **The player:** `player_pawn`, `player_controller`, `world_info`; `player_state_capture` / `player_state_apply` /
  `player_reset_movement` (save states); the `PLAYER_STATE` event with the old and new `EMovement` and `event_skip`
  (the Strang tool takes the move handler's place).
- **Movement:** the game's moves are objects: `TdPawn.Moves` (`TArray<UTdMove*>`), 128 `TdMove_*` classes in the SDK,
  each with `CanDoMove`, `StartMove`, `StopMove`, `ResetMove`, `SetMoveTimer`, `UpdateViewRotation`, `HandleDeath`.
  Step 8 patched `TdMove_Jump.StartMove` (Higher Jumps), step 11 injected into it, the trainer (Lua) reads and writes
  move state.
- **Kismet:** 305 Kismet classes (`Seq*`, `TdSeq*`) in the SDK; `USequence.SequenceObjects`, `USequenceOp`
  `InputLinks` / `OutputLinks` / `VariableLinks` / `EventLinks`, `ActivateCount`, `bActive`. Nothing in Mirage reads
  or edits sequences yet.
- **Animation:** 80 `AnimNode*` / `TdAnim*` classes; `PlayCustomAnim`, `PlayCustomAnimByDuration`, `CustomAnimNode`,
  `AnimTree`. Nothing in Mirage uses them yet.
- **Physics:** `KActor`, `KActorSpawnable`, `InterpActor`; `AddImpulse`, `AddForce`, `SetRBLinearVelocity`,
  `SetRBPosition`, `WakeRigidBody`; `Trace`, `FastTrace`, `TraceComponent`, `FindSpot`. The game ships PhysX 2.x
  (`PhysXCore.dll`, `NxCooking.dll`, `NxCharacter.dll`; the exact version is unverified). Nothing in Mirage uses them yet.
- **Gameplay pieces:** the traversal pieces are volumes (`TdZiplineVolume`, `TdSwingVolume`, `TdLadderVolume`,
  `TdBalanceWalkVolume`); level layers (step 13) already spawn any placeable class, including a mod's own
  (`LayerPad extends Trigger` in the tests).
- **Editor UI:** the menu's ImGui (1.92), the UI layer for mods, the Level Editor (its own gizmo), the Collision View
  (3D world drawing for mods: `world_draw_*`), the script editor, console and log viewer. The ImGui node editor
  (`imgui-node-editor` v0.9.4) and ImGuizmo 1.10 are pinned in `cmake/Versions.cmake` but not built or used.
- **TAS:** memory-snapshot rewinds, `.merp` replays verified per frame, macros in code; mod memory stays out of
  snapshots; `STATE_RESTORED` tells mods to look their objects up again.

Where Mirage started (2026-11-09), for the record:

| Had | Gap for a mod loader |
|------|----------------------|
| `dinput8.dll` proxy loader, installer, launcher | Loaded one DLL; no discovery, ordering or dependencies |
| `Mirage.dll`: 43 modules registered at build time (`MEM_MODULE`), settings, config, hotkeys, themes | Nothing could be added without rebuilding the DLL; no ABI |
| Game layer: pattern scans, Tick / ProcessEvent / death / level load hooks | Private to the DLL; one hook owner per function |
| mmultiplayer SDK (generated headers) | Wrong `UField`/`UStruct` layouts, 1-byte bools where the engine has 4-byte UBOOLs (`IsInState`, `SetMove`, ...) |
| UnrealLib M0–M9: packages, linker, properties, reflection, bytecode decoder, decompiler, assets, package writer, T3D | C++20 (the menu was C++17); no bytecode encoder; cooked packages hold no property memory offsets |
| Self-test in the real game (`mirage game-test`), 142 unit tests | Nothing that loaded third-party code |

Known limitations that shape the order of work (verified):

- A Lua module's tick costs 20-30 µs (release); a path read from C# about 10 µs. Fine for logic, too slow for
  per-object work every tick at scale.
- The game's Restart Checkpoint needs a checkpoint: on a map without one it leaves no player (found with
  `MirageEditorRoom`, 2026-11-13).
- About 2 in 12 full game-tests crash in the game's CRT heap (MSVCR80, `ntdll`) around the TAS steps, with no mod
  code on the stack; unexplained (step 6's notes).
- What a mod changes is undone by each feature on its own (content overrides, script patches, layers, classes);
  there is no common change log, so "what did this mod change?" has no single answer.

## Target architecture

```
MirrorsEdge.exe
 └─ dinput8.dll ............ loader: memory tracker, early phase, loads the menu DLL
     └─ Mirage.dll ..... runtime (Mirage): engine layer, reflection, hooks, events, content, editors, mod manager
         ├─ Lua 5.4 ....... one sandboxed state per Lua mod
         ├─ .NET 10 x86 ... C# mods (hostfxr), loaded only when a C# mod exists
         ├─ mods\mirage-*\  the menu's features as first-party mods (trainer, TAS, collision view, ...)
         └─ mods\<id>\ ..... mod.toml + main.lua / main.cs / <id>.dll + content\ levels\ maps\ routes\ classes
```

- **Stable C ABI** (`loader/include/mirage/api.h`: a versioned function table, plain structs, no STL across the boundary) with a
  header-only C++ wrapper (`mirage.hpp`), so mods built with another compiler keep working. Lua (`mod_lua.cpp`) and
  C# (`managed/Mirage`) bind to the same table.
- **Lua is the main way to write a mod.** Lua 5.4, compiled as C++ (errors unwind C++ frames instead of `longjmp`
  over them), one sandboxed state per mod (no `io`/`os.execute`/`package.loadlib`; files only inside the mod's folder),
  bound to the same API as the C ABI. Game objects are reached through the live reflection of step 2, so a Lua mod can
  read and write any property and call any function by name without generated bindings. Why not LuaJIT: Lua 5.4 builds
  from source with the project's CMake and static CRT, and its error handling interoperates with C++; LuaJIT's speed is
  not needed for mod logic, and the hot paths (hooks on functions no mod hooks) never enter Lua.
- **Mods need not contain code:** content, level layers, routes, and (planned) Kismet layers are data the runtime
  applies.

### Subsystem ownership

The repository's layout already separates these concerns; the gameplay work adds folders where a subsystem gets big
enough, not a reorganisation (proposed additions in *italics*):

| Concern | Where it lives | Notes |
|---|---|---|
| Public API | `loader/include/mirage/` | `api.h` (C ABI), `mirage.hpp`, `sdk.hpp`; append-only |
| Runtime core, mods, scripting | `loader/src/runtime/` (`mods`, `mod_lua`, `mod_dotnet`, `resolve`, `manifest`) | |
| Memory | `loader/src/runtime/mem/`, `loader/src/runtime/memory.cpp` | Budgets and pressure events (`docs/memory.md`) |
| Reflection and calls | `loader/src/runtime/reflect`, `call`, `live_layout`, `computed_layout` | |
| Hooks, injection, natives | `loader/src/runtime/fhooks`, `detours`, `inject`, `natives`, `script_patch` | |
| Engine integration | `loader/src/game/` (player, levels, object queries, flight) | Game thread only |
| Content and world data | `loader/src/runtime/content`, `texture_*`, `layers`, `maps`, `routes`, `classes` | |
| *World access (step 14)* | *`loader/src/runtime/world/`*: handles, actor queries, lifecycle events | Proposed |
| *Change log (step 15)* | *`loader/src/runtime/changes`* | Proposed |
| *Kismet (step 18)* | *`loader/src/runtime/kismet/`* | Proposed |
| *Movement, animation, physics (17, 20, 22, 23)* | *`loader/src/runtime/gameplay/`* | Proposed: one folder until it is big enough to split |
| Editors | `loader/src/features/misc/` today (Level Editor, script editor) | *`loader/src/editor/` (step 16): the workspace frame, inspector, explorer, node editor* |
| TAS | `loader/src/tas/`, `mods/memenu-tas` | |
| Tests | `tests/` (unit, test mods, map sources), `loader/src/selftest.cpp`, `bench/` | |

## Priorities

| Level | Meaning | Examples |
|---|---|---|
| **P0** | Core gameplay modification: usable as early as the engine allows | World access, actors, player and movement, Kismet, physics, triggers and pieces, custom mechanics, mod state under the TAS |
| **P1** | Tools that make gameplay modding practical | Editor core and inspector, Kismet editor and debugger, movement and animation workspaces, gameplay workspace, script REPL and debugger |
| **P2** | Platform infrastructure, built when a gameplay step needs it | Engine symbols, services between mods, C# parity, start-up and memory budgets, safe mode, saves, localization, local tooling |
| **P3** | Ecosystem, after the framework is useful | Distribution and updates, profiles, game builds, multiplayer, external control, leaderboards, offline testing, reports, ABI v2 |

Priorities are directional, not a reason to ignore dependencies: a P0 step that needs a piece of P2 builds the
minimum of it first (step 22 needs input actions, so it takes that part of old step 17; step 20 may need engine
symbols, so step 32 starts in a minimal form when it does).

## Old and new step numbers

The plan had 40 steps; it has 49 now. Steps 1–13 are unchanged. Four steps are new (14 world access, 16 editor core,
19 Kismet editor, 25 the gameplay workspace), and six old steps were split because their parts belong to different
priorities (10, 17, 20, 24, 31, 33).

| Old | New | Why it moved |
|---|---|---|
| 10 Tooling, docs, distribution | 35 (CLI, templates, docs), 40 (index, updates) | Local tooling is P2; distribution is P3 and must not block local mod creation |
| 14 Services between mods | 33 | P2: no gameplay step needs versioned services first; `share_set/get` covers today's needs |
| 15 Engine-level hooks | 32 (minimal form pulled in by 20 and 22) | Needed only where reflection and script hooks stop |
| 16 Rendering, Scaleform | 31 | P1 gameplay presentation, after the core systems |
| 17 Audio, input, controllers | 22 (input actions), 30 (audio, controllers) | Custom mechanics need input actions early; the rest later |
| 18 Multiplayer | 43 | P3: single-player gameplay first |
| 19 Determinism with mods | 24 | P0: custom mechanics must replay; moved up next to them |
| 20 Developer environment | 16 (inspector), 27 (debugger, profiler, native hot reload), 37 (permissions) | The inspector is the base of every editor; the rest follows |
| 21 C# parity | 34 | P2: grows with each gameplay step's API instead of as one step |
| 22 In-game IDE | 27 | Together with the REPL and debugger (P1) |
| 23 A hundred mods | 36 | P2: matters when many mods exist |
| 24 Safe mode, bisection, transactions | 15 (change log), 37 (safe mode, bisection) | Reversible edits are a prerequisite of editing tools; bisection isn't |
| 25 Static analysis, conflicts | 37 | P2, with safe mode |
| 26 Game builds | 42 | P3 |
| 27 Profiles | 41 | P3 |
| 28 Packages, updates | 40 | P3, with the index |
| 29 Kismet and missions | 18 (runtime), 26 (missions) | P0: Kismet is how the levels' gameplay is wired |
| 30 AI and enemies | 28 | P1, after the core systems it uses |
| 31 Movement and animation | 17 (inspection, tuning), 22 (new mechanics), 23 (animation) | P0, split so tuning lands before new mechanics |
| 32 Physics and gameplay pieces | 20 (physics, queries), 21 (triggers, pieces), 25 (the course) | P0 |
| 33 Cameras, photo mode, cinematics | 29 (camera API), 48 (photo mode, cinematics) | The camera API is gameplay; photo mode is ecosystem |
| 34 Saves and mod data | 38 | P2 |
| 35 Localization | 39 | P2 |
| 36 External control | 44 | P3 |
| 37 Challenges, leaderboards | 45 | P3 |
| 38 Offline testing | 46 | P3 |
| 39 Reports | 47 | P3 |
| 40 ABI v2 | 49 | Last by definition; must not delay gameplay APIs |

## Gameplay-first strategy

The next steps are vertical slices, each usable on its own:

1. **Reach and change the world** (14, 15, 16): handles and actor queries; every change revertible; an inspector to
   see it all. After these, a mod can find anything in the level, change it and put it back, and a developer can do
   the same by hand.
2. **The player and movement** (17): read and tune every move, live, with undo.
3. **Kismet** (18, 19): read the levels' logic, change it from data or code, then visually.
4. **Physics and pieces** (20, 21): props, forces and queries; triggers and the game's traversal pieces from mods.
5. **New mechanics** (22, 23, 24): moves of a mod's own, with input and animation, replayed by the TAS.
6. **Experiences** (25, 26): a playable course and missions assembled from all of the above, in a workspace.

Each slice ends with one of the end-to-end scenarios in [Testing](#testing-strategy) passing in the game.

## Roadmap

## Phase 1: done (steps 1–13)

Steps 1–9 made the loader and moved the menu's features into mods; steps 11–13 made it a platform for new
script, classes and levels. Their text is the record of what was built, measured and decided; it is kept as it
was written, with references to later steps renumbered.

### 1. Groundwork: C++20, UnrealLib in the process, runtime split

- Move the menu to C++20 and link UnrealLib (x86, static CRT `/MT` like the rest; its CMake already supports both).
- Split the source tree: `runtime/` (what becomes `mirage.dll`), `loader/include/` (public headers, empty for now), `mods/mirage/`
  (the features, still linked in for this step).
- Load Core.u, Engine.u and TdGame.u with UnrealLib inside the game, lazily and off the game thread; measure time and
  memory (32-bit process: the TAS snapshots already use a lot of it).

- Build Lua 5.4 from source (as C++, static CRT, x86) into the DLL and run a script in the game: the toolchain and
  the error path (a Lua error caught as a C++ exception, no leak) proven before any API is built on it.

**Milestone:** `mirage.dll` builds with UnrealLib and Lua linked; full `game-test` passes; the log shows the three
script packages parsed in the game with their class counts, the time taken and the memory used, within a set budget,
and a Lua script run in the game (including a caught error).

**Status (2026-11-09): done, except the source tree split** (`loader/src/runtime/` holds the new code; the move of the rest
into `runtime/` / `loader/include/` / `mods/mirage/` waits for step 9, when the features become mods). C++20, UnrealLib and Lua 5.4
(compiled as C++) are linked. In the game (self-test "Mirage runtime"): Core, Engine and TdGame read in 0.07 s (release),
2,039 classes with 11,169 properties, 44 MB while loaded and nothing left after (counted by the menu's own operator
new, `loader/src/runtime/memory.cpp`: the process's numbers move with the game's allocations); 13 Lua checks (sandbox, memory
limit, instruction budget, C++ destructors run when a Lua error unwinds).

### 2. A live engine model built on UnrealLib

- `mirage::Object / Class / Property / Function` handles over the live `GObjects`, typed with UnrealLib's
  `ReflectionSchema` (property types, enums, structs, function signatures from the game's own packages).
- Property access by name with type checks; function calls through `ProcessEvent` with parameter buffers sized and laid
  out from the engine's own `UFunction` (UBOOL = 4 bytes, return value last): the fix for the `IsInState` class of bugs.
- Compute memory layouts from UnrealLib (super's size, property sizes and alignment) and verify them against the live
  `UProperty::Offset` values (`player_state.cpp` already finds that field).

**Milestone:** every property of every class in Core, Engine and TdGame: computed offset == live offset (0 mismatches,
or each one explained and handled); `IsInState`, `SetMove` and the other bool-parameter calls go through the new path;
the self-test passes.

**Status (2026-11-09): done.** Self-test "Mirage layouts": all 13,481 classes, structs, functions and states UnrealLib
reads, and all 33,987 of their properties (offset, size, array size, bool bit), match the engine. The 70 the engine
has on top are intrinsic native types never exported to a package (Core.Class, Core.Field, the property classes,
Engine.World, Engine.Level, Engine.StaticMesh and its two structs, the net drivers, ...). The link rules, found by the
comparison (`loader/src/runtime/computed_layout.cpp`): byte aligned to 1, everything else to 4, structs to at least 4 and
Matrix to 16; consecutive bools share a dword except in functions; struct and class sizes are not padded, but a struct
property's element size is; a map property is 60 bytes. Self-test "Mirage calls": `loader/src/runtime/call.h` lays parameters
out from the engine's UFunction and runs functions with a native index too: UE3's ProcessEvent refuses those (that is
why the SDK's IsInState and SetLocation "returned" garbage), so the index (UFunction+0x94, checked against IsInState =
281 and SetLocation = 267) is cleared for the call. The menu's SDK calls that were wrong now go through it
(`loader/src/runtime/engine_calls.h`): SetMove (its flags came from stack garbage and its return value was written past the
SDK's struct), SetRotation and MoveSmooth (never ran: Teleport to Spawn now really turns the view, checked by its self-test;
Flight's collision mode calls MoveSmooth, which now runs, not yet covered by a test), ToggleReactionTime, SetSoundMode, StartGame, RestartFromLastCheckpoint.

### 3. The SDK generator

- Offline tool (UnrealLib + the layouts from step 2) that writes typed C++ headers: classes with correct offsets,
  UBOOL bitfields, enums, structs, function wrappers with correct parameter structs.
- Also writes the decompiled UnrealScript of each class next to its header (the reference mod authors read), and Lua
  type stubs (LuaLS `---@class` annotations) for every game class, so Lua mods get completion and type checks in an
  editor while the runtime itself stays reflection-driven.
- Replace the mmultiplayer SDK in the menu's own code.

**Milestone:** the generated SDK compiles with zero warnings; the menu builds on it with no mmultiplayer headers left;
`game-test` passes; a report lists every layout the old SDK had wrong.

**Status (2026-11-10): done.** `mirage sdk` builds and runs
`mirage_sdkgen` (`tools/sdkgen/`, configured by `sdkgen.toml`, read with toml11) and compiles what it writes. Inputs:
the layouts computed from the packages (the same `runtime/computed_layout.cpp` the self-test proves) plus
`engine-layouts.json`, what only the engine knows (601 intrinsic and native-only types, saved by the self-test "Mirage
layouts"). Output, in 1.5 s: `sdk/` with 2,354 classes, 515 structs and all 11,310 functions (static array parameters
included), with every offset and size a `static_assert`. It compiles with no warnings at /W4. Bits are checked at run
time (`detail::CheckLayouts`). `build/sdk-reference/` holds the decompiled script of the 2,110 classes (2,107 with no
placeholders), the LuaLS stubs and `old-sdk-report.md`. The report lays out the old SDK's C++ the way MSVC does under
its `pack(4)` and compares it with the engine: 197 classes, 16 structs and 6,002 parameter structs wrong (IsInState's
return value at 0x9 instead of 0xC, SetMove's parameters 4 bytes instead of 16, the intrinsic classes too small).
Known gap: two HTTPDownload properties lie inside its native base's opaque block and can't be declared. The menu builds on
`sdk/` (committed; mmultiplayer is only downloaded for the report), with one source change (`StaticMesh` is now typed:
intrinsic classes are named by their import). The full `game-test` passes, and "Mirage layouts" also runs the SDK's
bitfield checks in the game. The "still draws after switching off" check had been failing for Kickglitch Helper
before this step too: Game Status and Collision View text changed under it, so they are switched off while a HUD is
measured.

### 4. Mod format and ABI v1

- `mod.toml` (TOML, read with toml11): id, semver version, name, authors, runtime version range, game build,
  dependencies (required / optional / incompatible, with version ranges), entry DLL, load phase, settings schema,
  content folders.
- `mirage/api.h` v1: logging, the module and settings system (today's `ctl::` controls) and per-mod config, exposed
  through the ABI; the C++ wrapper gives mods the same `MEM_MODULE` / `ctl::Slider` style as today.
- The Lua API (`mirage` table) over the same calls: `mirage.module{...}`, settings, log, config, plus game object
  access through reflection (`player.Location.Z = 500`, `pawn:SetMove("Walking")`). One binding layer, so the C ABI
  and Lua can't drift apart.
- ABI tests: struct sizes and offsets frozen, the table only grows.

**Milestone:** a "hello" mod built in a separate CMake project (and with another MSVC toolset) and the same mod
written in Lua are both loaded, register a module with settings that save and load, and show in the menu; the ABI
tests pass.

**Status (2026-11-11): done, with two items moved on.** `loader/include/mirage/api.h` is ABI v1: a `MirageApi` function table
that starts with its size and only grows, a size-prefixed module description with five callbacks, settings
(checkbox, slider, int slider, text, key), per-mod `config.toml`, and drawing primitives (a native mod can't share the
menu's ImGui). `mirage.hpp` wraps it in the menu's own `Module` / `ctl::` style. `loader/src/runtime/manifest` reads `mod.toml`
with toml11 and warns about unknown keys; `semver` handles versions and npm/Cargo ranges. `loader/src/runtime/mods` runs each
mod module as a normal menu `Module`. Every call into native code is under SEH: a crash is logged and the module
stopped. The Lua binding (`mod_lua`) is written only against `MirageApi`. Its callbacks hold the mod's lock and get the
instruction budget, and `require` stays inside the mod's folder. Mods load in id order after their runtime range and
required and incompatible mods are checked; topological order is step 5.

The check: `tests/test_abi.cpp` freezes every offset; semver and manifest have unit tests. The in-game self-test
"Mirage mods" covers `examples/mods/hello`, built by its own CMake project with **clang-cl** instead of a second MSVC
toolset (only one is installed, and another compiler is the stronger proof), and `examples/mods/hello-lua`. Both load,
show in the Mods column, run `on_enable` / `on_tick` / `on_render` / `on_disable`, save, load and clamp their
settings, and keep their own config. `mirage game-test` builds and deploys them.

Settings can be declared in `mod.toml`, as Geode does in `mod.json` (2026-11-11): one `[settings.<key>]` table each,
with type (bool, int, float, string, choice, key), name, description, default and range, options or format. The runtime
makes them when their module registers, first in its panel, and the menu's config saves their values. Code finds them
by key: `setting_find` and `setting_type` are appended to ABI v1, Lua has `mirage.setting("key")`, C++ has
`mirage::Get<T>("key")`. Settings made in code still work alongside them.

Moved on: game object access from Lua through reflection (`player.Location.Z = 500`) goes with the hooks and events
of step 6, where Lua first gets game objects.

### 5. Loader and mod lifecycle

- Discovery in `mods\`, manifest validation, dependency resolution (topological order, semver ranges, cycles, missing
  or incompatible mods are disabled with a reason shown in the menu).
- Load phases: `early` (from the dinput8 loader, before the engine: memory tracking, file redirects), `engine` (SDK
  globals and hooks ready), `world` (first level loaded); unload in reverse order.
- Fault isolation: SEH around every call into a mod; a crash is attributed to the mod whose code faulted (address to
  module) and that mod is disabled at the next start, with the report.
- Lua mods: one state per mod, loaded from `main.lua`, with a memory limit and an instruction budget per callback
  (a runaway script is stopped and named, not left to freeze the game). Hot reload is the normal case for Lua (the file
  changed: the mod is reloaded); native mods only when they declare they support it.
- TAS: mod memory is classified (the TAS snapshot memory map) so rewinds don't corrupt mods; mods can opt in to
  having their state rewound.

**Milestone:** unit tests over synthetic manifests (chains, cycles, version conflicts, optional deps); in the game,
a set of test mods loads in the right order, a deliberately crashing mod is caught, named and disabled, and the rest
keep running; a hot reload round trip works ten times in a row.

**Status (2026-11-11): done, except two parts.** `loader/src/runtime/resolve` decides what loads and in what order:
dependencies first, then phase, then id. A mod is left out, with the reason, for a runtime out of range, being off or
having crashed, a missing, out-of-range, left-out or later-phase requirement, an incompatible mod, or a cycle of
required mods, and that spreads to the mods that need it. Optional dependencies only order (with warnings when they
can't), and optional cycles are broken. 13 unit tests cover chains, cycles, version conflicts, optional deps, phases and
the off list.

Phases: early at the menu's start, engine once the game layer is up, world on the first tick after a level loaded.
Mods of the later phases publish their modules as a batch: the menu's module list only grows once it runs (reserved,
count published atomically), the render thread adds them, gives them their saved config and the scheduler takes
them. The config now keeps the entries of modules not loaded this time.

Crash isolation: every call into native code is guarded. A crash stops the whole mod and logs its name, callback,
exception and `dll+offset`. It also writes `crashes.toml` next to the mods, which leaves the mod out at the next start
until its entry is deleted. The menu's crash handler names a mod whose code crashed on a thread it started.

Hot reload: Lua mods by default, native ones with `hot_reload = true` (their DLL runs from a copy in `.mirage`). A
watcher on the game thread checks the files twice a second and waits until they stop changing. A reload switches the
mod's modules off (its old `on_disable` undoes its changes), waits for calls still running, unloads, and loads again.
The modules it registers again are the same menu modules, so settings, keys and on/off state carry over.

Self-test "Mirage lifecycle" (`tests/mods`, deployed by `game-test`): order across the early and engine phases, the
cycle and missing-dependency mods left out, a world-phase mod, `crasher` (native, clang-cl) caught at
`crasher.dll+0x14E0` while the others go on and left out by the next resolution, Lua hot reload 10 times (new code
each time, setting and on state kept, 1 KB left after the first), the watcher, and native hot reload 10 times. One full
run of the five with this code crashed the game in `ntdll` during the "Speedrun timer" level restart, with no mod
involved; the four after it passed. Not explained yet.

Left: TAS memory classification for mods (the snapshot memory map; mods opting in to being rewound) waits for step 6,
where mods get game objects. Attributing crashes on a mod's own thread is in place but not covered by a test (it would
take the game down).

### 6. Hooks and events

- Event bus: tick, render, level load, death, input, player state change, script events (the existing hooks, now
  shared).
- UFunction hooks: before / after / replace, for script and native functions (through `ProcessEvent` and the native
  function pointer), keyed by the function, with a fast path so unhooked calls stay cheap (the per-tick cost lessons
  in the performance notes).
- Managed native detours (MinHook behind the runtime): several mods on one function, in priority order, conflicts
  reported, everything removed on unload.
- Lua gets events and UFunction hooks (with the parameters as a table, writable for "before" hooks); native detours
  stay native-only.

**Milestone:** two mods (one Lua, one native) hook the same UFunction, both run in priority order and unload
cleanly; the `ProcessEvent` hook costs under a set budget per call for unhooked functions and for a Lua hook (measured
in the performance module).

**Status (2026-11-11): done.** Function hooks (`loader/src/runtime/fhooks`): every script function runs through
`UObject::ProcessInternal`, whether it is called from script, from native code or through ProcessEvent, so that one
function is detoured. A lock-free table keyed by the frame's function sends hooked calls to their hooks, and an
unhooked call costs a hash and a load (measured: 0.7-0.8 ns). Hooks run before, after or instead of the function, in
priority order, and see its parameters by name (`runtime/reflect`: properties, struct members and parameters through
the engine's own UProperty objects, by dotted path). Native functions get a per-function thunk in `UFunction::Func`:
before and after only, without parameters. One with a native index is called straight from bytecode and only reaches
its hooks through ProcessEvent, so its code is detoured instead. The offsets are found in the game: `Func` at 0xAC
(where every script function holds ProcessInternal's address), and the frame's `Node` at 0x4 and `Locals` at 0x10.

Mods get these by appending to ABI v1: events (tick, level load, death, player state, key, and a TAS rewind putting the
game's memory back), function hooks, calls, game objects with values by path, and managed native detours
(`runtime/detours`). Several mods can chain on one function in priority order; every `original` is relinked as
detours come and go, two mods on one target is logged, and a function the menu hooks itself is refused. 3 unit tests
cover the chains. A mod's hooks, events and detours run like its module callbacks: only while it runs, counted for
reloads, guarded against crashes, and removed when it unloads, reloads or crashes. In Lua: `mirage.on`, `mirage.hook`
(`call.params.CullDistance = 1`, `call:skip()`), `mirage.pawn() / controller() / world() / find()` with object and
struct proxies (`pawn.Location.Z = 500`), and `mirage.call`. In C++: `mirage::On`, `mirage::Hook`,
`mirage::Object`, `mirage::Call`, `mirage::Detour`, `mirage::FindPattern`.

TAS: snapshots never hold a mod's memory. Its DLLs, the copies hot reload runs, its heaps and its allocations are under
`mirage\`, which the memory map leaves out (checked by the self-test). Its changes to the game's own memory are rewound
with the game. The `state_restored` event tells mods to look their objects up again. Rewinding a mod's own state was
the "opt in" idea; it isn't done, because a mod can rebuild its state from that event.

The check, self-test "Mirage hooks from mods" (`tests/mods/hook-native`, `hook-lua`): both hook
`CheckMaxEffectDistance` while their modules are on, and run in priority order (each turns `CullDistance` into
x * 10 + its number: 1, then 12). They keep that through three hot reloads of both, leave no hook behind when off, read
the pawn's `Location.Z` by name, and get tick and level load events. Costs, also shown in the Performance module: a
call through ProcessEvent takes 0.18 us, a native hook adds 0.28 us (budget 5) and a Lua hook 1.12 us (budget 50).
Self-test "Mirage function hooks" covers the core: priority, parameters read and changed, after hooks, replace, a
call from script, removal.

Seen while testing: 2 of about 12 full runs this session crashed the game in `ntdll`'s heap code. One was during the
Speedrun timer level restart, the other during the TAS workflow, a free in the game's CRT heap (MSVCR80) with the AMD
driver on the stack, before any hook or detour existed. The runs around them passed. Not explained yet; a TAS rewind of
the game's CRT heap while the driver holds memory in it is the first suspect. To look into separately, against a build
from before Mirage.

### 7. Content mods through UnrealLib

- A virtual file system for `TdGame\CookedPC`: the engine's package opens are redirected to overlay packages that
  the runtime builds with UnrealLib's writer, cached by the hash of their inputs.
- What a mod can ship: replacement textures, sounds and meshes (UnrealLib's asset code, reversed), `defaultproperties`
  and property overrides as JSON, map edits as T3D (UnrealLib's T3D import).
- The game folder is never written: removing the mod removes its changes.

**Milestone:** an example mod that replaces a texture and one that changes default properties work in the game with
no game file touched; turning them off restores the original on the next level load; UnrealLib's corpus sweeps still
pass.

**Status (2026-11-11): done for defaults and textures.** Default property overrides work, as data applied at runtime
rather than baked into overlay packages. `content/defaults.toml` holds the overrides (`runtime/content`). They are
applied through reflection on the game thread, re-applied after level loads and undone exactly, and a module named after
the mod turns them on and off (`examples/mods/high-steps`, self-test "Mirage content").

Textures: `content/textures/<full texture path>.png` (or `.tga/.bmp/.jpg`, encoded to the texture's own format with a
mip chain; `.dds` as stored). A survey of the Prologue and menu maps settled the design: maps hold *copies* of the
textures they use, which stream their large mips from uncompressed storage packages by raw offset, and every texture
whose mips are all inline is NeverStream or resident. So a replaced texture is written as NeverStream with every mip
inline into each package that holds a copy (`runtime/texture_files`), and storage packages are never touched: the
append-only overlay planned above isn't needed. In the game (`runtime/texture_overlays`, `runtime/file_redirect`):

- An index of which packages hold which texture, from the package headers only (2,406 packages in 2.7 s, then cached
  by size and time: 0.2 s).
- A copy is made the first time the game opens a package that holds a replaced texture, in the opening thread
  (126-297 ms per Prologue map, written uncompressed: LZO took seconds), then cached by a hash of the original and the
  images. Building them all up front was tried and dropped: one rooftop texture is in 98 packages (1.35 GB).
- kernelbase `CreateFileW` / `GetFileAttributesExW` hooks open the copy instead. A package keeps the file it first got
  for the whole session: switching it under the engine, which keeps its files open by name, hung the loader. Turning
  textures on or off applies to packages not loaded yet, and to all of them after a restart.
- Uncompressed copies must clear `PKG_StoreCompressed`, or the engine crashes reading the summary.

`examples/mods/retexture` and the self-test "Mirage textures" show it (the live texture object has the image's size).
Left for later in this step: sounds, meshes and T3D map edits.

### 8. Script mods: bytecode assembly and patching

- Add a bytecode encoder to UnrealLib (the inverse of its decoder) and prove it on the corpus: every function of every
  package decodes and re-encodes byte-identical.
- Runtime script patching: replace a function's bytecode in the loaded `UFunction` (script, ScriptSize, label table),
  and mixin-style injection (call a mod's function at the start, before each return, or around a call site).
- Also offline: patched script baked into overlay packages (step 7) for patches that must exist before the level loads.

**Milestone:** 100% byte-identical encode round trip over the Mirror's Edge corpus; an example mod changes a script
function's behaviour (for example the jump logic in TdPawn) with no native detour, and turning it off restores it.

**Status (2026-11-11): done.** UnrealLib (branch `bytecode-encoder`) has `encode_script`, the decoder's inverse: all
13,770 scripts of the game (477,343 tokens) round-trip byte-identical, and `assemble_script`, which lays an edited tree
out again (jump targets follow their statements, relative skips keep the compiler's own slack; checked over the corpus by
inserting and removing tokens everywhere). In the game, `runtime/script_patch` decodes a live function's bytecode, and
mods edit it through the API (`script_open` ... `script_apply`, Lua `mirage.script`). `examples/mods/higher-jumps`
patches `TdMove_Jump.StartMove` and a standing jump goes 2.25 times as high (self-test "Mirage script patch"); off, the
original bytecode is back. Not done: mixin-style injection (calling a mod's code from inside bytecode; function hooks
cover before/after), and patches baked into packages.

### 9. The menu becomes mods, and a mod manager

- The current features move into first-party mods (`memenu-trainer`, `memenu-tas`, `memenu-visual`, ...) that use only
  the public API; anything they need that the API lacks is added to the API, not reached around. Simple ones (God
  Mode, Game Speed, Teleport to Spawn) are rewritten in Lua as the examples mod authors copy; the TAS and the
  collision view stay native.
- The menu becomes the mod manager: installed mods, enable/disable, per-mod settings and hotkeys, dependency and
  conflict view, each mod's log and crash reports.

**Milestone:** the first-party mods compile against `loader/include/` headers only (enforced by the build: no runtime include
paths); every one of today's self-test checks passes with the features loaded as mods.

**Status (2026-11-11): mod manager done; every feature the API can express is a mod; the rest waits on API.**

- First-party mods live in `mods/`, are embedded in the DLL (`cmake/EmbedMods.cmake`) and written to the mods folder
  at startup (`runtime/firstparty`), so every install has them.
- `mods/memenu-trainer` (Lua): God Mode, Teleport to Spawn, Game Speed, Kill Bots, Freeze Timer.
  `mods/memenu-visual` (Lua): FOV, Crosshair, Show FPS, Watermark, Frame Rate Limit, Clear Frame.
  `mods/memenu-trainer` also has No Clip, Flight (flight.lua), Kickglitch, Beamer, Strang, Auto Kickglitch (tools.lua).
  `mods/memenu-trainer` also has Kickglitch Helper (helper.lua) and Tool Overlay.
  `mods/mirage-levels` (Lua): Levels (the level loader), Sublevels (loader.lua), Restart Checkpoint, Main Menu. Same names and settings as the native modules they
  replace (removed), and the self-test's checks pass on them. API appended for them: `notify`, `draw_circle`,
  `draw_circle_filled`, `frame_rate`, `setting_color` (and `color` settings in mod.toml), `objects_of_class`
  (Lua `mirage.all`: every live object of a class, by short name or path); `call_set` takes string parameters in a
  call the mod makes; `event_skip` (a player state callback takes the move state handler's place: Strang),
  `menu_open`, `input_key` (a real key event), `share_set` / `share_get` (values for other mods and the menu's tests:
  the kick buffer's counts, "flight"), `MIRAGE_MODULE_EVERY_TICK` (never deferred). Frame Rate Limit lost its "Back to 62" button and warning line (no buttons or text in a
  module's column yet: the UI layer below).
- **The UI layer:** `api->ui()` returns `MirageUi`, a second append-only table: the menu's ImGui through C (windows,
  children, layout, text, buttons, checkboxes, sliders, drags, inputs, multiline text, combos, selectables, colors,
  progress bars, plots, trees, tables, tooltips, popups, scrolling, drawing in a window, keys). Mirage's flag and color
  numbers are its own, translated to ImGui's, so an ImGui update never changes a mod. Windows come from on_render;
  `MirageModuleDesc.on_panel` (appended) draws in the module's settings panel; `setting_set_visible` hides a setting
  that doesn't apply. The runtime wraps every mod render callback in ImGui's error recovery: a window or color the mod
  left open is closed for it. Lua: `mirage.ui.*` (edited values come back: `changed, v = ui.slider_float(...)`).
  Self-test "Mirage UI layer" (tests/mods/ui-lua) draws most widgets and leaves a window open on purpose.
- **Arrays:** paths index arrays, 0-based ("StreamingLevels[2].PackageName", static "Foo[3]"); an array without an
  index reads as `MIRAGE_VALUE_ARRAY` with its count. Lua: arrays are proxies, 1-based, with `#`.
- **Native first-party mods:** a mod in `mods/<id>/` with a `CMakeLists.txt` calls `mirage_native_mod()`
  (`cmake/FirstPartyMods.cmake`): its own DLL target with only `loader/include/` (and allowed header-only libraries: nlohmann/json)
  on its include path, static CRT, embedded in the menu's DLL as RCDATA and written out by `runtime/firstparty` with the
  mod's other files. `mods/memenu-input-overlay` is the first (C++). API appended for it: the game's input as the engine
  gets it (`input_key_state`, `input_mouse`, `input_kps`, `input_now`, `input_reset_counts`), `tas_session`, files in
  the mod's own folder (`file_read`, `file_write`, `file_list`, `file_stamp`, `file_open_folder`), and in `MirageUi`
  `window_draw_text_sized`, `style_push` / `style_pop`. The player's old `input_layouts\` and `keybindings.json` are
  moved into the mods' folders once at startup.
- **The runtime's own tools stay in the runtime** (decided 2026-11-11): the Mod Manager, the Console (the menu's shell:
  it drives modules, settings, aliases and scripts), Log Viewer, Performance, Themes, Hotkeys, Config Actions, Game
  Status (the integration's diagnostics) and the Self Test manage the loader and every mod, like Geode's own mod list
  and settings. They are not game features, so they are not first-party mods.
- **API-only, enforced:** Lua mods reach the game through `mirage.*` alone. For native parts, `EmbedMods.cmake`
  fails the build when a first-party mod's source has a quoted `#include` other than `"mirage/..."` or its own files.
  No first-party native mod exists yet: when the TAS or the collision view moves, it is built as its own project
  against `loader/include/` (like `examples/mods/hello`), never with the menu's include paths.
- **The mod manager** (`features/misc/mod_manager`, Misc tab, its own window): every mod found, loaded or not, and why;
  details, load order, phase, reloads; its modules with switches; required / optional / incompatible mods and
  dependents; warnings; its log lines. Reload now, switch off or on for the next start (`disabled.toml`; switching off
  turns its modules off now), allow a crashed mod again (out of `crashes.toml`), open its folder. Per-mod settings and
  hotkeys stay in the modules' own columns, where the mod's modules appear. Self-test: the window draws and lists
  every mod.

**What's left and the API each part needs** (nothing reaches around the API, so these stay native until it exists):

| Features | Missing API |
| --- | --- |
| Console, Log Viewer, Info Lines (editor), Performance, Themes, Speedrun Timer, Speedometer, Input Overlay, Key Bindings, Hotkeys, Config Actions, Game Status, Level Loader, Sublevels, Save States (list), Tool Overlay | A UI layer: windows and widgets (text, inputs, tables, trees, plots) beyond the settings in a module's column. The biggest gap: most remaining features are UI. |
| TAS, Save State / Load State, Collision View | Stay native (memory snapshots, the engine's world, 3D drawing): native first-party mods once a native mod can get a 3D draw list and engine-thread scheduling through the API. |

Also measured: a Lua module's tick costs 20-30 us in release (native ~1 us), worth bringing down before many more
features move. Seen once in a full self-test (and not on the rerun): a crash in the Console step on menu heap memory
inside a reservation the TAS snapshots released when their session ended. The TAS's session-end release can free memory
that is no longer the game's: to look into with the TAS (not step 9).

### 10. Tooling, docs and distribution

Not started. Split by the gameplay-first re-sequencing (2026-11-13): the CLI, templates and docs are
step 35 (P2), the index, packages and updates step 40 (P3).

### 11. Script injection and mod-defined natives

Finishes what step 8 left out: calling a mod's code from inside UnrealScript, not only before or after a function.

- **Injection points** (mixin-style, like Fabric's Mixin): at a function's head, before each `return`, before or after
  a call to a named function, or at a bytecode offset found by a pattern over decoded tokens. The assembler (step 8)
  splices a call to a stub native in, re-lays out jump offsets, and keeps the original bytes for unpatching.
- **Locals and the return value:** the injected callback gets the frame (`FFrame::Locals`, already found), reads and
  writes locals and out parameters by name through reflection, and can return early with a value or cancel the call.
- **New natives:** a mod registers a C or Lua function as a native under a free `iNative`/`Func` slot and declares it on
  a class, so patched bytecode (and step 12's new classes) can call mod code like engine code.
- **Conflicts:** several mods injecting into one function are ordered by priority and dependency; the mod manager shows
  each function's injections and who owns them; a failed pattern match disables that mod's injection, never the game.

**Milestone:** a Lua mod injects into the middle of a TdGame function (changing a local before a branch), a second mod
injects into the same function, both run in the declared order, and hot reload unpatches and repatches with the
original bytecode byte-identical in between. Cost of an injection point measured (target: within 2x a function hook).

**Status (2026-11-12): done, except declaring natives on a class (step 12 makes the UFunctions).** `runtime/inject`
finds the engine's native table (GNatives) and puts a stub native in a free slot; each injection point becomes
`if (stub(site)) return ReturnValue;` in the function's bytecode (UnrealLib's assembler; jumps to the statement retargeted).
Points: `head`, `return`, `before:<Function>[#n]`, `after:<Function>[#n]`, `token:<i>`. Callbacks read and write
parameters and locals by name and may return early (not inside a ForEach). Injection is a layer over step 8's patches: a
patch of an injected function goes under the injections. API `inject_function`, Lua `mirage.inject`, C# `Mod.Inject`;
the mod manager lists each mod's injections and the other mods injecting into the same functions. Mod natives
(`runtime/natives`): `native_register(name, "float(float, int)", fn)`, Lua `mirage.native`, C# `Mod.Native`: a slot each,
a thunk that reads the arguments from the caller's bytecode, the same index after a hot reload. Self-tests "Mirage script
injection", "... (Lua)" and "Mirage natives (Lua)": a Lua mod sets the local `CurrentWeaponType` mid-`StartMove` (standing
jump 124 → 58), a second runs after it and sees it, Higher Jumps under it (130), hot reload keeps it, off: the original
bytes. An empty injection adds 0.10 µs a call, an empty function hook 0.22 µs.

### 12. New classes, objects and an UnrealScript compiler

- **Runtime classes:** a mod defines a class (a subclass of an existing one: an `Actor`, a `TdPawn`, a weapon) with new
  properties, default values and functions (Lua, native, or step 11 natives); the runtime builds the `UClass`, its
  property chain, layout (step 2's link rules) and default object in the engine's memory, so `Spawn` and the engine's
  own reflection treat it like a cooked class.
- **UnrealScript source:** an `.uc` compiler on UnrealLib (parser and type checker over the live class tree, codegen
  through the step 8 encoder), for mods that want to write script the way the game was written. Covers the subset the
  decompiler's output uses, so decompile → edit → recompile round-trips every TdGame function.
- **States:** new states and state code on new and existing classes (`FStateFrame` already mapped).
- **Lifetime:** classes and objects of a mod are tracked; unloading the mod destroys its actors and unregisters its
  classes; GC roots for objects a mod holds (`mirage.keep(obj)`), weak handles everywhere else.

**Milestone:** a mod adds `MirageBouncePad` (a new placeable actor class with a touch event and a property), spawns
it in Prologue, the player bounces; the same class written in `.uc` and compiled in the game behaves identically; the
corpus round-trip (decompile + compile every TdGame function) is byte-identical for an agreed share, the rest listed.

**Status (2026-11-13): done.** Mods add classes two ways ([docs/mod-classes.md](mod-classes.md)):

- **UnrealScript:** a `.uc` file in the mod's folder, compiled in the game by UnrealLib's compiler (`uc-compiler`
  branch) into a script package in `mirage\classes\` that the engine loads itself (`runtime/classes`: the package cache
  plus StaticLoadObject), so the engine makes the UClass, links it and builds its default object. The class builder
  covers what script classes declare: consts, enums, structs, variables with every modifier, functions, delegates,
  states (ignores, state functions, state code with labels and latent calls), interfaces, `defaultproperties` with
  subobjects and dynamic array operations, class modifiers; the rules found in the game's packages (field order, probe
  masks from the engine's probe names, flag inheritance, each class's own component templates) are in UnrealLib's
  `docs/compiler.md`. `native("Name")` binds a declared native function to the mod's own native (step 11's leftover).
- **Described from Lua or C#** (`mirage.define_class`, `Mod.DefineClass`, API `class_define`): the runtime writes the
  UnrealScript (an override takes the parent's declaration from UnrealLib's decompiler) and the class's functions run
  the mod's code through script injections (step 11).

Lifetime: a mod's classes are tracked; its unload destroys their actors and makes them abstract (no new spawns); the
same source loaded again revives the same class; `mirage.keep`/`Mod.Keep` (API `object_keep`) roots objects until the
mod lets go or unloads; every other handle is weak (checked against GObjects on use). Package names hash the source
and the builder version, so hot reload of an edited class is a new package.

Milestone checks (self-test, in the game): "Mirage classes (.uc)", "(Lua)", "(C#)" and "(example mod)" each spawn a
bounce pad on the player in Edge_p, the pad's Touch (UnrealScript, Lua, C#) launches the player the same way; the
described classes' new function returns the mod's value; `native("double")` runs the mod's Lua native; a hot reload
destroys the class's actors and brings the same class back. The compiler's corpus checks (UnrealLib, offline): every
function and state of the game's script packages decompiled and compiled again, 12,866 of 13,244 byte-identical
(97.1%; the rest listed in `docs/compiler.md`); every class (2,338) decompiled, built and decompiled again: 2,220 build
(the rest need replication, native interfaces or `map{}`: C++ only), 2,187 come back identical (93.5%).

Not done, by choice: adding a state to an existing cooked class (a subclass with the state works; the game's own
states are patched and injected through steps 8 and 11), replication (single player), native interfaces.

### 13. Levels and world building

- **Level additions:** a mod places actors into an existing level without a new package (a TOML/T3D layer: class,
  location, rotation, properties), spawned on level load and on every checkpoint restart, removed on unload.
- **Streamed sublevels from mods:** a mod's package becomes a streaming level of an existing map (`StreamingLevels`
  appended at load), with its own trigger volumes; loaded deterministically under the TAS (step 9's blocking load).
- **New maps:** a mod's `.me1` / `.upk` map shows in level select (UIDataProvider_TdMaps entries appended), with
  checkpoints, time-trial routes (start, checkpoints, finish, ghost) and the speedrun timer's splits.
- **In-game editor:** a free camera placement mode in the menu (pick a class, place, gizmo for move/rotate/scale, edit
  properties, save the layer to the mod's folder); the Collision View draws what was placed.

**Milestone:** a mod adds a new time-trial route to an existing map and a new small map built with the 2008 editor;
both appear in level select, play, restart from checkpoints, and a TAS of each records and plays back verified.

**Status (2026-11-13): done.**
Layers (`levels/<map>.toml`, `runtime/layers`), streamed sublevels, maps in level select (`maps/<Name>.me1`) and
time-trial routes (`routes/<map>.toml`) each have a self-test. The milestone ("Mirage time trials", "Mirage maps"): a
route on the Prologue and the same route on a mod's map each finish, and a TAS of each records and plays back verified
in the same time; the mod's map is in level select and restarts from its checkpoint. The Level Editor (`features/misc/level_editor`) places
an actor of any class where the camera looks, moves it live with a gizmo, edits its location, rotation, scale and
properties, and saves the layer, which is spawned again from the file; the Collision View draws a mod's class as a
live cylinder (self-test "Mirage level editor"). Finished on 2026-11-13: a searchable list of every placeable class
the game has loaded (186 in the Prologue) beside the mod's own; markers on every placed actor (click to select);
undo / redo of the layer (Ctrl+Z / Ctrl+Y), duplicate (Ctrl+D), delete; snapping of the gizmo to a grid, an angle and
a scale step; drop to floor (the collision's bottom onto the world under it), bring here, go to it; and the selected
actor's editable (`var()`) properties with their live values, one click to add one to the layer. The self-test checks
each (the gizmo only when the game isn't minimized: checked with `--visible`). Layers and routes are read again when they change, without reloading
the mod. The new map is `MirageEditorRoom`, built in the game's own UnrealEd (`MirrorsEdge.exe editor`) from the
editor's command box and Save As ([tests/maps-src/MirageEditorRoom](../tests/maps-src/MirageEditorRoom/README.md)):
self-test "Mirage editor map" loads it from the level select, and a TAS walking across it records and plays back
verified. The game's Restart Checkpoint needs a checkpoint: on a map without one it leaves no player, so restarting
such a map is loading its start (what a TAS run does).

## Phase 2: gameplay core (steps 14–27)

Every step here follows one template: objective, scope, approach, dependencies, API, editor, tests, costs, TAS, failure
and recovery, milestone. "Hard" dependencies must be done first; "soft" ones make the step better but can follow.

### 14. Live world access: handles, actors and lifecycle (P0)

**Objective.** A mod finds and follows anything in the running level (the player, actors near it, actors of a class, a
level's actors) through handles that can't dangle across destruction, garbage collection, level loads and TAS rewinds.

**Scope.**

- Handles with a generation: `{GObjects index, object pointer, serial}`. The serial is a per-slot counter the runtime
  keeps (incremented when the runtime sees the slot change), so a slot reused by a new object at the same address
  reads as stale. To investigate first: whether UE3's `UObject::Index` and the GObjects slot clearing are enough, or
  whether a hook on object destruction (`UObject::ConditionalDestroy` / `BeginDestroy`, by pattern) is needed.
- Queries: actors of a level (`ULevel::Actors`, at `0x3C`, verified for the persistent level), of a class (an index by
  class kept per level, built once per load and updated by spawn and destroy, not a GObjects scan per call), within a
  radius of a point, by tag and by name; the world and its levels; the player's pawn, controller, camera and game info.
- Lifecycle events appended to ABI v1: actor spawned, actor destroyed, level streamed in and out, garbage collected
  (after `CollectGarbage`), checkpoint restart. Each on the game thread.
- Relationships: `Owner`, `Base`, attached actors, `Outer` chain, components, by handle.

**Approach.** Builds on `game/object_query` (actor cache, object index), `game/object_ref` (slot-checked references)
and `runtime/reflect` (paths). Spawn and destroy are found by hooking `Actor.PostBeginPlay` / `Destroyed` (script
events, step 6's hooks) where every actor class reaches them; native-only spawns that skip them are to investigate.

**Depends on.** Hard: 2, 6. Soft: 13 (level layers already track their own actors).

**API (proposed).** `object_handle` / `handle_get` / `handle_valid`, `actors_query(filter, out, capacity)` with a
filter struct (class, radius, tag, level), `MIRAGE_EVENT_ACTOR_SPAWNED`, `_ACTOR_DESTROYED`, `_LEVEL_STREAMED`,
`_GARBAGE_COLLECTED`, `_RESTART`. Lua: `mirage.actors{class="TdBotPawn", near=pawn, radius=2000}`, handles as the
existing object proxies with a validity check. C#: the same through `Mod.Actors(...)`. All three in one step.

**Editor.** None of its own; step 16's explorer is the first user.

**Tests.** Unit: the handle and serial logic over a fake object table; the query filter. In game: "Mirage world
access": find the player, list the actors within 2,000 units, spawn and destroy an actor and see both events, hold a
handle across a level load, a checkpoint restart, a garbage collection and a TAS rewind and get "invalid" (never a
crash or a different object) for the ones that died.

**Costs (targets).** A handle check under 10 ns; a class query from the index under 50 µs for 4,500 actors (the
Prologue's count, measured in step 9's cache work); no full GObjects scan per tick in any steady state (the lesson of
`doc` performance notes: scans were the real per-tick cost).

**TAS.** Handles are runtime memory (outside snapshots); after a rewind they are re-checked, and events fire again
for actors the rewind brought back (to investigate: whether a rewind should send destroy/spawn for actors that appear
or vanish).

**Failure and recovery.** A stale handle reads as invalid and every API call on it fails softly with a logged reason;
a query during a load returns nothing.

**Milestone (Scenario A, part).** A Lua and a C# mod each find the player and the actors near it, print their classes
and properties, follow one actor through a level load and a rewind, and none of the five lifecycle cases above
crashes or returns a wrong object; the self-test proves each case.

**Status (2026-11-13): done for handles, queries and lifecycle events; three parts left.**

- **Handles without a destruction hook** (the investigation's answer): a `MirageHandle` keeps the object's address,
  slot, name and class. The engine names objects uniquely (`Class_N`, counting up), so a new object in a freed slot at
  the same address has another name and the handle stops matching; a destroyed actor stays in the object list until
  garbage collection (verified: its slot still holds it), so an actor with `bDeleteMe` doesn't match either. No serial,
  no hook. ABI v1 appended `object_handle`, `handle_object`, `actors_query`, `MirageHandle`, `MirageActorQuery`, and
  `MirageEvent.object` (the struct is size-prefixed: `sizeof` 24 → 28; C#'s layout check updated).
- **Every Lua object is a handle:** a proxy keeps its object's identity and lets go of it once it no longer matches
  (reads nil, calls fail); `mirage.valid(obj)`; `==` compares the address an object had, so `actor_removed`'s actor
  equals the one a mod kept.
- **Queries** (`actors_query`, Lua `mirage.actors{class, tag, name, near, radius}`, C# `Mod.Actors(...)`) read the
  per-tick actor lists (`game::ActorsInPlay`); nearest first with a radius.
- **Lifecycle events** (`runtime/world_events`): `actor_added` (with the class), `actor_removed`, `level_streamed`,
  `restart` (a new pawn in the same level). Actors are found by comparing the levels' lists with the last tick's, only
  while a mod subscribes: an unchanged list costs one compare of memory; a change sorts and diffs.
- **Self-test "Mirage world access"** (`tests/mods/world-access`): a spawned, tagged actor comes as `actor_added` with
  its class; found by tag (alone), around the player (the player first) and by class, a missing class matches nothing;
  destroyed, it comes as `actor_removed` `==` the kept one, which is no longer valid and reads nil; its handle is
  invalid, a forged handle (another name at the same address) is refused; a checkpoint restart comes as `restart` and
  the old player's handle is invalid. Measured (debug, 4,449 actors in the Prologue): 2.4-2.7 ms a tick on average over
  the test's ticks, where most ticks had a change (spawns, a restart); a restart's diff, with ~700 Lua events, 73-86 ms.

**Update (2026-11-14): the three parts are done.** `level_streamed` is driven by the self-test (layer-test's sublevel
LayerTestArea hidden and shown again: an "out" and an "in" event). The C# side has its test mod (`tests/mods/world-cs`,
self-test "Mirage world access (C#)": ActorAdded with the class, `Mod.Actors` by tag, around the player and by class,
ActorRemoved equal to the kept probe whose handle no longer resolves, Restart). An unchanged tick is measured on its
own: 8.1 µs in release, 148 µs in debug, for 4,605 actors in the Prologue; a restart's diff at most 3.5 ms in release.
A TAS rewind is the menu's now (the TAS stayed in Mirror's Edge Menu); the garbage-collection event was dropped (handles
already cover what it was for); relationships (`Owner`, `Base`, `Outer`) are reached by path as before.

### 15. Reversible changes: the change log (P0, infrastructure)

**Objective.** Everything a mod changes through the API is recorded, so unloading, disabling, hot reloading or
restarting reverts it exactly, and the mod manager can say what a mod changed. Editing tools (16, 19, 25) and every
gameplay step build on this.

**Scope.** (From old step 24's "Transactions".)

- One change log per mod: property writes through `object_set` (old value kept on first write per object and path),
  default-object overrides (step 7's content already does this: it becomes the first user), spawned actors (13's layers
  already track theirs), script patches and injections (8, 11 keep originals), detours, Kismet edits (18), movement
  overrides (17).
- Scopes: a change can belong to the mod (reverted on unload), to the level (reverted on level unload), or be marked
  permanent for the session by the player (a "keep" in the inspector).
- Grouping for undo: a transaction (begin, changes, end) is the unit of the editors' undo and redo.
- "What did this mod change?" in the mod manager, with a revert per entry.

**Approach.** The runtime owns the log, not each feature; features register a revert function for their kind of
change. Objects that died since are skipped (their handle is invalid: step 14). The old value is copied as raw bytes
of the property (size from the live `UProperty`), strings and arrays as their elements.

**Depends on.** Hard: 14 (handles). Soft: 7, 8, 11, 13 (moved onto it).

**API (proposed).** Implicit for `object_set` and the other writers; `change_begin(name)` / `change_end()` for
transactions; `change_list` for the mod manager. Lua `mirage.transaction("move pad", function() ... end)`.

**Editor.** The mod manager's "Changes" tab; undo and redo in every editor (16 onward).

**Tests.** Unit: log, scopes and grouping over fake objects. In game: "Mirage change log": a test mod writes 20
properties, a default, spawns 3 actors and patches a function; unload reverts each one, checked byte for byte against a
copy of the touched memory taken before; a hot reload in between keeps the log right; a level load reverts only the
level-scoped changes.

**Costs (targets).** Under 1 µs per logged write (a write the log already has costs a lookup only); the log's memory
counted in the mod's budget (`docs/memory.md`), under 1 MB for a mod making 10,000 changes.

**TAS.** The log is runtime memory: a rewind puts the game's memory back but not the log, so entries are kept and
re-applied state is compared against them after `STATE_RESTORED` (to investigate: whether reverting after a rewind
needs the log rewound too).

**Failure and recovery.** A revert that can't be done (object gone, level unloaded) is skipped and listed; a crash in
a mod still reverts what the log holds when the runtime unloads it.

**Milestone (Scenario H).** The test mod above is disabled, re-enabled, hot reloaded and unloaded mid-level; every
change is reverted byte-identically each time, and the mod manager lists every change it made.

**Status (2026-11-13): done for property writes; the other kinds of change keep their own undo.** One decision changed
the scope: writes are recorded **inside transactions only** (`change_begin` / `change_end`, Lua `mirage.transaction`,
C# `Mod.Transaction`), not every `object_set`. Recording everything would make every Lua hot reload undo gameplay
writes (the trainer teleports the player and sets its health; a reload would teleport it back). Inside a transaction
the first old value of each object property is kept (`runtime/changes`, through step 14's handles); `change_revert` /
`mirage.revert` / `Mod.Revert` put them back newest first, and the runtime does it when the mod unloads, reloads or
crashes; an object gone since is skipped and counted. `change_list` (Lua `mirage.changes()`, C# `ChangesJson`) and
the mod manager's "Changes" (with "Put back") list them. ABI v1 appended the four calls (offsets 560-572).

Self-test "Mirage change log" (`tests/mods/changes-lua`): gravity -800 → -400 (written twice inside the transaction:
the first old value kept) and KillZ −1000; StallZ written outside; switched off, the mod's revert puts gravity and
KillZ back exactly and leaves StallZ; on again then a hot reload: the unload put it back, so the new code halves the
level's gravity once (0.5×, not 0.25×); the mod manager lists the transaction with its 2 values.

Left: default overrides (step 7), script patches and injections (8, 11), layers (13) and classes (12) still undo
themselves as before and aren't listed in "Changes" yet; level-scoped transactions and the editors' undo through this
log come with steps 16 and 19 (the Level Editor keeps its own layer history).

### 16. Editor core, Object Inspector and World Explorer (P1)

**Objective.** The in-game workspaces every later editor sits in, and the first two: browse the world and inspect and
edit any live object.

**Scope.**

- **Workspace frame:** ImGui docking (the menu runs ImGui 1.92 with docking available: to verify it is enabled in the
  build), persistent layouts per workspace, a selection shared between workspaces, undo and redo (step 15's
  transactions), context menus, search, live refresh at a set rate, and panels that mods can add (through `MirageUi`).
- **World Explorer:** levels and their actors (step 14's queries), grouped by class or level, filtered by name, class,
  tag and distance; select in the list or by looking at it (the Collision View's look-at already finds actors under
  the crosshair); focus the camera.
- **Object Inspector:** every property of a live object by its class (from step 2's model), with typed editors (numbers,
  vectors, rotators, enums, bools, names, strings, object references followed by a click, arrays, structs); watch values
  per frame; the object's class chain, functions (call one with parameters), states, decompiled script (step 3's
  reference) and the hooks and injections on it. Edits go through the change log.

**Approach.** The existing Level Editor (step 13) and the Collision View become users of the frame, not separate
windows. Reads happen on the game thread in a budgeted pass per refresh and are drawn from a copy on the render thread
(the existing rule: UObjects only on the game thread).

**Depends on.** Hard: 14, 15. Soft: 9 (UI layer), 13 (Level Editor).

**API.** `MirageUi` gains a workspace panel registration (proposed) so a mod's tool docks with the rest; editor
selection as an event.

**Tests.** Unit: the inspector's value formatting and parsing for every property type. In game: "Mirage inspector":
open the player in the inspector, change `GroundSpeed` (or the equivalent TdPawn property) and see the pawn's
property change, undo it, follow `Controller` to the controller; the explorer lists the actors step 14's query
returns; the frame time with the inspector open on the player stays under target.

**Costs (targets).** Under 0.5 ms a frame with the explorer and an inspector open (debug build at most 2 ms); refresh
reads under 0.2 ms of the game thread at 10 Hz.

**TAS.** Edits are changes like any other (step 15); the inspector shows that it is editing during a recording.

**Failure and recovery.** An object that dies while inspected greys out and its panel says why; nothing in the editor
holds a raw pointer across frames.

**Milestone (Scenario A).** A developer finds the player in the explorer, inspects it, changes its speed and jump
height live, undoes it, and the same changes made from a Lua mod show in the inspector; turning the mod off restores
the values.

**Status (2026-11-13): done, without docking.** The menu's ImGui is the release branch (no
`ImGuiConfigFlags_DockingEnable` in its `imgui.h`): switching the whole menu to the docking branch is left for later, so
the editors are ordinary windows whose places ImGui's layout file keeps. `features/editor/editor_core` holds the
selection (a step 14 handle: it reads as nothing once the object is gone), an object's properties as rows (every
member, sorted, `var()` ones marked; structs, static and dynamic arrays opened in place, up to 64 elements; object
references selectable), typed writes (float, int, byte, bool, name) made as step 15 transactions under the owner
"editor", and their Undo. The **World Explorer** (Debug tab) lists every actor in play, nearest first or grouped by
class, filtered, with "Look at" (the actor under the crosshair); the **Object Inspector** shows the selection's rows
live, edits in place, Undo, "The player". Reads happen on the game thread while a window is shown (explorer once a
second, inspector five times), drawn from a copy on the render thread.

Self-test "Mirage inspector" (`tests/mods/inspector-lua`): the explorer lists the player among 4,589 actors; selected,
its GroundSpeed reads the pawn's value (720), Location opens into X/Y/Z, its Controller row points to the controller;
GroundSpeed 1234 and JumpZ 777 typed in land on the pawn and two Undos put both back exactly; the mod's doubled JumpZ
shows in the inspector and so does its revert; a selected actor that is destroyed reads as gone, with no rows; both
windows draw. Measured (debug, Prologue): the explorer's refresh 7.3 ms (once a second; 12.3 ms before names were
cached), the inspector's 1.1 ms (633 rows with Location open, five times a second).

Left: docking and saved workspace layouts per workspace; panels from mods (a `MirageUi` registration); a selection
event for mods; functions and the decompiled script in the inspector; editing objects, strings and structs as a whole.

### 17. The player and movement: inspection and tuning (P0)

**Objective.** Everything about Faith's movement is visible and tunable, live, per mod, with undo.

**Scope.** (Old step 31's "Tuning".)

- **Inspection:** the current and previous move (`EMovement`), the move objects (`TdPawn.Moves`), each move's state
  and timers, velocity and acceleration, grounded / falling, the floor and wall under and beside the player (traces:
  step 20), the controller's input this tick (the engine's input, already read for the Input Overlay), and the
  properties each `TdMove_*` class declares.
- **Tuning:** every movement constant the moves and the pawn declare (speeds, timings, wallrun lengths, jump heights,
  air control, gravity scale, friction where exposed) overridable per mod as data (`movement.toml`) or code, through
  the change log; resets on disable. Which constants are script properties and which are native-only is the step's
  first investigation (a survey of the 128 `TdMove_*` classes and `TdPawn` / `TdPlayerPawn`, written up like step 7's
  texture survey).
- **Transitions:** a record of move transitions (from, to, reason where the game says, tick), from the
  `PLAYER_STATE` event.

**Approach.** Reflection for script properties; script patches (8) and injections (11) for constants that are literals
in bytecode; engine symbols (32, minimal form) only for what is native C++. The trainer's existing tools are the first
users.

**Depends on.** Hard: 14, 15. Soft: 16 (the workspace), 20 (surface traces), 32 (native constants).

**API (proposed).** `movement_get(MirageMovement*)` (a plain struct: move, previous move, velocity, flags), the move
list, and `movement_override(class, property, value)` as a recorded change; Lua `mirage.movement`, C#
`Mod.Movement`. Native-only constants are marked as such in the docs.

**Editor.** The **Movement** workspace: the current move and its history as a timeline, the move's properties live,
the tuning table with per-value reset, and a graph of speed over time.

**Tests.** In game: "Mirage movement tuning": a mod raises the sprint speed and the jump height; a TAS macro runs and
jumps; the measured top speed and jump apex change by the set amount (not by gravity or time alone); off, the same
macro matches the unmodded recording frame for frame.

**Costs (targets).** Movement inspection under 20 µs a tick when the workspace is closed and nothing subscribed (zero
when nothing asks); tuning costs nothing per tick (a property write once).

**TAS.** Tuned values are game memory: snapshots rewind them. A replay records the active overrides' mod and version
(step 24); playing a replay with different tuning reports it before it starts.

**Failure and recovery.** An override of a property that doesn't exist on this build is refused with the reason; the
change log restores every value.

**Milestone (Scenario A).** A mod tunes five movement constants from a TOML file, the effect is measured by a TAS
macro, the workspace shows the moves and the overrides, and disabling the mod mid-level brings back the game's
movement exactly (the macro matches its unmodded recording).

### 18. Kismet runtime (P0)

**Objective.** Read and change the levels' Kismet from mods, using the game's own sequence objects and execution, not
a new graph system.

**Scope.** (Old step 29's "Read" and "Change".)

- **Read:** the sequences of the loaded levels (each level's `GameSequences`, nested `Sequence` objects), their
  objects (`SequenceObjects`), each op's input, output, variable and event links, variables (`SeqVar_*`), events
  (`SequenceEvent`), actions (`SequenceAction`), their properties, and runtime state (`bActive`, `ActivateCount`).
- **Change:** create and delete sequence objects (new objects of existing classes, outered to the sequence, added to
  `SequenceObjects`), duplicate them, connect and disconnect links (the link structs in the ops' arrays), set
  properties, all through the change log (15) so unload restores the original graph.
- **Kismet layers:** `kismet/<map>.toml` in a mod, applied at level load like step 13's actor layers (objects, links,
  properties, referring to existing objects by name), so data-only mods can rewire a level.
- **Mod-defined nodes:** new `SequenceAction` / `SequenceEvent` / `SeqVar` classes from step 12 (`.uc` or described
  from Lua/C#) whose `Activated` runs mod code; firing an event from a mod (`CheckActivate` / `ActivateRemoteEvent`
  equivalents).
- **Execution trace:** which ops activated, with which inputs, per tick (a hook on the sequence update or on the ops'
  activation; which function is to investigate: `USequence::UpdateOp`, `ExecuteActiveOps` are native).

**Approach.** Everything is UObjects already in the engine: reflection (2) reads and writes them, classes (12) make
new node types, hooks (6) and the native-function work (32, minimal) see execution. To investigate before designing
the write path: how the engine builds its per-sequence active-op list (whether new ops must be registered beyond
`SequenceObjects`), how cooked sequences keep links (they may be pruned of editor-only data such as positions), and
whether a sequence edited at runtime survives checkpoint restarts (the game reloads checkpoint state into actors and
sequences).

**Depends on.** Hard: 2, 6, 12, 14, 15. Soft: 32 (execution trace through natives).

**API (proposed).** `kismet_sequences(level)`, `kismet_objects(seq)`, `kismet_links(op)`, `kismet_create(seq, class)`,
`kismet_link(from_op, out_index, to_op, in_index)`, `kismet_unlink`, `kismet_fire(event)`, a trace subscription. Lua
and C# in the same step.

**Editor.** A text view first (sequence tree, op properties, links as lists); the visual editor is step 19.

**Tests.** Unit: the Kismet layer parser. In game: "Mirage Kismet": read the Prologue's sequences and check counts
against the level's package (UnrealLib reads the same sequence objects offline: the two must agree); a mod adds a
`SeqAct_Log`-style action after an existing trigger's output and the trace shows it firing when the player enters the
trigger; a mod-defined action runs Lua; unload restores the original links (compared with the offline read).

**Costs (targets).** Reading a level's whole graph under 20 ms (on demand, never per tick); the trace under 2 µs per op
activation while on, nothing while off.

**TAS.** Sequences are game memory: rewinds restore them. A Kismet layer is applied at the same tick on every load
(like level layers). Mod-defined actions must be deterministic (step 24's rules).

**Failure and recovery.** An invalid link (wrong index, wrong type) is refused before it is written; a layer entry
that names a missing object is skipped and listed (like step 13's layer warnings).

**Milestone (Scenario C).** A mod inspects a real Prologue sequence, adds a node, connects it to an existing trigger,
and the node runs custom Lua when the player walks in; a checkpoint restart keeps it consistent; unloading the mod
restores the sequence to exactly what the package holds.

### 19. Kismet editor and debugger (P1)

**Objective.** A visual node editor over the game's real sequences, and a debugger for their execution.

**Scope.**

- **Editor:** the sequence as a graph (ops as nodes, links as wires, variables as nodes, nested sequences as nodes you
  open); create, delete and duplicate nodes from a searchable, categorised list of the Kismet classes (305 in the SDK);
  connect and disconnect pins; edit properties in step 16's inspector; multi-select, copy and paste, comments, groups;
  zoom and pan; error and warning badges (unlinked required inputs, type mismatches); undo and redo through step 15.
  Live: edits apply to the running level and can be saved as the mod's Kismet layer (step 18's format).
- **Layout:** cooked sequences may not keep editor positions (to investigate, step 18); the editor lays nodes out
  automatically when they are missing and saves positions in the layer.
- **Debugger:** the active ops highlighted as they fire, execution history per tick, variable values, breakpoints on
  an op (the game pauses at the frame boundary through the TAS's pause path, never mid-tick), step to the next
  activation, and the log of events.

**Approach.** `imgui-node-editor` (pinned, v0.9.4) is the candidate: evaluate it first for ImGui 1.92 compatibility,
frame cost with 500 nodes, and how it stores state, against a small custom canvas (the Level Editor already draws its
own gizmo). The graph is always the game's data: the editor's model is rebuilt from step 18's reads and every edit
goes through step 18's API.

**Depends on.** Hard: 16, 18. Soft: 24 (deterministic breakpoints during a TAS).

**Tests.** Unit: graph model diffing (what changed between two reads), auto-layout. In game: "Mirage Kismet editor":
open the Prologue's sequence, add a node and a wire through the editor's API (the self-test drives it like a user),
see the change in the game and in the saved layer; a breakpoint on a trigger's output pauses the game when the player
enters, the step continues it.

**Costs (targets).** Under 2 ms a frame (release) with a 500-node sequence open; nothing when closed.

**TAS.** Breakpoints use the TAS pause, so a paused recording continues exactly; edits made while recording are
changes recorded with the replay (step 24).

**Milestone (Scenario D).** A developer opens a level's sequence in the editor, adds and wires a node, changes its
properties, triggers it in the game and watches it execute, saves it as the mod's layer, and unloading the mod
restores the original sequence.

### 20. Physics and world queries (P0)

**Objective.** Mods inspect and drive rigid bodies and ask the world questions (rays, sweeps, overlaps) within a budget.

**Scope.** (Old step 32's "Physics queries" and props.)

- **Rigid bodies:** enumerate `KActor`s and other primitives with `PHYS_RigidBody`; read and set position, rotation,
  linear and angular velocity, mass, gravity, sleep and wake (`SetRBLinearVelocity`, `SetRBPosition`,
  `WakeRigidBody`); forces, impulses, torque (`AddForce`, `AddImpulse`); physical materials; constraints (UE3
  `RB_ConstraintActor`, to verify in the SDK).
- **Spawning props:** a `KActorSpawnable` with a mod's or the game's static mesh, through level layers (13) and code.
- **Queries:** line traces (`Trace`, `FastTrace`, `TraceComponent`), extent traces (sweeps), point and box overlaps
  (to investigate: what UE3 exposes to script, and what needs the native `UWorld::SingleLineCheck` /
  `MultiLineCheck` through engine symbols, step 32); results as plain structs (hit actor handle, location, normal,
  material).
- **Collision control:** collision flags per actor and component (`bCollideActors`, `BlockRigidBody`, channels where
  UE3 has them).

**Approach.** Script-visible functions through `mirage::call`; anything native-only through step 32. The Collision
View already draws primitives and finds what the crosshair hits: its code is the reference for shapes.

**Depends on.** Hard: 14, 15. Soft: 32 (native traces), 16 (the workspace).

**API (proposed).** `physics_body_get/set` (a plain struct), `physics_impulse`, `physics_force`, `world_trace`,
`world_sweep`, `world_overlap`; Lua `mirage.physics`, C# `Mod.Physics`.

**Editor.** The **Physics** workspace: the selected body's state live, sliders for mass, velocity, damping, buttons
for impulses, a "shoot a ray" tool drawing the hit.

**Tests.** Unit: the query result structs and budget accounting. In game: "Mirage physics": a mod spawns a prop above
the player, it falls and lands (position changes by more than gravity over the waited ticks would explain only with the
body simulated), an impulse moves it sideways, mass changes the result, 500 traces in one tick are timed; unload
removes the prop.

**Costs (targets).** 500 line traces under 1 ms (old step 32's target, unmeasured); body reads under 1 µs each.
Query budgets per mod per frame, with a warning when a mod exceeds its share.

**TAS.** PhysX state is the part of determinism most at risk: to investigate whether rewinds restore PhysX's own
memory (its heap is game memory in the snapshot map) and whether a replay with rigid bodies verifies. Until then,
props are documented as possibly non-deterministic.

**Failure and recovery.** A body spawned in a wall is placed with `FindSpot` or refused; a mod over its query budget
gets empty results with a logged reason, never a frame drop.

**Milestone (Scenario E).** A mod spawns a physics prop, applies forces, changes its mass and collision, queries the
world around it, and cleans it up on unload; the workspace shows the body live; the 500-trace cost is measured and
recorded here.

### 21. Triggers, volumes and gameplay pieces (P0)

**Objective.** Mods place, configure and connect the game's interactive pieces and their own triggers.

**Scope.** (Old step 32's "Pieces" and "Triggers".)

- **Triggers:** box, sphere and cylinder volumes from mods (a `Trigger` / `TriggerVolume` subclass from step 12, or
  placed through layers), with enter, leave and stay callbacks, overlap counts, and filters by class and tag; drawn by
  the Collision View.
- **The game's pieces:** a survey first of what Mirror's Edge's traversal really is (the `TdZiplineVolume`,
  `TdSwingVolume`, `TdLadderVolume`, `TdBalanceWalkVolume` volumes, springboards, ledges, doors, elevators,
  valves, buttons and what configures them: splines, attach points, Kismet), then each made placeable from a mod with
  its parameters, through level layers and the editor. Which pieces need level geometry or Kismet beside them is part
  of the survey.
- **Connections:** a trigger's events wired to Kismet (18) or to mod code; pieces' own events (the zipline's
  attach and detach) as mod events where the game exposes them.

**Approach.** Level layers (13) already spawn any placeable class and set its properties; this step adds the pieces'
special setup (splines, links between parts) and the trigger callback path (`Touch` / `UnTouch` hooks or the mod's own
trigger class, as `LayerPad` in the tests does).

**Depends on.** Hard: 13, 14, 15. Soft: 18 (wiring to Kismet), 20 (physics props), 16 (editor).

**API (proposed).** `trigger_create(shape, filter, callbacks)`, piece creation through `layer_actor` (exists) plus
per-piece setup helpers once the survey says what they need. Lua and C# together.

**Editor.** In the gameplay workspace (25); here, the Level Editor gains trigger shapes and piece placement.

**Tests.** In game: "Mirage triggers": a trigger fires enter, stay and leave as a TAS macro walks through it (counts
checked), a filter ignores a bot; "Mirage pieces": a zipline and a swing pole placed by a layer are used by a TAS macro
(the move state reaches the zipline and swing moves, not just the player's position).

**Costs (targets).** A trigger's overlap check costs what the engine's own `Touch` does (no per-tick polling by
Mirage).

**TAS.** Triggers and pieces are actors: rewinds and restarts handle them like level layers (13: spawned on every
restart, the same tick on every load).

**Failure and recovery.** A piece whose setup the survey didn't cover is refused with the reason rather than spawned
half-configured.

**Milestone (Scenario F).** A mod creates a trigger, connects it to an interactive object (a door or a moving
platform), and runs custom logic when the player enters; a zipline and a swing pole from a layer work and a TAS
records and replays using them.

### 22. Input actions and custom movement mechanics (P0)

**Objective.** Mods add moves of their own (a double jump, a dash, a grapple) with entry conditions, physics, input,
animation and transitions, and the game's movement comes back exactly when they are off.

**Scope.** (Old step 31's "New moves" and old step 17's "Input actions".)

- **Input actions:** a mod declares actions in `mod.toml` (`[actions.dash] default = "Q"`); they are bound like the
  game's (to investigate: whether the game's own key-binding screen can list them, or the menu's Key Bindings shows
  them), recorded and replayed by the TAS as the key events they are.
- **A move's lifecycle:** registration; entry conditions (`CanDoMove`); start (`StartMove`); update per tick; input
  while active; physics (velocity, gravity, collision); animation (step 23); transitions out; stop (`StopMove`);
  cleanup on disable.
- **How:** investigate first which of these the engine allows: (a) a new `TdMove` subclass from step 12 added to the
  pawn's `Moves` array with a new `EMovement` value (enum values are fixed in the compiled script: to investigate
  whether an index past the enum's end survives the native code that switches on it); (b) reusing an existing move
  slot (an unused `EMovement` such as a bot-only move) with the mod's class; (c) a "virtual move" driven by hooks on
  the current move's tick, without a move object. The milestone's double jump decides between them by experiment.

**Approach.** Classes (12) and injections (11) give mod code inside the move; hooks on `TdPawn` move dispatch and
`event_skip` (step 9, used by the Strang tool) let a mod take over a transition. The move's physics uses step 20.

**Depends on.** Hard: 12, 15, 17. Soft: 23 (animation), 20. The milestone's replay check needs step 24, which doesn't
depend on this step and is built alongside it.

**API (proposed).** `move_register(desc)` with callbacks (can_start, start, tick, stop) and its input action; Lua
`mirage.move{ name = "DoubleJump", ... }`; C# `Mod.Move<T>()`.

**Editor.** The Movement workspace (17) shows mod moves beside the game's, their conditions and transitions.

**Tests.** In game: "Mirage custom move": a double-jump mod; a TAS macro jumps, presses the action at the apex and
reaches a height a single jump can't (checked against the unmodded jump measured in the same run); a replay of it
verifies; off, a macro pressing the same keys matches the unmodded recording.

**Costs (targets).** A mod move's tick under 5 µs native, under 30 µs Lua (today's Lua module tick).

**TAS.** The move's state is game memory when it lives in a move object or pawn properties (rewound); state a mod keeps
on its own side uses step 24's opt-in.

**Failure and recovery.** A crash in a move's callback stops the mod and puts the player back in the walking move
(`player_reset_movement` exists); the move is unregistered on unload.

**Milestone (Scenario B, Scenario I).** A double-jump mod registers its move, detects when it can start, applies its
physics, plays its animation, transitions correctly, works in an existing level, is recorded and replayed by the TAS
verified, and disabling it mid-level restores the original movement exactly.

### 23. Animation (P0 for playback, P1 for tools)

**Objective.** Mods inspect and play the game's animations and give their moves animations.

**Scope.** (Old step 31's "Animation", without importing new ones.)

- **Inspection:** the player's and bots' AnimTrees (`AnimTree`, the 80 `AnimNode*` / `TdAnim*` classes), the nodes'
  weights and active children, the AnimSets and their sequences (names, lengths, notifies).
- **Playback:** play a sequence on a pawn (`PlayCustomAnim`, `PlayCustomAnimByDuration` on `CustomAnimNode`), set rate,
  blend in and out, stop; first-person (the arms) and third-person meshes where they are separate (to verify).
- **For moves:** a mod move (22) names the animation it plays and its blend times.
- **The Animation browser:** every AnimSet loaded, its sequences, preview on the player (in place, with the game
  paused or a free camera).
- Deferred to step 31: importing new animations (needs the model pipeline).

**Approach.** Reflection and calls on the script-visible animation API; the survey says which node types the player's
tree uses and where a custom slot can play a mod's chosen sequence.

**Depends on.** Hard: 14. Soft: 16 (browser), 22 (moves use it).

**API (proposed).** `anim_sets(pawn)`, `anim_play(pawn, sequence, rate, blend_in, blend_out)`, `anim_stop`; Lua and
C#.

**Tests.** In game: "Mirage animation": list the player's AnimSets (counts against the packages read offline), play a
known sequence and see the `CustomAnimNode` report it active for its length, then stop.

**Costs (targets).** Playback is the engine's cost; the browser lists without loading packages that aren't loaded.

**TAS.** Animation state is game memory (rewound); to investigate whether animation affects movement (root motion) in
ways a replay must account for.

**Milestone.** The browser lists the player's animations and previews one; a mod plays an animation on a move it
added (the double jump of step 22) with blends, recorded by the TAS.

### 24. Mod state under the TAS: determinism, rewinds and replays (P0)

**Objective.** Gameplay mods work with the TAS: their state rewinds with the game, they behave the same on every
playback, and replays know which mods they need.

**Scope.** (Old step 19.)

- **Mods in snapshots:** mods opt in to being rewound (`mirage.tas.state(...)`: the values to keep, or a serialise /
  restore pair); a TAS rewind restores them with the game, so mods with gameplay state (scoring, timers, custom moves,
  new actors from step 12) work in practice mode. Mod memory stays out of the snapshot by default (step 5's rule).
- **Deterministic mod API:** mod randomness through `mirage.random` (seeded by the session), time through the game's
  clock, never the wall clock while a session runs; a lint in the CLI flags what isn't (step 35).
- **Replays record their mods:** a `.merp` stores each loaded mod's id, version and hash and its declared gameplay
  effect; playback warns or refuses on a mismatch (step 9's Preflight extended).
- **Verified categories:** a run verified by the menu carries a signed summary: mods loaded and whether each is
  "cosmetic" (draws only, declared and checked: no writes, no hooks that return values) or "gameplay".
- **Checkpoint restarts:** each gameplay step documents what its state does on a restart; this step adds the event
  (step 14) and a test that every P0 system survives one.

**Depends on.** Hard: 9 (TAS service), 14. Soft: 12 (actors from mods), 15.

**Tests.** In game: "Mirage mods under the TAS": a challenge mod with a score and spawned actors is recorded with
practice-mode rewinds, plays back verified, and the same replay with the mod at another version reports the mismatch
before it starts.

**Costs (targets).** Opted-in state costs its serialised size per keyframe; total mod state under 1 MB per keyframe
across mods (the snapshot budget is shared with the game's ~500 dirty pages per keyframe measured in the self-test).

**Milestone (Scenario I).** The challenge mod above, and step 22's double jump, both record, rewind and replay
verified, and survive a checkpoint restart during recording.

### 25. The gameplay workspace and a playable course (P1)

**Objective.** One workspace where a developer builds a small playable experience in an existing level from mod-owned
pieces, triggers, physics and Kismet, and tests it.

**Scope.**

- The Level Editor (13) grows into the **Gameplay** workspace: place pieces (21), triggers, physics props (20) and the
  mod's classes from a palette; edit transforms with the gizmo and properties in the inspector (16); wire triggers to
  Kismet (19) or Lua; see collision (the Collision View's drawing); save everything as the mod's layers (actors, Kismet).
- **Play-test loop:** start from the course's start, restart, record a TAS run of it (route timing from step 13's
  routes), all from the workspace.
- **Organisation:** groups, names, hide and lock, search.

**Depends on.** Hard: 16, 19, 20, 21. Soft: 22, 26.

**Tests.** In game: "Mirage course": a test mod's course (zipline → swing pole → springboard, a trigger opening a door
through Kismet) is loaded from its layers, completed by a TAS replay verified, restarted from its checkpoint
consistently, and unloading the mod leaves the level as the package has it.

**Costs (targets).** The workspace under 3 ms a frame (release) with 200 placed objects visible.

**Milestone (Scenario G).** The course above is built in the workspace (the self-test drives the editor's API to
place and wire it), saved, played by a TAS verified, restarted from a checkpoint, and removed cleanly; old step 32's
milestone ("a zipline to a swing pole to a springboard, usable and recorded by the TAS") is part of it.

### 26. Missions, objectives and checkpoints (P1)

**Objective.** Mods give levels goals: objectives, checkpoints, success and failure, shown in the game's own HUD.

**Scope.** (Old step 29's "Missions".)

- A small layer over Kismet (18) and mod code: objectives (reach a point, use a piece, defeat enemies, beat a time),
  their order, success and failure, fed to the speedrun timer's splits.
- **Checkpoints in mod maps and courses:** the game's Restart Checkpoint needs a checkpoint, and on a map without one
  it leaves no player (found 2026-11-13); this step places checkpoints from mods (the game's checkpoint actors and how
  the checkpoint manager registers them: to investigate) so mod maps and courses restart like story levels.
- The game's HUD objective display, if script-accessible (to investigate), else the menu's HUD.
- Dialogue lines and sounds come with step 30.

**Depends on.** Hard: 18, 21. Soft: 24, 30.

**Tests.** In game: "Mirage mission": a mod adds an objective to the Prologue (reach a point, then a mod-defined
sequence action opens a door); the HUD shows it; a checkpoint restart keeps it consistent; a TAS plays it through.

**Milestone.** Old step 29's milestone: a mod adds a new objective to a story chapter, the game's own HUD shows it,
checkpoint restarts keep it consistent, and the self-test plays it through; `MirageEditorRoom` gains a checkpoint from
a mod and restarts from it.

### 27. Script environment: editor, REPL, debugger and profiler (P1)

**Objective.** Mod authors write, run, debug and profile code inside the running game.

**Scope.** (Old step 22, and old step 20's debugger, profiler and native hot reload.)

- **The in-game editor** (`features/misc/script_editor.cpp` grown): syntax highlighting, line numbers, find / replace
  and bracket matching (an ImGui text editor widget, measured: under 1 ms a frame for a 2,000-line file); completion,
  signature help and hover docs from Roslyn for C# and from the API's declarations for Lua; go to definition into the
  SDK and the decompiled script; templates per kind (module, hook, move, trigger, Kismet action); new file, rename,
  and "export as project" (a `.csproj` or a folder ready for step 35's CLI); multiple cursors are out of scope.
- **REPL:** run a line or a selection against the game and the mod's own state, Lua and C#; the console's commands share
  it; an expression watch.
- **Lua debugger:** breakpoints, stepping and variable views over the Debug Adapter Protocol, so VS Code attaches to
  the running game; the game pauses at a frame boundary on a break (the TAS's pause path), never mid-frame.
- **Profiler per mod:** time per callback, hook and injection, Lua memory, allocations; budgets shown in the mod
  manager and warnings when a mod costs frames.
- **Native hot reload with state:** a native mod's DLL rebuilt by the CLI is swapped in at a frame boundary with its
  state carried over (a serialise / restore pair, like step 24's).

**Depends on.** Hard: 9, 16. Soft: 24.

**Milestone.** A mod author breaks in a Lua hook from VS Code, inspects a locals table and the player object, changes a
property in the inspector, reloads a native DLL without losing state, and the profiler names the slowest callback; a
new C# mod is written in the game from template to a working HUD with completion only, and the editor never drops the
frame rate below the 62 cap.

## Phase 3: more game systems (steps 28–31)

These use the core of phase 2. Their text is carried over from the earlier plan with step numbers updated; each gains
the same check-list as phase 2 in short form.

### 28. AI and enemies (P1)

*Was step 30.* **Depends on:** 14, 18, 20 (hard); 24 (soft: AI in replays). **Tests:** in game, the milestone below as
"Mirage AI". **Costs (targets):** a mod behaviour within the AI's own update, under 50 µs per bot per update in Lua.
**TAS:** AI must replay verified (step 24's determinism); to investigate whether the game's AI uses the CRT `rand()`
the TAS already seeds. **Failure:** a bot whose mod behaviour crashes falls back to the game's behaviour.

- **Spawning:** enemies (the game's cop and soldier archetypes) spawned at points with their loadout and team, through
  the API and in level layers; removed on unload and on restart like any mod actor.
- **Behaviour:** hooks and services over the AI controllers (perception, targeting, cover, chase), and mod-defined
  behaviours in Lua or C# that run at the AI's own update rate within a budget.
- **Navigation:** path data for new geometry (step 13 maps and level additions): generated offline by the CLI, loaded
  with the level.

**Milestone:** a mod sets up an ambush in an existing level with three enemies using a mod-defined flanking behaviour;
they navigate a mod-added area, and the TAS replays the encounter verified (AI included in the determinism of step 24).

### 29. Cameras (P1)

*Was step 33 (the camera API); photo mode and cinematics are step 48.* **Depends on:** 14 (hard). **Tests:** "Mirage
camera": a mod takes the camera, moves it along a path, gives it back, and the player's view is the game's again.
**TAS:** a mod camera is presentation, not gameplay, unless it changes the player's view rotation (then it is recorded
like camera keys today). **Failure:** a crashed camera mod releases the camera.

- **Camera API:** a mod takes the camera (position, rotation, FOV, post process) with a priority, and gives it back.

**Milestone:** a mod takes the camera with a priority (a third-person follow camera), the game's camera returns when
it lets go or unloads, and a TAS recording with it on replays verified (the camera doesn't change gameplay).

### 30. Audio and controllers (P1)

*Was step 17 (input actions moved to step 22).* **Depends on:** 7 (content), 22 (actions). **Tests:** "Mirage audio":
a mod sound plays at a location and through the HUD with the game's volume; a replaced `SoundCue` plays the mod's file.
**Costs (targets):** decoding outside the game thread; under 10 MB of address space for a mod's loaded sounds.
**TAS:** sounds are presentation; controller input is recorded like keys.

- **Sound:** play WAV/OGG from a mod's folder at a location or on the HUD (through the game's audio device so volume
  sliders apply), replace a `SoundCue` by path like textures, and music that follows the level.
- **Controllers:** XInput read for mods (sticks, triggers, rumble) and bindable to actions; the TAS records them like
  keys.

**Milestone:** a mod's "dash" (step 22's action) is bound to a key and a gamepad button, plays a mod sound on use, and a
TAS of it plays back verified.

### 31. Rendering, materials and the game's UI (P1)

*Was step 16.* **Depends on:** 7, 9, 14 (hard); 32 (soft: render hooks need engine symbols). **Tests:** the milestone
below as self-tests. **Costs (targets):** a full-screen pass under 0.5 ms at 1080p; no fps drop below the 62 cap.
**Memory:** render targets counted in the mod's budget. **TAS:** presentation only.

- **Render hooks:** draw callbacks at named points of the frame (after the world, after post process, before the HUD,
  after everything), a D3D9 device and state block handed to native mods, render targets and textures a mod owns.
- **Shaders and post process:** a mod ships HLSL (compiled at build time by the CLI, or at load), applied as a full-screen
  pass or replacing a material's pixel shader; parameters animatable from Lua (a "runner vision" filter, colour grading).
- **Scaleform:** the game's menus and HUD are GFx movies: list, read and replace them (a mod ships its own `.gfx`),
  call ActionScript functions and set variables from Lua, and receive `fscommand`s. A mod adds a page to the game's own
  options menu.
- **Models and materials at runtime:** load a mesh and textures from a mod's files (glTF/FBX converted by the CLI
  through UnrealLib), swap the player's arms and body (step 7's texture path extended to meshes).

**Milestone:** a mod adds a post-process pass with a slider in the game's own options menu (Scaleform), another swaps
Faith's arms for a mod-shipped mesh; both on and off at runtime, no fps drop below the 62 cap on the test machine.

## Phase 4: platform infrastructure, as gameplay needs it (steps 32–39)

Each of these is built in its minimal form when a gameplay step first needs it, and finished here.

### 32. Engine-level hooks: functions in the executable (P2, minimal form earlier)

*Was step 15.* Pulled forward in a minimal form (a few verified symbols, no database format yet) by steps 17
(native movement constants), 18 (Kismet execution trace), 20 (native traces) and 31 (render hooks), whichever needs it
first. **Tests:** the milestone below as "Mirage engine hooks". **Costs:** a detour adds what MinHook's trampoline does
(measured for step 6's detours).

Function hooks (step 6) cover UnrealScript and natives with a `UFunction`; most of the engine (physics, movement,
rendering, streaming, audio) is C++ with no `UFunction`.

- **A symbol database:** engine functions named by signature (pattern + verification), collected from the Ghidra work
  already done, shipped as data (`symbols.toml`, versioned with the game build), extendable by mods.
- **Detours through the runtime:** a mod hooks an engine function by symbol name: before / after / replace, with typed
  arguments for known signatures, and one owner (the runtime) for every detour (MirageHook today, through `core/hooking.h`), chained in priority
  order. Mid-function hooks (register context in, registers out) for the places with no clean boundary.
- **Safety:** detours installed only with the game thread parked at a frame boundary; each one is SEH-wrapped and
  attributed; a crash inside one removes it and names the mod; the TAS treats detour trampolines as runtime memory.

**Milestone:** a native mod changes the player's ground friction by hooking the physics step by symbol name, a Lua mod
hooks the same function after it, both unload cleanly, and the self-test crashes inside a test detour on purpose and the
game keeps running with the detour gone and the mod named.

### 33. Services between mods (P2)

*Was step 14.* **Depends on:** 5, 6. `share_set` / `share_get` cover today's needs; this comes when two real mods need
versioned code from each other.

`share_set` / `share_get` (step 9) pass values; this lets mods build on each other's code.

- **Services:** a mod exports a named, versioned interface (a C function table or a Lua table) declared in `mod.toml`
  (`[provides] "inventory" = "1.2.0"`); dependents ask for it by name and semver range and get it after the provider
  started (step 5's resolver orders them; a missing provider is a load error naming it).
- **Custom events:** mods declare their own events (`"parkour:combo"`), others subscribe; the same dispatch, budgets
  and crash attribution as the runtime's events.
- **Settings and UI of other mods:** a mod may add a page to another's panel or a column to the mod manager through a
  provider's service, not by reaching into it.
- **Library mods:** mods with no modules of their own (a math library, a UI kit, a pattern database), loaded only when
  something depends on them.

**Milestone:** a library mod provides a "trick scoring" service; two independent mods (a HUD and a Lua challenge mode)
use it at once; upgrading the library across a minor version keeps both working, a major version is refused with the
reason in the mod manager.

### 34. C# at parity with Lua and native (P2, continuous)

*Was step 21.* Each gameplay step adds its C# API in the same step (phase 2 rule); this step closes the rest and makes
it checked.

C# mods (.NET 10 x86 through hostfxr, `docs/csharp-mods.md`) run, hot reload and compile scripts in the game; their
API is a friendly subset with the rest behind raw function pointers.

- **Full API coverage:** the UI layer (`MirageUi`), script patching, detours, the TAS service and step 33's services
  get C# wrappers; a generated test (like `tests/test_abi.cpp`) fails the build when `api.h` grows without them.
- **Typed game objects:** a generator over the SDK (`sdk/`) emits C# structs and wrappers (`TdPawn.Velocity`,
  `TdPlayerController.AttackPress()`) on top of the path API, so mods get compile-time names; the cost of a typed read
  is measured against the path read (~10 µs today) and must be at most a tenth of it.
- **Shipping:** the installer ships `mirage\dotnet` (only when the player has a C# mod or opts in: ~70 MB on disk);
  Mirage.dll on NuGet with a `dotnet new mirage-mod` template; compiled scripts cached on disk by source hash, so a
  restart doesn't pay Roslyn again (1.2 s now).
- **Debugging:** Visual Studio and VS Code attach to the game's managed runtime (breakpoints in a mod's `.cs`, scripts
  included through their in-memory PDBs).

**Milestone:** a first-party Lua mod (memenu-trainer) is ported to C# with typed objects and passes the same
self-test; a breakpoint in its OnTick hits from VS Code; a second start with only script mods doesn't load Roslyn.

### 35. Tooling and docs for mod authors (P2)

*Was the first part of step 10 (distribution is step 40).*

- `mirage` CLI (the `mirage` tool grown): new mod from a template (Lua or native), build, package, install into the
  game, run the in-game test harness for a mod (Lua mods can carry their own in-game tests).
- Docs: getting started, the API reference, the generated SDK and decompiled script, examples for each kind of mod.
- The determinism lint of step 24 and the static checks of step 37 run from the CLI.

**Milestone:** someone who has never seen the code builds, tests and runs a gameplay mod (a trigger and a tuned move)
with only the docs and the CLI.

### 36. A hundred mods: start-up, memory and budgets (P2)

*Was step 23.*

- **Start-up budget:** loading is measured per mod and per phase; mods load in parallel where their dependencies allow
  (parsing, Lua compile, C# compile on workers; only the publish on the game thread). Target: 100 small mods add under
  2 s to the start and under 100 MB of address space.
- **Lazy mods:** a mod declares `load = "on-demand"` and loads the first time one of its modules is switched on, a
  service is asked for, or its event fires; library mods (step 33) are on-demand by default.
- **Address-space accounting per mod:** the memory manager (`runtime/mem`) attributes Lua heaps, C# contexts, textures
  and native DLL images to their mod; the mod manager shows them; low address space unloads on-demand mods first.
- **Frame budgets:** each mod's tick and render time is tracked (step 27's profiler); a mod over its budget for a
  sustained period is throttled to the scheduler's Background priority and named, never silently.

**Milestone:** a generated set of 100 mods (Lua, C#, native, content) starts within the budget, the memory report
attributes every MB to a mod or the runtime, and one deliberately slow mod is throttled and named while the rest keep
their frame time.

### 37. Safe mode, bisection, conflicts and permissions (P2)

*Was steps 24 (without the change log, now step 15), 25, and the permissions of step 20.*

- **Safe mode:** a start after a crash during the previous start (or Shift held at launch) loads no mods and says why;
  the mod manager offers to bisect.
- **Automatic bisection:** the runtime restarts the game with half the suspected mods, using the crash records and
  the dependency graph, until it names the mod (or pair of mods) that crashes; the player confirms each restart.
- **Before loading:** a mod's touch set is read without running it: the functions it hooks or injects into, the
  properties and defaults it writes, the textures and packages it replaces (from `mod.toml`, content folders, script
  patches, and the API calls found in Lua and C# sources and IL).
- **Conflicts:** overlapping touch sets are shown in the mod manager before the conflict happens ("both replace
  `T_Faith_Body`", "both skip `TdPawn.TakeDamage`"), with the load order deciding and a per-pair override.
- **Lints:** the CLI warns on what tends to break (writes every tick, hooks on hot functions without a filter, wall-clock
  time in gameplay code: step 24's determinism lint).
- **Permissions:** `mod.toml` declares what a mod uses (files outside its folder, network, engine hooks, process memory);
  the mod manager shows them at install and the runtime refuses what wasn't declared; signed releases in the index.

**Milestone:** two mods that crash only together are found by bisection in at most 4 restarts; the conflicts between
the first-party mods and a set of deliberately overlapping test mods are reported before load, with no false
positives on the first-party set; a mod that opens a socket without declaring `network` is refused and says why.

### 38. Save games and persistent mod data (P2)

*Was step 34.*

- **Mod data with saves:** a mod's state saved in a sidecar file next to each game save slot (`<slot>.mirage.toml`),
  loaded with the slot; the game's own save files are never changed.
- **Schema versions:** a mod declares its data version and a migration function; old data is migrated once and kept
  as a backup.
- **Global progress:** per-profile data (unlocks, best times) that isn't tied to a slot, with the same migrations.

**Milestone:** a mod's collectible count survives save, quit, load and a mod update with a migration; deleting the
mod leaves the game's saves loadable by the unmodded game.

### 39. Localization and text (P2)

*Was step 35.*

- **Strings:** mods ship translations (`lang/<code>.toml`) and get the player's language from the game; missing keys
  fall back to English and are listed for translators.
- **The game's text:** a mod overrides or adds entries of the game's localization (subtitles, menu text) per language,
  as content (step 7).
- **Fonts:** coverage checked for the languages a mod declares; the menu's fonts get the missing glyph ranges.

**Milestone:** the first-party mods are translated into one more language with no code change, and a mod replaces a
subtitle line in the game's own subtitles in two languages.

## Phase 5: ecosystem (steps 40–49)

After gameplay modding is useful. Carried over with numbers updated.

### 40. Distribution: index, packages, updates and rollbacks (P3)

*Was the last part of step 10, and step 28.*

- Installer and launcher handle mods; a mod index (a JSON registry in a Git repository) with hashes, versions and
  compatibility; update checks in the mod manager.

Answers the open question of a packaged format.

- **`.mirage` packages:** a zip with `mod.toml` at its root, read in place (no unpacking; the content VFS reads from
  it), hashes per file, an optional signature (step 37); folders keep working for development.
- **Updates:** delta downloads from the index, applied at the next start (never under a running mod), with the previous
  version kept: one-click rollback, and automatic rollback when the new version crashes on its first start.
- **Dependencies:** installing a mod installs what it requires (semver-resolved against the index), and removing one
  offers to remove what nothing else needs.

**Milestone:** a mod is updated, crashes on start, and is rolled back automatically with the reason shown; a package
installs with its two dependencies from a clean state, and the folder form and the package form load identically; a
release candidate of the menu installs, finds a mod in the index, installs and updates it (old step 10's milestone).

### 41. Profiles and load sets (P3)

*Was step 27.*

- **Profiles:** named sets of enabled mods with their settings (`profiles\<name>.toml`): "Speedrun (legal)", "Practice",
  "Content"; switching reloads only what differs (hot reload where possible, a restart only for what can't).
- **Category rules:** a speedrun profile can be locked to a category's allowed mods and versions (the list published
  with step 45), and the TAS / timer show the active profile.
- **Sharing:** a profile exports as one code or file with mod ids, versions and hashes; importing installs what's
  missing from the index.

**Milestone:** switching from a 30-mod content profile to the speedrun profile takes under 3 s without a restart, and a
shared profile imported on a clean install reproduces the exact mod set and settings.

### 42. Game builds and compatibility (P3)

*Was step 26.*

- **Build detection:** the Steam, EA app / Origin and GOG executables (and their patches) identified by hash; the
  runtime knows which offsets, patterns, SDK and `symbols.toml` (step 32) belong to which build.
- **Per-build data:** the SDK generator (step 3) and the symbol database run per build; layouts that differ are data,
  never `#ifdef`s; a mod declares `game = [...]` builds or relies on the API, which hides the difference.
- **CI on every build:** game-test runs against each supported executable the developer owns; the README lists what
  passes.

**Milestone:** the full self-test passes on two different game builds from one menu DLL; a mod built against one runs
on the other with no change, and one declaring a build it isn't on says so instead of loading.

### 43. Multiplayer for mods (P3)

*Was step 18.* Single-player gameplay comes first; nothing in phases 2–4 may depend on it.

The menu's history includes mmultiplayer; this makes multiplayer a service every mod can use.

- **Sessions:** host / join through a relay or direct UDP, lobby with the mod list (joining requires the same mods and
  versions; the mod manager offers to install what's missing from the index).
- **Replication:** mods mark values and objects as replicated (owner, rate, interpolation) and send RPCs; players see
  each other's ghosts with animation (the existing pose capture), tags, and their mods' visuals.
- **Modes as mods:** race (shared start, ghosts, finish order), tag, and co-op challenges written in Lua against the API.
- **Rules:** no mod code runs on the network thread; message budgets per mod; malformed packets are dropped and logged.

**Milestone:** two machines join a session, see each other move, run a race mode written in Lua to a shared finish
with matching times; a mod mismatch on join names the missing mod and installs it.

### 44. External control: stream tools, LiveSplit, companions (P3)

*Was step 36.*

- **Local API:** a WebSocket server (off by default; step 37 permission) exposing events and commands that mods
  choose to publish: level, time, deaths, the TAS state, mod-defined values.
- **Integrations:** LiveSplit (splits and game time from the speedrun timer), browser-source stream overlays, and
  chat-driven events for streamers (votes that a mod turns into gameplay, rate-limited).
- **Security:** localhost only unless enabled, a token per client, every command declared by a mod and logged.

**Milestone:** LiveSplit splits a run from the menu's timer with game time exact to the tick, and a browser overlay
shows a mod's live values with under 50 ms of latency.

### 45. Challenges and leaderboards (P3)

*Was step 37.*

- **Challenge format:** a challenge is a mod (route, rules, allowed mods) with a stable id and version; the game's time
  trials are the first challenges.
- **Leaderboards:** submitted runs carry their replay and step 24's signed summary; the server replays them headless
  where possible (step 46's simulated engine for the checks it can do) and ranks verified runs only.
- **Rules:** cosmetic mods allowed by default, gameplay mods only where the challenge lists them; the speedrun profile
  of step 41 is the default for submissions.

**Milestone:** a community challenge is published, played by two accounts, both runs verified and ranked; a run made
with an unlisted gameplay mod is rejected with the mod named.

### 46. Offline mod testing (P3)

*Was step 38.*

- **A simulated engine:** the testhost (already used for parts of the menu) grows into enough of the API's behaviour
  (objects, properties, events, a fixed tick) to run a mod's logic without the game: mods ship tests, the CLI runs them.
- **Recorded worlds:** a snapshot of a level's objects and properties taken in the game replays into the simulated
  engine, so tests read realistic values.
- **CI:** a GitHub Actions template for mods (build, test, package, publish to the index); the menu's own CI runs the
  first-party mods' tests the same way.

**Milestone:** every first-party mod has tests that run offline in under 30 s in CI, and a mod's PR on GitHub is built,
tested and packaged by the template without the game installed.

### 47. Reports for mod authors (P3)

*Was step 39.*

- **Crash and error reports:** opt-in per player, sent per mod to the mod's declared endpoint (or the index's), with
  the crash reporter's attribution and symbolicated stacks (PDBs, portable PDBs, Lua tracebacks), and no personal data
  (paths and names scrubbed, the exact payload shown before the first send).
- **Health:** the index shows a mod's crash rate per version and game build; a version over a threshold is flagged for
  rollback (step 40).

**Milestone:** a deliberate crash in a test mod reaches its author's endpoint symbolicated with file and line; the
payload contains nothing that identifies the player.

### 48. Photo mode and cinematics (P3)

*Was the rest of step 33.* **Depends on:** 29, 31.

- **Photo mode:** pause, free camera, depth of field, filters (step 31's post process), screenshots at higher resolution
  than the screen.
- **Cinematics:** keyframed camera paths over a replay (the TAS's ghosts and recordings), exported as a video frame
  sequence at a fixed frame rate regardless of the game's speed.

**Milestone:** a 30-second keyframed shot over a TAS replay renders to an image sequence at 60 fps with no dropped or
repeated frame, and photo mode works in every level of the story.

### 49. ABI v2 and long-term support (P3)

*Was step 40.* Last by definition, and never a reason to hold back a gameplay API: v1 grows by appending.

- **Deprecation policy:** an API entry is deprecated with a replacement and a warning for at least two minor versions,
  and removed only in a new ABI.
- **ABI v2:** what years of v1 taught (handles instead of raw pointers where lifetimes bit, async calls, batching for
  property access), with v1 kept running through a shim: every v1 mod in the index loads unchanged.
- **Releases:** an LTS line of the runtime with security and game-build fixes only; the API reference, guides and
  examples published as a versioned docs site generated from the headers and the C# and Lua bindings.

**Milestone:** every mod in the index built for ABI v1 passes its tests on the v2 runtime through the shim, and the
docs site shows each entry's status (stable, deprecated, new in v2).

## Plans by system

Where each gameplay system's plan lives, in one place.

### Kismet and the visual node editor

Step 18 reads and changes the real sequences (UE3's `Sequence`, `SequenceOp`, links and variables: 305 classes in the
SDK), adds Kismet layers (`kismet/<map>.toml`) and mod-defined actions and events (step 12 classes running mod code),
and traces execution. Step 19 puts a node editor and debugger on top, always over the game's data, never a separate
graph; `imgui-node-editor` (pinned, unused) is evaluated against a custom canvas first. Step 26 builds missions on it.
Open: whether cooked sequences keep editor positions, how the engine registers active ops, and how checkpoint restarts
treat runtime-edited sequences (step 18's first investigations).

### Movement and animation

Step 17 inspects and tunes (the 128 `TdMove_*` classes, `TdPawn.Moves`, the move constants, script and native), with
a Movement workspace. Step 22 adds input actions and moves of a mod's own, choosing between a `TdMove` subclass, a
reused move slot and hook-driven "virtual" moves by experiment. Step 23 inspects AnimTrees and plays the game's
animations (`PlayCustomAnim` on `CustomAnimNode`), with an Animation browser; importing new animations waits for
step 31's model pipeline. Step 24 makes all of it replay under the TAS.

### Physics and gameplay objects

Step 20 gives rigid bodies (`KActor`, PhysX 2.x), forces and world queries, with a Physics workspace. Step 21 adds
triggers and the game's traversal pieces (after surveying how ziplines, swing poles, ladders, springboards and doors
are built), placed through level layers. Step 25 assembles them into a playable course in the Gameplay workspace. Open:
whether PhysX state rewinds and replays deterministically (step 20's investigation).

### Other game systems

| System | Step | Priority | Notes |
|---|---|---|---|
| Player and controller (health, damage, state, abilities) | 14, 17 | P0 | Through world access and the movement work; health and damage hooks are step 6's |
| Missions, objectives, checkpoints, timed challenges | 26 (routes: 13) | P1 | Checkpoints in mod maps are part of it |
| AI and enemies | 28 | P1 | Spawning, behaviour hooks, navigation for new geometry |
| Cameras | 29 | P1 | Photo mode and cinematics: 48 |
| Audio, controllers | 30 | P1 | Input actions are in 22 |
| Rendering, materials, post process, Scaleform UI, models | 31 | P1 | Texture replacement exists (7) |
| Saves and persistent mod data | 38 | P2 | |
| Localization | 39 | P2 | |

### The in-game editors

Step 16 builds the frame (docking, layouts, selection, undo through step 15, search, mod panels); each workspace comes
with the step that gives it something to edit:

| Workspace | Step | State |
|---|---|---|
| World Explorer, Object Inspector | 16 | Planned |
| Movement | 17 (22 adds mod moves) | Planned |
| Kismet Editor, Kismet Debugger | 19 | Planned |
| Physics | 20 | Planned |
| Animation browser | 23 | Planned |
| Gameplay (the Level Editor grown) | 25 | Level Editor exists (13) |
| Collision viewer | exists (Collision View, `mods/mirage-collision`) | Moves into the frame in 16 |
| Script editor, REPL, debugger | 27 | Script editor and console exist |
| Mod manager | exists (9) | Gains "Changes" in 15 |
| Profiler | 27 (per mod); the Performance module exists | |

Rules for all of them: reads on the game thread in a budgeted pass, drawn from a copy on the render thread; nothing
when closed; every edit a recorded change.

### Public API strategy

- **One ABI, appended:** every gameplay step appends to `MirageApi` (frozen by `tests/test_abi.cpp`), with plain
  structs, handles from step 14, no STL across the boundary, and the thread each call needs written beside it (most
  are game thread; the runtime schedules a call from another thread onto it, or refuses it).
- **Three languages in the same step:** a capability is done when C/C++, Lua and C# have it, or it is listed as
  native-only with the reason (raw memory, D3D state, symbol detours). Step 34 makes the coverage a build check.
- **Errors are values:** calls return a status and log the reason; nothing throws across the ABI.
- **Lifetime is the runtime's:** everything a mod creates is owned by its mod record and undone on unload (step 15).
- **Shapes (illustrative, not existing APIs):**

  ```lua
  local pawn = mirage.pawn()
  mirage.movement.override("TdMove_Jump", "JumpHeight", 1.5)   -- a recorded change (step 15), undone on unload
  local pad = mirage.spawn("MyMod.BouncePad", { location = pawn.Location })
  mirage.trigger.box{ center = pad.Location, extent = {200, 200, 100},
                      on_enter = function(actor) mirage.info("entered") end }
  ```

  ```cpp
  auto pawn = mirage::Pawn();
  mirage::Transaction t("course");          // step 15
  auto pad = mirage::Spawn("MyMod.BouncePad", pawn.Location());
  mirage::Trigger::Box(pad.Location(), {200, 200, 100}).OnEnter([](mirage::Actor a) { /* ... */ });
  ```

  The language support of each capability:

  | Capability | C/C++ | Lua | C# | Note |
  |---|---|---|---|---|
  | Objects, calls, hooks, injection, natives, classes, layers (steps 6–13) | Yes | Yes | Yes | Exists |
  | Native detours, patterns | Yes | No | Function pointers | Native code by nature |
  | UI layer, world drawing | Yes | Yes | Partly | Step 34 closes the gap |
  | World access, change log (14, 15) | Planned | Planned | Planned | |
  | Movement, Kismet, physics, triggers, moves, animation (17–23) | Planned | Planned | Planned | |

## Order and dependencies

What is done:

```
1 ─► 2 ─► 3 ─────────────► 9
     │         4 ─► 5 ─► 6 ─┘
     └──────────► 7 ─► 8 ─► 11 ─► 12 ─► 13
```

What comes next. Solid arrows are hard dependencies, dotted ones soft; steps on one line with no arrow between them
can run in parallel:

```
13, 6 ─► 14 ─► 15 ─┬─► 16 ─────────────┬─► 19 ─┐
                   ├─► 17 ─┬────────────┤       ├─► 25 ─► 26
            12 ────┼─► 18 ─┴────────────┘       │
                   ├─► 20 ─► 21 ───────────────┘
                   └─► 17, 12 ─► 22 ◄┄ 23, 24
              14 ─► 23          9, 14 ─► 24
   32 (minimal) ┄► 17, 18, 20        16, 9 ─► 27
   14, 18, 20 ─► 28     14 ─► 29     7, 22 ─► 30     7, 9, 14 ┄ 32 ─► 31
   phase 4 (32–39) as needed;  phase 5 (40–49) after phase 2
```

Notes on the edges:

- 14 (handles) under everything: no gameplay step holds raw pointers.
- 15 (change log) before any editor and any step that writes the game (17, 18, 20, 21): reversibility is the rule from
  the start, not a retrofit.
- 18 needs 12 for mod-defined Kismet nodes; 22 needs 12 for move classes. 12 is done.
- 19 needs 16 (frame, inspector) and 18 (the data); 25 needs 19, 20, 21 and 16.
- 22 needs 17 (it extends the move system it inspects) and its milestone needs 24 (built alongside).
- 32 is a soft dependency of 17, 18, 20 and 31: each starts with what reflection reaches and pulls in a minimal symbol
  set only for what is native-only.

**Recommended sequence:** 14 → 15 → (16 and 17 in parallel) → 18 → 20 → 21 → (19 and 24 in parallel) → 22 → 23 →
25 → 26 → 27; then 28–31 by demand; 32–39 as their first user needs them (minimal forms earlier); 40–49 after phase 2.
Each arrow ends with its milestone passing in the game, so useful gameplay features ship every step: after 15 a mod can
change anything and undo it; after 17 it tunes movement; after 18 it rewires Kismet; after 21 it builds with pieces.

### The next ten steps (2026-11-14)

The repository split changes what Mirage's steps deliver. The editors and workspaces (steps 16, 19, 25, 27) and the
TAS's side of step 24 belong to Mirror's Edge Menu. Each Mirage step delivers the runtime, the API (C, Lua and C#
together, so step 34 grows with every step), a test mod and an in-game self-test. Milestones that said "a TAS macro
matches its recording" now use the input service (`KeyEvent` / `HoldKey`) and compare measured values. "Off" means the
change log has nothing left of the mod. Two steps move up: engine hooks (32), because MirageHook has landed and steps
17, 18 and 20 all need it, and tooling (35), because the loader is now the product and every later API should ship
with a template and docs.

| # | Step | Why now | Milestone |
|---|------|---------|-----------|
| 1 | **A green self-test** (stabilization; step 14's leftovers) | Every later milestone is a self-test. The full run fails today, the same way before and after MirageHook. | Five full `game-test` runs in a row pass, debug and release. |
| 2 | **Engine hooks by symbol** (step 32, minimal form) | 17, 18 and 20 each need a native function; MirageHook gives scanning, mid-function hooks and transactions. | A mod hooks a native function by symbol name, and a crash inside a detour removes it and names the mod. |
| 3 | **Tooling for mod authors** (step 35, moved up) | The loader is the product. Templates and reference docs make steps 4–10 usable as they land. | A new Lua, C# or native mod goes from `mirage new` to running in the game with only the docs. |
| 4 | **The player and movement** (step 17, no workspace) | The first P0 gameplay API, built on the change log. | Scenario A: five constants tuned from `movement.toml`, effect measured, exact restore on disable. |
| 5 | **Kismet runtime** (step 18) | How the levels' gameplay is wired. | Scenario C. |
| 6 | **Physics and world queries** (step 20) | Props, forces and traces are what 7 and 8 build on. | Scenario E. |
| 7 | **Triggers, volumes and pieces** (step 21) | Wires 5 and 6 together into level gameplay. | Scenario F. |
| 8 | **Input actions and custom moves** (step 22) | New mechanics; the input service already exists. | A double jump from a mod, bound to its action and taken by the input service, gone when off. |
| 9 | **Animation playback** (step 23, playback only) | Custom moves need animations; the browser and preview tools stay with the menu. | A mod's move plays a game animation and stops it cleanly. |
| 10 | **Safe mode and conflicts** (step 37, first part) | Third-party mods make crashes at start-up and overlapping changes the main support cost. | A crash during start-up leads to a start with no mods; two overlapping test mods are reported before load. |

1. **A green self-test.** The steps that fail in a full run but pass alone: "Mirage lifecycle" (the crasher mod's
   deliberate crash reaches the log as a game crash), "Mirage hooks from mods" (a native hook adds ~11 µs a call
   against a 5 µs budget in debug; measure in release before changing the budget or the code), "Mirage classes
   (example mod)" (the bounce pad doesn't launch the player), the engine crash during "Mirage level additions" (state
   left by an earlier step), and the hang in "Crash reporter" (a minidump written while another thread holds a lock
   it needs; the restructure removed one such deadlock). Then step 14's leftovers: drive `level_streamed` in the
   self-test, add a C# world-access test mod, measure an unchanged tick in release.

   **Status (2026-11-14): nearly done; the milestone (five green runs in a row in each build) isn't met yet.** Fixed:
   the guarded mod crash logged as a game crash; the hook budgets (release numbers, ten times in debug); the bounce
   pad's launch measured from the switch-on; mods' classes kept from the garbage collector (an injected class's
   functions were freed through GMalloc: the engine crash in "Mirage level additions"); the "Crash reporter" hang (a
   minidump written from inside the running game: now by `mirage_crashdump.exe`); textures racing the first level
   load; checkpoint restarts missed when the new pawn took the old one's address; a call into a class default object
   (the engine crashed reading its missing WorldInfo: the runtime now refuses such calls); an unattended crash window.
   Step 14's leftovers are done (see step 14). The last round of ten full runs passed 8: the Lua injection's reload
   check was too strict (fixed since), and one release run crashed on the render thread during "Memory manager"
   (`MirrorsEdge.exe+0x14B7C18`, creating a texture with the RHI's D3D device pointer null: a device reset at that
   moment?) — open, seen once.
2. **Engine hooks by symbol.** `game::FindPattern` becomes MirageHook's scanner (resolve steps, cache). The patterns
   already in the code (ProcessEvent, ProcessInternal, death, the config lookup, input) become named entries in a
   `symbols.toml` verified at start. Append to ABI v1: hook by symbol (before / after / replace, chained by priority
   through `runtime/detours`) and mid-function hooks. Detours are SEH-wrapped and attributed. The rest of step 32 (a
   symbol database from the Ghidra work, typed arguments) waits for its first user.

   **Status (2026-11-14): done.** `runtime/symbols.toml` (built into the DLL) names 14 patterns; `symbols::Resolve`
   scans them at start with MirageHook's scanner (26 ms in release) and checks each matches the number of places the
   table says (InitializeListElementProviders is one function compiled twice: `matches = 2`, the first used); the
   runtime's code provides 7 more names (FrameTiming, PreDeath, PostDeath, GConfig, GMalloc, ProcessEvent,
   ProcessInternal). `game::FindPattern` scans with MirageHook too. ABI v1 appended `symbol_find`, `detour_symbol`,
   `mid_hook_create`/`mid_hook_remove` (`MirageRegisters`), mirrored in C++ (`mirage::Symbol`, `DetourSymbol`,
   `MidHook`), C# (`Mod.Symbol`, `Detour`, `DetourSymbol`, `MidHook`) and Lua (`mirage.symbol`: Lua can't detour
   native code). Every detour runs behind a guard (`runtime/detour_guard`): a thunk records the registers and the stack
   at the entry, and when the detour faults in its own module a vectored handler puts them back and goes on in the
   function it would have called; the mod is stopped and named like any crash. Mid-function hooks call the mod behind
   SEH. Self-test "Mirage engine symbols" (`tests/mods/hook-symbol`): every symbol found, a detour of StaticLoadObject
   by name counts a load, a mid-function hook inside it reads the Name argument from the registers, and a fault in
   the detour lets the load finish, stops and names the mod, and the game goes on. Unit tests: the built-in table, and a
   faulting detour skipped in the middle of a chain.
3. **Tooling for mod authors.** `mirage new <id> --lang lua|cs|native` from templates; `mirage mod build`,
   `package`, `install`; `mirage mod test` runs a mod's own in-game tests through the self-test harness. The API
   reference is generated from `api.h`, plus a getting-started page. Docs grow with each later step.

   **Status (2026-11-14): done, but nobody new has tried the docs yet.** `mirage new <id> --lang lua|cs|native` makes a
   mod from `tools/mirage_tools/templates/` (a module with a declared setting, one made in code, a tick and a test);
   `mirage mod check|build|package|install|test` work on any kind (native through its CMake project with clang-cl or
   MSVC, C# projects through dotnet; Lua, C# scripts and content need no build); installing keeps the settings the game
   saved. Mods carry their own in-game tests: ABI v1 appended `test_register` (`mirage.test` in Lua, `mirage::Test`,
   `Mod.Test`), the self-test step "Mod tests" runs them on the game thread with the mod's modules on, and
   `mirage mod test <folder>` installs the mod and runs that step for it alone (verified: a new mod of each language
   from `mirage new` passes its test in the game). `docs/api-reference.md` is generated from `api.h` (`mirage docs
   api`; a unit test fails when it is out of date), and `docs/getting-started.md` takes a mod from nothing to the game.
4. **The player and movement.** The survey of the 128 `TdMove_*` classes first (which constants are script
   properties, which are bytecode literals, which are native and need step 2). Then `movement_get`,
   `movement_override` through the change log, `movement.toml`, and the transition record from `PLAYER_STATE`.
   **Status (2026-11-15): done for tuning, inspection and transitions; literal constants left.** The survey
   (`docs/movement-survey.md`): 262 config tuning values in 44 move classes plus the pawns', all script properties, so
   tuning is content (step 7's defaults overrides with `instances = true`), and no `movement_override` or
   `movement.toml` was needed. ABI v1 appended `movement_get` (`MirageMovement`; Lua `mirage.movement()`, C#
   `Mod.Movement()`). Self-test "Mirage movement tuning" passes Scenario A: five constants on, the jump 124 to 279 as
   predicted, off, every value back exactly. The transition record: ABI v1 appended `movement_transitions` (the last
   256, JSON with the engine's EMovement names; Lua `mirage.transitions()`, C# `Mod.MovementTransitionsJson()`).
   Building it showed the PLAYER_STATE event fired for few changes: its hook is the pawn's physics step, whose
   "changed" argument is set only when physics starts over within a tick. The tick now dispatches every other change
   after the fact (event_skip still works only in the physics step's). Left: the constants in bytecode and native code.
5. **Kismet runtime.** As step 18: read sequences, change them through the change log, nodes from mod classes
   (step 12), and sequence events delivered to mods.
   **Status (2026-11-15): done.**
   `runtime/kismet` reads every sequence of the loaded levels from memory (ops, inputs, outputs and their links,
   variable links, runtime state) and edits them: links added (the engine's arrays grown through GMalloc) and removed,
   new ops made by the engine's own constructor (`StaticConstructObject`, a symbol found through the script VM's `new`;
   the op's links come from its class defaults) and taken out again (from `SequenceObjects`, the active and delayed
   lists, every link to it). ABI v1 appended `kismet_read` (JSON; Lua `mirage.kismet()` as tables, C#
   `Mod.KismetJson()`), `kismet_link`, `kismet_unlink` and `kismet_create` (links by description or index; Lua, C#,
   C++). Each edit's undo goes in the change log (`changes::RecordUndo`, new: structural edits next to property
   values), in the mod's open transaction or one of its own, so revert and unload put the graph back. A mod's node is
   a class from step 12 (`define_class` extending `SequenceAction`, `Activated` in Lua). The investigation (self-test
   "Mirage Kismet (read)", the Prologue): 131 sequences (one root, 130 nested), 2,077 objects (305 events, 928
   actions, 605 variables), 1,247 links, read in 7.8 ms in release (target 20); cooking kept every object's editor
   title; every link leads to an op that was read; a checkpoint restart keeps the same sequence objects. Self-test
   "Mirage Kismet (edit)" (tests/mods/kismet-edit) is Scenario C: the mod takes a Prologue touch event's links (a
   `Trigger`'s), wires it to its own Ping action, the player walks in and the Lua runs once (the op's own counter
   agrees); the edit survives a checkpoint restart; a hot reload puts the graph back and the reloaded mod edits it
   again (one Ping op, a new one); the module off (`mirage.revert`) leaves the graph exactly as read before (ops and
   links compared). Kismet layers (`runtime/kismet_layers`): a mod's `kismet/<map>.toml` adds ops, unlinks, links
   and sets properties of the level's ops by name (the map's own level first when a sublevel has the same name, or
   the end of the path), applied at level loads, undone at unload and re-applied when the file changes; an entry
   naming nothing is skipped and listed. `kismet_fire` fires an event through SequenceEvent's own CheckActivate (the
   general conditions: enabled, MaxTriggerCount, ReTriggerDelay; not a class's own, such as a touch event's overlap,
   whose override only tests). The trace (`kismet_trace`, `kismet_trace_read`) compares each op's ActivateCount (an
   event's TriggerCount: the engine doesn't count events in ActivateCount) tick to tick while a mod has it on: about
   4 us a tick in release (15 in debug) for the Prologue's ~1,370 ops, nothing while off; inputs aren't recorded. Self-test "Mirage Kismet
   (layer)" writes a layer from names it reads (a SeqAct_Log op, a touch event's link moved to it, its properties
   set, an entry naming nothing), fires the event twice, sees both in the trace, removes the file and gets the graph
   back. UnrealLib reads Edge_p's file in the read step: its 199 Kismet objects and 118 links are exactly what the
   engine holds; the engine adds one thing at load, each streamed sublevel's root sequence nested in the level's root.
6. **Physics and world queries.** As step 20: rigid bodies, spawned props, traces and overlaps as plain structs, with
   native-only queries through step 2.
7. **Triggers, volumes and pieces.** As step 21, from code and from level layers.
8. **Input actions and custom moves.** As step 22. The experiment between a new `TdMove` subclass, a reused move
   slot and a "virtual move" comes first and decides the API. The replay part of step 24 stays with the menu. Mirage
   keeps `STATE_RESTORED` and adds what the menu's TAS needs to rewind a mod's move.
9. **Animation playback.** `PlayCustomAnim` and the custom animation node from mods, blended in and stopped on
   disable, for step 8's moves.
10. **Safe mode and conflicts.** A start after a start-up crash (or with Shift held) loads no mods and says why. Each
    mod's touch set comes from `mod.toml`, its content folders and its script patches, and overlaps are shown in the
    mod manager before load. Automatic bisection and permissions are the rest of step 37.

After these, the order above resumes: 24's mod-side hooks as the menu needs them, then 26, 28–31 by demand, and the
rest of phase 4 as its first user needs it.

## Testing strategy

- **Unit tests** (`tests/`, `mirage test`): everything that runs without the game (parsers, layer and Kismet formats,
  handle logic, change-log logic, graph diffing, auto-layout, query filters).
- **In-game self-tests** (`loader/src/selftest.cpp`, `mirage game-test`): the only proof a game feature works. Each gameplay
  step adds named steps, driven by TAS macros where movement is involved, checking state only the feature can cause
  (never gravity or time alone), waiting on state, not tick counts, and running fast (no deliberate stalls).
- **Test mods** (`tests/mods/`): one per capability, deployed by `game-test`; game packages they need are copied from
  the player's install, never committed (maps made here, like `MirageEditorRoom`, are kept with their sources).
- **Benchmarks** (`bench/`, `mirage bench`): costs that run without the game (formats, queries over recorded data).
- **Performance:** each step's costs are measured in the self-test's "performance" step and recorded here.

End-to-end scenarios, each the milestone of a step:

| Scenario | What it proves | Step |
|---|---|---|
| A. Live object modification | Find the player, inspect, change movement values, restore on unload | 14, 16, 17 |
| B. Custom movement mechanic | A double jump with conditions, physics, animation, input | 22 |
| C. Kismet modification | A real sequence inspected, a node added and wired, custom behaviour runs | 18 |
| D. Visual node editing | The same through the node editor, observed in the game | 19 |
| E. Physics interaction | A prop spawned, forced, changed, cleaned up | 20 |
| F. Gameplay object creation | A trigger wired to an interactive object running custom logic | 21 |
| G. Playable level extension | A course from pieces, triggers, physics and Kismet | 25 |
| H. Recovery and restoration | Disable, unload, restart: every change reverted | 15 (and every step's own check) |
| I. TAS compatibility | A custom mechanic recorded, rewound, replayed verified, across restarts | 22, 24 |

## Performance and memory budgets

Measured (with where):

| What | Cost | Where measured |
|---|---|---|
| Unhooked script function | 0.7-0.8 ns | Step 6, self-test |
| Call through ProcessEvent | 0.18 µs | Step 6 |
| Native hook / Lua hook | +0.28 µs / +1.12 µs | Step 6 |
| Empty injection / empty function hook | +0.10 µs / +0.22 µs | Step 11 |
| Lua module tick | 20-30 µs (release) | Step 9 |
| Path read from C# | ~10 µs | Step 21's notes (now 34) |
| Menu work per tick | ~0.8 ms average (debug, during the self-test) | Self-test "performance" |
| Package read (Core, Engine, TdGame) | 0.07 s, 44 MB while loaded | Step 1 |
| Texture encode, 1024² DXT5 chain | 8.0 ms (threaded; 37 ms before) | `bench/bench_textures.cpp`, 2026-11-13 |
| TAS keyframe | ~500 dirty pages, ~6-12 ms freeze | Self-test log, 2026-11-13 |
| .NET runtime | ~89 MB of address space when a C# mod exists; Roslyn 1.2 s at start | Step 21's notes |

Targets for the new steps are in each step and stay targets until measured. Two rules hold for every step: nothing
costs per tick while its feature is off or its editor closed, and every subsystem's memory is attributed in the memory
manager (`runtime/mem`) and reacts to the `MEMORY_PRESSURE` event. The shared budget is the 32-bit address space: the
game, TAS snapshots, .NET when present, textures and the editors all draw from it, so each new subsystem states its
peak and steady size.

## Risks

Known risks (seen or certain) and risks to investigate. Severity: H (blocks a milestone or crashes), M (costs a
redesign), L (costs time).

| Risk | Known / to investigate | Severity | Mitigation | Validation | Blocks |
|---|---|---|---|---|---|
| UnrealLib in a 32-bit process next to TAS snapshots runs out of address space | Known | H | Load only script packages, lazily; budgets per subsystem; pressure events | Step 1's measurement; memory manager reports | Any step that loads packages |
| 32-bit address space exhaustion from editors, .NET, textures, mod state | Known | H | Nothing allocated while off; attribution per mod (36); C# only when needed | Self-test memory reports with all editors open | 16, 19, 25 |
| Intermittent heap crash around TAS steps (MSVCR80, `ntdll`), ~2 in 12 full runs | Known, unexplained | M | Compare with a pre-Mirage build; suspect a rewind of the game's CRT heap | Repeated full runs | None directly; obscures failures |
| Computed layouts disagree with the engine for native classes | Known, handled | M | Step 2 checks every offset live; native-only fields from the live engine | "Mirage layouts" | — |
| UObject lifetime: a handle outlives its object (GC, level unload, rewind, slot reuse) | Known | H | Step 14's generations and lifecycle events; no raw pointers in editors | "Mirage world access" | 14 and all after |
| Incorrect reflected function parameters (UBOOL size, natives refused by ProcessEvent) | Known, handled | M | `mirage::call` lays out from the UFunction, clears the native index | "Mirage calls" | — |
| Runtime class registration breaks later (GC, serialisation) | Known risk | H | Classes through the engine's own loader (12); never saved into game packages or saves | Step 12's self-tests, GC in tests | 18, 21, 22 |
| Kismet edits incompatible with how the engine runs sequences (active op lists, pruned editor data) | To investigate | H | Step 18 starts with an offline/online comparison and minimal edits | "Mirage Kismet" | 18, 19, 26 |
| Invalid sequence links crash the sequence update | To investigate | H | Validate every link before writing; refuse type mismatches | Fuzzed link tests in the self-test | 18, 19 |
| Movement state corruption by a mod move (an `EMovement` past the enum) | To investigate | H | Step 22's experiment chooses the approach; reset to walking on failure | "Mirage custom move" | 22 |
| Animation incompatibility (a sequence on the wrong skeleton, root motion) | To investigate | M | Play only sequences of the pawn's own AnimSets first | "Mirage animation" | 23 |
| Physics instability or non-determinism (PhysX 2.x under rewinds) | To investigate | H | Document props as non-deterministic until step 20 proves otherwise | Replays with props in the self-test | 20, 24, 25 |
| Actor cleanup failures (a mod's actors left after unload or restart) | Known risk | M | Change log (15) and layers' tracking; count actors before and after | Every step's unload check | 15 onward |
| Level transitions and checkpoint restarts break mod state | Known (Restart Checkpoint needs a checkpoint) | M | Restart event (14); each step documents its restart behaviour; checkpoints from mods (26) | Restart in every gameplay self-test | 24, 26 |
| TAS desynchronisation from mod gameplay | Known risk | H | Step 24's rules (seeded random, game clock), opted-in state, replays record mods | Verified replays in 22, 24, 25 | 22, 24 |
| Runtime modifications that can't be reverted | Known risk | H | Only API writers, all through the change log (15); permanent changes explicit | Byte-for-byte revert checks | 15 onward |
| Mods hooking or editing the same thing fight | Known | M | One owner for every hook; priorities; conflicts in the mod manager (37) | Overlapping test mods | — |
| Third-party code crashes the game | Known, handled | H | SEH isolation, attribution, auto-disable | "Mirage lifecycle" | — |
| Script execution overhead (Lua 20-30 µs per module tick) | Known | M | Lua runs only where hooked; budgets; native for hot paths; bring the tick cost down | Performance step | 22 (Lua moves) |
| Editor overhead drops frames | Known risk | M | Nothing when closed; budgeted refresh; measured per workspace | Performance step with editors open | 16, 19, 25 |
| A Lua mod freezes or bloats the game | Known, handled | M | Instruction budget, memory limit; the mod is stopped and named | "Mirage runtime" | — |
| Engine symbols differ between game builds | Known | M | `symbols.toml` keyed by build hash; unverified symbols are absent | Step 42 | 32, 42 |
| ABI drift breaks old mods | Known, handled | H | Frozen struct tests, append-only table | `test_abi` | — |
| Three languages triple the API surface | Known | M | Same-step rule; coverage as a build check (34) | `test_abi` and the C# check | — |
| The .NET runtime's ~89 MB crowds snapshots and textures | Known | M | Loaded only with a C# mod; attributed (36) | Memory reports | — |
| Game systems differ between story levels beyond what the self-test covers | Known risk | M | Milestones play real story content, not only the Prologue | Level list grows per step | 18, 21, 28 |
| Byte-identical bytecode encoding is harder than decoding (jump offsets, labels) | Known, handled | M | Proven on the full corpus before runtime patching relied on it | Step 8's corpus check | — |
| The UnrealScript compiler is a project of its own | Known, handled | M | Scoped to round-tripping the decompiler's output | Step 12's corpus checks | — |
| Multiplayer and mod services widen what a bad mod can do | Known | M | Permissions (37); network off unless declared; per-mod message budgets | Step 43 | P3 only |
| Bisection restarts the game repeatedly and annoys the player | Known | L | Only on request, each restart confirmed, at most log2(n) + 1 restarts, crash records narrow the set first | Step 37 | 37 |
| A local WebSocket API is an attack surface | Known | M | Off by default, localhost only, a token per client, only commands mods declared | Step 44 | 44 |
| Crash reports leak personal data | Known | M | Opt-in, scrubbed, the exact payload shown before the first send | Step 47 | 47 |

## Decisions

- Scripting: Lua 5.4, compiled as C++ (2026-11-09).
- Mod manifest: `mod.toml` (TOML, read with toml11), not JSON (2026-11-10).
- Mod settings are declared in `mod.toml` (`[settings.<key>]`), like Geode's `mod.json` (2026-11-11).
- C# mods on .NET (now 10, x86) hosted through hostfxr, not Mono (2026-11-11): Mono's only 32-bit Windows build is
  unmaintained. C# scripts (`main.cs`) are compiled in the game with Roslyn; built assemblies work too (2026-11-12).
- The build's scripting language (`MIRAGE_DOTNET`): C# when on, Lua when off; new script mods and the script editor
  use it, and Lua mods run in every build (2026-11-11).
- The runtime's own tools (mod manager, console, log viewer, performance, themes, hotkeys, config actions, game status,
  self test) stay in the runtime, not in mods (2026-11-11).
- Gameplay first (2026-11-13): the roadmap was re-sequenced so the core gameplay systems (steps 14–27) come before
  platform and ecosystem work; infrastructure is built in its minimal form when a gameplay step needs it.
- Kismet is the game's own: Mirage edits UE3 sequence objects and never introduces a parallel graph system (2026-11-13).
- Every change a mod makes goes through one change log (step 15) before any editor writes the game (2026-11-13).
- A packaged mod is a `.mirage` zip read in place; folders stay for development (proposed in step 40).

## Open questions

Each with the investigation that answers it.

- ~~**Handles:**~~ answered in step 14: the slot, the object's unique name and its class identify it; destroyed actors
  are caught by `bDeleteMe`. No serial and no destruction hook.
- **Kismet writes:** what must the engine know beyond `SequenceObjects` for a new op to run; are cooked sequences
  missing editor data the editor needs; do checkpoint restarts reset runtime-edited sequences? (Step 18: read a level's
  sequences offline with UnrealLib and live, compare, then make the smallest edit and observe it.)
- **Custom moves:** can an `EMovement` value past the enum's end survive the native code, or must a mod move reuse a
  slot or run without a move object? (Step 22's experiment.)
- **Native-only movement constants:** which of the moves' numbers are script properties, bytecode literals or native?
  (Step 17's survey.)
- **PhysX determinism:** do rewinds restore PhysX's state, and do replays with rigid bodies verify? (Step 20.)
- **Traversal pieces:** what does each piece need besides its volume (splines, attach points, Kismet, geometry)?
  (Step 21's survey.)
- **Checkpoints from mods:** how does the checkpoint manager find checkpoints, and can a mod add one at runtime?
  (Step 26.)
- **Docking:** is ImGui docking enabled in the menu's build, and does it fit the menu's window model? (Step 16.)
- **Node editor:** `imgui-node-editor` or a custom canvas, by frame cost and fit with ImGui 1.92. (Step 19.)
- Whether the mod index should be curated or open.
- WebAssembly as a fourth, sandboxed compiled-mod format (any language, safe by construction): considered for after
  step 40, only if a runtime earns its place by measurement (size, address space, call cost) against C# and native.
- Whether the runtime should host mods out of process for untrusted code (a crash then can't take the game down, at the
  cost of a cross-process call per API use): measure the call cost before deciding.
