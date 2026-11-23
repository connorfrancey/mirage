# Memory management (`loader/src/runtime/mem/`)

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

## Memory Profiler

Open the Mirage menu and enable **Debug → Memory Profiler**. Set the capture interval from 1 to 60 seconds. Closing the window or turning off the module discards the capture session. Hiding the Mirage menu leaves capture running.

| View | What to check |
|------|---------------|
| Overview | Compare free virtual address space and the largest free block. The aligned span excludes any unusable prefix before the next Windows allocation boundary. Reserved VA occupies addresses. Committed VA includes committed private, mapped, and image pages. Windows reports private bytes separately from the resident working set, which measures physical pages in memory. |
| Address Space Map | Scroll over the map to zoom around the pointer, drag to pan, and click to select an address. Filter by state, backing type, and minimum size. Yellow marks free spans under 1 MiB. Bright shades mark occupied regions of at least 16 MiB. Zoom in to see regions smaller than a screen bin. The tooltip queries the captured region at the pointer's exact address. |
| Memory Timeline | Follow committed VA, reserved VA, free VA, and the largest free block over the most recent 240 complete captures. Plots use independent scales and equal spacing per sample. The overview's VA peaks cover the current capture session. |
| Region Inspector | Search by address, state, backing type, protection, or module, and click a column header to sort. Addresses refer to the last displayed capture and region ends are exclusive. |
| Diagnostics | Review the pressure level, free-region size distribution, and recent manager findings. Fragmentation is `1 − largest free block / total free space`. The aligned span, requested alignment, and Windows commit headroom all affect whether a reservation or commit can succeed. Growth findings warrant investigation and can also reflect level loads or caching. |

The window is read-only. `VirtualQuery` metadata is collected during the memory manager's existing low-priority walk, so capture cannot run faster than the manager's cadence. The process can change while Windows queries run. Incomplete walks, missing process counters, and stale captures are flagged. The capture retains up to 32,768 regions (1 MiB of region records on x86). Walk totals cover all queried regions even when the map and inspector reach that cap. The inspector indexes one capture at a time on an existing worker and clips rendering to visible rows. Disabling the window releases capture and history storage. The pressure manager continues on its own cadence.

Module attribution covers loaded image mappings and separately reported Lua heaps. Private allocations made by native mods or the game generally have no identifiable owner. The failure count covers failed requests through Mirage's counted `operator new` path. UE3, direct Windows API calls, aligned allocation, and external allocators have separate failure paths.

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
kept in `mirage\cache\memory_profiles.json`.

## Safety rules

- No allocation headers: blocks stay plain CRT memory whatever frees them.
- Nothing on the allocation path allocates or can fail an allocation; the profiler's table is reserved with
  `VirtualAlloc` (~3 MB) on first use and drops samples when full.
- The profiler never samples its own buffers and never waits on its own lock (`t_busy`).
- Only read-only kernel queries for the process; no heap walks or heap locks (the game's heaps share the process).
- The monitor thread is joined at unload, never detached.
- The memory it reserves comes from the menu's DLL, so TAS snapshots leave it out (`tas/memory_map.h`).
