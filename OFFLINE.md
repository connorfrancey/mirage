# Working on the loader offline

A plan to clean up, organize and improve the loader (`loader/src/`) by hand, without an assistant. Every task says
where, how, how to check it and what to commit. The tasks are in order: early ones make the later ones easier, and
none of them changes what a mod sees. It replaces `docs/refactor-plan.md` (written 2026-11-18 for the whole
repository); delete that file once you're following this one.

Measured on 2026-11-18. Numbers will drift: re-measure with the commands given before you start a task.

## The rules

- **One change, one commit.** A move, a rename or a split never shares a commit with a behaviour change. When the
  self-test turns red, the guilty commit is small enough to read.
- **Build and test after every step.** `uv run mirage errors` (every error, grouped by file) after a move;
  `uv run mirage test` (unit tests, 10 s) after every commit; the game steps of the area you touched
  (`uv run mirage game-test --no-build --only "Step A,Step B"`); a full `uv run mirage game-test` (about 7 minutes) at
  the end of each part below.
- **Keep a baseline.** Before part 1, run the full game-test and copy
  `<game>\Binaries\mirage\selftest-result.json` to `baseline.json` outside the repository (the game is in
  `C:\Program Files (x86)\Steam\steamapps\common\mirrors edge`). After each part the same steps pass with the same
  notes (counts, distances; timings within reason).
- **Known flaky steps** (pass when run alone, 2026-11-18): "Mirage movement tuning", "Mirage grapple", "Memory
  manager" (its debug budget, 10.97 ms of 10). And a game crash at level load, `MirrorsEdge.exe+0x74D433`, about one
  launch in 25, in the game's own code. Rerun the step before suspecting your change.
- **Use the IDE's refactorings** (CLion: Rename, Move, Extract Function, Change Signature), not search and replace:
  they update every caller and include.
- **Bisect, don't guess.** If a step fails after several commits:
  `git bisect start HEAD <good>` then `git bisect run uv run mirage game-test --only "<step>"`.
- **Don't touch the frozen ABI.** `loader/include/mirage/api.h` only grows at its end (`tests/test_abi.cpp` guards
  it). Internal names change freely; anything a mod sees doesn't.
- **Commit only your paths:** `git commit -- <paths>`, so files you keep staged for yourself stay out.

## Part 0: a clean starting point (an evening)

**0.1 Decide the untracked files.** `git status` lists files that are neither committed nor ignored. Nothing in the
loader includes any of these (checked with `grep -rn "<name>" loader/src`):
`loader/src/util/parsing/csv2.hpp`, `loader/src/util/vk_util.h`, `loader/src/imgui/imgui-combo-filter.cpp/.h`,
`loader/src/imgui/imgui_extensions.h`, `loader/src/crashhandler/analyzer/4gb_patch.h`. For each: delete it, or commit
it with the code that uses it. Unused files in `loader/src/` are still compiled (the runtime is globbed), and
`imgui-combo-filter.cpp` brings 5 warnings (C4706, C4996 `strncpy`). Also decide `AGENTS.md`, `docs/FEATURES.md`,
`docs/TOOLS.md`, `opencode.jsonc`, `.mcp.json`, `.myollamaenhancer/`: commit, delete or add to `.gitignore`.

**0.2 Write down what stays local.** `cmake/Libraries.cmake` and `cmake/Versions.cmake` carry your own uncommitted
rewrite. Either commit it (build, test, commit) or note in `CLAUDE.md` / `AGENTS.md` that it stays local.

**Check:** `git status` shows only what you chose to keep; `uv run mirage build` reports 0 warnings in our code;
`uv run mirage build -c release` builds.

## Part 1: delete what nothing uses (an evening)

These `loader/src/core/` headers are used only by their own unit tests (count with
`grep -rl "core/<name>.h\"" loader/src tests`):

| Header | Used by |
|---|---|
| `adaptive_cache.h` | `tests/test_caches_more.cpp` only |
| `function_cache.h` | `tests/test_cache2.cpp` only |
| `resource_cache.h/.cpp` (and `weak_cache.h`, which only it and a test use) | `tests/test_infra.cpp`, `tests/test_caches_more.cpp` |
| `ttl_cache.h` | `tests/test_cache.cpp` only |

For each: delete the header (and its `.cpp`), delete its tests (just those `TEST`s, not the whole file if it tests
other things), remove them from `tests/CMakeLists.txt` and `cmake/Features.cmake` if listed, build, test, commit
("core: remove <name>, unused"). One header per commit, so one can come back with `git revert` if a use turns up.

Then look for more: for every header in `loader/src/core/`, `loader/src/game/` and `loader/src/runtime/`, count its
includers; a header only its own `.cpp` and tests include is a candidate. Check that nothing reaches it by a
different include path first (`grep -rn "<name>.h"`).

**Check:** unit tests green (fewer of them), full game-test green.

## Part 2: one copy of each small helper (a weekend)

The same few lines are written again in many files. Each copy is a place a fix can be missed.

