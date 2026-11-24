# Verification

| Scope | Command or source |
| --- | --- |
| C++ components | `uv run mirage test`; filter with `-R`. CTest's configured options determine the set. [Test targets](../tests/CMakeLists.txt) |
| Python authoring and tooling | `uv run python -m unittest discover -s tools/mirage_tools/tests`. [Python tests](../tools/mirage_tools/tests) |
| Actual game behavior | `uv run mirage game-test --no-build --only "Step A,Step B"` after a fresh build. `FeatureList()` defines exact step names. [Self-test](../loader/src/selftest.cpp) |
| A mod's own runtime checks | `mirage mod test <folder>` uses the game harness and the mod's registered tests. Lua uses `mirage.test`. [Author CLI](../tools/mirage_tools/modtools.py) |
| Graphics-only harness | The optional `mirage_testhost` exercises D3D9 without game UObjects. The staged `verbose-build` workflow uses it. [Harness](../tools/testhost/src/main.cpp) |

Get authorization for the game install before launching the self-test and announce the visible game window. The harness deploys test mods, opens the game non-elevated, and closes it. Left Alt plus Right Alt minimizes it to the taskbar, and clicking restores it. Use `--minimized` if requested. Read `Binaries/mirage/logs/mirage.log` and step results for verification. [Harness](../tools/mirage_tools/gametest.py)

On Windows x86, optimized MSVC builds of `mirage_tests` keep frame pointers, matching the loader so the allocation sampler can capture and assert on stack frames. Set `PYTHONIOENCODING=utf-8` before running tests in a redirected Windows shell so it can print test diagnostics. [Test target](../tests/CMakeLists.txt)

Wait for state transitions with `WaitFor` and `WaitPlayerReady`. Avoid deliberate stalls and assertions that time or gravity alone could satisfy. Test packages copied by [example_mods.py](../tools/mirage_tools/example_mods.py) come from the player's installation. Keep shipped game data outside commits.
