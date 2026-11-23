
from __future__ import annotations

import re
import shlex

from dataclasses import dataclass
from difflib import SequenceMatcher
from typing import Callable, Iterable, Literal

from .. import sdkbrowse as sb


SearchMode = Literal["types", "members"]

OPERATORS = {
    "kind",
    "extends",
    "member",
    "function",
    "file",
    "flag",
    "offset",
    "type",
    "name",
    "path",
}

KIND_ALIASES = {
    "class": {"class"},
    "struct": {"struct", "scriptstruct"},
    "enum": {"enum"},
}

OFFSET_RE = re.compile(r"^(?:0x)?[0-9a-f]+$", re.IGNORECASE)


@dataclass(frozen=True, slots=True)
class SearchTerm:
    value: str
    operator: str | None = None
    negated: bool = False


@dataclass(frozen=True, slots=True)
class SearchQuery:
    terms: tuple[SearchTerm, ...]
    valid: bool = True
    error: str | None = None

    @property
    def empty(self) -> bool:
        return not self.terms


@dataclass(frozen=True, slots=True)
class SearchResult:
    type: sb.Type
    member: sb.Field | str | None
    name: str
    description: str
    score: float


@dataclass(slots=True)
class _IndexedType:
    type: sb.Type
    cpp: str
    name: str
    path: str
    file: str
    kind: str
    super: str
    fields: list[sb.Field]
    functions: list[str]
    field_names: list[str]
    function_names: list[str]


class SearchParser:
    """Parse advanced search queries into structured terms."""

    @staticmethod
    def parse(query: str) -> SearchQuery:
        if not query.strip():
            return SearchQuery(())

        try:
            tokens = shlex.split(query)
        except ValueError as exc:
            return SearchQuery((), valid=False, error=str(exc))

        terms = []

        for token in tokens:
            negated = token.startswith("-")

            if negated:
                token = token[1:]

            if not token:
                continue

            operator = None
            value = token

            if ":" in token:
                possible_operator, value = token.split(":", 1)

                if possible_operator.lower() in OPERATORS:
                    operator = possible_operator.lower()

                else:
                    value = token

            if not value:
                return SearchQuery(
                    (),
                    valid=False,
                    error=f"Missing value for {operator}:",
                )

            terms.append(
                SearchTerm(
                    value=value,
                    operator=operator,
                    negated=negated,
                )
            )

        return SearchQuery(tuple(terms))


