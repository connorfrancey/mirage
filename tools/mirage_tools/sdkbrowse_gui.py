"""The SDK browser's window (`mirage sdk-browse --gui`): the generated SDK's types (sdkbrowse.load) in a PyQt6 window.

On the left, the types or the members (fields and functions) matching the search, narrowed by kind. On the right, the
type inspector for the selected type: a header (kind, size, header file and line, inheritance breadcrumbs, member
statistics) above tabs that only appear when they have something to show:

    Overview, Fields, Functions, Values (enums), Inheritance, References, Memory Layout, Source

A type named in a field, a function or a relationship opens on a double click or a link; Back and Forward (Alt+Left /
Alt+Right) walk the types seen.
"""

from __future__ import annotations

import dataclasses
import html
import re
import sys
from functools import lru_cache
from pathlib import Path
from typing import Callable

from PyQt6.QtCore import QAbstractTableModel, QModelIndex, QRect, QSortFilterProxyModel, Qt, QUrl, pyqtSignal
from PyQt6.QtGui import (QAction, QBrush, QColor, QDesktopServices, QFont, QKeySequence, QPainter, QPalette, QPen,
                         QSyntaxHighlighter, QTextCharFormat, QTextCursor, QTextFormat)
from PyQt6.QtWidgets import (QAbstractItemView, QApplication, QCheckBox, QComboBox, QHBoxLayout, QHeaderView, QLabel,
                             QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QMenu, QPlainTextEdit, QPushButton,
                             QSplitter, QTableView, QTabWidget, QTextBrowser, QTextEdit, QToolTip, QTreeWidget,
                             QTreeWidgetItem, QVBoxLayout, QWidget)

from . import sdkbrowse as sb

from .core.search import SearchIndex, SearchParser, SearchResult

MAX_ROWS = 2000     # Results shown at once (members of every type can run to thousands)
KINDS = {"All kinds": None, "Classes": {"Class"}, "Structs": {"ScriptStruct", "Struct"}, "Enums": {"Enum"}}
FIELD_KINDS = ["All fields", "Pointers", "Containers", "SDK types", "Primitives"]
KIND_COLORS = {"Class": "#3d7ab8", "ScriptStruct": "#3e9a63", "Struct": "#3e9a63", "Enum": "#c0802a"}
TITLES = {"overview": "Overview", "fields": "Fields", "functions": "Functions", "values": "Values",
          "inheritance": "Inheritance", "references": "References", "layout": "Memory Layout", "source": "Source"}
SORT_ROLE = Qt.ItemDataRole.UserRole + 1    # What a column sorts by (an int for offsets and sizes, not the text)

_IDENT = re.compile(r"[A-Za-z_]\w*")
_LEADING = re.compile(r"(static|virtual|inline|extern|friend|constexpr|explicit)\b\s*")
_TRAILING = ("const", "override", "final", "noexcept")
_BUILTIN = {"int", "long", "char", "short", "float", "double", "unsigned", "signed", "bool", "void", "__int64"}
_PARAM = re.compile(r"^(?P<type>.*[\s*&>])(?P<name>[A-Za-z_]\w*)\s*(?P<array>\[[^\]]*\])?$")


# ---- Reusable table widgets ---------------------------------------------------------------------------------------


@dataclasses.dataclass
class Cell:
    text: str
    key: object = None              # What the column sorts by; the lowercase text when None
    tip: str = ""
    link: bool = False              # Drawn in the link colour: the cell names an SDK type
    fg: QColor | None = None
    bg: QColor | None = None
    bold: bool = False


class TableModel(QAbstractTableModel):
    """Rows of `Cell`s, each row with a payload (any object) that the views and menus read back."""

    def __init__(self, headers: list[str], parent=None) -> None:
        super().__init__(parent)
        self.headers = headers
        self.rows: list[list[Cell]] = []
        self.payloads: list[object] = []
        self.link_color = QColor()
        self.bold_font = QFont()

    def set_rows(self, rows: list[list[Cell]], payloads: list[object] | None = None) -> None:
        self.beginResetModel()
        self.rows = rows
        self.payloads = payloads if payloads is not None else [None] * len(rows)
        self.endResetModel()

    def rowCount(self, parent=QModelIndex()) -> int:
        return 0 if parent.isValid() else len(self.rows)

    def columnCount(self, parent=QModelIndex()) -> int:
        return 0 if parent.isValid() else len(self.headers)

    def headerData(self, section, orientation, role=Qt.ItemDataRole.DisplayRole):
        if orientation == Qt.Orientation.Horizontal and role == Qt.ItemDataRole.DisplayRole:
            return self.headers[section]
        return None

    def data(self, index, role=Qt.ItemDataRole.DisplayRole):
        if not index.isValid():
            return None
        cell = self.rows[index.row()][index.column()]
        if role == Qt.ItemDataRole.DisplayRole:
            return cell.text
        if role == SORT_ROLE:
            return cell.key if cell.key is not None else cell.text.lower()
        if role == Qt.ItemDataRole.ToolTipRole:
            return cell.tip or None
        if role == Qt.ItemDataRole.ForegroundRole:
            return QBrush(self.link_color) if cell.link else (QBrush(cell.fg) if cell.fg else None)
        if role == Qt.ItemDataRole.BackgroundRole:
            return QBrush(cell.bg) if cell.bg else None
        if role == Qt.ItemDataRole.FontRole and cell.bold:
            return self.bold_font
        return None


class DataTable(QTableView):
    """A QTableView over a TableModel and a sorting proxy, configured like every table of the inspector.

    `sort_column`: None for a table that is not sortable, otherwise the column it starts sorted by (-1 for none).
    `menu_builder(menu, index)` fills the context menu of the row under the cursor.
    """

    def __init__(self, headers: list[str], font: QFont, sort_column: int | None = None) -> None:
        super().__init__()
        self.source = TableModel(headers, self)
        self.proxy = QSortFilterProxyModel(self)
        self.proxy.setSourceModel(self.source)
        self.proxy.setSortRole(SORT_ROLE)
        self.setModel(self.proxy)
        self.menu_builder: Callable[[QMenu, QModelIndex], None] | None = None
        self.sortable = sort_column is not None

        self.setFont(font)
        bold = QFont(font)
        bold.setBold(True)
        self.source.bold_font = bold
        self.source.link_color = self.palette().color(QPalette.ColorRole.Link)
        self.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.verticalHeader().setVisible(False)
        self.verticalHeader().setDefaultSectionSize(20)
        self.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Interactive)
        self.horizontalHeader().setStretchLastSection(True)
        if sort_column is not None:
            self.setSortingEnabled(True)
            self.sortByColumn(sort_column, Qt.SortOrder.AscendingOrder)

    def set_rows(self, rows: list[list[Cell]], payloads: list[object] | None = None) -> None:
        self.clearSpans()
        self.source.set_rows(rows, payloads)

    def set_sorting(self, enabled: bool) -> None:
        """Turns header sorting on or off; off, the rows stay in the order they were given (grouped rows)."""
        if not self.sortable or self.isSortingEnabled() == enabled:
            return
        self.setSortingEnabled(enabled)
        if not enabled:
            self.proxy.sort(-1)

    def resize_columns(self, limit: int = 520) -> None:
        self.resizeColumnsToContents()
        for column in range(self.proxy.columnCount()):
            if self.columnWidth(column) > limit:
                self.setColumnWidth(column, limit)

    def payload(self, index: QModelIndex) -> object | None:
        if not index.isValid():
            return None
        row = self.proxy.mapToSource(index).row()
        return self.source.payloads[row] if 0 <= row < len(self.source.payloads) else None

    def current_payload(self) -> object | None:
        return self.payload(self.currentIndex())

    def select_payload(self, predicate: Callable[[object], bool]) -> bool:
        for row, payload in enumerate(self.source.payloads):
            if payload is not None and predicate(payload):
                index = self.proxy.mapFromSource(self.source.index(row, 0))
                self.setCurrentIndex(index)
                self.scrollTo(index, QAbstractItemView.ScrollHint.PositionAtCenter)
                return True
        return False

    def row_text(self, index: QModelIndex) -> str:
        return "\t".join(self.proxy.index(index.row(), c).data() or "" for c in range(self.proxy.columnCount())
                         if not self.isColumnHidden(c))

    def contextMenuEvent(self, event) -> None:
        index = self.indexAt(event.pos())
        if not index.isValid() or self.menu_builder is None:
            return
        self.setCurrentIndex(index)
        menu = QMenu(self)
        self.menu_builder(menu, index)
        if not menu.isEmpty():
            menu.exec(event.globalPos())


class DetailView(QTextBrowser):
    """Read-only rich text whose links are not followed but emitted: a type name, or "tab:<page>"."""
    navigate = pyqtSignal(str)

    def __init__(self) -> None:
        super().__init__()
        self.setOpenLinks(False)
        self.setOpenExternalLinks(False)
        self.anchorClicked.connect(lambda url: self.navigate.emit(url.toString()))


