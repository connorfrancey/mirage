"""Audit the public declarations reported by Doxygen XML."""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path
from xml.etree import ElementTree as ET


def location(node: ET.Element) -> str:
    loc = node.find("location")
    if loc is None:
        return ""
    path = (loc.get("declfile") or loc.get("file", "")).replace("\\", "/")
    match = re.search(r"(?:^|/)(loader/(?:include/mirage|src)/.*\.(?:h|hpp))$", path)
    return match[1] if match else ""


def documented(node: ET.Element) -> bool:
    return any(any(part.strip() for part in block.itertext()) for name in ("briefdescription", "detaileddescription")
               if (block := node.find(name)) is not None)


def audit(xml_dir: Path) -> dict[str, dict[str, bool]]:
    index = ET.parse(xml_dir / "index.xml").getroot()
    records: dict[str, dict[str, bool]] = defaultdict(dict)

    def add(node: ET.Element, name: str, kind: str) -> None:
        path = location(node)
        if not path or not name or name.startswith("@") or "::detail::" in name or name.startswith("detail::"):
            return
        # The signature distinguishes overloads. Whitespace and source line numbers
        # may change independently of the documented declaration.
        signature = re.sub(r"\s+", "", node.findtext("argsstring", "")) if kind == "function" else ""
        symbol = f"{kind} {name}{signature}"
        records[path][symbol] = records[path].get(symbol, False) or documented(node)

    for entry in index.findall("compound"):
        kind = entry.get("kind", "")
        if kind not in {"class", "struct", "union", "concept", "file", "namespace", "group"}:
            continue
        compound = ET.parse(xml_dir / f"{entry.attrib['refid']}.xml").find("compounddef")
        if compound is None:
            continue
        parent = compound.findtext("compoundname", "")
        if kind in {"class", "struct", "union", "concept"} and compound.get("prot", "public") == "public":
            add(compound, parent, "type")
        for member in compound.findall("./sectiondef/memberdef"):
            if member.get("prot", "public") != "public":
                continue
            member_kind = member.get("kind", "")
            name = member.findtext("name", "")
            qualified = member.findtext("qualifiedname")
            if qualified is None and kind in {"class", "struct", "union", "namespace"}:
                qualified = f"{parent}::{name}"
            if kind != "group":
                add(member, qualified or name, member_kind)
    if not records.get("loader/include/mirage/api.h", {}).get("type MirageApi"):
        raise ValueError("Doxygen XML is missing the documented MirageApi table. Check the native ABI input filter.")
    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xml", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    try:
        records = audit(args.xml)
        baseline = json.loads(args.baseline.read_text(encoding="utf-8")) if args.baseline else {}
        if not isinstance(baseline, dict) or any(
            not isinstance(symbols, list) or any(not isinstance(symbol, str) for symbol in symbols)
            for symbols in baseline.values()
        ):
            raise ValueError("Expected a map of header paths to undocumented symbols in the baseline")
    except (OSError, ET.ParseError, ValueError) as exc:
        parser.exit(1, f"Documentation coverage failed: {exc}\n")

    headers = {}
    failures = []
    for path, members in sorted(records.items()):
        missing = sorted(symbol for symbol, covered in members.items() if not covered)
        new = sorted(set(missing) - set(baseline.get(path, ())))
        headers[path] = {"documented": len(members) - len(missing), "total": len(members), "undocumented": missing,
                         "new_undocumented": new}
        failures.extend(f"{path}: {symbol}" for symbol in new)
    summary = {key: sum(row[key] for row in headers.values()) for key in ("documented", "total")}
    report = {"summary": summary, "headers": headers}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    print(f"Documented {summary['documented']} of {summary['total']} declarations. Report: {args.output}")
    if args.baseline and failures:
        print("Public declarations without documentation:")
        print("\n".join(f"  {failure}" for failure in failures))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
