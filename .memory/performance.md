# Performance and diagnostics

- `sched` budgets low-priority work and reports module tick and render timings. Keep work proportional to changes or scoped queries. [Scheduler](../loader/src/core/scheduler.h), [object querying](../loader/src/game/object_query.h)
- `mod_profile` records per-mod callback cost and exposes it in Mod Manager. `MIRAGE_BUILD_BENCHMARKS` enables the standalone `mirage_bench` target. Release benchmarks and real game timings answer different questions. Record stage, feature flags, preset, game state, and command with measurements. [Profiler](../loader/src/runtime/mod_profile.h), [root build](../CMakeLists.txt)
- Cache lifetimes and eviction scopes belong with the data owner. Budget against process fragmentation as well as byte totals. Keep archive and asset work within x86 memory limits. [Caches](../loader/src/core/cache.h), [memory](memory-management.md)
- Start troubleshooting with the game log and generated build ID. `diag` enumerates modules and flags hook interference. The crash reporter records stacks and minidumps, and the hang monitor publishes stall and recovery information. [Log implementation](../loader/src/log.cpp), [diagnostics](../loader/src/diag.h), [infrastructure guide](../docs/infrastructure.md)

Runtime budget and cache defaults can change with build options and settings. Read current code before publishing performance claims.
