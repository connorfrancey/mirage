# Runtime infrastructure

What the menu and the Mirage runtime build on, besides the memory manager (`docs/memory.md`). Each part says where it
lives, what thread it runs on, and how it fails safe.

| Part | Where | What it gives |
|------|-------|---------------|
| Crash reporter | `runtime/crash_reporter.h`, Debug → Crash Reporter | Fatal crashes handled by Mirage instead of the game's dialog: a report and a minidump in `mirage\crashlogs\`, the report opened once the game is gone, "the game crashed last time" at the next start |
| Hang detector | `runtime/hang.h` (same module) | Notices a game thread without ticks or a render thread without frames, says where it is stuck, writes a report, tells the player the failsafe key |
| Event bus | `core/events.h` | Typed events between parts that don't know each other; delivery now, on the game or render thread, or on a worker |
| Background tasks | `core/tasks.h` | A small pool of low-priority workers; results continued on the game or render thread; cancellation |
| Resource cache | `core/resource_cache.h` | Files read once and shared; kept under the cache budget; same contents shared (XXH3) |
| Config cache | `runtime/config_cache.h` | Mirage's TOML files parsed once, re-read when they change on disk, typed lookups by dotted key |
| Weak cache | `core/weak_cache.h` | Values found while someone holds them, never kept alive by the cache |
| Adaptive cache | `core/adaptive_cache.h` | ARC: balances recency and frequency on its own; a scan of one-off keys can't flush the hot set |
| Reflection path cache | `runtime/reflect.h` | Dotted paths parsed once; each segment's member found through a one-entry inline cache |
| Adaptive update scheduler | `core/scheduler.h` | Under load, Low/Background entries are stretched by their own cost: expensive ones give way first |
| Allocators | `runtime/mem/allocators.h`, `pool.h`, `arena.h` | Buddy, small-object (size classes), small buffer, stack, slab pools, arenas |
| Hashing | `core/hash.h` | XXH3 (xxHash, header only) for keys and content fingerprints |

Already there before and reused, not duplicated: the cache budget manager (`core/cache_evict.h`, now driven by the
memory manager's adaptive budget), the cache scheduler (`core/cache_sched.h`), the crash logger (`diag.h`: first-chance
faults in the menu and mods), the failsafe key (`failsafe.h`), and the allocation tracker (`runtime/mem/heap.h`,
`sampler.h`).

## Crash reporter

How it takes over, without reverse engineering the game: UE3's crash handler calls `dbghelp!MiniDumpWriteDump` (the
game imports it) before its dialog; that call is hooked, and with an exception record from this process it is the
moment the game decided the crash is fatal. An unhandled-exception filter covers crashes outside the game's guard, and
`SetUnhandledExceptionFilter` is hooked so the game's own filter can't replace it (it is kept, and called in "the
game's own" mode).

In the dying process the report is written by a thread made at startup and parked: fixed buffers and Win32 calls for
the essentials (what, where, whose code, registers, the frame-pointer stack, code addresses on the stack), written to
disk first; then the module list, the mods loaded and the memory status, each guarded and skipped if it fails; then a
minidump (from that thread, as dbghelp advises). At most 20 s, then the process ends whatever happened. Crashes in a
mod's guarded call are not fatal and never come here: the runtime stops the mod.

Files: `crash-<time>.miragecrashlog` (plain text) and `.dmp` for fatal crashes (shown at the next start),
`report-<time>.miragecrashlog` for reports that didn't end the game (hangs, the self-test). At the next start the
reports are zstd-compressed into `crashlogs\.old\<name>.miragecrashlog.zst` (like `logs\.old`), the newest 100 kept;
the menu lists and shows archived ones too. Minidumps stay uncompressed (compressing tens of MB would hold up the start).

## Hang detector

A watchdog thread checks every 250 ms. A thread is hung after 10 s (settings) without progress, except while a level
loads, a TAS session runs (it freezes threads itself), a debugger is attached, or the game is minimized (and, for the
render thread, while another window is in front). It suspends the stuck thread once, for microseconds, to copy its
registers and stack into fixed buffers (nothing allocated or logged meanwhile: it may hold the heap lock), resumes it,
then reports. It never kills or changes the game. Events: `events::HangDetected`, `events::HangRecovered`.

## Event bus

`events::Bus().Subscribe<T>(fn, {priority, where, owner})` returns a `Subscription` (unsubscribes when destroyed);
`Publish(T{...})`. Immediate handlers run inside Publish by priority; game/render/worker handlers get a copy queued
through the dispatchers dllmain sets. A throwing handler is logged and skipped. The menu's events: `LevelLoad`,
`MemoryPressure`, `HangDetected`, `HangRecovered`, `ModReloaded`.

## Background tasks

`tasks::Run(fn, {priority, name})` returns a `Task<T>`: `Wait`, `Ready`, `Result`, `Cancel`, `Then(fn, On::GameThread)`.
Workers: cores - 1, at most 3, below normal priority. A task that throws is Failed with its message. `Stop()` at
unload drops the queue, asks running tasks to stop (`tasks::Cancelled()`), and joins the workers. Without `Start()`
(tests, tools) tasks run inline.

## Measured

- Path cache, debug build, in the game: 1.94 us -> 1.55 us per resolve (1.3x) on five typical paths. The rest is the
  debug build's container checks; release not measured yet.
- XXH3 against FNV-1a: `uv run mirage bench` (bench/bench_hash.cpp).
- Adaptive scheduler: per-entry stretch is a pure function (`sched::AdaptiveScale`, unit tested); entries keep the
  old caps (Low 8x, Background 64x), so nothing runs less often than before at full pressure.
