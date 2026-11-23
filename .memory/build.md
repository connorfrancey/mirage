# Build and diagnostics

Run the `mirage` CLI using `uv` from the checkout. It finds Visual Studio and prepares the x86 MSVC tools before calling CMake and Ninja. Use this entry point for native builds to preserve the configured compiler and architecture.

```powershell
uv run mirage configure
uv run mirage build
uv run mirage build -c release
uv run mirage errors
uv run mirage features
uv run mirage test -R Manifest
uv run mirage install-game
```

- The root configure requires CMake 4.2 or newer. `CMakePresets.json` selects preset-specific build directories. Windows debug uses `cmake-build-x86-debug`; the normal DLL is also copied to `build/Mirage.dll`. Minimal builds have their own presets and output. Follow the root build includes for the current output policy. [Root build](../CMakeLists.txt), [presets](../CMakePresets.json)
- The feature stage and `--enable`, `--disable`, or `--only` switches affect the current cache. `MIRAGE_GAME` selects engine integration. `MIRAGE_UNREALLIB` gates the UnrealLib test/status targets. The game build links UnrealLib through the dependency block. [Features](../cmake/Features.cmake), [dependencies](../cmake/Libraries.cmake)
- A workstation can select a local UnrealLib via `-D CPM_UnrealLib_SOURCE=...`. Inspect the active cache first. Local `Libraries.cmake` or `Versions.cmake` edits can belong to the user. [Toolchain](../tools/mirage_tools/toolchain.py), [dependency pins](../cmake/Versions.cmake)
- On a failed build, inspect `mirage-build.log` or `mirage-configure.log` in that preset's directory. Ninja setup errors appear in those logs even when the CLI counts zero compiler errors. [Build driver](../tools/mirage_tools/build.py)

Use [testing](testing.md) for permission and verification rules. Use [tooling](tooling.md) for generation and docs commands.

Mirage and legacy memenu install to the same `dinput8.dll` path in the game. The last installation determines which proxy the game loads.