class Section(QWidget):
    """A bold caption above a widget."""

    def __init__(self, title: str, body: QWidget) -> None:
        super().__init__()
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(2)
        self.label = QLabel(title)
        font = self.label.font()
        font.setBold(True)
        self.label.setFont(font)
        layout.addWidget(self.label)
        layout.addWidget(body)

    def set_title(self, text: str) -> None:
        self.label.setText(text)


# ---- What the inspector reads out of the SDK ---------------------------------------------------------------------


@dataclasses.dataclass
class Param:
    type: str
    name: str = ""
    default: str = ""


@dataclasses.dataclass
class FuncInfo:
    """A function declaration split into its parts (best effort: the declaration stays the source of truth)."""
    name: str
    decl: str
    ret: str = ""
    params: list[Param] = dataclasses.field(default_factory=list)
    flags: list[str] = dataclasses.field(default_factory=list)

    @property
    def params_text(self) -> str:
        return ", ".join(p.type + (f" {p.name}" if p.name else "") for p in self.params)


@dataclasses.dataclass
class FieldRef:
    field: sb.Field
    owner: sb.Type


@dataclasses.dataclass
class GroupRef:
    owner: sb.Type


@dataclasses.dataclass
class LayoutRow:
    """A byte range of the object: a field, a hole (owner "") or the inherited region of a collapsed base."""
    start: int
    end: int
    owner: str
    field: sb.Field | None = None
    overlap: bool = False

    @property
    def size(self) -> int:
        return self.end - self.start


def _split_top(text: str) -> list[str]:
    """Splits on the commas that are not inside <>, () or []."""
    parts, depth, current = [], 0, []
    for ch in text:
        if ch in "<([":
            depth += 1
        elif ch in ">)]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(current))
            current = []
        else:
            current.append(ch)
    parts.append("".join(current))
    return [p.strip() for p in parts if p.strip()]


