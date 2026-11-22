"""Expose the C ABI's existing comments to Doxygen without editing the header."""

from __future__ import annotations

import re
import sys
from pathlib import Path

TOKENS = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)


def document(text: str) -> str:
    def convert(match: re.Match[str]) -> str:
        token = match[0]
        if not token.startswith("/*") or token.startswith(("/**", "/*!")):
            return token
        # Doxygen recognizes nested block comments, including a slash-star in
        # globs such as content/*.toml inside the C header's prose.
        body = token[2:-2].replace("/*", "/&#42;")
        token = "/*" + body + "*/"
        if body.lstrip().startswith("----"):
            return token
        token = "/*" + body.replace("\\", "\\\\").replace("<", "&lt;").replace(">", "&gt;") + "*/"
        start = text.rfind("\n", 0, match.start()) + 1
        if match.start() == 0 and token.count("\n") >= 2:
            _, _, rest = token.split("\n", 2)
            return "/** @file\n * @ingroup mirage_c_api\n * @brief Native Mirage mod ABI. " + rest.removeprefix(" * ")
        if text[start:match.start()].strip():
            return "/**<" + token[2:]
        # Attach leading prose to a declaration rather than to section dividers.
        following = text[match.end():].lstrip()
        if following.startswith(("#", "typedef ", "struct ", "enum ")) or re.match(
            r"[\w *]+\(MIRAGE_CALL\s*\*", following
        ):
            return "/**" + token[2:]
        return token

    return TOKENS.sub(convert, text)


if __name__ == "__main__":
    text = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
    sys.stdout.buffer.write(document(text).encode("utf-8"))
