# Refactor plan (by hand)

A plan for organizing and improving Mirage without an assistant: what to change, in what order, and how to know each
change broke nothing. Each phase ends in a state that builds, passes `uv run mirage test` and a full
`uv run mirage game-test`, and is committed on its own. Don't mix a refactor with a feature or a fix in one commit:
when the self-test turns red, the commit that did it should be small enough to read.

## Ground rules

- **One move at a time.** Move or split one file, build, test, commit. A rename and a behaviour change never share a
  commit.
- **The self-test is the safety net.** Unit tests can't prove game features. Before a phase, run the full
  `uv run mirage game-test` and keep its table (`<game>\Binaries\mirage\logs\mirage.log`) as the baseline; after the
  phase the same steps must pass with the same notes (counts, distances, timings within reason).
- **Use the IDE's refactorings**, not search and replace: CLion's Rename, Move, Extract Function and Change Signature
  update every caller. After a move, `uv run mirage errors` lists what broke, grouped by file.
- **Bisect, don't guess.** When a game step fails after several commits, `git bisect` with
  `uv run mirage game-test --only "<step>"` finds the commit.
- **Keep the frozen ABI frozen.** `loader/include/mirage/api.h` only grows at its end; `tests/test_abi.cpp` guards it.
  Internal names can change freely; anything a mod sees can't.

## Phase 0: a clean starting point (half a day)

1. Settle the working tree. `git status` holds files that are neither committed nor ignored: `AGENTS.md`,
   `docs/FEATURES.md`, `docs/TOOLS.md`, `loader/src/imgui/imgui-combo-filter.*`, `loader/src/util/parsing/csv2.hpp`,
   `loader/src/util/vk_util.h`, `loader/src/crashhandler/analyzer/4gb_patch.h`, `opencode.jsonc`, `.mcp.json`,
   `.myollamaenhancer/`. For each: commit it, delete it, or add it to `.gitignore`. Files nothing includes are
   deletions (check with a search for the header's name).
2. Fix the warnings in `loader/src/imgui/imgui-combo-filter.cpp` (C4706 assignment in a condition, C4996
   `strncpy`) or drop the file if nothing uses it, so "0 warnings in our code" means something again.
3. Push UnrealLib's `uc-compiler` branch (commit `10012b5`: auto states and the label table offset) and bump
   `UNREALLIB_GIT_TAG` in `cmake/Versions.cmake`, so release builds have the fix the debug build has; remove the
   `CPM_UnrealLib_SOURCE` line added to `cmake-build-x86-debug/CMakeCache.txt` when you no longer want the local
   checkout.
4. Decide what stays uncommitted on purpose (`cmake/Libraries.cmake`, `cmake/Versions.cmake` pins) and write that
   down in `CLAUDE.md`/`AGENTS.md`, so the next person doesn't commit them by accident.

**Check:** a clean `git status` (or only the files you chose to keep local), a release build
(`uv run mirage build -c release`), unit tests and a full game-test green.

## Phase 1: finish the source tree split (1-2 days)

The plan's step 1 left "the source tree split" undone: `loader/src/` still has loose files beside its folders
(`hooks.cpp` 948 lines, `gui.cpp`, `theme.cpp`, `log.cpp`, `diag.cpp`, `config.cpp`, `dllmain.cpp`, `selftest.cpp`).

1. Write the target layout first, in `docs/FILE-MAP.md`'s terms: which folder owns what (`core/` infrastructure,
   `game/` engine access, `runtime/` the mod loader, `render/`, `features/` the modules, `ui/` for gui and theme,
   `diag/` for log, diagnostics and the crash handler). One owner per concept.
2. Move one file at a time with the IDE (it rewrites the includes), add it to `cmake/Features.cmake` or the target's
   source list as the build tells you, build, commit.
3. Regenerate the file map (`python tools/file_map.py`) after each folder is done.

**Check:** `uv run mirage build`, unit tests, a full game-test; `git log --stat` shows renames (`R`), not
delete-and-add, so history follows the files (`git log --follow`).

## Phase 2: split the big files (1 week)

The biggest files mix several concerns, which makes every change touch a file thousands of lines long:

| File | Lines | Split along |
|---|---|---|
| `loader/src/selftest.cpp` | 7,665 | one file per area under `loader/src/selftest/` (layers, AI, Kismet, classes, physics, UI, ...), the shared helpers (`Begin`, `Probe`, `WaitFor`, `RestartAndWait`, `OnGameThread`) in `selftest/common.h`; `FeatureList()` stays the one list |
| `loader/src/runtime/mod_lua.cpp` | 3,057 | one file per `mirage.*` area (`lua_draw.cpp`, `lua_world.cpp`, `lua_kismet.cpp`, `lua_physics.cpp`, ...), the class and registration table in `mod_lua.cpp` |
| `loader/src/runtime/mod_game.cpp` | 1,892 | the C ABI by area, matching `api.h`'s sections (classes, layers, physics, actors, Kismet, movement) |
| `loader/src/runtime/mods.cpp` | 1,765 | discovery and load order, loading and unloading, the API table |

How, for each file:

1. Pick the smallest self-contained area (one `api.h` section, one group of self-test steps).
2. Extract it to a new file with Move, keeping names and behaviour; anything it needs from the rest becomes a
   declaration in a small internal header, not a new abstraction.
3. Build, run the unit tests and the game-test steps of that area (`--only "Mirage layers,..."`), commit.

**Check:** after each split, the full game-test once; no file over about 1,500 lines except generated ones.

## Phase 3: one source for each API entry (2-3 days)

Adding one function to the mod API today touches six places by hand: `api.h`, `mods.cpp`'s table, the
implementation's header, `tests/test_abi.cpp`, the Lua binding and C#'s `Mod.cs`, then two generators
(`tools/gen_csharp_api.py`, `tools/gen_api_docs.py`), and forgetting the last one fails `ApiReference.UpToDate`.

1. Write the checklist into `docs/AI-GUIDE.md` (or a `docs/adding-an-api-function.md`) so it is followed the same way
   every time.
2. Make `uv run mirage build` (or a pre-commit hook) run both generators, so their output can't go stale.
3. Optional, if the checklist keeps being missed: generate `test_abi.cpp`'s `MIRAGE_AT` lines from `api.h` the way
   the C# bindings are generated.

**Check:** add a throwaway function end to end following only the checklist, then revert it.

## Phase 4: the docs (2 days)

1. `docs/mirage-plan.md` is 190 KB and mixes the plan, status reports and investigation notes. Keep the plan and a
   short status per step in it; move each step's findings (how the game spawns bots, how states are found, the
   movement survey) into `docs/notes/<topic>.md`, linked from the step.
2. Fill or delete the stubs: `docs/FEATURES.md`, `docs/TOOLS.md`, `docs/LIBRARIES.md` (referenced by `CLAUDE.md`'s
   "measure first" rule), `docs/TODO.md`.
3. Make one place the entry point (the README) with links to getting started, mod authoring (`csharp-mods.md`,
   `mod-classes.md`, `api-reference.md`) and contributor docs.

**Check:** every link resolves (a markdown link checker, or a grep for `](` and a look at each).

## Phase 5: test health (ongoing)

1. Run the full game-test five times in a row in debug and in release; the plan's own milestone for the self-test is
   five green runs in each. Every flaky step gets a cause written down and fixed by waiting on state, never by adding
   a sleep (the rule in `CLAUDE.md`).
2. Unit-test what doesn't need the game: TOML layer reading and writing (`layers::Read`/`Write`), manifest parsing,
   load order, the class source cache key. Each is a function of files in, files out.
3. Time the full game-test and keep the number; a refactor that makes it slower is a regression too.

## Phase 6: smaller improvements (pick as time allows)

- Name the magic numbers found by reverse engineering (`0xA0` state flags, `0x2b2` NAME_Auto, factory offsets) in one
  header with a comment saying where each came from, if code uses them.
- Look for code that grew twice: the mod-class lookup by `<mod id>:` exists in `runtime/layers.cpp` and
  `runtime/mod_game.cpp` (`ActorSpawn`); one helper in `runtime/classes` would serve both.
- Review every `shortcut:` comment (`grep -rn "shortcut:" loader`) and either do the upgrade or delete the shortcut.

## Order and size

| Phase | Effort | Depends on |
|---|---|---|
| 0 Clean start | half a day | - |
| 1 Tree split | 1-2 days | 0 |
| 2 Big files | 1 week | 1 |
| 3 API checklist | 2-3 days | 2 (mod_game, mod_lua split first) |
| 4 Docs | 2 days | any time |
| 5 Test health | ongoing | 0 |
| 6 Small things | as time allows | 2 |

Phase 0 first, always; 4 and 5 can run beside the others.