def parse_function(decl: str) -> FuncInfo:
    """Return type, parameters and flags (static, virtual, const...) of a C++ declaration."""
    name = sb.function_name(decl)
    info = FuncInfo(name=name, decl=decl)
    text = " ".join(decl.split()).rstrip(";").strip()
    match = re.search(rf"\b{re.escape(name)}\s*\(", text) if name else None
    if match:
        start = match.start()
    else:
        paren = text.find("(")
        tail = re.search(r"[~A-Za-z_][\w:]*\s*$", text[:paren]) if paren >= 0 else None
        start = tail.start() if tail else paren
    open_paren = text.find("(", max(start, 0))
    if start < 0 or open_paren < 0:
        return info
    depth, close = 0, -1
    for i in range(open_paren, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                close = i
                break
    if close < 0:
        return info

    head = re.sub(r"(?:\w+::)+$", "", text[:start].strip()).strip()
    while (m := _LEADING.match(head)):
        info.flags.append(m.group(1))
        head = head[m.end():]
    info.ret = head.strip()

    for raw in _split_top(text[open_paren + 1:close]):
        if raw == "void":
            continue
        raw, _, default = raw.partition("=")
        raw, default = raw.strip(), default.strip()
        m = _PARAM.match(raw)
        if m and m.group("name") not in _BUILTIN:
            info.params.append(Param(m.group("type").strip() + (m.group("array") or ""), m.group("name"), default))
        else:
            info.params.append(Param(raw, "", default))

    tail = text[close + 1:]
    info.flags.extend(word for word in re.findall(r"\w+", tail) if word in _TRAILING)
    if re.search(r"=\s*0\b", tail):
        info.flags.append("pure")
    return info


def build_layout(ancestry: list[sb.Type], inherited: bool) -> tuple[list[LayoutRow], int]:
    """The byte ranges of an object, in offset order, with the holes between its fields.

    `ancestry` runs from the root type to the inspected type. Without `inherited` the base's bytes collapse to one row.
    """
    t = ancestry[-1]
    owners = ancestry if inherited else [t]
    fields = sorted(((f, b.cpp) for b in owners for f in b.fields), key=lambda e: (e[0].offset, e[0].size))
    rows: list[LayoutRow] = []
    bit_words: dict[tuple[str, int], int] = {}
    cursor = 0
    if not inherited and len(ancestry) > 1 and ancestry[-2].size:
        base = ancestry[-2]
        rows.append(LayoutRow(0, base.size, base.cpp))
        cursor = base.size
    for f, owner in fields:
        if f.offset > cursor:
            rows.append(LayoutRow(cursor, f.offset, ""))
        overlap = f.offset < cursor
        if f.bit_mask is not None:
            key = (owner, f.offset)
            used = bit_words.get(key, 0)
            if used:
                overlap = bool(used & f.bit_mask)
            bit_words[key] = used | f.bit_mask
        rows.append(LayoutRow(f.offset, f.offset + f.size, owner, f, overlap=overlap))
        cursor = max(cursor, f.offset + f.size)
    total = max(t.size or 0, cursor)
    if total > cursor:
        rows.append(LayoutRow(cursor, total, ""))
    return rows, total


@lru_cache(maxsize=None)
def header_path(file: str) -> Path | None:
    """The generated header on disk, wherever sb.SDK_DIR points relative to `sdk/include/sdk/`."""
    if not file:
        return None
    base = Path(sb.SDK_DIR)
    for candidate in (base / file, base / "include" / "sdk" / file, base / "sdk" / file):
        if candidate.is_file():
            return candidate
    return None


# ---- Inspector widgets ---------------------------------------------------------------------------------------------


class TypeHeader(QWidget):
    """Kind, names, size, header location, inheritance breadcrumbs and member statistics of the inspected type.

    Emits `navigate` with a type name or "tab:<page>" when a link is clicked.
    """
    navigate = pyqtSignal(str)

    def __init__(self) -> None:
        super().__init__()
        layout = QVBoxLayout(self)
        layout.setContentsMargins(2, 2, 2, 2)
        layout.setSpacing(3)
        self.title = self._label(layout)
        self.location = self._label(layout)
        self.chain = self._label(layout)
        self.stats = self._label(layout)

    def _label(self, layout: QVBoxLayout) -> QLabel:
        label = QLabel(textFormat=Qt.TextFormat.RichText)
        label.setWordWrap(True)
        label.setTextInteractionFlags(Qt.TextInteractionFlag.TextBrowserInteraction)
        label.linkActivated.connect(self.navigate)
        layout.addWidget(label)
        return label

    def show_empty(self) -> None:
        self.title.setText("<i>Pick a type on the left</i>")
        for label in (self.location, self.chain, self.stats):
            label.setVisible(False)

    def show_type(self, t: sb.Type, ancestry: list[sb.Type], stats: list[tuple[str, str]], link: Callable[[str], str],
                  muted: str, has_source: bool) -> None:
        esc = html.escape
        badge = (f"<span style='background-color:{KIND_COLORS.get(t.kind, '#777777')}; color:white;'>"
                 f"&nbsp;{esc(t.kind)}&nbsp;</span>")
        self.title.setText(f"{badge} <b style='font-size:15px'>{esc(t.path)}</b> &nbsp;<code>{esc(t.cpp)}</code>")
        size = f"0x{t.size:04X} ({t.size}) bytes" if t.size is not None else "size unknown"
        where = esc(f"sdk/include/sdk/{t.file}:{t.line}")
        where = f"<a href='tab:source'>{where}</a>" if has_source else f"<span style='color:{muted}'>{where}</span>"
        self.location.setText(f"{size} &nbsp;·&nbsp; {where}")
        if len(ancestry) > 1:
            trail = " › ".join(link(b.cpp) for b in ancestry[:-1])
            self.chain.setText(f"<span style='color:{muted}'>Inherits</span> {trail} › <b>{esc(t.cpp)}</b>")
        stat_text = " &nbsp;·&nbsp; ".join(f"<a href='tab:{tab}'>{esc(text)}</a>" for text, tab in stats)
        self.stats.setText(stat_text or f"<span style='color:{muted}'>No members</span>")
        self.location.setVisible(True)
        self.chain.setVisible(len(ancestry) > 1)
        self.stats.setVisible(True)


class LayoutStrip(QWidget):
    """The whole object as one bar: a segment per field (coloured by the type that declares it), hatched holes."""
    segmentClicked = pyqtSignal(object)

    def __init__(self) -> None:
        super().__init__()
        self.setFixedHeight(46)
        self.setMouseTracking(True)
        self.segments: list[tuple[LayoutRow, QColor | None, str]] = []
        self.total = 0
        self.selected: LayoutRow | None = None

    def set_layout(self, segments: list[tuple[LayoutRow, QColor | None, str]], total: int) -> None:
        self.segments, self.total, self.selected = segments, total, None
        self.update()

    def set_selected(self, row: object) -> None:
        self.selected = row if isinstance(row, LayoutRow) else None
        self.update()

    def _rect(self, row: LayoutRow) -> QRect:
        x0 = int(row.start * self.width() / self.total)
        x1 = max(x0 + 1, int(row.end * self.width() / self.total))
        return QRect(x0, 2, x1 - x0, 24)

    def paintEvent(self, _event) -> None:
        if not self.total:
            return
        painter = QPainter(self)
        palette = self.palette()
        for row, color, _tip in self.segments:
            rect = self._rect(row)
            if color is None:
                painter.fillRect(rect, QBrush(palette.color(QPalette.ColorRole.Mid), Qt.BrushStyle.BDiagPattern))
            else:
                painter.fillRect(rect, color)
            painter.setPen(palette.color(QPalette.ColorRole.Window))
            painter.drawLine(rect.left(), rect.top(), rect.left(), rect.bottom())
            if row.overlap:
                painter.setPen(QPen(QColor("#d04040"), 2))
                painter.setBrush(Qt.BrushStyle.NoBrush)
                painter.drawRect(rect.adjusted(1, 1, -1, -1))
        if self.selected is not None:
            painter.setPen(QPen(palette.color(QPalette.ColorRole.Highlight), 3))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawRect(self._rect(self.selected).adjusted(1, 1, -1, -1))
        painter.setPen(palette.color(QPalette.ColorRole.Text))
        painter.drawText(QRect(0, 30, self.width(), 14), Qt.AlignmentFlag.AlignLeft, "0x0000")
        painter.drawText(QRect(0, 30, self.width(), 14), Qt.AlignmentFlag.AlignRight, f"0x{self.total:X}")

    def _segment_at(self, x: float) -> tuple[LayoutRow, QColor | None, str] | None:
        if not self.total or self.width() <= 0:
            return None
        offset = x * self.total / self.width()
        for segment in self.segments:
            if segment[0].start <= offset < max(segment[0].end, segment[0].start + 1):
                return segment
        return None

    def mouseMoveEvent(self, event) -> None:
        segment = self._segment_at(event.position().x())
        if segment:
            QToolTip.showText(event.globalPosition().toPoint(), segment[2], self)
        else:
            QToolTip.hideText()

    def mousePressEvent(self, event) -> None:
        segment = self._segment_at(event.position().x())
        if segment:
            self.segmentClicked.emit(segment[0])


class CppHighlighter(QSyntaxHighlighter):
    """Just enough C++ colouring for the generated headers: keywords, numbers, strings, comments, preprocessor."""

    def __init__(self, document) -> None:
        super().__init__(document)

        def fmt(color: str, italic: bool = False) -> QTextCharFormat:
            f = QTextCharFormat()
            f.setForeground(QColor(color))
            f.setFontItalic(italic)
            return f

        keywords = ("class|struct|enum|union|namespace|typedef|template|typename|public|private|protected|virtual|"
                    "static|const|inline|extern|operator|return|if|else|for|while|switch|case|break|using|void|bool|"
                    "int|float|double|char|short|long|unsigned|signed")
        self.comment = fmt("#6a9955", italic=True)
        self.rules = [
            (re.compile(rf"\b(?:{keywords})\b"), fmt("#4a86c5")),
            (re.compile(r"\b(?:0x[0-9A-Fa-f]+|\d+(?:\.\d+)?f?)\b"), fmt("#8aa05a")),
            (re.compile(r'"(?:[^"\\]|\\.)*"'), fmt("#c8825a")),
            (re.compile(r"^\s*#.*"), fmt("#a070b0")),
            (re.compile(r"//.*"), self.comment),
        ]

    def highlightBlock(self, text: str) -> None:
        for pattern, fmt in self.rules:
            for m in pattern.finditer(text):
                self.setFormat(m.start(), m.end() - m.start(), fmt)
        self.setCurrentBlockState(0)
        in_comment = self.previousBlockState() == 1
        start = 0 if in_comment else text.find("/*")
        while start >= 0:
            end = text.find("*/", start if in_comment else start + 2)
            if end < 0:
                self.setCurrentBlockState(1)
                length = len(text) - start
            else:
                length = end + 2 - start
            self.setFormat(start, length, self.comment)
            in_comment = False
            start = text.find("/*", start + length) if end >= 0 else -1


class SourceView(QWidget):
    """A generated header in a read-only editor, scrolled to a line and with that line marked."""

    def __init__(self, font: QFont) -> None:
        super().__init__()
        self.path: Path | None = None
        self.line = 1
        self.path_label = QLabel()
        self.path_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.position = QLabel()
        self.find = QLineEdit(placeholderText="Find in header (Enter)")
        self.find.setClearButtonEnabled(True)
        goto = QPushButton("Go to type")
        open_file = QPushButton("Open file")
        self.editor = QPlainTextEdit()
        self.editor.setReadOnly(True)
        self.editor.setFont(font)
        self.editor.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        self.highlighter = CppHighlighter(self.editor.document())

        top = QHBoxLayout()
        top.addWidget(self.path_label, 1)
        top.addWidget(self.position)
        top.addWidget(self.find, 1)
        top.addWidget(goto)
        top.addWidget(open_file)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addLayout(top)
        layout.addWidget(self.editor)

        goto.clicked.connect(self.go_to_line)
        open_file.clicked.connect(self._open_file)
        self.find.returnPressed.connect(self._find)
        self.editor.cursorPositionChanged.connect(self._cursor_moved)

    def show_location(self, path: Path, line: int) -> None:
        if path != self.path:
            try:
                self.editor.setPlainText(path.read_text(encoding="utf-8", errors="replace"))
            except OSError as error:
                self.editor.setPlainText(f"Cannot read {path}: {error}")
            self.path = path
        self.line = max(1, line)
        self.path_label.setText(f"{path}:{self.line}")
        self.go_to_line()

    def go_to_line(self) -> None:
        block = self.editor.document().findBlockByNumber(self.line - 1)
        cursor = QTextCursor(block)
        self.editor.setTextCursor(cursor)
        self.editor.centerCursor()
        fmt = QTextCharFormat()
        tint = QColor(self.palette().color(QPalette.ColorRole.Highlight))
        tint.setAlpha(70)
        fmt.setBackground(tint)
        fmt.setProperty(QTextFormat.Property.FullWidthSelection, True)
        selection = QTextEdit.ExtraSelection()
        selection.format = fmt
        selection.cursor = cursor
        self.editor.setExtraSelections([selection])

    def _find(self) -> None:
        text = self.find.text()
        if text and not self.editor.find(text):
            self.editor.moveCursor(QTextCursor.MoveOperation.Start)
            self.editor.find(text)

    def _open_file(self) -> None:
        if self.path:
            QDesktopServices.openUrl(QUrl.fromLocalFile(str(self.path)))

    def _cursor_moved(self) -> None:
        cursor = self.editor.textCursor()
        self.position.setText(f"Ln {cursor.blockNumber() + 1}, Col {cursor.columnNumber() + 1}")


# ---- The window ----------------------------------------------------------------------------------------------------


class SdkBrowser(QMainWindow):
    def __init__(self, types: list[sb.Type], api_output: Path | None = None) -> None:
        super().__init__()
        from mirage_apigen import DEFAULT_OUTPUT
        from mirage_apigen.browser import BrowserMetadata

        self.api_metadata = BrowserMetadata(api_output or DEFAULT_OUTPUT)
        self.types = sorted(types, key=lambda t: t.cpp.lower())
        self.search_index = SearchIndex(self.types)
        self.by_cpp = {t.cpp: t for t in self.types}
        self.children: dict[str, list[sb.Type]] = {}
        for t in self.types:
            if t.super:
                self.children.setdefault(t.super, []).append(t)
        self.current: sb.Type | None = None
        self.history: list[tuple[str, str | None]] = []
        self.position = -1
        self.ancestry: list[sb.Type] = []           # Root type first, the current type last
        self.func_infos: list[FuncInfo] = []        # The current type's functions, parsed
        self._refs: tuple[dict, dict] | None = None  # (uses, used_by): type -> referenced type -> where, built once
        self._dirty: set[str] = set()               # Lazy tabs that must be rebuilt when next shown
        self._wanted = "fields"                     # The tab the person last chose, kept across types

        self.setWindowTitle(f"Mirage SDK browser: {len(types)} types ({sb.SDK_DIR})")
        self.resize(1400, 850)
        self.mono = QFont("Consolas")
        self.mono.setStyleHint(QFont.StyleHint.Monospace)
        mono = self.mono

        # ---- Left: search and results
        self.search = QLineEdit(placeholderText="Search (Ctrl+F): ATdPawn, TdGame.TdPawn, AActor::bHidden")
        self.search.setClearButtonEnabled(True)
        self.mode = QComboBox()
        self.mode.addItems(["Types", "Members"])
        self.kind = QComboBox()
        self.kind.addItems(list(KINDS))
        self.results = QListWidget()
        self.results.setFont(mono)
        self.count = QLabel()
        left = QWidget()
        lv = QVBoxLayout(left)
        lv.setContentsMargins(0, 0, 0, 0)
        lv.addWidget(self.search)
        row = QHBoxLayout()
        row.addWidget(self.mode)
        row.addWidget(self.kind)
        lv.addLayout(row)
        lv.addWidget(self.results)
        lv.addWidget(self.count)

        split = QSplitter()
        split.addWidget(left)
        split.addWidget(self._build_inspector())
        split.setSizes([420, 980])
        self.setCentralWidget(split)

        # ---- Navigation
        toolbar = self.addToolBar("Navigation")
        toolbar.setMovable(False)
        self.back = QAction("◀ Back", self, shortcut=QKeySequence("Alt+Left"), triggered=lambda: self._step(-1))
        self.forward = QAction("Forward ▶", self, shortcut=QKeySequence("Alt+Right"), triggered=lambda: self._step(1))
        toolbar.addAction(self.back)
        toolbar.addAction(self.forward)
        self.addAction(QAction(self, shortcut=QKeySequence("Ctrl+F"), triggered=self._focus_search))

        self.search.textChanged.connect(self.refresh)
        self.mode.currentIndexChanged.connect(self.refresh)
        self.kind.currentIndexChanged.connect(self.refresh)
        self.results.currentItemChanged.connect(self._picked)
        self.refresh()
        self.show_type()
        self._update_nav()

    # ---- Building the inspector -------------------------------------------------------------------------------------

    def _build_inspector(self) -> QWidget:
        self.header = TypeHeader()
        self.header.navigate.connect(self._navigate)
        self.filter = QLineEdit(placeholderText="Filter members: name, type, flags or 0xOFFSET (space-separated terms)")
        self.filter.setClearButtonEnabled(True)

        self.pages: dict[str, QWidget] = {}
        self.page_keys: dict[QWidget, str] = {}
        self.tabs = QTabWidget()
        self._build_overview_tab()
        self._build_fields_tab()
        self._build_functions_tab()
        self._build_values_tab()
        self._build_inheritance_tab()
        self._build_references_tab()
        self._build_layout_tab()
        self._build_source_tab()

        self.builders: dict[str, Callable[[], None]] = {
            "overview": self._fill_overview, "inheritance": self._fill_inheritance,
            "references": self._fill_references, "layout": self._fill_layout, "source": self._fill_source}
        self.filter.textChanged.connect(self._apply_filter)
        self.tabs.currentChanged.connect(self._tab_changed)

        right = QWidget()
        rv = QVBoxLayout(right)
        rv.setContentsMargins(0, 0, 0, 0)
        rv.addWidget(self.header)
        rv.addWidget(self.filter)
        rv.addWidget(self.tabs)
        return right

    def _add_page(self, key: str, widget: QWidget) -> None:
        self.pages[key] = widget
        self.page_keys[widget] = key
        self.tabs.addTab(widget, TITLES[key])

    def _details(self) -> DetailView:
        view = DetailView()
        view.navigate.connect(self._navigate)
        return view

    def _build_overview_tab(self) -> None:
        self.overview = self._details()
        self._add_page("overview", self.overview)

    def _build_fields_tab(self) -> None:
        self.field_kind = QComboBox()
        self.field_kind.addItems(FIELD_KINDS)
        self.inherited = QCheckBox("Inherited fields")
        self.group = QCheckBox("Group by class")
        self.group.setChecked(True)
        self.group.setEnabled(False)
        self.fields = DataTable(["Offset", "Size", "Type", "Name", "Flags", "From"], self.mono, sort_column=0)
        self.fields.menu_builder = self._field_menu
        self.fields.activated.connect(self._field_activated)
        self.fields.selectionModel().currentRowChanged.connect(lambda *_: self._field_selected())
        self.field_details = self._details()

        bar = QHBoxLayout()
        bar.addWidget(self.field_kind)
        bar.addWidget(self.inherited)
        bar.addWidget(self.group)
        bar.addStretch(1)
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 4, 0, 0)
        layout.addLayout(bar)
        layout.addWidget(self._vsplit(self.fields, self.field_details))
        self._add_page("fields", page)

        self.field_kind.currentIndexChanged.connect(self._fill_fields)
        self.inherited.toggled.connect(self._inherited_toggled)
        self.group.toggled.connect(self._fill_fields)

    def _build_functions_tab(self) -> None:
        self.functions = DataTable(["Function", "Returns", "Parameters", "Flags"], self.mono, sort_column=-1)
        self.functions.menu_builder = self._function_menu
        self.functions.activated.connect(self._function_activated)
        self.functions.selectionModel().currentRowChanged.connect(lambda *_: self._function_selected())
        self.function_details = self._details()
        self._add_page("functions", self._vsplit(self.functions, self.function_details))

    def _build_values_tab(self) -> None:
        self.values = DataTable(["Value", "Name"], self.mono, sort_column=0)
        self.values.menu_builder = self._generic_menu(self.values)
        self._add_page("values", self.values)

    def _build_inheritance_tab(self) -> None:
        self.tree = QTreeWidget()
        self.tree.setHeaderLabels(["Type", "Path", "Subclasses", "Size"])
        self.tree.setFont(self.mono)
        self.tree.setUniformRowHeights(True)
        self.tree.setColumnWidth(0, 320)
        self.tree.setColumnWidth(1, 320)
        self.tree.setColumnWidth(2, 90)
        self.tree.itemExpanded.connect(self._tree_expanded)
        self.tree.itemActivated.connect(self._tree_activated)
        self._add_page("inheritance", self.tree)

    def _build_references_tab(self) -> None:
        headers = ["Type", "Kind", "Path", "Count", "Via"]
        self.uses_table = DataTable(headers, self.mono, sort_column=0)
        self.used_by_table = DataTable(headers, self.mono, sort_column=0)
        self.uses_section = Section("Uses", self.uses_table)
        self.used_by_section = Section("Used by", self.used_by_table)
        for table in (self.uses_table, self.used_by_table):
            table.menu_builder = self._generic_menu(table)
            table.activated.connect(lambda index, tbl=table: self._open_payload(tbl, index))
        splitter = QSplitter(Qt.Orientation.Vertical)
        splitter.addWidget(self.uses_section)
        splitter.addWidget(self.used_by_section)
        self._add_page("references", splitter)

    def _build_layout_tab(self) -> None:
        self.layout_inherited = QCheckBox("Include inherited fields")
        self.layout_inherited.setChecked(True)
        self.layout_summary = QLabel()
        self.layout_legend = QLabel(textFormat=Qt.TextFormat.RichText)
        self.layout_legend.setWordWrap(True)
        self.layout_strip = LayoutStrip()
        self.layout_table = DataTable(["Offset", "End", "Size", "Field", "Type", "Owner"], self.mono)
        self.layout_table.menu_builder = self._layout_menu
        self.layout_table.activated.connect(self._layout_activated)
        self.layout_table.selectionModel().currentRowChanged.connect(
            lambda *_: self.layout_strip.set_selected(self.layout_table.current_payload()))
        self.layout_strip.segmentClicked.connect(
            lambda row: self.layout_table.select_payload(lambda payload: payload is row))

        bar = QHBoxLayout()
        bar.addWidget(self.layout_inherited)
        bar.addWidget(self.layout_summary, 1)
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 4, 0, 0)
        layout.addLayout(bar)
        layout.addWidget(self.layout_strip)
        layout.addWidget(self.layout_legend)
        layout.addWidget(self.layout_table, 1)
        self._add_page("layout", page)
        self.layout_inherited.toggled.connect(self._layout_toggled)

    def _build_source_tab(self) -> None:
        self.source = SourceView(self.mono)
        self._add_page("source", self.source)

    @staticmethod
    def _vsplit(top: QWidget, bottom: QWidget) -> QSplitter:
        splitter = QSplitter(Qt.Orientation.Vertical)
        splitter.addWidget(top)
        splitter.addWidget(bottom)
        splitter.setStretchFactor(0, 1)
        splitter.setSizes([520, 150])
        return splitter

    def _focus_search(self) -> None:
        self.search.setFocus()
        self.search.selectAll()

    # ---- The results list -------------------------------------------------------------------------------------------

    
    def refresh(self) -> None:
        query = self.search.text().strip()
        qualified = sb.QUALIFIED_MEMBER.fullmatch(query)
        mode = "members" if qualified or self.mode.currentText() == "Members" else "types"
        kinds = KINDS[self.kind.currentText()]
    
        parsed = SearchParser.parse(query)
    
        if not parsed.valid:
            self.results.clear()
            self.count.setText(f"Invalid query: {parsed.error}")
            return
    
        if qualified:
            found = [
                SearchResult(t, member, sb.function_name(member) if isinstance(member, str) else member.name,
                             member if isinstance(member, str) else f"0x{member.offset:04X}  {member.type}", 100)
                for t, member in sb.find_members(self.types, query) if not kinds or t.kind in kinds
            ]
        else:
            found = self.search_index.search(query=query, mode=mode, kinds=kinds, limit=MAX_ROWS)
    
        self.results.setUpdatesEnabled(False)
        self.results.blockSignals(True)
    
        try:
            self.results.clear()
    
            for result in found:
                if mode == "members":
                    item = QListWidgetItem(
                        f"{result.type.cpp}::{result.name}   "
                        f"{result.description}"
                    )
                    item.setData(
                        Qt.ItemDataRole.UserRole,
                        (result.type.cpp, result.name),
                    )
                else:
                    item = QListWidgetItem(
                        f"{result.type.cpp:<40} {result.type.path}"
                    )
                    item.setData(
                        Qt.ItemDataRole.UserRole,
                        (result.type.cpp, None),
                    )
    
                item.setToolTip(
                    f"Relevance: {result.score:.1f}\n"
                    f"Type: {result.type.cpp}"
                )
    
                self.results.addItem(item)
    
        finally:
            self.results.blockSignals(False)
            self.results.setUpdatesEnabled(True)
    
        total = len(found)
        self.count.setText(
            f"{total} result(s)"
            + (f" (limit: {MAX_ROWS})" if total == MAX_ROWS else "")
        )
    
        if total == 1:
            self.results.setCurrentRow(0)
    

    def _picked(self, item: QListWidgetItem | None) -> None:
        if item:
            cpp, member = item.data(Qt.ItemDataRole.UserRole)
            self.open(cpp, member)

    # ---- Navigation -------------------------------------------------------------------------------------------------

    def open(self, cpp: str, member: str | None = None, record: bool = True) -> None:
        t = self.by_cpp.get(cpp)
        if not t:
            return
        entry = (cpp, member)
        if record and (not self.history or self.history[self.position] != entry):
            del self.history[self.position + 1:]
            self.history.append(entry)
            self.position = len(self.history) - 1
        self.current = t
        if member:
            self._reset_filters()   # A filter must not hide the member being opened
        self.show_type()
        if member:
            self._select_member(member)
        self._update_nav()

    def _step(self, delta: int) -> None:
        if 0 <= self.position + delta < len(self.history):
            self.position += delta
            self.open(*self.history[self.position], record=False)

    def _update_nav(self) -> None:
        self.back.setEnabled(self.position > 0)
        self.forward.setEnabled(self.position < len(self.history) - 1)

    def _navigate(self, href: str) -> None:
        """A link of the header or of a detail view: a type name, or "tab:<page>"."""
        if href.startswith("api:") and self.current:
            path = self.api_metadata.artifact(self.current.path, href[4:])
            if path:
                QDesktopServices.openUrl(QUrl.fromLocalFile(str(path)))
        elif href.startswith("tab:"):
            page = self.pages.get(href[4:])
            if page is not None and self.tabs.isTabVisible(self.tabs.indexOf(page)):
                self.tabs.setCurrentWidget(page)
        else:
            self.open(href)

    def _reset_filters(self) -> None:
        for widget in (self.filter, self.field_kind):
            widget.blockSignals(True)
        self.filter.clear()
        self.field_kind.setCurrentIndex(0)
        for widget in (self.filter, self.field_kind):
            widget.blockSignals(False)

    def _select_member(self, name: str) -> None:
        for key, table, match in (
                ("fields", self.fields, lambda p: isinstance(p, FieldRef) and p.field.name == name),
                ("functions", self.functions, lambda p: isinstance(p, FuncInfo) and p.name == name)):
            page = self.pages[key]
            if self.tabs.isTabVisible(self.tabs.indexOf(page)) and table.select_payload(match):
                self.tabs.setCurrentWidget(page)
                return

    # ---- The type ---------------------------------------------------------------------------------------------------

    def _ancestry(self, t: sb.Type) -> list[sb.Type]:
        bases = list(sb.chain(self.types, t)) or [t]
        if len(bases) > 1 and bases[0].cpp == t.cpp:
            bases.reverse()
        if bases[-1].cpp != t.cpp:
            bases.append(t)
        return bases

    def show_type(self) -> None:
        t = self.current
        if not t:
            self.header.show_empty()
            self.filter.setVisible(False)
            self.tabs.setVisible(False)
            return
        self.filter.setVisible(True)
        self.tabs.setVisible(True)
        self.ancestry = self._ancestry(t)
        self.func_infos = [parse_function(fn) for fn in t.functions]
        uses, used_by = self._refs_of(t)
        has_source = header_path(t.file) is not None
        enum = t.kind == "Enum"
        has_fields = any(b.fields for b in self.ancestry)
        visible = {
            "overview": True,
            "fields": not enum and has_fields,
            "functions": bool(t.functions),
            "values": enum,
            "inheritance": len(self.ancestry) > 1 or bool(self.children.get(t.cpp)),
            "references": bool(uses or used_by),
            "layout": not enum and has_fields,
            "source": has_source,
        }

        self.tabs.blockSignals(True)        # Hiding the current tab would otherwise build a tab half-way through
        for key, page in self.pages.items():
            self.tabs.setTabVisible(self.tabs.indexOf(page), visible[key])
        self._fill_fields()
        self._fill_functions()
        self._fill_values()
        self._set_title("references", str(len(set(uses) | set(used_by))) if visible["references"] else "")
        self.header.show_type(t, self.ancestry, self._stats(t, uses, used_by), self._link, self._muted(), has_source)
        self._dirty = {"overview", "inheritance", "references", "layout", "source"}
        target = "values" if enum else self._wanted
        if not visible.get(target):
            target = "fields" if visible["fields"] else "overview"
        self.tabs.setCurrentWidget(self.pages[target])
        self.tabs.blockSignals(False)
        self._populate_current()

    def _stats(self, t: sb.Type, uses: dict, used_by: dict) -> list[tuple[str, str]]:
        stats = []
        if t.kind == "Enum":
            stats.append((f"{len(t.values)} values", "values"))
        else:
            own = len(t.fields)
            inherited = sum(len(b.fields) for b in self.ancestry[:-1])
            if own or inherited:
                stats.append((f"{own} fields" + (f" (+{inherited} inherited)" if inherited else ""), "fields"))
        if t.functions:
            stats.append((f"{len(t.functions)} functions", "functions"))
        subclasses = len(self.children.get(t.cpp, ()))
        if subclasses:
            stats.append((f"{subclasses} subclasses", "inheritance"))
        if uses:
            stats.append((f"uses {len(uses)} types", "references"))
        if used_by:
            stats.append((f"used by {len(used_by)} types", "references"))
        return stats

    def _tab_changed(self, index: int) -> None:
        key = self.page_keys.get(self.tabs.widget(index))
        if key:
            self._wanted = key
        self._populate_current()

    def _populate_current(self) -> None:
        """Builds the tab on show, once per type: only the tabs that are cheap are built when the type opens."""
        key = self.page_keys.get(self.tabs.currentWidget())
        if not self.current or key not in self._dirty:
            return
        self._dirty.discard(key)
        self.builders[key]()

    def _set_title(self, key: str, suffix: str = "") -> None:
        self.tabs.setTabText(self.tabs.indexOf(self.pages[key]), TITLES[key] + (f" ({suffix})" if suffix else ""))

    def _apply_filter(self) -> None:
        if self.current:
            self._fill_fields()
            self._fill_functions()
            self._fill_values()

    # ---- Helpers ----------------------------------------------------------------------------------------------------

    def _muted(self) -> str:
        return self.palette().color(QPalette.ColorRole.PlaceholderText).name()

    def _tint(self, role: QPalette.ColorRole, alpha: int) -> QColor:
        color = QColor(self.palette().color(role))
        color.setAlpha(alpha)
        return color

    def _link(self, cpp: str) -> str:
        return f'<a href="{html.escape(cpp)}">{html.escape(cpp)}</a>' if cpp in self.by_cpp else html.escape(cpp)

    def _linkify(self, text: str) -> str:
        """`text` escaped, with the SDK types it names as links."""
        out, last = [], 0
        for m in _IDENT.finditer(text):
            if m.group() in self.by_cpp:
                out.append(html.escape(text[last:m.start()]))
                out.append(self._link(m.group()))
                last = m.end()
        out.append(html.escape(text[last:]))
        return "".join(out)

    def _links(self, names, limit: int = 20, more_tab: str | None = None) -> str:
        names = sorted(names, key=str.lower)
        text = ", ".join(self._link(n) for n in names[:limit])
        if len(names) > limit:
            rest = f"+{len(names) - limit} more"
            text += f" … <a href='tab:{more_tab}'>{rest}</a>" if more_tab else f" … {rest}"
        return text

    def _types_in(self, text: str, include_self: bool = False) -> list[str]:
        """The SDK types named in a C++ type ("TArray<class AActor*>" -> AActor), in order, each once."""
        own = None if include_self or not self.current else self.current.cpp
        found: list[str] = []
        for token in _IDENT.findall(text):
            if token in self.by_cpp and token != own and token not in found:
                found.append(token)
        return found

    def _type_in(self, text: str) -> str | None:
        types = self._types_in(text)
        return types[0] if types else None

    def _copy(self, text: str) -> None:
        QApplication.clipboard().setText(text)
        self.statusBar().showMessage(f"Copied: {text[:80]}", 2500)

    @staticmethod
    def _action(menu: QMenu, text: str, callback: Callable[[], None]) -> None:
        menu.addAction(text).triggered.connect(lambda _checked=False: callback())

    def _open_payload(self, table: DataTable, index: QModelIndex) -> None:
        payload = table.payload(index)
        if isinstance(payload, str):
            self.open(payload)

    def _generic_menu(self, table: DataTable) -> Callable[[QMenu, QModelIndex], None]:
        def build(menu: QMenu, index: QModelIndex) -> None:
            self._action(menu, "Copy row", lambda: self._copy(table.row_text(index)))
            self._action(menu, "Copy cell", lambda: self._copy(index.data() or ""))
            payload = table.payload(index)
            if isinstance(payload, str) and payload in self.by_cpp:
                menu.addSeparator()
                self._action(menu, f"Open {payload}", lambda: self.open(payload))
        return build

    def _references(self) -> tuple[dict, dict]:
        """(uses, used_by): for every type the types its fields and functions name, and the reverse, with where."""
        if self._refs is None:
            QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
            try:
                uses: dict[str, dict[str, list[str]]] = {}
                used_by: dict[str, dict[str, list[str]]] = {}
                for t in self.types:
                    mine = uses.setdefault(t.cpp, {})
                    members = [(f.type, f.name) for f in t.fields]
                    members += [(fn, f"{sb.function_name(fn)}()") for fn in t.functions]
                    for text, via in members:
                        for token in set(_IDENT.findall(text)):
                            if token != t.cpp and token in self.by_cpp:
                                mine.setdefault(token, []).append(via)
                                used_by.setdefault(token, {}).setdefault(t.cpp, []).append(via)
                self._refs = (uses, used_by)
            finally:
                QApplication.restoreOverrideCursor()
        return self._refs

    def _refs_of(self, t: sb.Type) -> tuple[dict[str, list[str]], dict[str, list[str]]]:
        uses, used_by = self._references()
        return uses.get(t.cpp, {}), used_by.get(t.cpp, {})

    # ---- Fields -----------------------------------------------------------------------------------------------------

    def _inherited_toggled(self) -> None:
        self.group.setEnabled(self.inherited.isChecked())
        self._fill_fields()

    def _field_kinds(self, f: sb.Field) -> set[str]:
        kinds = set()
        if "*" in f.type:
            kinds.add("Pointers")
        if "<" in f.type or "[" in f.type:
            kinds.add("Containers")
        if self._types_in(f.type, include_self=True):
            kinds.add("SDK types")
        return kinds or {"Primitives"}

    def _field_matches(self, f: sb.Field, owner: sb.Type, terms: list[str], kind: str) -> bool:
        if kind != FIELD_KINDS[0] and kind not in self._field_kinds(f):
            return False
        haystack = f"{f.name} {f.type} {f.annotations} 0x{f.offset:04x} {owner.cpp}".lower()
        return all(term in haystack for term in terms)

    def _field_cells(self, f: sb.Field, owner: sb.Type) -> list[Cell]:
        t = self.current
        fg = None if t is None or owner.cpp == t.cpp else self.palette().color(QPalette.ColorRole.PlaceholderText)
        target = self._type_in(f.type)
        return [Cell(f"0x{f.offset:04X}", f.offset, fg=fg), Cell(f"0x{f.size:X}", f.size, fg=fg),
                Cell(f.type, link=bool(target), tip=f"Double click: open {target}" if target else "", fg=fg),
                Cell(f.name, fg=fg), Cell(f.annotations, fg=fg), Cell(owner.cpp, fg=fg)]

    def _fill_fields(self) -> None:
        t = self.current
        if not t:
            return
        include = self.inherited.isChecked()
        grouped = include and self.group.isChecked()
        terms = self.filter.text().lower().split()
        kind = self.field_kind.currentText()
        owners = self.ancestry if include else [t]
        entries = [(f, b) for b in owners for f in b.fields if self._field_matches(f, b, terms, kind)]
        entries.sort(key=lambda e: e[0].offset)

        columns = self.fields.source.columnCount()
        rows: list[list[Cell]] = []
        payloads: list[object] = []
        if grouped:
            tint = self._tint(QPalette.ColorRole.Mid, 60)
            for b in owners:
                group = [e for e in entries if e[1].cpp == b.cpp]
                if not group:
                    continue
                tag = "this type" if b.cpp == t.cpp else "inherited"
                label = f"{b.cpp}  ({tag}, {len(group)} field(s))"
                rows.append([Cell(label, bold=True, bg=tint)] + [Cell("", bg=tint) for _ in range(columns - 1)])
                payloads.append(GroupRef(b))
                for f, owner in group:
                    rows.append(self._field_cells(f, owner))
                    payloads.append(FieldRef(f, owner))
        else:
            for f, owner in entries:
                rows.append(self._field_cells(f, owner))
                payloads.append(FieldRef(f, owner))

        self.fields.set_sorting(not grouped)
        self.fields.set_rows(rows, payloads)
        if grouped:
            for i, payload in enumerate(payloads):
                if isinstance(payload, GroupRef):
                    self.fields.setSpan(i, 0, 1, columns)
        self.fields.setColumnHidden(5, not include or grouped)
        self.fields.resize_columns()
        if grouped:     # The group captions span the table: they must not set the width of the Offset column
            self.fields.setColumnWidth(0, min(self.fields.columnWidth(0), 110))
        self._set_title("fields", str(len(entries)))
        self._field_selected()

    def _field_selected(self) -> None:
        ref = self.fields.current_payload()
        if isinstance(ref, FieldRef):
            self.field_details.setHtml(self._field_html(ref))
        else:
            self.field_details.setHtml(f"<i style='color:{self._muted()}'>Select a field for details</i>")

    def _field_html(self, ref: FieldRef) -> str:
        f, owner = ref.field, ref.owner
        muted = self._muted()
        targets = self._types_in(f.type)
        parts = [f"<b>{html.escape(f.name)}</b> <span style='color:{muted}'>declared in</span> {self._link(owner.cpp)}",
                 f"<code>{self._linkify(f.declaration)}</code>",
                 f"Offset <b>0x{f.offset:04X}</b> ({f.offset}) &nbsp; Size <b>0x{f.size:X}</b> ({f.size}) &nbsp; "
                 f"End <b>0x{f.offset + f.size:04X}</b>"]
        if f.annotations:
            parts.append(f"Flags: {html.escape(f.annotations)}")
        parts.append("Category: " + ", ".join(sorted(self._field_kinds(f))))
        if targets:
            parts.append("Types: " + ", ".join(self._link(x) for x in targets))
        return "<br>".join(parts)

    def _field_activated(self, index: QModelIndex) -> None:
        ref = self.fields.payload(index)
        if isinstance(ref, FieldRef) and (target := self._type_in(ref.field.type)):
            self.open(target)

    def _field_menu(self, menu: QMenu, index: QModelIndex) -> None:
        ref = self.fields.payload(index)
        if isinstance(ref, FieldRef):
            self._field_actions(menu, ref, self.fields.row_text(index), in_layout=False)

    def _field_actions(self, menu: QMenu, ref: FieldRef, row_text: str, in_layout: bool) -> None:
        f = ref.field
        self._action(menu, "Copy declaration", lambda: self._copy(f.declaration))
        self._action(menu, "Copy qualified name", lambda: self._copy(f"{ref.owner.cpp}::{f.name}"))
        self._action(menu, "Copy offset", lambda: self._copy(f"0x{f.offset:04X}"))
        self._action(menu, "Copy name", lambda: self._copy(f.name))
        self._action(menu, "Copy type", lambda: self._copy(f.type))
        self._action(menu, "Copy row", lambda: self._copy(row_text))
        menu.addSeparator()
        for target in self._types_in(f.type):
            self._action(menu, f"Open type {target}", lambda target=target: self.open(target))
        if self.current and ref.owner.cpp != self.current.cpp:
            self._action(menu, f"Open declaring type {ref.owner.cpp}", lambda: self.open(ref.owner.cpp))
        if not in_layout:
            self._action(menu, "Show in Memory Layout", lambda: self._show_in_layout(ref))

    # ---- Functions --------------------------------------------------------------------------------------------------

    def _fill_functions(self) -> None:
        t = self.current
        if not t:
            return
        terms = self.filter.text().lower().split()
        infos = [i for i in self.func_infos if all(term in i.decl.lower() for term in terms)]
        rows = []
        for info in infos:
            returns = self._type_in(info.ret)
            rows.append([Cell(info.name, tip=info.decl),
                         Cell(info.ret or "—", link=bool(returns), tip=f"Double click: open {returns}" if returns else ""),
                         Cell(f"({info.params_text})", len(info.params), tip=info.decl),
                         Cell(", ".join(info.flags))])
        self.functions.set_rows(rows, list(infos))
        self.functions.resize_columns()
        self._set_title("functions", str(len(infos)))
        self._function_selected()

    def _function_selected(self) -> None:
        info = self.functions.current_payload()
        if isinstance(info, FuncInfo):
            self.function_details.setHtml(self._function_html(info))
        else:
            self.function_details.setHtml(f"<i style='color:{self._muted()}'>Select a function for details</i>")

    def _function_html(self, info: FuncInfo) -> str:
        muted = self._muted()
        parts = [f"<b>{html.escape(info.name)}()</b> <span style='color:{muted}'>in</span> "
                 f"{self._link(self.current.cpp) if self.current else ''}",
                 f"<code>{self._linkify(info.decl)}</code>",
                 f"Returns: <code>{self._linkify(info.ret) if info.ret else '—'}</code>"]
        if info.flags:
            parts.append("Flags: " + html.escape(", ".join(info.flags)))
        if info.params:
            body = "".join(f"<tr><td>{i}</td><td><code>{self._linkify(p.type)}</code></td>"
                           f"<td><code>{html.escape(p.name)}</code></td><td><code>{html.escape(p.default)}</code></td></tr>"
                           for i, p in enumerate(info.params))
            parts.append("Parameters:<table cellspacing='0' cellpadding='2'><tr><th>#</th><th>Type</th><th>Name</th>"
                         f"<th>Default</th></tr>{body}</table>")
        else:
            parts.append("Parameters: none")
        return "<br>".join(parts)

    def _function_activated(self, index: QModelIndex) -> None:
        info = self.functions.payload(index)
        if isinstance(info, FuncInfo) and (target := self._type_in(info.decl)):
            self.open(target)

    def _function_menu(self, menu: QMenu, index: QModelIndex) -> None:
        info = self.functions.payload(index)
        if not isinstance(info, FuncInfo):
            return
        self._action(menu, "Copy declaration", lambda: self._copy(info.decl))
        if self.current:
            qualified = f"{self.current.cpp}::{info.name}"
            self._action(menu, "Copy qualified name", lambda: self._copy(qualified))
        self._action(menu, "Copy name", lambda: self._copy(info.name))
        self._action(menu, "Copy row", lambda: self._copy(self.functions.row_text(index)))
        targets = self._types_in(info.decl)
        if targets:
            menu.addSeparator()
        for target in targets:
            self._action(menu, f"Open type {target}", lambda target=target: self.open(target))

    # ---- Enum values ------------------------------------------------------------------------------------------------

    @staticmethod
    def _int(value: object) -> int | None:
        try:
            return int(str(value), 0)
        except ValueError:
            return None

    def _fill_values(self) -> None:
        t = self.current
        if not t:
            return
        terms = self.filter.text().lower().split()
        items = [(n, v) for n, v in t.values if all(term in f"{n} {v}".lower() for term in terms)]
        keys = [self._int(v) for _, v in items]
        numeric = all(k is not None for k in keys)
        rows = [[Cell(str(v), k if numeric else None), Cell(str(n))] for (n, v), k in zip(items, keys)]
        self.values.set_rows(rows, [(n, v) for n, v in items])
        self.values.resize_columns()
        self._set_title("values", str(len(items)))

    # ---- Overview ---------------------------------------------------------------------------------------------------

    def _descendants(self, cpp: str) -> int:
        seen, stack = set(), [cpp]
        while stack:
            for child in self.children.get(stack.pop(), ()):
                if child.cpp not in seen:
                    seen.add(child.cpp)
                    stack.append(child.cpp)
        return len(seen)

    def _fill_overview(self) -> None:
        t = self.current
        if not t:
            return
        esc = html.escape
        muted = self._muted()
        uses, used_by = self._refs_of(t)

        def table(rows: list[tuple[str, str | None]]) -> str:
            body = "".join(f"<tr><td style='color:{muted}' width='150'>{k}</td><td>{v}</td></tr>"
                           for k, v in rows if v)
            return f"<table cellspacing='0' cellpadding='3'>{body}</table>"

        size = f"0x{t.size:X} ({t.size} bytes)" if t.size is not None else None
        source = f"<a href='tab:source'>{esc(t.file)}:{t.line}</a>" if header_path(t.file) else f"{esc(t.file)}:{t.line}"
        out = ["<h3>Type</h3>" + table([
            ("Kind", esc(t.kind)), ("Name", esc(t.path)), ("C++ name", f"<code>{esc(t.cpp)}</code>"),
            ("Package", esc(t.path.rpartition(".")[0])), ("Size", size), ("Header", source)])]

        own = len(t.fields)
        inherited = sum(len(b.fields) for b in self.ancestry[:-1])
        members: list[tuple[str, str | None]] = []
        if t.kind == "Enum":
            members.append(("Values", str(len(t.values))))
        else:
            members.append(("Fields", f"{own} own, {inherited} inherited"))
        members.append(("Functions", str(len(t.functions))))
        out.append("<h3>Members</h3>" + table(members))

        if t.path in self.api_metadata.types:
            out.append(self._api_overview(t.path, table))

        if t.kind != "Enum" and own + inherited:
            layout, total = build_layout(self.ancestry, True)
            padding = sum(r.size for r in layout if not r.owner)
            overlaps = sum(1 for r in layout if r.overlap)
            base = self.ancestry[-2].size if len(self.ancestry) > 1 else None
            out.append("<h3>Memory</h3>" + table([
                ("Total size", f"0x{total:X} ({total} bytes)"),
                ("Base size", f"0x{base:X} ({base} bytes)" if base else None),
                ("Own size", f"0x{total - base:X} ({total - base} bytes)" if base and total >= base else None),
                ("Padding", f"{padding} bytes ({padding * 100 / total:.1f}%)" if total else None),
                ("Overlaps", str(overlaps) if overlaps else None)]))

        children = [c.cpp for c in self.children.get(t.cpp, ())]
        descendants = self._descendants(t.cpp)
        out.append("<h3>Relationships</h3>" + table([
            ("Extends", " › ".join(self._link(b.cpp) for b in self.ancestry[:-1]) or None),
            ("Direct subclasses", self._links(children, 30, "inheritance") if children else None),
            ("All descendants", str(descendants) if descendants > len(children) else None),
            ("Uses", self._links(uses, 20, "references") if uses else None),
            ("Used by", self._links(used_by, 20, "references") if used_by else None)]))

        if t.kind == "Enum" and t.values:
            shown = "<br>".join(f"<code>{esc(str(n))} = {esc(str(v))}</code>" for n, v in t.values[:12])
            more = f"<br>… <a href='tab:values'>+{len(t.values) - 12} more</a>" if len(t.values) > 12 else ""
            out.append(f"<h3>Values</h3>{shown}{more}")
        self.overview.setHtml("".join(out))

    def _api_overview(self, path: str, table: Callable) -> str:
        info = self.api_metadata.types[path]
        members = self.api_metadata.members[path]
        generated = sum(member.get("generated") is True for _, member in members)
        editor = sum(member.get("editor") is True for _, member in members)
        links = []
        for key, label in (("docs", "Markdown reference"), ("definitions", "LuaLS definitions"),
                           ("runtime_definitions", "Runtime LuaLS definitions")):
            if key == "runtime_definitions" and info.get("definitions") == info.get(key):
                continue
            if self.api_metadata.artifact(path, key):
                links.append(f"<a href='api:{key}'>{label}</a>")
        targets = info.get("targets", [])
        rows = [("Runtime exposure", f"{generated} of {len(members)} members"),
                ("SDK inspection", f"{editor} of {len(members)} members"),
                ("SDK inspection type", html.escape(str(info.get("lua_name", "")))),
                ("Generated targets", html.escape(", ".join(map(str, targets))) if isinstance(targets, list) else ""),
                ("Generated files", " · ".join(links))]
        names_and_status = []
        for member_path, member in members:
            names = member.get("lua_names")
            status = ", ".join(map(str, names)) if isinstance(names, list) and names else member.get("reason", "")
            names_and_status.append((html.escape(member_path.rpartition(".")[2]), html.escape(str(status))))
        return "<h3>Scripting API</h3>" + table(rows) + (table(names_and_status) if names_and_status else "")

    # ---- Inheritance ------------------------------------------------------------------------------------------------

    def _tree_item(self, parent, t: sb.Type, lazy: bool) -> QTreeWidgetItem:
        count = len(self.children.get(t.cpp, ()))
        size = f"0x{t.size:X}" if t.size is not None else ""
        item = QTreeWidgetItem(parent, [t.cpp, t.path, str(count) if count else "", size])
        item.setData(0, Qt.ItemDataRole.UserRole, t.cpp)
        if lazy and count:
            QTreeWidgetItem(item, ["…"])    # Replaced by the children when the item is first expanded
        return item

    def _fill_inheritance(self) -> None:
        t = self.current
        if not t:
            return
        self.tree.clear()
        parent: QTreeWidget | QTreeWidgetItem = self.tree
        for b in self.ancestry[:-1]:
            parent = self._tree_item(parent, b, lazy=False)
            parent.setExpanded(True)
        current = self._tree_item(parent, t, lazy=True)
        bold = QFont(self.mono)
        bold.setBold(True)
        for column in range(4):
            current.setFont(column, bold)
        current.setExpanded(True)
        self.tree.setCurrentItem(current)
        self.tree.scrollToItem(current)

    def _tree_expanded(self, item: QTreeWidgetItem) -> None:
        if item.childCount() == 1 and item.child(0).data(0, Qt.ItemDataRole.UserRole) is None:
            item.takeChild(0)
            cpp = item.data(0, Qt.ItemDataRole.UserRole)
            for child in sorted(self.children.get(cpp, ()), key=lambda c: c.cpp.lower()):
                self._tree_item(item, child, lazy=True)

    def _tree_activated(self, item: QTreeWidgetItem, _column: int) -> None:
        cpp = item.data(0, Qt.ItemDataRole.UserRole)
        if cpp and (not self.current or cpp != self.current.cpp):
            self.open(cpp)

    # ---- References -------------------------------------------------------------------------------------------------

    def _ref_rows(self, refs: dict[str, list[str]]) -> tuple[list[list[Cell]], list[object]]:
        rows, payloads = [], []
        for cpp in sorted(refs, key=str.lower):
            ref = self.by_cpp[cpp]
            vias = list(dict.fromkeys(refs[cpp]))
            shown = ", ".join(vias[:4]) + (f" +{len(vias) - 4}" if len(vias) > 4 else "")
            rows.append([Cell(cpp, link=True), Cell(ref.kind), Cell(ref.path), Cell(str(len(refs[cpp])), len(refs[cpp])),
                         Cell(shown, tip=", ".join(vias[:50]))])
            payloads.append(cpp)
        return rows, payloads

    def _fill_references(self) -> None:
        t = self.current
        if not t:
            return
        uses, used_by = self._refs_of(t)
        for table, section, refs, label in ((self.uses_table, self.uses_section, uses, "Uses"),
                                            (self.used_by_table, self.used_by_section, used_by, "Used by")):
            rows, payloads = self._ref_rows(refs)
            table.set_rows(rows, payloads)
            table.resize_columns()
            section.set_title(f"{label}: {len(refs)} type(s)")

    # ---- Memory layout ----------------------------------------------------------------------------------------------

    def _layout_toggled(self) -> None:
        self._dirty.add("layout")
        self._populate_current()

    def _fill_layout(self) -> None:
        t = self.current
        if not t:
            return
        esc = html.escape
        layout, total = build_layout(self.ancestry, self.layout_inherited.isChecked())
        colors = {b.cpp: QColor.fromHsv((i * 53) % 360, 140, 215) for i, b in enumerate(self.ancestry)}
        gray = self._tint(QPalette.ColorRole.Mid, 70)
        muted = self.palette().color(QPalette.ColorRole.PlaceholderText)
        red = QColor("#d04040")

        rows: list[list[Cell]] = []
        segments: list[tuple[LayoutRow, QColor | None, str]] = []
        for r in layout:
            f = r.field
            if f is not None:
                tint = QColor(colors[r.owner])
                tint.setAlpha(60)
                name = f.name + ("  (overlaps the previous field)" if r.overlap else "")
                type_text, fg, target = f.type, red if r.overlap else None, self._type_in(f.type)
                tip = f"{r.owner}::{f.name}\n{f.type}\n0x{r.start:04X}–0x{r.end:04X} ({r.size} bytes)"
                segments.append((r, colors[r.owner], tip))
            elif r.owner:
                tint = QColor(colors[r.owner])
                tint.setAlpha(35)
                name, type_text, fg, target = f"‹inherited from {r.owner}›", "", muted, None
                segments.append((r, colors[r.owner], f"{name}\n0x{r.start:04X}–0x{r.end:04X} ({r.size} bytes)"))
            else:
                tint = gray
                name, type_text, fg, target = "‹padding›", f"{r.size} byte(s)", muted, None
                segments.append((r, None, f"padding\n0x{r.start:04X}–0x{r.end:04X} ({r.size} bytes)"))
            rows.append([Cell(f"0x{r.start:04X}", r.start, bg=tint), Cell(f"0x{r.end:04X}", r.end, bg=tint),
                         Cell(f"0x{r.size:X}", r.size, bg=tint), Cell(name, bg=tint, fg=fg),
                         Cell(type_text, link=bool(target), bg=tint, fg=fg), Cell(r.owner, bg=tint)])
        self.layout_table.set_rows(rows, list(layout))
        self.layout_table.resize_columns()
        self.layout_strip.set_layout(segments, total)

        fields = sum(1 for r in layout if r.field is not None)
        padding = sum(r.size for r in layout if not r.owner)
        overlaps = sum(1 for r in layout if r.overlap)
        percent = f" ({padding * 100 / total:.1f}%)" if total else ""
        self.layout_summary.setText(f"0x{total:X} ({total}) bytes · {fields} fields · {padding} bytes of padding{percent}"
                                    + (f" · {overlaps} overlap(s)" if overlaps else ""))
        owners = {r.owner for r in layout if r.owner}
        legend = [f"<span style='color:{colors[b.cpp].name()}'>■</span> {esc(b.cpp)}"
                  for b in self.ancestry if b.cpp in owners]
        legend.append(f"<span style='color:{muted.name()}'>▨</span> padding")
        self.layout_legend.setText(" &nbsp; ".join(legend))

    def _layout_ref(self, row: object) -> FieldRef | None:
        if isinstance(row, LayoutRow) and row.field is not None and row.owner in self.by_cpp:
            return FieldRef(row.field, self.by_cpp[row.owner])
        return None

    def _layout_activated(self, index: QModelIndex) -> None:
        ref = self._layout_ref(self.layout_table.payload(index))
        if ref and (target := self._type_in(ref.field.type)):
            self.open(target)

    def _layout_menu(self, menu: QMenu, index: QModelIndex) -> None:
        ref = self._layout_ref(self.layout_table.payload(index))
        if ref:
            self._field_actions(menu, ref, self.layout_table.row_text(index), in_layout=True)
        else:
            self._action(menu, "Copy row", lambda: self._copy(self.layout_table.row_text(index)))

    def _show_in_layout(self, ref: FieldRef) -> None:
        if self.current and ref.owner.cpp != self.current.cpp and not self.layout_inherited.isChecked():
            self.layout_inherited.blockSignals(True)
            self.layout_inherited.setChecked(True)
            self.layout_inherited.blockSignals(False)
            self._dirty.add("layout")
        self.tabs.setCurrentWidget(self.pages["layout"])    # Builds the tab if it is not built yet
        self.layout_table.select_payload(lambda row: isinstance(row, LayoutRow) and row.field is ref.field)

    # ---- Source -----------------------------------------------------------------------------------------------------

    def _fill_source(self) -> None:
        t = self.current
        path = header_path(t.file) if t else None
        if t and path:
            self.source.show_location(path, int(t.line or 1))


def run(query: str | None = None, api_output: Path | None = None) -> int:
    types = sb.load()
    if not types:
        print(f"No SDK headers in {sb.SDK_DIR} (run `mirage sdk`)", file=sys.stderr)
        return 1
    app = QApplication.instance() or QApplication(sys.argv[:1])
    window = SdkBrowser(types, api_output)
    if query:
        window.search.setText(query)
        exact = [t for t in sb.find(types, query) if query.lower() in (t.cpp.lower(), t.name.lower(), t.path.lower())]
        if exact:
            window.open(exact[0].cpp)
    window.show()
    return app.exec()