**2.1 `Lower`.** `std::string Lower(std::string)` is defined in 17 files (`grep -rn "std::string Lower(" loader/src`):
`diag.cpp`, `gui.cpp`, `selftest.cpp`, `game/exec_commands.cpp`, `game/player_state.cpp`,
`features/misc/kismet_editor.cpp`, and in `runtime/`: `anim`, `inject`, `kismet_layers`, `layers`, `maps`, `mod_io`,
`routes`, `texture_files`, `touch`. Make `loader/src/util/text.h` (header-only, `namespace text`, a one-line comment
on top) with `inline std::string Lower(std::string s)`, and replace the copies one file per commit: delete the local
function, include `util/text.h`, use `text::Lower`. If a copy differs (one lowercases only ASCII, one uses
`std::tolower` on `char` without the `unsigned char` cast), keep the correct one and note it in the commit.

**2.2 `Utf8` and `FromUtf8`.** The UTF-8 path conversions are defined in `runtime/manifest.cpp`, `mods.cpp`,
`mod_io.cpp`, `texture_files.cpp` and `texture_overlays.cpp`, and written inline as
`fs::path(std::u8string(s.begin(), s.end()))` elsewhere (`grep -rn "u8string(" loader/src`). Move both into
`util/text.h` (`text::Utf8(path)`, `text::PathFromUtf8(string)`), then replace the copies and the inline forms.

**2.3 A mod by id.** 31 places loop over `mirage::mods::List()` to find one mod
(`grep -rn "mods::List()" loader/src`). Add to `runtime/mods.h`:
`std::optional<Info> Find(const std::string& id);` (loaded or not; the caller checks `loaded`), implemented next to
`List()`. Then a second helper for the `"<mod id>:path"` form that level layers use for classes and meshes, written
three times (`runtime/layers.cpp` around lines 173 and 523, `runtime/mod_game.cpp` around 1153):
`std::filesystem::path ResolveModFile(const Info& self, const std::string& name, std::string* why)`: the file in
`self`'s folder, or in the named mod's with `"id:"`, empty and `why` if that mod isn't loaded. Replace the loops one
caller per commit. The `script_editor.cpp` `DirOf` and `ProblemOf` become one-liners over `Find`.

