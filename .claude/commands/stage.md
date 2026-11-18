---
description: Status of the current Mirage plan stage (step) from docs/mirage-plan.md, git and the last self-test
argument-hint: "[step number, default: the current one]"
allowed-tools: Bash(git log:*), Bash(git status:*), Bash(python:*), Read, Grep
---

Report the status of a stage of the Mirage plan. Read only; change nothing.

1. Pick the stage. If `$ARGUMENTS` is a number, that step. Otherwise the current one: in `docs/mirage-plan.md`, read
   "Order and dependencies" (the build order line and "The next ten steps") and take the first step in that order whose
   `**Status (...)**` paragraph doesn't say it is done (a step with no Status paragraph is not started).
2. From that step's `### N.` section give: its title and priority, its milestone in one line, and from its Status
   paragraph what is done and what is left (the "except ..." and "Next:" parts). Name hard dependencies that are not done.
3. Evidence from the repository:
   - `git log --oneline -15`: the commits for this step (their subjects start with `Step N` or mention it).
   - `git status --short`: uncommitted files that belong to this step's work (ignore the user's own staged files listed
     in the session's git status, such as cmake pins, AGENTS.md and .idea).
   - The last in-game self-test: `<game>\Binaries\mirage\selftest-result.json` (the game folder is
     `C:\Program Files (x86)\Steam\steamapps\common\mirrors edge`): steps passed of total, the failing steps with their
     problems, and the notes of steps named for this stage. Say when it ran (the file's time) and that it may predate
     the latest changes.
4. Answer in at most 15 lines: stage and state first (done / in progress / not started), then left to do as a short
   list, then evidence (commits, uncommitted work, self-test). End with the next concrete action.
