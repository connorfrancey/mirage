# Maintaining project memory

[INDEX.md](INDEX.md) is the entry point for Codex, Claude Code, and contributors. These versioned notes are navigation aids. Code and build definitions describe current behavior. Tests describe the behavior they exercise. Specs describe format or API contracts. Roadmaps and session notes serve as historical and planning evidence.

## Workflow

1. Read the index and pick only the files needed for the task.
2. Verify the linked symbols, compile flags, call sites, and tests in the current checkout. Check the Git diff when a file is in progress.
3. Implement and verify the requested change through its normal entry point.
4. Update the owning memory note when a boundary, constraint, or source location changes. Link to deeper docs and delete obsolete facts after verifying their replacements.
5. Record durable tradeoffs under [decisions](decisions/README.md). Write a short [session note](sessions/README.md) when a result, blocker, or handoff would save the next agent time.

Keep machine-specific observations and one-off test results in session notes. Mark proposed work and worktree-only code explicitly. Record unresolved discrepancies with both sources. Keep credentials, personal logs, crash dumps, game packages, and generated source dumps out of memory.

The initial baseline was inspected on 2026-11-22 at committed revision `98da129` plus a dirty worktree. See the [baseline session](sessions/2026-11-22-baseline.md) for worktree qualifications.
