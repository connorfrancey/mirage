# Mirage: from menu to mod loader

Mirage turns the Mirror's Edge menu into a mod loader in the spirit of Fabric (a loader plus a hook system, with
everything else in mods) and Geode (a native runtime with a stable API, an in-game mod manager and tooling). The menu's
own features become the first mods built on it. UnrealLib (`C:\dev\cpp\lib\UnrealLib`, the C++ UELib port) is the
foundation for everything that has to understand the game's packages and script. Mods are written in **Lua** (the
default: no compiler, hot reload, sandboxed) or as native DLLs (for what Lua is too slow or too limited for); both use
the same API.

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

## Decisions

- Scripting: Lua 5.4, compiled as C++ (2026-11-09).
- Mod manifest: `mod.toml` (TOML, read with toml11), not JSON (2026-11-10).
- Mod settings are declared in `mod.toml` (`[settings.<key>]`), like Geode's `mod.json` (2026-11-11).

## Open questions

- The file format for a packaged mod (a folder only, or also a zip such as `.mirage`).
- Whether the mod index should be curated or open.
