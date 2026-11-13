# Mirage: from menu to mod loader

Mirage turns the Mirror's Edge menu into a mod loader in the spirit of Fabric (a loader plus a hook system, with
everything else in mods) and Geode (a native runtime with a stable API, an in-game mod manager and tooling). The menu's
own features become the first mods built on it. UnrealLib (`C:\dev\cpp\lib\UnrealLib`, the C++ UELib port) is the
foundation for everything that has to understand the game's packages and script. Mods are written in **Lua** (no
compiler, hot reload, sandboxed), **C#** (.NET, scripts compiled in the game or built assemblies, hot reload; see
`doc/csharp-mods.md`) or as native DLLs (for what the others are too slow or too limited for); all use the same API.

## Where we start

| Have | Gap for a mod loader |
|------|----------------------|
| `dinput8.dll` proxy loader, installer, launcher | Loads one DLL; no discovery, ordering or dependencies |
| `medge_menu.dll`: 43 modules registered at build time (`MEM_MODULE`), settings, config, hotkeys, themes | Nothing can be added without rebuilding the DLL; no ABI |
| Game layer: pattern scans, Tick / ProcessEvent / death / level load hooks | Private to the DLL; one hook owner per function |
| mmultiplayer SDK (generated headers) | Wrong `UField`/`UStruct` layouts, 1-byte bools where the engine has 4-byte UBOOLs (`IsInState`, `SetMove`, ...) |
| UnrealLib M0–M9: packages, linker, properties, reflection, bytecode decoder, decompiler, assets, package writer, T3D | C++20 (menu is C++17); no bytecode encoder; cooked packages hold no property memory offsets |
| Self-test in the real game (`memenu game-test`), 142 unit tests | Nothing that loads third-party code |

## Target architecture

```
MirrorsEdge.exe
 └─ dinput8.dll ............ loader: memory tracker, early phase, loads mirage.dll
     └─ mirage.dll ......... runtime: engine layer, reflection, hooks, events, VFS, mod manager UI
         ├─ Lua 5.4 ....... one sandboxed state per Lua mod
         ├─ mods\memenu\ ... the current features, as first-party mods (trainer, TAS, Info Lines, ...)
         └─ mods\<id>\ ..... mod.toml + main.lua and/or <id>.dll + content\ (packages, textures, script patches)
```

- **Stable C ABI** (`mirage/api.h`: a versioned function table, plain structs, no STL across the boundary) with a
  header-only C++ wrapper (`mirage/mirage.hpp`), so mods built with another compiler version keep working.