class SearchIndex:
    """
    Indexed search over generated SDK types and members.

    Supports:
        kind:class
        extends:AActor
        member:Health
        function:Tick
        file:TdPawn
        flag:Native
        offset:0x120
        type:FVector
        name:TdPawn
        path:TdGame
        -kind:enum
    """

    def __init__(
        self,
        types: Iterable[sb.Type],
        function_name: Callable[[str], str] = sb.function_name,
        fuzzy_threshold: float = 0.72,
    ) -> None:
        self.types: list[_IndexedType] = []
        self.by_cpp: dict[str, _IndexedType] = {}
        self.fuzzy_threshold = fuzzy_threshold

        for t in types:
            fields = list(t.fields)
            functions = list(t.functions)

            indexed = _IndexedType(
                type=t,
                cpp=t.cpp,
                name=t.name,
                path=t.path,
                file=t.file,
                kind=t.kind.lower(),
                super=t.super or "",
                fields=fields,
                functions=functions,
                field_names=[f.name for f in fields],
                function_names=[
                    function_name(fn) for fn in functions
                ],
            )

            self.types.append(indexed)
            self.by_cpp[t.cpp] = indexed

        self.children: dict[str, set[str]] = {}

        for indexed in self.types:
            if indexed.super:
                self.children.setdefault(
                    indexed.super, set()
                ).add(indexed.cpp)

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def search(
        self,
        query: str,
        mode: SearchMode = "types",
        kinds: set[str] | None = None,
        limit: int = 2000,
    ) -> list[SearchResult]:
        parsed = SearchParser.parse(query)

        if not parsed.valid:
            return []

        if parsed.empty and mode == "members":
            return []

        results = []

        for indexed in self.types:
            if kinds and indexed.type.kind not in kinds:
                continue

            if mode == "types":
                result = self._search_type(indexed, parsed)

                if result:
                    results.append(result)

            elif mode == "members":
                results.extend(
                    self._search_members(indexed, parsed)
                )

            else:
                raise ValueError(f"Unknown search mode: {mode}")

        results.sort(
            key=lambda result: (
                -result.score,
                result.type.cpp.lower(),
                result.name.lower(),
            )
        )

        return results[:limit]

    # ------------------------------------------------------------------
    # Type search
    # ------------------------------------------------------------------

    def _search_type(
        self,
        indexed: _IndexedType,
        query: SearchQuery,
    ) -> SearchResult | None:
        score = 0.0

        for term in query.terms:
            matched, term_score = self._match_type_term(
                indexed, term
            )

            if term.negated:
                if matched:
                    return None
                continue

            if not matched:
                return None

            score += term_score

        return SearchResult(
            type=indexed.type,
            member=None,
            name=indexed.cpp,
            description=indexed.path,
            score=score,
        )

    def _match_type_term(
        self,
        indexed: _IndexedType,
        term: SearchTerm,
    ) -> tuple[bool, float]:
        value = term.value.lower()
        operator = term.operator

        if operator == "kind":
            expected = KIND_ALIASES.get(value, {value})
            return indexed.kind in expected, 100.0

        if operator == "extends":
            matched = self._contains(
                indexed.super, value
            )
            return matched, 80.0 if matched else 0.0

        if operator == "file":
            matched = self._contains(indexed.file, value)
            return matched, 90.0 if matched else 0.0

        if operator == "name":
            return self._score(
                value, [indexed.name, indexed.cpp]
            )

        if operator == "path":
            matched, score = self._score(
                value, [indexed.path]
            )
            return matched, score

        if operator == "type":
            return self._score(
                value,
                [
                    f.type
                    for f in indexed.fields
                ],
            )

        if operator == "member":
            return self._score(
                value, indexed.field_names
            )

        if operator == "function":
            return self._score(
                value, indexed.function_names
            )

        if operator == "flag":
            return self._score(
                value,
                [
                    f.flags
                    for f in indexed.fields
                ],
            )

        if operator == "offset":
            return self._match_offset(indexed, value)

        if operator is not None:
            return False, 0.0

        # Unqualified search: type name, C++ name, path,
        # filename, and members.
        return self._score(
            value,
            [
                indexed.cpp,
                indexed.name,
                indexed.path,
                indexed.file,
                *indexed.field_names,
                *indexed.function_names,
            ],
        )

    # ------------------------------------------------------------------
    # Member search
    # ------------------------------------------------------------------

    def _search_members(
        self,
        indexed: _IndexedType,
        query: SearchQuery,
    ) -> list[SearchResult]:
        results = []

        
        members = [
            (field, field.name, field.type, field.flags, field.offset, False)
            for field in indexed.fields
        ]

        members.extend(
            (fn, indexed.function_names[i], str(fn), "", None, True)
            for i, fn in enumerate(indexed.functions)
        )

        for member, name, type_name, flags, offset, is_function in members:
            score = 0.0
            matched_all = True

            for term in query.terms:
                matched, term_score = self._match_member_term(
                    indexed,
                    name,
                    type_name,
                    flags,
                    offset,
                    term,
                    is_function,
                )

                if term.negated:
                    if matched:
                        matched_all = False
                        break
                    continue

                if not matched:
                    matched_all = False
                    break

                score += term_score

            if not matched_all:
                continue

            description = (
                f"0x{member.offset:04X}  {member.type}"
                if isinstance(member, sb.Field)
                else str(member)
            )

            results.append(
                SearchResult(
                    type=indexed.type,
                    member=member,
                    name=name,
                    description=description,
                    score=score,
                )
            )

        return results

    def _match_member_term(
        self,
        indexed: _IndexedType,
        name: str,
        type_name: str,
        flags: str,
        offset: int | None,
        term: SearchTerm,
        is_function: bool,
    ) -> tuple[bool, float]:
        value = term.value.lower()
        operator = term.operator

        if operator == "kind":
            expected = KIND_ALIASES.get(value, {value})
            return indexed.kind in expected, 100.0

        if operator == "extends":
            matched = self._contains(indexed.super, value)
            return matched, 80.0 if matched else 0.0

        if operator == "file":
            matched = self._contains(indexed.file, value)
            return matched, 90.0 if matched else 0.0

        if operator == "member":
            matched, score = self._score(value, [name])
            return matched, score

        if operator == "function":
            matched, score = self._score(
                value, [name] if is_function else []
            )
            return matched, score

        if operator == "type":
            matched, score = self._score(value, [type_name])
            return matched, score

        if operator == "flag":
            matched, score = self._score(value, [flags])
            return matched, score

        if operator == "offset":
            if offset is None:
                return False, 0.0

            try:
                expected = int(value, 16)
            except ValueError:
                return False, 0.0

            return offset == expected, 100.0

        if operator == "name":
            return self._score(value, [name])

        if operator == "path":
            return self._score(value, [indexed.path])

        if operator is not None:
            return False, 0.0

        return self._score(
            value,
            [
                name,
                type_name,
                flags,
                indexed.cpp,
                indexed.path,
            ],
        )

    # ------------------------------------------------------------------
    # Matching utilities
    # ------------------------------------------------------------------

    @staticmethod
    def _contains(candidate: str, query: str) -> bool:
        return query in candidate.lower()

    def _score(
        self,
        query: str,
        candidates: Iterable[str],
    ) -> tuple[bool, float]:
        best = 0.0
        matched = False

        for candidate in candidates:
            candidate = str(candidate).lower()

            if not candidate:
                continue

            if candidate == query:
                score = 120.0

            elif candidate.startswith(query):
                score = 105.0

            elif query in candidate:
                score = 90.0

            else:
                ratio = SequenceMatcher(
                    None, query, candidate
                ).ratio()

                if ratio < self.fuzzy_threshold:
                    continue

                score = 50.0 + ratio * 30.0

            matched = True
            best = max(best, score)

        return matched, best

    @staticmethod
    def _match_offset(
        indexed: _IndexedType,
        value: str,
    ) -> tuple[bool, float]:
        try:
            expected = int(value, 16)
        except ValueError:
            return False, 0.0

        matched = any(
            field.offset == expected
            for field in indexed.fields
        )

        return matched, 100.0 if matched else 0.0
