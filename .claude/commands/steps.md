---
description: List the Mirage plan's steps by phase with their state (done, in progress, not started)
argument-hint: "[--open: only the steps not done]"
allowed-tools: Bash(python tools/plan_steps.py:*)
---

List the steps of the Mirage plan. Read only; change nothing.

Run `python tools/plan_steps.py $ARGUMENTS` from the repository root and show its output as it is, in a code block (it
reads `docs/mirage-plan.md`: each step's bold Status sentence, or its item in "The next ten steps"). Add nothing but one
line after it naming the step in progress with the lowest number, if any.
