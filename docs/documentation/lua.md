# Lua API

Place `main.lua` next to `mod.toml`. The loader creates a Lua state for the mod and supplies the `mirage` table. Start with the [first mod tutorial](../getting-started.md) and use the [C ABI reference](../api-reference.md) for the underlying engine operations.

`on_tick` callbacks can access game objects and reflection. Resolve live proxies when you need them because level changes can invalidate them. Use `on_render` for drawing and ImGui operations. A Lua execution error, instruction-budget violation, or memory-limit failure is reported through the mod runtime.

The generated runtime SDK accessors are available in builds configured with `MIRAGE_BUILD_APIGEN=ON`. Optional metadata-only inspection pages describe UE3 declarations for editor use.
