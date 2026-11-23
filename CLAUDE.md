# CLAUDE.md

Mirage is a C++ mod loader and development toolkit for Mirror's Edge (2008, Windows x86, UE3).

Start at [.memory/INDEX.md](.memory/INDEX.md). Read the notes relevant to the task and verify them against current source, build definitions, and tests before editing. Update affected notes after meaningful changes and record lasting tradeoffs in the decision log.

- Build and test through `uv run mirage`. Never run a bare `cmake --build`. Get permission for game installation changes and announce the launch before running `mirage game-test`.
- Keep UE objects and engine mutations on the game thread and ImGui controls on the render thread. Preserve the append-only `MirageApi` ABI and regenerate the SDK through `tools/sdkgen`.
- Preserve user changes and staging. Use scoped commits for verified milestones and push only when asked. Ask before destructive operations.

[Development conventions](.memory/conventions.md) · [Build and verification](.memory/build.md) · [Mod authoring](docs/mod-authoring-ai.md)
