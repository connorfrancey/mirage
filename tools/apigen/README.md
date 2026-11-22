# MirageAPIGen

MirageAPIGen consumes the JSON layout interchange format shared by the in-game
layout dump and SDK generator. It runs with Python 3.12 or later and uses Click
for the command line. The parser, analyzer, renderers and CMake entry point
use the standard library.

## Commands

```powershell
uv run mirage api generate --all
uv run mirage api generate --target luals --inspection --package TdGame
uv run mirage api inspect TdGame.TdPawn
uv run mirage api validate
uv run mirage api coverage --json
uv run mirage api diff old-api-manifest.json new-api-manifest.json --strict
uv run mirage api clean --dry-run
```

The SDK tool can export merged metadata from the installed game's packages
without replacing headers or launching the game.

```powershell
uv run mirage sdk --metadata build/sdk-metadata.json
uv run mirage api generate --inspection
uv run mirage sdk-browse ALAudio.ALAudioDevice --gui
```

The default input is `build/sdk-metadata.json` when present. Otherwise it
falls back to the checked-in `tools/sdkgen/engine-layouts.json`, a supplemental
dump that covers native layouts and contains a subset of the SDK. Use
`--input` to select any compatible JSON file. The examples for `TdGame.TdPawn`
require the merged metadata. The SDK browser continues to read generated C++
headers for its original views and adds API coverage and links from the API
manifest when one is available. Pass `--api-output` to the GUI for a custom
output directory.

The same commands are available through `uv run mirage-apigen`. For a standalone
checkout invocation, add `tools` to `PYTHONPATH` and run
`python -m mirage_apigen` with Click installed. Neither invocation imports the GUI.

## Layout schema

The input is an array of records keyed by fully qualified `path`. Class and
struct records can carry `super`, `size` and `properties`. A property has a
`name` and `kind`, with optional `offset`, element `size`, `flags`, Boolean
`mask`, static array `dim`, referenced `type`, dynamic array `inner` and
`innerType`. An object descriptor for `inner` can describe nested arrays.
Enum records can supply `values` as a name-to-integer map or an array of
names or name/value objects.

The existing dump can include top-level `Function` records. Their
`properties` use the same parameter flags as the SDK generator. Optional
extensions can supply explicit `parameters` with `direction` (`in`, `out`,
`inout` or `return`) and `optional`, a separate `return` descriptor,
`owner`, `nativeIndex`, `invocation` and `verified`. Function records may
also appear in a class's `functions` array. These fields describe metadata.
Runtime support requires an independently reviewed binding.

## Exposure and runtime support

`config/api.toml` explicitly permits two reviewed, read-only
`ALAudio.ALAudioDevice` Float getters. All other members are denied by default.
An allowlist narrows the set of eligible members, and a denylist excludes
matches. Every runtime member needs its own `expose = true` rule and a
matching reviewed contract in `analysis.py`. A member rule can also specify
aliases, a public name, deprecation, experimental opt-in, a binding identifier
and a description. Setting `writable = true` also requires a reviewed write
contract. Engine access stays on the game thread.

Enable the bundled runtime registrations at build time.

```powershell
uv run mirage configure -D MIRAGE_BUILD_APIGEN=ON -D MIRAGE_BUILD_TESTS=ON
uv run mirage build
uv run mirage test --no-build -R MirageAPIGen.Bindings
```

`mirage.sdk.get_vorbis_game_threshold(object)` and
`mirage.sdk.get_vorbis_editor_threshold(object)` take a live Mirage object
proxy. They return a number on success and nil if the object is stale or its
class or reflected declaration fails validation. Call them on the game thread,
for example from a tick callback. CMake builds the bundled policy and links its
package registration units into the existing Lua function registry. Python
runs at build time only. For a custom policy, generate the C++ units and add
them as loader sources in place of the bundled units.

The generator emits runtime accessors from source-reviewed contracts. The
runtime checks a live handle, the fully qualified owning class, the matching
reflected declaration and the scalar kind. It obtains values through
`MirageApi::object_get`. The adapter uses `object_set` for future reviewed
scalar writes. JSON offsets and flags never select a runtime memory address.

LuaLS and Markdown share the runtime manifest and its public names. Pass
`--inspection` to add separate `MirageSDK.*` annotations and SDK metadata pages
for class and struct fields, enums and known function signatures. Types outside
a filtered inspection view use their Lua category where their detailed
declaration is outside the selection. `config/inspection.toml` gives examples
of editor-only mappings for Unreal vectors, rotators, colors, names and strings.
Use `editor_fallback = "omit"`, `"unknown"` or `"any"` for unresolved editor annotations.

To extend runtime support, implement and test a typed adapter using the
existing Lua and reflection interfaces and add a reviewed contract. For a
UFunction this includes receiver and function identity, game-thread and
lifetime checks, parameter layout and direction verification, and Lua
marshalling for results and out parameters. Register the reviewed wrapper in
`analysis.py` and declare it alongside the other adapters in `lua_functions.h`.
No generic C++ invocation wrapper is registered. Existing `call::Params`
and `call::Call` are the place to start when adding one for a known function.

## Generated files and verification

The generator normalizes UE3 records into a package/type index, analyzes
inheritance and references, projects reviewed members into a public API, then
generates package registration units, LuaLS definitions, documentation and
reports. The default output is `build/api`. The output includes
`api-manifest.json`, `coverage.json`, `diagnostics.json` and
`generation.json`. Coverage distinguishes declared SDK metadata from reviewed
runtime exposure. Diff compares two public manifests or two SDK JSON arrays.
Only public manifest comparisons describe script compatibility.

The generation manifest records hashes for configuration, SDK types,
dependencies and generated files. Rendered files are written atomically when
their contents change. Generation removes obsolete tracked files and preserves
other files in the output directory. `--dry-run` previews changes, `--clean`
forces a fresh render, and `--force` allows removal of modified obsolete
generated files. Use `validate --json` for diagnostics and stale file status in
automation. `--strict` promotes warnings to command failures. The supplemental
dump has expected warnings for references omitted from the input.
`--verbose` shows phase timings and diagnostics. `generate --profile` prints
Python profiling statistics.

Run the Python suite from the checkout. The CMake test calls the compiled
generated callbacks in a fresh Lua state using the real registration interface
and a game-free adapter.

```powershell
$env:PYTHONPATH = "tools"
uv run python -m unittest discover -s tools/apigen/tests
```

The in-game smoke test is opt-in. The following command installs the test mod
and starts Mirror's Edge using the last build.

```powershell
uv run mirage mod test tools/apigen/tests/game
```
