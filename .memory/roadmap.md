# Implementation status

Use [mirage-plan.md](../docs/mirage-plan.md) for milestone numbers and acceptance criteria. The active worktree also has restructuring notes in `docs/refactor-plan.md`. Match status claims to current sources and relevant tests. Some timeline notes describe Mirror's Edge Menu or earlier stages.

| Status | Area |
| --- | --- |
| Implemented baseline | Proxy, injected loader, dependency resolution, stable ABI, Lua/managed mods, compiled SDK, mod manager, editor surfaces, runtime diagnostics, data layers and asset replacements. |
| In progress | Step 35 has scaffolds, offline checks, packaging, and docs. The gameplay walkthrough and determinism lint remain open. A TOML declaration split and raw package CLI appear in the active worktree. [Session](sessions/2026-11-22-baseline.md) |
| Partial | The game-input monitor and injection service are present. Full legacy replay/TAS restoration, the Speedrun Timer, and Input Overlay were outside the restored menu milestone. Sound-wave replacement is implemented with package-level regression coverage. |
| Planned or separately scoped | Proposed internal library split, MUMS distribution envelope and update service, and player-drivable vehicles. Check the corresponding roadmap sections before starting work. |

Update this file only when a status boundary changes. Keep dates, exact commands, test counts, and temporary issues in [session notes](sessions/README.md).
