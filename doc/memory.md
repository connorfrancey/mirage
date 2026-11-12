# Memory management (`src/runtime/mem/`)

The runtime's memory manager watches the process and the menu's own memory, finds leaks and abnormal patterns, and
gives memory back when the process runs short. It is autonomous but conservative: it only ever touches the menu's own
memory (caches, spare pool slabs, spare arena chunks) and tells mods to do the same; the game's memory and allocators
are never touched, hooked, walked or locked.

## Parts

| File | What | Thread |
|------|------|--------|
| `heap.h` | Counters of the menu's heap fed by `operator new`/`delete` (`runtime/memory.cpp`): live/peak, by size class, by tag; sampled allocator timing (1 call in 1024); large allocations caught with their stack | any (lock-free) |
| `sampler.h` | Sampling heap profiler and leak detector: one allocation per ~256 KB (Poisson by bytes, unbiased estimates) recorded with stack, tag, thread and time in a fixed table outside the heap | any |
| `analysis.h` | Pure reasoning (unit tested): trends, anomaly detection, address-space fragmentation, pressure with hysteresis, adaptive cache budget, per-level predictor | - |
| `process.h` | Process counters and an address-space walk (`VirtualQuery`, `GetProcessMemoryInfo`, `GlobalMemoryStatusEx`) | monitor |
| `pool.h` | Fixed-size block pools (`FixedPool`, `SyncPool`, `ObjectPool<T>`) with a registry, slab trimming and a heap fallback | owner / any |
| `arena.h` | Temporary memory (`Scratch` scopes on a per-thread arena, `ScratchAllocator<T>`) and long-lived memory (`PersistentAllocate`, `PersistentString`) | owner / any |
| `allocators.h` | `BuddyAllocator` (power-of-two blocks in one reservation, split and merged), `SmallAllocator` / `SmallObject<T>` (size classes up to 256 B over SyncPools), `SmallBuffer<N>` (inline, then heap), `StackAllocator` (LIFO, checked, markers) | locked / owner |
| `manager.h` | Orchestration: monitor thread, game-thread job, pressure handlers, report, leak checks | both |

## How it runs

- **Monitor thread** (below normal priority, joined at unload): every second a full address-space walk (~13,000
  regions, ~18 ms in a debug build), the process counters, anomaly detection on the menu's heap, large allocations,
  the pressure level, the predictor; every 10 s the profiler's biggest stacks (leak suspects: a stack whose live bytes
  grow in a straight line for five minutes).
- **Game-thread job** (`cache::Maintenance`, "Memory manager"): applies settings, reads each Lua mod's memory, and
  acts. A step with nothing to do is ~0.08 ms in debug; an action (every cache enforced, pools trimmed, mods told) up
  to ~10 ms in debug, at most once per level change and every 5 s while High or Critical.

## Pressure

| Level | Reached when (any of) | Response |
|-------|-----------------------|----------|
| Normal | - | Cache budget may grow (caches full, being trimmed and used), +12.5% per 30 s, up to the max setting |
| Elevated | largest free block < 256 MB, free address space < 512 MB, commit left < 1 GB, menu heap > 1.5x its budget, a falling largest-free trend that hits Critical within 2 min, or a level that went near High last time | Cache budget x0.75 per sample (not below the min) |
| High | largest < 128 MB, free < 256 MB, commit < 512 MB | Budget halved per sample (the eviction job trims over its next passes: no single long frame); pools and arenas trimmed; handlers and mods (`MIRAGE_EVENT_MEMORY_PRESSURE`) told, again every 5 s |
| Critical | largest < 48 MB, free < 96 MB, commit < 192 MB | Budget to the min and caches trimmed at once; otherwise as High. Taken at once (other levels need 2 samples) |

A lower level is taken only after 10 samples at it. After pressure, the cache budget returns to its earlier value in
steps. Observe-only mode (the Memory module switched off, or "Act on memory pressure" off) measures and reports
everything and changes nothing. An exception in the monitor or the job stops acting (logged); the game goes on.

## Using it

```cpp
// Tag allocations (shows in the report: allocated so far, live estimated)
static const auto kTag = mirage::mem::RegisterTag("My subsystem");
mirage::mem::TagScope tag(kTag);

// Temporary memory: freed at the end of the scope, no heap call per allocation
mirage::mem::Scratch scratch;
auto* ids = scratch.Array<int>(count);

// Pools for objects made and destroyed often
mirage::mem::SyncObjectPool<Node> nodes("Graph nodes");
Node* n = nodes.Create(...);  nodes.Destroy(n);

// Give memory back under pressure
mirage::mem::AddPressureHandler("My cache", 10, [](mirage::mem::Level level, bool rising) { return cache.Shrink(level); });

// A leak check of one operation (this thread)
mirage::mem::LeakCheck check;  DoWork();  auto leaked = check.Finish();   // leaked.sites: stacks still holding memory
```

Mods: `api->memory_stats(mod, &stats)` (pressure, process, address space, menu heap, the mod's Lua memory) and
`MIRAGE_EVENT_MEMORY_PRESSURE` (Lua: `mirage.on("memory_pressure", function(e) collectgarbage() end)`).

Diagnostics: the **Memory** module (Debug tab: settings, live status, full report, Mark / Log live since mark), the
console's `memory [report | stacks | trim | mark | since]`, and the log. What the manager learns about each level is
kept in `memenu\cache\memory_profiles.json`.

## Safety rules

- No allocation headers: blocks stay plain CRT memory whatever frees them.
- Nothing on the allocation path allocates or can fail an allocation; the profiler's table is reserved with
  `VirtualAlloc` (~3 MB) on first use and drops samples when full.
- The profiler never samples its own buffers and never waits on its own lock (`t_busy`).
- Only read-only kernel queries for the process; no heap walks or heap locks (the game's heaps share the process).
- The monitor thread is joined at unload, never detached.
- The memory it reserves comes from the menu's DLL, so TAS snapshots leave it out (`tas/memory_map.h`).
