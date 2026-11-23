# Development conventions

- Inspect implementations, caller contracts, feature flags, and tests first. Keep changes incremental and preserve existing behavior except where the task changes it.
- Keep UE objects on the game thread and ImGui controls on the render thread. Check `game::Pawn()`, `Controller()`, and `World()` at use sites because level transitions invalidate them. [Architecture](architecture.md)
- Resolve engine hooks by pattern through MirageHook. Use shared object queries and the engine-call wrappers. Generate SDK changes from `tools/sdkgen`. [Engine integration](unreal-engine.md)
- Keep `MirageApi` append-only. Update corresponding Lua and managed interfaces and verify ABI offsets. Feature modules belong in `cmake/Features.cmake`. [Scripting](scripting.md), [component map](modules.md)
- Make `OnDisable` undo module side effects. Treat unload, reload, failed setup, and a disappearing level as normal paths. Use existing transaction or lifecycle APIs when they fit. [Mod system](mod-system.md)
- Write comments and docs as plain prose describing what the code does and why. Reuse existing libraries. Measure and justify new dependencies.
- Work on `main` or a feature branch. Preserve staged and unstaged work, including local dependency pins. Commit verified milestones with scoped paths, using `git commit -- <paths>` when the whole files belong to the change. Push only when asked.
- Ask before destructive operations or changes to the user's game installation. Announce visible game launches. Review diagnostics and diffs and report what was actually run. [Build](build.md), [testing](testing.md)
