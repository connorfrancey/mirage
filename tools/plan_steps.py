"""Lists the steps of docs/mirage-plan.md by phase: number, title, priority and state. A step's state comes from its
bold `**Status (date): ...**` sentence: in its own `### N.` section, or, for the steps done through "The next ten
steps", in that list's item (mapped to the step by the table's "(step N" note). "done..." is done, any other status
is in progress, none is not started. Run from the repository root: `python tools/plan_steps.py` (`--open`: only the
steps not done). The /steps Claude command runs it."""
import re
import sys

PLAN = "docs/mirage-plan.md"
WIDTH = 100     # The status text is cut to this


def bold_status(lines: list[str], start: int) -> str:
    """The bold Status sentence that starts on lines[start], without its "Status (date): " prefix"""
    text = ""
    for line in lines[start:start + 6]:
        text += (" " if text else "") + line.strip()
        if text.count("**") >= 2:
            break
    return re.sub(r"^Status \([^)]*\):\s*", "", text.split("**")[1]).strip()


def parse(text: str):
    """Steps as [phase, number, title, priority, status] in the plan's order"""
    lines = text.splitlines()
    steps, phase, current = [], None, None
    next_ten = {}       # List item number -> plan step, from the table
    item = None         # The numbered list item being read, after the table
    for i, line in enumerate(lines):
        if line.startswith("## "):
            phase, current, item = line[3:].strip(), None, None
            continue
        m = re.match(r"### (\d+)\. (.*?)(?: \((P\d)\))?\s*$", line)
        if m:
            current = [phase, int(m.group(1)), m.group(2), m.group(3) or "", ""]
            steps.append(current)
            continue
        if line.startswith("### "):
            current = None
            continue
        m = re.match(r"\| (\d+) \| \*\*.*?\*\* \(step (\d+)", line)
        if m:
            next_ten[int(m.group(1))] = int(m.group(2))
            continue
        m = re.match(r"(\d+)\. \*\*", line)
        if m and next_ten:
            item = next_ten.get(int(m.group(1)))
            continue
        if line.lstrip().startswith("**Status"):
            status = bold_status(lines, i)
            if current and not current[4]:
                current[4] = status
            elif item is not None:
                for s in steps:
                    if s[1] == item and not s[4]:
                        s[4] = status
                item = None
    return steps


def state(status: str) -> str:
    if not status:
        return "not started"
    return "done" if status.lower().startswith("done") else "in progress"


def main() -> None:
    only_open = "--open" in sys.argv[1:]
    with open(PLAN, encoding="utf-8") as f:
        steps = parse(f.read())
    shown_phase = None
    counts = {"done": 0, "in progress": 0, "not started": 0}
    for phase, number, title, priority, status in steps:
        st = state(status)
        counts[st] += 1
        if only_open and st == "done":
            continue
        if phase != shown_phase:
            print(f"\n{phase}")
            shown_phase = phase
        note = status if len(status) <= WIDTH else status[:WIDTH - 3].rstrip() + "..."
        if st == "done" and note.lower() in ("done", "done."):
            note = ""
        print(f"  {number:>2}. {title}{f' ({priority})' if priority else ''}  [{st}]" + (f"  {note}" if note else ""))
    print(f"\n{counts['done']} done, {counts['in progress']} in progress, {counts['not started']} not started")


if __name__ == "__main__":
    main()
