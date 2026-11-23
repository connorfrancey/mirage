# {{name}}

{{description}}

A Mirage mod for Mirror's Edge.

## Layout

| Path | What it holds |
|---|---|
| `mod.toml` | The manifest: id, version, the runtime versions it is for, settings, dependencies |
| `src/` | Code for Lua, C# and native mods |
| `content/` | Property overrides and replacement textures, meshes and sounds (`content/README.md`) |
| `models/`, `levels/`, `maps/`, `shaders/` | Made when needed: models to place, level additions, maps, post-process shaders |

## Working on it

```
uv run mirage mod check      # inspect mod.toml and referenced project files offline
uv run mirage mod build      # a native mod's DLL (Lua and C# script mods have nothing to build)
uv run mirage mod install    # into the game's mods folder; with hot_reload, a running game reloads it
uv run mirage mod test       # the game started with the mod, its own tests run
uv run mirage mod package    # a zip to share: dist/mods/{{id}}-<version>.zip
```

Run them in this folder (or give the folder as the last argument).
