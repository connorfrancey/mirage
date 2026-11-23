# Unreal Engine 3 integration

- Start with [game.h](../loader/src/game/game.h) for accessors, loading callbacks, and game-thread dispatch. [levels.h](../loader/src/game/levels.h) owns level commands and selection.
- [symbols.toml](../loader/src/runtime/symbols.toml) is embedded by CMake. `symbols::Resolve` and `game::FindPattern` bridge engine addresses, while [core/hooking.h](../loader/src/core/hooking.h) wraps MirageHook. Check feature gates and signatures before hooking an engine function.
- [uobject.h](../loader/src/game/uobject.h) supplies raw layout-aware helpers. [object_query.h](../loader/src/game/object_query.h) and [object_cache.h](../loader/src/game/object_cache.h) supply class and path lookups. Treat object pointers and query results as scoped to the appropriate thread and lifecycle. Handle reuse is possible on restart. [Object references](../loader/src/game/object_ref.h)
- [reflect.h](../loader/src/runtime/reflect.h) resolves dotted property paths against reflected members. `mirage::call` builds parameter layouts and handles native-index functions and 4-byte UE3 UBOOLs. Follow the wrappers in [call.h](../loader/src/runtime/call.h) and [engine_calls.h](../loader/src/runtime/engine_calls.h). Player-position writes use the pawn's `Location` according to the established game-layer convention.
- Engine types live in the generated `Classes` namespace. Change `tools/sdkgen` and run `uv run mirage sdk` to regenerate when layouts or emitter behavior change. Check both runtime reflection and generated ABI checks. [SDK generation](../tools/sdkgen/main.cpp), [generated checks](../sdk/src/Checks.cpp)

Mod-level function hooks, injected bytecode, and class compilation are covered in [scripting](scripting.md).