- **Lua is the main way to write a mod.** Lua 5.4, compiled as C++ (errors unwind C++ frames instead of `longjmp`
  over them), one sandboxed state per mod (no `io`/`os.execute`/`package.loadlib`; files only inside the mod's folder),
  bound to the same API as the C ABI. Game objects are reached through the live reflection of step 2, so a Lua mod can
  read and write any property and call any function by name without generated bindings. Why not LuaJIT: Lua 5.4 builds
  from source with the project's CMake and static CRT, and its error handling interoperates with C++; LuaJIT's speed is
  not needed for mod logic, and the hot paths (hooks on functions no mod hooks) never enter Lua.
- **Mods need not contain code:** content and script patches are data that the runtime applies through UnrealLib.
- **The rules that cost crashes so far stay rules:** no work under the loader lock, the rendering thread, the
  62 fps cap, the TAS memory ownership (see the project memory notes).

## The plan

Each step ends in a milestone with a check that proves it; a milestone is committed when its check passes.

### 1. Groundwork: C++20, UnrealLib in the process, runtime split

- Move the menu to C++20 and link UnrealLib (x86, static CRT `/MT` like the rest; its CMake already supports both).
- Split the source tree: `runtime/` (what becomes `mirage.dll`), `api/` (public headers, empty for now), `mods/memenu/`
  (the features, still linked in for this step).
- Load Core.u, Engine.u and TdGame.u with UnrealLib inside the game, lazily and off the game thread; measure time and
  memory (32-bit process: the TAS snapshots already use a lot of it).

- Build Lua 5.4 from source (as C++, static CRT, x86) into the DLL and run a script in the game: the toolchain and
  the error path (a Lua error caught as a C++ exception, no leak) proven before any API is built on it.

**Milestone:** `mirage.dll` builds with UnrealLib and Lua linked; full `game-test` passes; the log shows the three
script packages parsed in the game with their class counts, the time taken and the memory used, within a set budget,
and a Lua script run in the game (including a caught error).

**Status (2026-11-09): done, except the source tree split** (`src/runtime/` holds the new code; the move of the rest
into `runtime/` / `api/` / `mods/memenu/` waits for step 9, when the features become mods). C++20, UnrealLib and Lua 5.4
(compiled as C++) are linked. In the game (self-test "Mirage runtime"): Core, Engine and TdGame read in 0.07 s (release),
2,039 classes with 11,169 properties, 44 MB while loaded and nothing left after (counted by the menu's own operator
new, `src/runtime/memory.cpp`: the process's numbers move with the game's allocations); 13 Lua checks (sandbox, memory
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
comparison (`src/runtime/computed_layout.cpp`): byte aligned to 1, everything else to 4, structs to at least 4 and
Matrix to 16; consecutive bools share a dword except in functions; struct and class sizes are not padded, but a struct
property's element size is; a map property is 60 bytes. Self-test "Mirage calls": `src/runtime/call.h` lays parameters
out from the engine's UFunction and runs functions with a native index too: UE3's ProcessEvent refuses those (that is
why the SDK's IsInState and SetLocation "returned" garbage), so the index (UFunction+0x94, checked against IsInState =
281 and SetLocation = 267) is cleared for the call. The menu's SDK calls that were wrong now go through it
(`src/runtime/engine_calls.h`): SetMove (its flags came from stack garbage and its return value was written past the
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

**Status (2026-11-10): done.** `memenu sdk` builds and runs
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

**Status (2026-11-11): done, with two items moved on.** `api/mirage/api.h` is ABI v1: a `MirageApi` function table
that starts with its size and only grows, a size-prefixed module description with five callbacks, settings
(checkbox, slider, int slider, text, key), per-mod `config.toml`, and drawing primitives (a native mod can't share the
menu's ImGui). `mirage.hpp` wraps it in the menu's own `Module` / `ctl::` style. `src/runtime/manifest` reads `mod.toml`
with toml11 and warns about unknown keys; `semver` handles versions and npm/Cargo ranges. `src/runtime/mods` runs each
mod module as a normal menu `Module`. Every call into native code is under SEH: a crash is logged and the module
stopped. The Lua binding (`mod_lua`) is written only against `MirageApi`. Its callbacks hold the mod's lock and get the
instruction budget, and `require` stays inside the mod's folder. Mods load in id order after their runtime range and
required and incompatible mods are checked; topological order is step 5.

The check: `tests/test_abi.cpp` freezes every offset; semver and manifest have unit tests. The in-game self-test
"Mirage mods" covers `examples/mods/hello`, built by its own CMake project with **clang-cl** instead of a second MSVC
toolset (only one is installed, and another compiler is the stronger proof), and `examples/mods/hello-lua`. Both load,
show in the Mods column, run `on_enable` / `on_tick` / `on_render` / `on_disable`, save, load and clamp their
settings, and keep their own config. `memenu game-test` builds and deploys them.

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

**Status (2026-11-11): done, except two parts.** `src/runtime/resolve` decides what loads and in what order:
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

**Status (2026-11-11): done.** Function hooks (`src/runtime/fhooks`): every script function runs through
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
`memenu\`, which the memory map leaves out (checked by the self-test). Its changes to the game's own memory are rewound
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

**Milestone:** the first-party mods compile against `api/` headers only (enforced by the build: no runtime include
paths); every one of today's self-test checks passes with the features loaded as mods.

**Status (2026-11-11): mod manager done; every feature the API can express is a mod; the rest waits on API.**

- First-party mods live in `mods/`, are embedded in the DLL (`cmake/EmbedMods.cmake`) and written to the mods folder
  at startup (`runtime/firstparty`), so every install has them.
- `mods/memenu-trainer` (Lua): God Mode, Teleport to Spawn, Game Speed, Kill Bots, Freeze Timer.
  `mods/memenu-visual` (Lua): FOV, Crosshair, Show FPS, Watermark, Frame Rate Limit, Clear Frame.
  `mods/memenu-trainer` also has No Clip, Flight (flight.lua), Kickglitch, Beamer, Strang, Auto Kickglitch (tools.lua).
  `mods/memenu-trainer` also has Kickglitch Helper (helper.lua) and Tool Overlay.
  `mods/memenu-levels` (Lua): Levels (the level loader), Sublevels (loader.lua), Restart Checkpoint, Main Menu. Same names and settings as the native modules they
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
  (`cmake/FirstPartyMods.cmake`): its own DLL target with only `api/` (and allowed header-only libraries: nlohmann/json)
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
  against `api/` (like `examples/mods/hello`), never with the menu's include paths.
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

- `mirage` CLI (the `memenu` tool grown): new mod from a template (Lua or native), build, package, install into the
  game, run the in-game test harness for a mod (Lua mods can carry their own in-game tests).
- Docs: getting started, the API reference, the generated SDK and decompiled script, examples for each kind of mod.
- Installer and launcher handle mods; a mod index (a JSON registry in a Git repository) with hashes, versions and
  compatibility; update checks in the mod manager.

**Milestone:** someone who has never seen the code builds, tests and publishes a mod with only the docs and the CLI;
a release candidate installs, finds a mod in the index, installs and updates it.

## Beyond 1.0: steps 11–20

Steps 1–10 make Mirage a loader that can run the menu's own features as mods. Steps 11–20 make it a platform: mods
that add to the game rather than adjust it (new classes, levels, UI, sounds, multiplayer), mods that build on each
other, and the tools to write and debug them inside the running game. Each keeps the rules of 1–10: everything goes
through the API (appended, never changed), every capability has an in-game self-test, and nothing breaks the TAS's
determinism or the 62 fps cap.

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

**Status (2026-11-13): done.** Mods add classes two ways ([doc/mod-classes.md](mod-classes.md)):

- **UnrealScript:** a `.uc` file in the mod's folder, compiled in the game by UnrealLib's compiler (`uc-compiler`
  branch) into a script package in `memenu\classes\` that the engine loads itself (`runtime/classes`: the package cache
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

### 14. Mods for mods: services, events and versioned interfaces

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

### 15. Engine-level hooks: functions in the executable

Function hooks (step 6) cover UnrealScript and natives with a `UFunction`; most of the engine (physics, movement,
rendering, streaming, audio) is C++ with no `UFunction`.

- **A symbol database:** engine functions named by signature (pattern + verification), collected from the Ghidra work
  already done, shipped as data (`symbols.toml`, versioned with the game build), extendable by mods.
- **Detours through the runtime:** a mod hooks an engine function by symbol name: before / after / replace, with typed
  arguments for known signatures, and one owner (the runtime) for every detour (MinHook today), chained in priority
  order. Mid-function hooks (register context in, registers out) for the places with no clean boundary.
- **Safety:** detours installed only with the game thread parked at a frame boundary; each one is SEH-wrapped and
  attributed; a crash inside one removes it and names the mod; the TAS treats detour trampolines as runtime memory.

**Milestone:** a native mod changes the player's ground friction by hooking the physics step by symbol name, a Lua mod
hooks the same function after it, both unload cleanly, and the self-test crashes inside a test detour on purpose and the
game keeps running with the detour gone and the mod named.

### 16. Rendering and the game's UI

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

### 17. Audio, input and controllers

- **Sound:** play WAV/OGG from a mod's folder at a location or on the HUD (through the game's audio device so volume
  sliders apply), replace a `SoundCue` by path like textures, and music that follows the level.
- **Input actions:** a mod declares actions in `mod.toml` (`[actions.dash] default = "Q"`); they appear in the game's
  own key-binding screen next to the game's and are saved with the profile (survives step 9's Keep Key Bindings).
- **Controllers:** XInput read for mods (sticks, triggers, rumble) and bindable to actions; the TAS records them like
  keys.

**Milestone:** a mod adds a "dash" action bound in the game's own controls menu to a key and a gamepad button, plays a
mod sound on use, and a TAS of it plays back verified.

### 18. Multiplayer for mods

The menu's history includes mmultiplayer; this makes multiplayer a service every mod can use.

- **Sessions:** host / join through a relay or direct UDP, lobby with the mod list (joining requires the same mods and
  versions; the mod manager offers to install what's missing from the index).
- **Replication:** mods mark values and objects as replicated (owner, rate, interpolation) and send RPCs; players see
  each other's ghosts with animation (the existing pose capture), tags, and their mods' visuals.
- **Modes as mods:** race (shared start, ghosts, finish order), tag, and co-op challenges written in Lua against the API.
- **Rules:** no mod code runs on the network thread; message budgets per mod; malformed packets are dropped and logged.

**Milestone:** two machines join a session, see each other move, run a race mode written in Lua to a shared finish
with matching times; a mod mismatch on join names the missing mod and installs it.

### 19. Determinism, replays and verification with mods

- **Mods in snapshots:** mods opt in to being rewound (`mirage.tas.state(...)`: the values to keep, or a serialise /
  restore pair); a TAS rewind restores them with the game, so mods with gameplay state (scoring, timers, new actors from
  step 12) work in practice mode. Mod memory stays out of the snapshot by default (step 5's rule).
- **Deterministic mod API:** mod randomness through `mirage.random` (seeded by the session), time through the game's
  clock, never the wall clock while a session runs; a lint in the CLI flags what isn't.
- **Replays record their mods:** a `.merp` stores each loaded mod's id, version and hash and its declared gameplay
  effect; playback warns or refuses on a mismatch (step 9's Preflight extended).
- **Verified categories:** a run verified by the menu (step 4 of the TAS) carries a signed summary: mods loaded and
  whether each is "cosmetic" (draws only, declared and checked: no writes, no hooks that return values) or "gameplay".

**Milestone:** a challenge mod with a score and its own spawned actors is recorded with practice-mode rewinds, plays
back verified, and the same replay with the mod at another version reports the mismatch before it starts.

### 20. A mod developer's environment in the game

- **Lua debugger:** breakpoints, stepping and variable views over the Debug Adapter Protocol, so VS Code attaches to
  the running game; the game pauses at a frame boundary on a break (the TAS's pause path), never mid-frame.
- **Object inspector:** browse live objects, classes and their properties (step 2's model), edit values, watch them per
  frame, follow references; jump from an object to its decompiled script (step 8) and its hooks and injections.
- **Profiler per mod:** time per callback, hook and injection, Lua memory, allocations; budgets shown in the mod
  manager and warnings when a mod costs frames.
- **Native hot reload with state:** a native mod's DLL rebuilt by the CLI is swapped in at a frame boundary with its
  state carried over (a serialise / restore pair, like step 19's).
- **Permissions:** `mod.toml` declares what a mod uses (files outside its folder, network, engine hooks, process memory);
  the mod manager shows them at install and the runtime refuses what wasn't declared; signed releases in the index.

**Milestone:** a mod author breaks in a Lua hook from VS Code, inspects a locals table and the player object, changes
a property in the inspector, reloads a native DLL without losing state, and the profiler names the slowest callback;
a mod that opens a socket without declaring `network` is refused and says why.

## Steps 21–40: an ecosystem

Steps 11–20 make Mirage a platform. Steps 21–40 make it one people can live on: three languages that are equally
first-class, a loader that stays fast and recoverable with a hundred mods installed, the game systems a content mod
needs that 11–20 don't reach (Kismet, AI, movement, saves), and the outside world: profiles, packages, stream tools,
leaderboards and a stable ABI for years. The rules of 1–20 hold, plus one: **every step names its cost** (load time,
frame time, address space) with a number measured in the self-test, because the game is a 32-bit process and every
new capability competes for the same 2 GB.

### Platform and languages

#### 21. C# at parity with Lua and native

C# mods (.NET 10 x86 through hostfxr, `doc/csharp-mods.md`) run, hot reload and compile scripts in the game; their
API is a friendly subset with the rest behind raw function pointers.

- **Full API coverage:** the UI layer (`MirageUi`), script patching, detours, the TAS service and step 14's services
  get C# wrappers; a generated test (like `tests/test_abi.cpp`) fails the build when `api.h` grows without them.
- **Typed game objects:** a generator over the SDK (`sdk/`) emits C# structs and wrappers (`TdPawn.Velocity`,
  `TdPlayerController.AttackPress()`) on top of the path API, so mods get compile-time names; the cost of a typed read
  is measured against the path read (~10 µs today) and must be at most a tenth of it.
- **Shipping:** the installer ships `memenu\dotnet` (only when the player has a C# mod or opts in: ~70 MB on disk);
  Mirage.dll on NuGet with a `dotnet new mirage-mod` template; compiled scripts cached on disk by source hash, so a
  restart doesn't pay Roslyn again (1.2 s now).
- **Debugging:** Visual Studio and VS Code attach to the game's managed runtime (breakpoints in a mod's `.cs`, scripts
  included through their in-memory PDBs).

**Milestone:** a first-party Lua mod (memenu-trainer) is ported to C# with typed objects and passes the same
self-test; a breakpoint in its OnTick hits from VS Code; a second start with only script mods doesn't load Roslyn.

#### 22. The in-game IDE

The script editor (`features/misc/script_editor.cpp`) grows into what a mod author needs without leaving the game.

- **Editing:** syntax highlighting, line numbers, find / replace and bracket matching (an ImGui text editor widget,
  measured: under 1 ms a frame for a 2,000-line file); multiple cursors are out of scope.
- **Language services:** completion, signature help and hover docs from Roslyn for C# (the SDK and Mirage API
  included) and from the API's own declarations for Lua; go to definition into the generated SDK and the decompiled
  script (step 8).
- **Live values:** an expression watch over the running mod (Lua and C#), and "run selection" in a REPL against the
  mod's own state; the console's Lua and C# commands share it.
- **Project shape:** new file, rename, templates per kind (module, hook, event handler, HUD), and "export as project"
  (a `.csproj` or a folder ready for the CLI of step 10).

**Milestone:** a new C# mod is written in the game from template to a working HUD with completion only (no external
editor), errors jump to their line, and the editor never drops the frame rate below the 62 cap.

#### 23. A hundred mods: start-up, memory and budgets

- **Start-up budget:** loading is measured per mod and per phase; mods load in parallel where their dependencies allow
  (parsing, Lua compile, C# compile on workers; only the publish on the game thread). Target: 100 small mods add under
  2 s to the start and under 100 MB of address space.
- **Lazy mods:** a mod declares `load = "on-demand"` and loads the first time one of its modules is switched on, a
  service is asked for, or its event fires; library mods (step 14) are on-demand by default.
- **Address-space accounting per mod:** the memory manager (`runtime/mem`) attributes Lua heaps, C# contexts, textures
  and native DLL images to their mod; the mod manager shows them; low address space unloads on-demand mods first.
- **Frame budgets:** each mod's tick and render time is tracked (step 20's profiler); a mod over its budget for a
  sustained period is throttled to the scheduler's Background priority and named, never silently.

**Milestone:** a generated set of 100 mods (Lua, C#, native, content) starts within the budget, the memory report
attributes every MB to a mod or the runtime, and one deliberately slow mod is throttled and named while the rest keep
their frame time.

#### 24. Safe mode, bisection and undoable mods

- **Safe mode:** a start after a crash during the previous start (or Shift held at launch) loads no mods and says why;
  the mod manager offers to bisect.
- **Automatic bisection:** the runtime restarts the game with half the suspected mods, using the crash records and
  the dependency graph, until it names the mod (or pair of mods) that crashes; the player confirms each restart.
- **Transactions:** everything a mod changes through the API (properties, defaults, textures, script patches, spawned
  actors, detours) is recorded in its change log; unloading reverts the log exactly, and a "what did this mod change?"
  view lists it.

**Milestone:** two mods that crash only together are found by bisection in at most 4 restarts; every change of a test
mod is listed and reverted byte-identically on unload, checked against a snapshot of the touched memory.

#### 25. Static analysis and conflict detection

- **Before loading:** a mod's touch set is read without running it: the functions it hooks or injects into, the
  properties and defaults it writes, the textures and packages it replaces (from `mod.toml`, content folders, script
  patches, and the API calls found in Lua and C# sources and IL).
- **Conflicts:** overlapping touch sets are shown in the mod manager before the conflict happens ("both replace
  `T_Faith_Body`", "both skip `TdPawn.TakeDamage`"), with the load order deciding and a per-pair override.
- **Lints:** the CLI warns on what tends to break (writes every tick, hooks on hot functions without a filter, wall-clock
  time in gameplay code: step 19's determinism lint).

**Milestone:** the conflicts between the first-party mods and a set of deliberately overlapping test mods are all
reported before load, with no false positives on the first-party set.

#### 26. Game builds and compatibility

- **Build detection:** the Steam, EA app / Origin and GOG executables (and their patches) identified by hash; the
  runtime knows which offsets, patterns, SDK and `symbols.toml` (step 15) belong to which build.
- **Per-build data:** the SDK generator (step 3) and the symbol database run per build; layouts that differ are data,
  never `#ifdef`s; a mod declares `game = [...]` builds or relies on the API, which hides the difference.
- **CI on every build:** game-test runs against each supported executable the developer owns; the README lists what
  passes.

**Milestone:** the full self-test passes on two different game builds from one menu DLL; a mod built against one runs
on the other with no change, and one declaring a build it isn't on says so instead of loading.

#### 27. Profiles and load sets

- **Profiles:** named sets of enabled mods with their settings (`profiles\<name>.toml`): "Speedrun (legal)", "Practice",
  "Content"; switching reloads only what differs (hot reload where possible, a restart only for what can't).
- **Category rules:** a speedrun profile can be locked to a category's allowed mods and versions (the list published
  with step 37), and the TAS / timer show the active profile.
- **Sharing:** a profile exports as one code or file with mod ids, versions and hashes; importing installs what's
  missing from the index.

**Milestone:** switching from a 30-mod content profile to the speedrun profile takes under 3 s without a restart, and a
shared profile imported on a clean install reproduces the exact mod set and settings.

#### 28. Packages, updates and rollbacks

Answers the open question of a packaged format.

- **`.mirage` packages:** a zip with `mod.toml` at its root, read in place (no unpacking; the content VFS reads from
  it), hashes per file, an optional signature (step 20); folders keep working for development.
- **Updates:** delta downloads from the index, applied at the next start (never under a running mod), with the previous
  version kept: one-click rollback, and automatic rollback when the new version crashes on its first start.
- **Dependencies:** installing a mod installs what it requires (semver-resolved against the index), and removing one
  offers to remove what nothing else needs.

**Milestone:** a mod is updated, crashes on start, and is rolled back automatically with the reason shown; a package
installs with its two dependencies from a clean state, and the folder form and the package form load identically.

### Game systems for content mods

#### 29. Kismet and mission scripting

The game's levels are driven by Kismet sequences (triggers, matinee, objectives); 11–13 reach script and actors, not the
sequences that tie them together.

- **Read:** the live sequence graph of a level (objects, links, variables) through the step 2 model, shown in a viewer.
- **Change:** a mod adds sequence objects and links at level load (a TOML layer, like step 13's actor layer), or
  defines new sequence actions and events whose body is mod code (step 11 natives, step 12 classes).
- **Missions:** a small layer on top: objectives, checkpoints, dialogue lines (step 17's sound), success and failure,
  fed to the speedrun timer's splits.

**Milestone:** a mod adds a new objective to a story chapter (reach a point, then a mod-defined sequence action opens a
door), the game's own HUD shows it, checkpoint restarts keep it consistent, and the self-test plays it through.

#### 30. AI and enemies

- **Spawning:** enemies (the game's cop and soldier archetypes) spawned at points with their loadout and team, through
  the API and in level layers; removed on unload and on restart like any mod actor.
- **Behaviour:** hooks and services over the AI controllers (perception, targeting, cover, chase), and mod-defined
  behaviours in Lua or C# that run at the AI's own update rate within a budget.
- **Navigation:** path data for new geometry (step 13 maps and level additions): generated offline by the CLI, loaded
  with the level.

**Milestone:** a mod sets up an ambush in an existing level with three enemies using a mod-defined flanking behaviour;
they navigate a mod-added area, and the TAS replays the encounter verified (AI included in the determinism of step 19).

#### 31. Movement and animation

Faith's movement is a set of move states; the trainer and TAS already read and write them.

- **New moves:** a mod adds a move (a subclass of the game's move classes, step 12) with its entry conditions, physics
  and transitions, and binds it to an input action (step 17).
- **Tuning:** every movement constant (speeds, timings, wallrun lengths) readable and overridable per mod with an
  undo (step 24's change log), shown in a tuning panel.
- **Animation:** play existing animation sets on new moves; import new ones through the CLI (step 16's model pipeline)
  and blend them with the game's.

**Milestone:** a mod adds a double jump with its own animation and sound, usable in time trials, recorded and played
back by the TAS, and switching the mod off mid-level leaves Faith's movement exactly as the game's.

#### 32. Physics objects and gameplay pieces

- **Pieces:** the level's gameplay actors (ziplines, swing poles, springboards, ledges, doors, valves) placeable from
  mods and the step 13 editor with their parameters, and dynamic physics props with mass and constraints.
- **Triggers:** a library of volumes (enter, leave, stay, count) with mod callbacks, drawn by the Collision View.
- **Physics queries:** ray, sweep and overlap tests against the world for mods, budgeted per frame.

**Milestone:** a mod builds a small course in an existing level from these pieces only (no new package): a zipline to a
swing pole to a springboard, all usable and recorded by the TAS; 500 physics queries per frame cost under 1 ms.

#### 33. Cameras, photo mode and cinematics

- **Camera API:** a mod takes the camera (position, rotation, FOV, post process) with a priority, and gives it back.
- **Photo mode:** pause, free camera, depth of field, filters (step 16's post process), screenshots at higher resolution
  than the screen.
- **Cinematics:** keyframed camera paths over a replay (the TAS's ghosts and recordings), exported as a video frame
  sequence at a fixed frame rate regardless of the game's speed.

**Milestone:** a 30-second keyframed shot over a TAS replay renders to an image sequence at 60 fps with no dropped or
repeated frame, and photo mode works in every level of the story.

#### 34. Save games and persistent mod data

- **Mod data with saves:** a mod's state saved in a sidecar file next to each game save slot (`<slot>.mirage.toml`),
  loaded with the slot; the game's own save files are never changed.
- **Schema versions:** a mod declares its data version and a migration function; old data is migrated once and kept
  as a backup.
- **Global progress:** per-profile data (unlocks, best times) that isn't tied to a slot, with the same migrations.

**Milestone:** a mod's collectible count survives save, quit, load and a mod update with a migration; deleting the
mod leaves the game's saves loadable by the unmodded game.

#### 35. Localization and text

- **Strings:** mods ship translations (`lang/<code>.toml`) and get the player's language from the game; missing keys
  fall back to English and are listed for translators.
- **The game's text:** a mod overrides or adds entries of the game's localization (subtitles, menu text) per language,
  as content (step 7).
- **Fonts:** coverage checked for the languages a mod declares; the menu's fonts get the missing glyph ranges.

**Milestone:** the first-party mods are translated into one more language with no code change, and a mod replaces a
subtitle line in the game's own subtitles in two languages.

### The outside world

#### 36. External control: stream tools, LiveSplit, companions

- **Local API:** a WebSocket server (off by default; step 20 permission) exposing events and commands that mods
  choose to publish: level, time, deaths, the TAS state, mod-defined values.
- **Integrations:** LiveSplit (splits and game time from the speedrun timer), browser-source stream overlays, and
  chat-driven events for streamers (votes that a mod turns into gameplay, rate-limited).
- **Security:** localhost only unless enabled, a token per client, every command declared by a mod and logged.

**Milestone:** LiveSplit splits a run from the menu's timer with game time exact to the tick, and a browser overlay
shows a mod's live values with under 50 ms of latency.

#### 37. Challenges and leaderboards

- **Challenge format:** a challenge is a mod (route, rules, allowed mods) with a stable id and version; the game's time
  trials are the first challenges.
- **Leaderboards:** submitted runs carry their replay and step 19's signed summary; the server replays them headless
  where possible (step 38's simulated engine for the checks it can do) and ranks verified runs only.
- **Rules:** cosmetic mods allowed by default, gameplay mods only where the challenge lists them; the speedrun profile
  of step 27 is the default for submissions.

**Milestone:** a community challenge is published, played by two accounts, both runs verified and ranked; a run made
with an unlisted gameplay mod is rejected with the mod named.

#### 38. Offline mod testing

- **A simulated engine:** the testhost (already used for parts of the menu) grows into enough of the API's behaviour
  (objects, properties, events, a fixed tick) to run a mod's logic without the game: mods ship tests, the CLI runs them.
- **Recorded worlds:** a snapshot of a level's objects and properties taken in the game replays into the simulated
  engine, so tests read realistic values.
- **CI:** a GitHub Actions template for mods (build, test, package, publish to the index); the menu's own CI runs the
  first-party mods' tests the same way.

**Milestone:** every first-party mod has tests that run offline in under 30 s in CI, and a mod's PR on GitHub is built,
tested and packaged by the template without the game installed.

#### 39. Reports for mod authors

- **Crash and error reports:** opt-in per player, sent per mod to the mod's declared endpoint (or the index's), with
  the crash reporter's attribution and symbolicated stacks (PDBs, portable PDBs, Lua tracebacks), and no personal data
  (paths and names scrubbed, the exact payload shown before the first send).
- **Health:** the index shows a mod's crash rate per version and game build; a version over a threshold is flagged for
  rollback (step 28).

**Milestone:** a deliberate crash in a test mod reaches its author's endpoint symbolicated with file and line; the
payload contains nothing that identifies the player.

#### 40. ABI v2 and long-term support

- **Deprecation policy:** an API entry is deprecated with a replacement and a warning for at least two minor versions,
  and removed only in a new ABI.
- **ABI v2:** what years of v1 taught (handles instead of raw pointers where lifetimes bit, async calls, batching for
  property access), with v1 kept running through a shim: every v1 mod in the index loads unchanged.
- **Releases:** an LTS line of the runtime with security and game-build fixes only; the API reference, guides and
  examples published as a versioned docs site generated from the headers and the C# and Lua bindings.

**Milestone:** every mod in the index built for ABI v1 passes its tests on the v2 runtime through the shim, and the
docs site shows each entry's status (stable, deprecated, new in v2).

## Order and dependencies

```
1 ─► 2 ─► 3 ─────────────► 9 ─► 10
     │         4 ─► 5 ─► 6 ─┘
     └──────────► 7 ─► 8
```

Steps 4–6 (the loader) and 7–8 (content and script through UnrealLib) can run in parallel once step 2 exists. Step 9
waits for 3 and 6 because the features need the generated SDK and shared hooks.

Steps 11–20:

```
8 ─► 11 ─► 12 ─► 13
6 ─► 14 ─► 15 ─► 16 ─► 17
           14 ─► 18
9 (TAS) ─► 19 ◄─ 12
10 ─► 20 (needs 11, 14, 15 for the inspector and profiler views)
```

11 (injection, natives) is what 12 (new classes) calls into, and 12 is what 13 (levels) places. 14 (services) comes
before 15–18 so the engine hooks, rendering, audio and multiplayer are offered as services mods can depend on and
version. 19 waits for 12 because new actors are what make mod state worth rewinding.

Steps 21–40:

```
C# today ─► 21 ─► 22                     (22 needs 20's debugger for its watch view)
5 ─► 23 ─► 24 ─► 25                      (25 reads 24's change log)
3, 15 ─► 26
10 ─► 27 ─► 28                           (28 resolves against 10's index)
13 ─► 29 ─► 30 ─► 31 ─► 32               (12's classes underneath each)
16 ─► 33        9 ─► 34 ─► 35
14 ─► 36        19 ─► 37 ◄─ 27, 38
10 ─► 38 ─► 39  all ─► 40
```

The platform steps (21–28) come first: content built on a loader that can't recover from a bad mod or slows down with
many would be built twice. 23 before 24 because bisection needs fast restarts, and 24's change log is what 25 analyses
and 31 undoes. The game systems (29–35) follow 12–13 because they place and subclass what those steps create. 37 waits
for 19 (verified replays), 27 (category profiles) and 38 (headless checks). 40 is last by definition: it is what the
years of v1 in 21–39 teach.

## Risks

| Risk | Plan |
|------|------|
| UnrealLib in a 32-bit process next to TAS snapshots runs out of address space | Load only script packages, lazily; measure in step 1 and keep a budget |
| Computed layouts disagree with the engine for native classes | Step 2 checks every offset live; native-only fields come from the live engine, not the packages |
| ABI drift breaks old mods | Frozen struct tests, append-only function table, runtime refuses mods built for a newer ABI |
| Mods hooking the same thing fight | One owner (the runtime) for every hook; priorities; conflicts shown in the mod manager |
| Third-party code crashes the game | SEH isolation, attribution, auto-disable; the existing crash logger names the mod |
| Byte-identical bytecode encoding is harder than decoding (jump offsets, labels) | Prove it on the full corpus before any runtime patching relies on it |
| A Lua mod freezes or bloats the game | Per-mod instruction budget and memory limit; the mod is stopped and named |
| Lua in hot hooks costs frame time | Lua runs only for functions a Lua mod hooks; the cost per call is measured (step 6) |
| Injected bytecode or runtime classes corrupt the engine in ways that crash much later (GC, serialisation) | Every injection and class verified against the live model before use; the GC and save paths exercised by the self-test with them present; nothing a mod creates is ever saved into the game's own packages or save game |
| The UnrealScript compiler is a project of its own | Scope it to round-tripping the decompiler's output (step 12 milestone), not the whole 2008 language |
| Engine symbols differ between game builds (Steam, Origin, GOG) | `symbols.toml` keyed by build hash; a symbol that doesn't verify is absent, never guessed; mods that need it don't load |
| Multiplayer and mod services widen what a bad mod can do | Step 20's permissions; network off unless declared; per-mod message budgets |
| Three languages triple the API surface to keep in step | Lua, C# and the C ABI are generated or checked from `api.h` (step 21's coverage test); a feature isn't done until all three have it or it is listed as native-only |
| The .NET runtime's ~89 MB of address space crowds TAS snapshots and textures | Loaded only when a C# mod exists (now); step 23 attributes it and unloads on-demand mods first; the runtime itself can't unload, so the mod manager says when a restart without C# mods would free it |
| Bisection restarts the game repeatedly and annoys the player | Only on request, each restart confirmed, at most log2(n) + 1 restarts, and the crash records narrow the set first |
| A local WebSocket API is an attack surface | Off by default, localhost only, a token per client, only commands mods declared (step 36) |
| Crash reports leak personal data | Opt-in, scrubbed, the exact payload shown before the first send (step 39) |
| Game systems (AI, Kismet, movement) differ between story levels in ways the self-test doesn't reach | Each step's milestone plays through real story content, not only Prologue; the level list grows with each |

## Decisions

- Scripting: Lua 5.4, compiled as C++ (2026-11-09).
- Mod manifest: `mod.toml` (TOML, read with toml11), not JSON (2026-11-10).
- Mod settings are declared in `mod.toml` (`[settings.<key>]`), like Geode's `mod.json` (2026-11-11).
- C# mods on .NET (now 10, x86) hosted through hostfxr, not Mono (2026-11-11): Mono's only 32-bit Windows build is
  unmaintained. C# scripts (`main.cs`) are compiled in the game with Roslyn; built assemblies work too (2026-11-12).
- The build's scripting language (`MEMENU_DOTNET`): C# when on, Lua when off; new script mods and the script editor
  use it, and Lua mods run in every build (2026-11-11).
- A packaged mod is a `.mirage` zip read in place; folders stay for development (proposed in step 28).

## Open questions

- Whether the mod index should be curated or open.
- WebAssembly as a fourth, sandboxed compiled-mod format (any language, safe by construction): considered for after
  step 28, only if a runtime earns its place by measurement (size, address space, call cost) against C# and native.
- Whether the runtime should host mods out of process for untrusted code (a crash then can't take the game down, at the
  cost of a cross-process call per API use): measure the call cost before deciding.
