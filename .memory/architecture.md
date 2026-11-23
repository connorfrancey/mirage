# Runtime architecture

## Startup and shutdown

1. The proxy forwards DirectInput and starts Mirage once the game is ready. The launcher is a separate injector. The DLL claims a per-process instance. [Proxy](../loader/proxy/src/loader.cpp), [DLL entry](../loader/src/dllmain.cpp)
2. `Run` initializes clocks, themes, and built-in modules, installs bundled mods, discovers mods, and loads the Early phase. It applies defaults and saved config, initializes scheduling, then installs rendering and file hooks. [Startup sequence](../loader/src/dllmain.cpp)
3. Engine initialization resolves symbols, installs game-thread dispatch and level callbacks, starts the memory and hang services, and loads Engine-phase mods. The level callback loads World-phase mods once and refreshes level-scoped data. [Game lifecycle](../loader/src/dllmain.cpp), [engine bridge](../loader/src/game/game.h)
4. On unload, stop scheduled work and callbacks before unloading mod code. `UnloadAll` tears mods down in reverse load order. [Shutdown sequence](../loader/src/dllmain.cpp), [mod lifecycle](../loader/src/runtime/mods.cpp)

## Ownership and communication

| Owner | Work and handoff |
| --- | --- |
| Game thread | UE objects, reflection writes, engine input, ticks, mod gameplay callbacks. Queue work with `game::RunOnGameThread`. |
| D3D9 render thread | EndScene, ImGui, controls, render callbacks and device resources. Queue with `hooks::RunOnRenderThread`. Consume snapshots of world data. |
| Workers | File parsing, index maintenance, diagnostics, and other non-engine work. Dispatch results to the thread that owns the state. |

`events::Bus` routes lifecycle notifications. `tasks` owns workers and dispatchers. `sched` controls game-thread work and budgets. [Events](../loader/src/core/events.h), [tasks](../loader/src/core/tasks.h), [scheduler](../loader/src/core/scheduler.h)