**2.4 The worker build cache.** `runtime/layers.cpp` (`BuiltClassOf`, a `.uc` compiled on a worker) and
`runtime/models.cpp` (`MeshOf`, a model's package built on a worker) hold the same machinery: a map by file of
{file time, `std::future`, done, error, the loaded object and its GObjects slot}, "pending" until the future is
ready, then loaded once and kept while its slot still holds it. Only do this after 2.1-2.3, and only if you want it:
a small template `runtime/worker_build.h` (`WorkerBuild<T>` with `Get(key, stamp, start, load, why, pending)`) would
serve both. Two users is the minimum for it to pay; if it reads worse than the two copies, skip it.

**Check:** after each helper, `grep` shows one definition; unit tests; the game steps of the files you touched;
a full game-test at the end of the part.

## Part 3: finish the source tree split (a weekend)

`loader/src/` still has loose files beside its folders: `hooks.cpp` (948 lines), `gui.cpp`, `dllmain.cpp`,
`diag.cpp`, `log.cpp`, `config.cpp`, `theme.cpp`, `selftest.cpp`, and headers `failsafe.h`, `test_mode.h`,
`unload.h`, `font.h`, `paths.h`, `build_info.h`.

1. Write the target down first, in `docs/FILE-MAP.md`'s words, one owner per concept:
   `core/` infrastructure, `game/` engine access, `runtime/` the mod loader, `render/` drawing,
   `ui/` (new: `gui`, `theme`, `font`), `diag/` (new: `log`, `diag`, `failsafe`, with `crashhandler/` beside it),
   `selftest/` (new, part 4), and `dllmain.cpp`, `hooks.cpp`, `config.cpp`, `paths.h`, `build_info.h`, `unload.h`
   at the top or in `core/` as you decide.
2. Move one file at a time with the IDE (it rewrites the includes). If the build can't find it, its folder may need
   adding where `CMakeLists.txt` globs sources (around line 67) or in `cmake/Features.cmake`.
3. After each folder: `python tools/file_map.py` to regenerate `docs/FILE-MAP.md`, then commit with the move.

**Check:** `git log --stat` shows renames (`R`), so `git log --follow` keeps each file's history; build, unit tests,
a full game-test.

## Part 4: split the biggest files (a week of evenings)

| File | Lines | Split along |
|---|---|---|
| `loader/src/selftest.cpp` | 8,026 | `loader/src/selftest/`: the shared helpers (`Begin`, `End`, `Probe`, `WaitFor`, `WaitTicks`, `WaitPlayerReady`, `WaitGrounded`, `OnGameThread`, `OnRenderThread`, `GameSeconds`, `Lower`) in `selftest/common.h/.cpp`; one file per area (layers and levels, classes, Kismet, physics and pieces, AI and navigation, models, textures and content, UI and editors, TAS, modules); `FeatureList()` stays the one list, in `selftest.cpp` |
| `loader/src/runtime/mod_lua.cpp` | 3,057 | one file per `mirage.*` area (`lua_world.cpp`, `lua_draw.cpp`, `lua_kismet.cpp`, `lua_physics.cpp`, ...); the state setup and registration in `mod_lua.cpp` |
| `loader/src/runtime/mod_game.cpp` | 1,892 | the C ABI by `api.h` section (classes, layers, actors, physics, Kismet, movement) |
| `loader/src/runtime/mods.cpp` | 1,765 | discovery and load order; loading and unloading; the API table |

For each file:

1. Pick the smallest self-contained piece (one `api.h` section, one group of self-test steps).
2. Cut it into a new file with the IDE's Move, names and behaviour unchanged. What it needs from the rest goes into
   a small internal header (`mod_internal.h` already plays that role for the runtime), not a new abstraction.
3. Build, unit tests, the game steps of that piece (`--only`), commit. Next piece.

**Check:** no hand-written file in `loader/src` over about 1,500 lines (`find loader/src -name "*.cpp" | xargs wc -l
| sort -rn | head`); a full game-test at the end of each file.

## Part 5: simple improvements (pick any, one at a time)

Each is small, safe and checkable on its own.

1. **A checklist for adding an API function.** Today one function touches `api.h`, the table in `mods.cpp`, its
   implementation, `tests/test_abi.cpp`, the Lua binding (`mod_lua.cpp`), C# (`managed/Mirage`), and two
   generators (`tools/gen_csharp_api.py`, `tools/gen_api_docs.py`; forgetting the last fails
   `ApiReference.UpToDate`). Write the list into `docs/AI-GUIDE.md` (it helps people too), then try it by adding a
   throwaway function end to end and reverting it.
2. **Run the generators in the build.** Make `uv run mirage build` call both generators (in
   `tools/mirage_tools/cli.py`, before the CMake build), so their output can't go stale.
3. **One definition of each engine flag.** The same UE3 flags are defined in several files: `RF_ClassDefaultObject`
   (`0x200`) in `runtime/classes.cpp` and `runtime/layers.cpp`, `FUNC_Native` (`0x400`) in `runtime/fhooks.cpp` and
   `runtime/inject.cpp`, `RF_RootSet` (`0x4000`) in `runtime/classes.cpp`, `CPF_ReturnParm` in `runtime/inject.cpp`
   (find them with `grep -rn "constexpr .*= 0x" loader/src`). Put them in one header, `game/engine_flags.h`, named as
   the engine names them, with a comment saying where each was verified; replace the local copies one file per
   commit.
4. **No fixed addresses.** `CLAUDE.md`'s rule is "patterns, never fixed addresses", but `runtime/navigation.cpp`
   (the in-game path builder, step 28) uses three: `kScoutExec = 0x00ec1100`, `kGWarn = 0x020186f8`, and the vtable
   slot `0x44c`. Give the two addresses named patterns in `runtime/symbols.toml` (as the other engine functions have,
   resolved by `symbols::Resolve`), keep the vtable slot as a named constant with its comment, and run
   "Mirage path building" (opt-in: `--only "Mirage path building"`) to check it still builds the paths.
5. **The `shortcut:` comments.** `grep -rn "shortcut:" loader/src` lists known limits (today:
   `runtime/model_files.cpp`, a box body instead of a convex hull; `selftest.cpp`, a 25 s walk timeout). For each,
   do the upgrade or decide it stays, and keep the comment truthful.
6. **The one TODO.** `log.h` line 15 ("Maybe replace with uuid system"): decide and delete the TODO either way (an
   increasing 64-bit id never repeats in a session; a UUID buys nothing here).
7. **Unit tests for file-in, file-out code.** These need no game and are cheap to cover: layer files
   (`layers::Read` / `Write` round trip, an unknown key kept), manifest parsing (`runtime/manifest.cpp`: a bad id, a
   missing entry file, a dependency cycle), and the load order (`runtime/resolve`). Add them to `tests/` beside
   `test_manifest.cpp` and friends.
8. **Make the flaky steps steady.** For each known flaky step above, run it alone ten times (PowerShell:
   `1..10 | % { uv run mirage game-test --no-build --only "<step>" }`) and in the full run. Find what it
   waits on by time instead of state and make it wait on state (`WaitFor` a condition; `CLAUDE.md` forbids sleeps).
   For "Memory manager", measure the action in release before deciding between code and budget.
9. **Time the full game-test.** Note the seconds it reports (392 s on 2026-11-18, debug) in the baseline. A cleanup
   that makes it slower is a regression too.

## Order and size

| Part | Effort | Needs |
|---|---|---|
| 0 Clean start | an evening | - |
| 1 Delete what nothing uses | an evening | 0 |
| 2 One copy of each helper | a weekend | 0 |
| 3 Source tree split | a weekend | 2 (fewer files to touch) |
| 4 Split the big files | a week of evenings | 3 |
| 5 Simple improvements | any time, one at a time | 0 |

Parts 0, 1 and 2 are the best value per hour: less code, one place for each fix, nothing that can surprise a mod.
