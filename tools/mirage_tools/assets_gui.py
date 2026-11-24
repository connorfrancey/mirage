"""The asset catalog's window (`mirage assets`): the game's textures, sounds, meshes, animations and materials.

The catalog (build/asset-catalog.sqlite, table `assets`, made by `mirage_assets catalog`) holds one row per asset with
its details. On the left, the assets matching the search and kind; on the right, the selected one: its details, the
objects it uses and the assets that use it (both open on a double click), and a preview. A texture shows its image, a
material its first texture, a sound plays. Previews are made by `mirage_assets preview` in the background and kept in
build/asset-previews.
"""

from __future__ import annotations

import csv
import html
import json
import os
import shutil
import sqlite3
from contextlib import closing
from pathlib import Path
from tempfile import NamedTemporaryFile

from PyQt6.QtCore import QAbstractTableModel, QModelIndex, QProcess, Qt, QTimer, QUrl
from PyQt6.QtGui import QAction, QFont, QKeySequence, QPixmap
from PyQt6.QtWidgets import (QAbstractItemView, QApplication, QComboBox, QFileDialog, QHBoxLayout, QHeaderView, QLabel,
                             QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QMessageBox, QPushButton, QSplitter,
                             QTableView, QTableWidget, QTableWidgetItem, QTabWidget, QVBoxLayout, QWidget)

try:    # Qt's FFmpeg backend plays the game's Ogg Vorbis; without QtMultimedia, Play opens the system's player
    from PyQt6.QtMultimedia import QAudioOutput, QMediaPlayer
except ImportError:     # pragma: no cover
    QMediaPlayer = None

MAX_ROWS = 5000
KINDS = ["All", "Texture", "Sound", "Mesh", "Animation", "Material"]
_LIGHTMAP = "(class like '%LightMap%' or class like 'ShadowMap%')"
# Texture types: the SQL that picks them (normal maps by their compression, or named _N like the rest)
TEXTURE_TYPES = {
    "Any texture": None,
    "Normal maps": r"(info like '%TC_Normalmap%' or full like '%\_N' escape '\')",
    "Diffuse (_D)": r"full like '%\_D' escape '\'",
    "Specular (_S)": r"full like '%\_S' escape '\'",
    "Without lightmaps": f"not {_LIGHTMAP}",
    "Lightmaps, shadow maps": _LIGHTMAP,
    "Cube maps": "class = 'TextureCube'",
}
COLUMNS = ["Name", "Kind", "Class", "Summary", "Read from", "Size"]
SQL_COLUMNS = ["full", "kind", "class", "summary", "package", "bytes"]    # What a click on a header sorts by


def load_catalog(jsonl: Path, db_path: Path) -> int:
    """`mirage_assets catalog`'s lines into SQLite; returns the number of assets.

    Written beside db_path and swapped in at the end, so an open asset window keeps the old catalog until then; the
    swap fails (PermissionError) while a window has it open.
    """
    rows, copies = [], {}
    with jsonl.open(encoding="utf-8", errors="replace") as lines:
        for line in lines:
            obj = json.loads(line)
            if "copies" in obj:
                copies = obj["copies"]
                continue
            rows.append(obj)
    with NamedTemporaryFile(dir=db_path.parent, suffix=".sqlite", delete=False) as file:
        temporary = Path(file.name)
    try:
        with closing(sqlite3.connect(temporary)) as db, db:
            # full: the asset's own "Package.Group.Name"; package and path identify the copy that was read.
            db.execute("create table assets(kind text, full text, package text, path text, class text, bytes integer, "
                       "summary text, info text, refs text, copies integer)")
            db.executemany("insert into assets values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           ((r["kind"], r["full"], r["package"], r["path"], r["class"], r["bytes"], r["summary"],
                             json.dumps(r["info"]), json.dumps(r["refs"]), copies.get(f"{r['class']} {r['full']}", 0) + 1)
                            for r in rows))
            db.execute("create index assets_kind on assets(kind)")
            db.execute("create index assets_full on assets(full collate nocase)")
            db.execute("create table asset_refs(target text collate nocase, source integer, "
                       "primary key(target, source)) without rowid")
            db.execute("insert or ignore into asset_refs select ref.value, assets.rowid "
                       "from assets, json_each(assets.refs) as ref")
        os.replace(temporary, db_path)
    finally:
        temporary.unlink(missing_ok=True)
    return len(rows)


CONTENT_HINTS = {
    "texture": "A replacement image in the mod's content folder (PNG, TGA, BMP, JPG or DDS), named by the texture's "
               "full path",
    "mesh": "A replacement model in the mod's content folder (glTF, FBX or OBJ; `mirage model <file> --convert`), "
            "named by the mesh's full path",
    "defaults": "Property overrides: this table in the mod's content/defaults.toml",
}


def content_kind(a: dict) -> str:
    if a["kind"] == "Texture" and a["class"] != "TextureCube":
        return "texture"
    return "mesh" if a["kind"] == "Mesh" else "defaults"


def content_path(a: dict) -> str:
    """Where a content mod replaces or changes this asset (see loader/src/runtime/content.h)."""
    return {"texture": f"content/textures/{a['full']}.png", "mesh": f"content/meshes/{a['full']}.glb",
            "defaults": f'content/defaults.toml: ["{a["full"]}"]'}[content_kind(a)]


class Rows(QAbstractTableModel):
    def __init__(self) -> None:
        super().__init__()
        self.rows: list[tuple] = []     # rowid, then COLUMNS

    def set_rows(self, rows: list[tuple]) -> None:
        self.beginResetModel()
        self.rows = rows
        self.endResetModel()

    def rowCount(self, parent: QModelIndex = QModelIndex()) -> int:   # noqa: N802 (Qt's name)
        return 0 if parent.isValid() else len(self.rows)

    def columnCount(self, parent: QModelIndex = QModelIndex()) -> int:   # noqa: N802
        return len(COLUMNS)

    def data(self, index: QModelIndex, role: int = Qt.ItemDataRole.DisplayRole):
        if role != Qt.ItemDataRole.DisplayRole:
            return None
        value = self.rows[index.row()][index.column() + 1]
        return f"{value / 1024:,.1f} KB" if index.column() == 5 else value

    def headerData(self, section: int, orientation: Qt.Orientation, role: int = Qt.ItemDataRole.DisplayRole):  # noqa: N802
        if role == Qt.ItemDataRole.DisplayRole and orientation == Qt.Orientation.Horizontal:
            return COLUMNS[section]
        return None


class AssetBrowser(QMainWindow):
    def __init__(self, db_path: Path, tool: Path, cooked: Path, previews: Path) -> None:
        super().__init__()
        self.db = sqlite3.connect(f"{db_path.resolve().as_uri()}?mode=ro", uri=True)
        self.indexed_refs = self.db.execute(
            "select 1 from sqlite_master where type = 'table' and name = 'asset_refs'"
        ).fetchone() is not None
        self.tool, self.cooked, self.previews = tool, cooked, previews
        self.current: dict | None = None
        self.process: QProcess | None = None
        self.preview_file: Path | None = None
        self.history: list[int] = []
        self.position = -1
        total = self.db.execute("select count(*) from assets").fetchone()[0]
        self.setWindowTitle(f"Mirage asset catalog: {total:,} assets")
        self.resize(1500, 900)
        mono = QFont("Consolas")
        mono.setStyleHint(QFont.StyleHint.Monospace)

        # ---- Left: search and results
        self.search = QLineEdit(placeholderText="Search names, classes, packages and summaries (Ctrl+F)")
        self.search.setClearButtonEnabled(True)
        self.kind = QComboBox()
        self.kind.addItems(KINDS)
        self.texture_type = QComboBox()
        self.texture_type.addItems(list(TEXTURE_TYPES))
        self.texture_type.setToolTip("Which textures (picking one shows textures only)")
        self.model = Rows()
        self.table = QTableView()
        self.table.setModel(self.model)
        # Sorted by the database, so the order covers every match, not only the rows shown
        header = self.table.horizontalHeader()
        header.setSortIndicatorShown(True)
        header.setSectionsClickable(True)
        header.setSortIndicator(0, Qt.SortOrder.AscendingOrder)
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.table.verticalHeader().setVisible(False)
        self.table.verticalHeader().setDefaultSectionSize(20)
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Interactive)
        self.table.horizontalHeader().setStretchLastSection(True)
        self.count = QLabel()
        self.export = QPushButton("Export CSV…")
        self.export.setToolTip("Export every matching asset in the current sort order")
        left = QWidget()
        lv = QVBoxLayout(left)
        lv.setContentsMargins(0, 0, 0, 0)
        row = QHBoxLayout()
        row.addWidget(self.search, 1)
        row.addWidget(self.kind)
        row.addWidget(self.texture_type)
        lv.addLayout(row)
        lv.addWidget(self.table)
        footer = QHBoxLayout()
        footer.addWidget(self.count, 1)
        footer.addWidget(self.export)
        lv.addLayout(footer)

        # ---- Right: the asset
        self.title = QLabel(textFormat=Qt.TextFormat.RichText)
        self.title.setWordWrap(True)
        self.title.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        # Where a content mod puts its replacement of this asset (runtime/content.h names files by the full path)
        self.content_path = QLineEdit(readOnly=True)
        self.content_path.setFont(mono)
        self.copy_path = QPushButton("Copy")
        content_row = QHBoxLayout()
        content_row.addWidget(QLabel("Content mod:"))
        content_row.addWidget(self.content_path, 1)
        content_row.addWidget(self.copy_path)
        self.image = QLabel(alignment=Qt.AlignmentFlag.AlignCenter)
        self.image.setMinimumHeight(260)
        self.status = QLabel()
        self.play = QPushButton("▶ Play")
        self.stop = QPushButton("■ Stop")
        self.save = QPushButton("Save preview…")
        self.open_file = QPushButton("Open file")
        buttons = QHBoxLayout()
        for b_ in (self.play, self.stop, self.save, self.open_file):
            buttons.addWidget(b_)
        buttons.addStretch(1)
        self.info = QTableWidget(0, 2)
        self.info.setHorizontalHeaderLabels(["Property", "Value"])
        self.info.setFont(mono)
        self.info.verticalHeader().setVisible(False)
        self.info.verticalHeader().setDefaultSectionSize(20)
        self.info.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.info.horizontalHeader().setStretchLastSection(True)
        self.info.setWordWrap(True)
        self.uses = QListWidget()
        self.used_by = QListWidget()
        for lst in (self.uses, self.used_by):
            lst.setFont(mono)
        self.tabs = QTabWidget()
        self.tabs.addTab(self.info, "Details")
        self.tabs.addTab(self.uses, "Uses")
        self.tabs.addTab(self.used_by, "Used by")

        right = QWidget()
        rv = QVBoxLayout(right)
        rv.setContentsMargins(0, 0, 0, 0)
        rv.addWidget(self.title)
        rv.addLayout(content_row)
        rv.addWidget(self.image, 2)
        rv.addWidget(self.status)
        rv.addLayout(buttons)
        rv.addWidget(self.tabs, 3)

        split = QSplitter()
        split.addWidget(left)
        split.addWidget(right)
        split.setSizes([800, 700])
        self.setCentralWidget(split)

        toolbar = self.addToolBar("Navigation")
        toolbar.setMovable(False)
        self.back = QAction("◀ Back", self, shortcut=QKeySequence("Alt+Left"), triggered=lambda: self._step(-1))
        self.forward = QAction("Forward ▶", self, shortcut=QKeySequence("Alt+Right"), triggered=lambda: self._step(1))
        toolbar.addAction(self.back)
        toolbar.addAction(self.forward)
        self.addAction(QAction(self, shortcut=QKeySequence("Ctrl+F"), triggered=self._focus_search))

        self.player = None
        if QMediaPlayer is not None:
            self.player = QMediaPlayer(self)
            self.audio = QAudioOutput(self)
            self.player.setAudioOutput(self.audio)
            self.player.errorOccurred.connect(lambda _e, text: self.status.setText(f"Can't play: {text}"))

        self.search_timer = QTimer(self, singleShot=True, interval=150)
        self.search_timer.timeout.connect(self.refresh)
        self.search.returnPressed.connect(self.refresh)
        self.search.textChanged.connect(lambda: self.search_timer.start())
        self.kind.currentIndexChanged.connect(self.refresh)
        self.texture_type.currentIndexChanged.connect(self.refresh)
        self.export.clicked.connect(self._export_results)
        self.copy_path.clicked.connect(lambda: QApplication.clipboard().setText(self.content_path.text()))
        header.sortIndicatorChanged.connect(lambda _column, _order: self.refresh())
        self.table.selectionModel().currentRowChanged.connect(self._picked)
        self.uses.itemActivated.connect(self._ref_activated)
        self.used_by.itemActivated.connect(self._ref_activated)
        self.play.clicked.connect(self._play)
        self.stop.clicked.connect(lambda: self.player and self.player.stop())
        self.save.clicked.connect(self._save)
        self.open_file.clicked.connect(lambda: self.preview_file and os.startfile(self.preview_file))   # noqa: S606
        self.refresh()
        self.show_asset(None)
        self._update_nav()

    def _focus_search(self) -> None:
        self.search.setFocus()
        self.search.selectAll()

    def closeEvent(self, event) -> None:
        self.search_timer.stop()
        process, self.process = self.process, None
        if process:
            process.kill()
            process.waitForFinished(1000)
        self.db.close()
        super().closeEvent(event)

    # ---- Results ----------------------------------------------------------------------------------------------------

    def _results_query(self) -> tuple[str, list[str], str]:
        where, args = [], []
        for term in self.search.text().lower().split():
            where.append("instr(lower(full || ' ' || class || ' ' || package || ' ' || summary), ?) > 0")
            args.append(term)
        kind = self.kind.currentText()
        texture_type = TEXTURE_TYPES[self.texture_type.currentText()] if kind in ("All", "Texture") else None
        if texture_type:
            kind = "Texture"
            where.append(texture_type)
        if kind != "All":
            where.append("kind = ?")
            args.append(kind)
        sql = "from assets"
        if where:
            sql += " where " + " and ".join(where)
        header = self.table.horizontalHeader()
        order = "desc" if header.sortIndicatorOrder() == Qt.SortOrder.DescendingOrder else "asc"
        column = SQL_COLUMNS[max(0, min(header.sortIndicatorSection(), len(SQL_COLUMNS) - 1))]
        return sql, args, f"order by {column} {order}, full, rowid"

    def refresh(self) -> None:
        self.search_timer.stop()
        self.texture_type.setEnabled(self.kind.currentText() in ("All", "Texture"))
        where, args, order = self._results_query()
        total = self.db.execute(f"select count(*) {where}", args).fetchone()[0]
        columns = ", ".join(SQL_COLUMNS)
        self.model.set_rows(self.db.execute(
            f"select rowid, {columns} {where} {order} limit {MAX_ROWS}", args
        ).fetchall())
        self.table.resizeColumnToContents(0)
        self.table.resizeColumnToContents(1)
        self.table.resizeColumnToContents(2)
        self.count.setText(f"{total:,} asset(s)" + (f", the first {MAX_ROWS:,} shown" if total > MAX_ROWS else ""))
        self.export.setEnabled(total > 0)

    def _export_results(self) -> None:
        target, _ = QFileDialog.getSaveFileName(self, "Export matching assets", "assets.csv", "CSV files (*.csv)")
        if not target:
            return
        where, args, order = self._results_query()
        columns = ", ".join(SQL_COLUMNS)
        try:
            with Path(target).open("w", encoding="utf-8-sig", newline="") as file:
                writer = csv.writer(file)
                writer.writerow([*COLUMNS[:-1], "Size (bytes)", "Object path in package", "Copies"])
                writer.writerows(self.db.execute(f"select {columns}, path, copies {where} {order}", args))
        except OSError as exc:
            QMessageBox.warning(self, "Could not export assets", str(exc))

    def _picked(self, current: QModelIndex, _previous: QModelIndex) -> None:
        if current.isValid():
            self.open(self.model.rows[current.row()][0])

    # ---- One asset --------------------------------------------------------------------------------------------------

    def _row(self, rowid: int) -> dict | None:
        cursor = self.db.execute("select rowid, kind, full, package, path, class, bytes, summary, info, refs, copies "
                                 "from assets where rowid = ?", (rowid,))
        row = cursor.fetchone()
        return dict(zip([d[0] for d in cursor.description], row)) if row else None

    def find(self, ref: str) -> int | None:
        """The catalog row of a qualified object path ("Package.Group.Name")."""
        row = self.db.execute("select rowid from assets where full = ? collate nocase order by rowid limit 1",
                              (ref,)).fetchone()
        return row[0] if row else None

    def _users_of(self, ref: str) -> list[tuple]:
        if self.indexed_refs:
            sql = ("select a.rowid, a.full, a.kind from asset_refs r join assets a on a.rowid = r.source "
                   "where r.target = ?")
        else:
            sql = ("select a.rowid, a.full, a.kind from assets a where exists "
                   "(select 1 from json_each(a.refs) where value = ? collate nocase)")
        return self.db.execute(sql + " order by a.full collate nocase, a.rowid limit 501", (ref,)).fetchall()

    def open(self, rowid: int, record: bool = True) -> None:
        if record and (not self.history or self.history[self.position] != rowid):
            del self.history[self.position + 1:]
            self.history.append(rowid)
            self.position = len(self.history) - 1
        self.show_asset(self._row(rowid))
        self._update_nav()

    def _step(self, delta: int) -> None:
        if 0 <= self.position + delta < len(self.history):
            self.position += delta
            self.open(self.history[self.position], record=False)

    def _update_nav(self) -> None:
        self.back.setEnabled(self.position > 0)
        self.forward.setEnabled(self.position < len(self.history) - 1)

    def show_asset(self, a: dict | None) -> None:
        process, self.process = self.process, None
        if process:
            process.kill()
        self.current = a
        if self.player:
            self.player.stop()
        self.preview_file = None
        self.image.clear()
        for w in (self.play, self.stop, self.save, self.open_file):
            w.setVisible(False)
        if not a:
            self.title.setText("<i>Pick an asset on the left</i>")
            self.status.clear()
            self.content_path.clear()
            return
        self.content_path.setText(content_path(a))
        self.content_path.setToolTip(CONTENT_HINTS.get(content_kind(a), ""))
        copies = f", in {a['copies']} packages" if a["copies"] > 1 else ""
        self.title.setText(f"<b style='font-size:15px'>{html.escape(a['full'])}</b><br>{a['kind']} · {a['class']} · "
                           f"read from <b>{html.escape(a['package'])}</b> · {a['bytes'] / 1024:,.1f} KB{copies}<br>"
                           f"{html.escape(a['summary'])}")
        info = json.loads(a["info"])
        self.info.setRowCount(len(info))
        for i, (name, value) in enumerate(info):
            self.info.setItem(i, 0, QTableWidgetItem(name))
            self.info.setItem(i, 1, QTableWidgetItem(f"{value:g}" if isinstance(value, float) else str(value)))
        self.info.resizeColumnToContents(0)
        self.info.resizeRowsToContents()

        refs = json.loads(a["refs"])
        self.uses.clear()
        for ref in refs:
            rowid = self.find(ref)
            item = QListWidgetItem(ref if rowid else f"{ref}   (not in the catalog)")
            item.setData(Qt.ItemDataRole.UserRole, rowid)
            self.uses.addItem(item)
        self.used_by.clear()
        users = self._users_of(a["full"])
        for rowid, full, kind in users[:500]:
            item = QListWidgetItem(f"{kind:<10} {full}")
            item.setData(Qt.ItemDataRole.UserRole, rowid)
            self.used_by.addItem(item)
        self.tabs.setTabText(1, f"Uses ({self.uses.count()})")
        self.tabs.setTabText(2, f"Used by ({self.used_by.count()}{'+' if len(users) > 500 else ''})")

        # The preview: the texture itself, a material's first texture, the sound
        target = a
        if a["kind"] == "Material":
            textures = [r for r in (self._row(rowid) for rowid in map(self.find, refs) if rowid)
                        if r and r["class"] == "Texture2D"]
            # Its diffuse texture when one is named so (T_Bag_D), not the normal or specular map
            textures.sort(key=lambda r: not r["path"].lower().endswith(("_d", "diffuse")))
            target = textures[0] if textures else None
        if target and (target["kind"] in ("Sound", "Mesh") or target["class"] != "TextureCube" and target["kind"] == "Texture"):
            self._preview(target)
        else:
            self.status.setText("No preview for this kind of asset")

    def _ref_activated(self, item: QListWidgetItem) -> None:
        if rowid := item.data(Qt.ItemDataRole.UserRole):
            self.open(rowid)

    # ---- Previews ---------------------------------------------------------------------------------------------------

    def _preview(self, a: dict) -> None:
        stem = f"{a['package']}.{a['path']}"
        for ext in ("png", "ogg", "wav"):
            if (cached := self.previews / f"{stem}.{ext}").exists():
                self._show_preview(cached, a)
                return
        self.status.setText(f"Making the preview of {a['full']}…")
        process = QProcess(self)
        self.process = process
        target = dict(a)

        def finished(_code: int, _status: QProcess.ExitStatus) -> None:
            process.deleteLater()
            if process is not self.process:     # A newer asset was picked meanwhile
                return
            self.process = None
            out = bytes(process.readAllStandardOutput()).decode(errors="replace").strip().splitlines()
            err = bytes(process.readAllStandardError()).decode(errors="replace").strip()
            if process.exitCode() == 0 and out and Path(out[-1]).exists():
                self._show_preview(Path(out[-1]), target)
            else:
                self.status.setText(f"No preview: {err or 'mirage_assets failed'}")

        def failed(error: QProcess.ProcessError) -> None:
            if error == QProcess.ProcessError.FailedToStart:
                if process is self.process:
                    self.process = None
                    self.status.setText(f"No preview: {process.errorString()}")
                process.deleteLater()

        process.finished.connect(finished)
        process.errorOccurred.connect(failed)
        process.start(str(self.tool), ["preview", str(self.cooked), a["package"], a["path"], str(self.previews)])

    def _show_preview(self, file: Path, a: dict) -> None:
        self.preview_file = file
        self.save.setVisible(True)
        self.open_file.setVisible(True)
        if file.suffix == ".png":
            pixmap = QPixmap(str(file))
            self.image.setPixmap(pixmap.scaled(min(512, pixmap.width()), min(512, pixmap.height()),
                                               Qt.AspectRatioMode.KeepAspectRatio,
                                               Qt.TransformationMode.SmoothTransformation))
            note = f" (from {a['full']})" if self.current and a["full"] != self.current["full"] else ""
            what = "LOD 0, rendered" if a["kind"] == "Mesh" else f"{pixmap.width()}x{pixmap.height()} preview"
            self.status.setText(what + note)
        else:
            self.play.setVisible(True)
            self.stop.setVisible(self.player is not None)
            self.status.setText(f"{file.suffix[1:].upper()} sound, {file.stat().st_size / 1024:,.1f} KB")

    def _play(self) -> None:
        if not self.preview_file:
            return
        if self.player is None:
            os.startfile(self.preview_file)     # noqa: S606
            return
        self.player.setSource(QUrl.fromLocalFile(str(self.preview_file)))
        self.player.play()

    def _save(self) -> None:
        if not self.preview_file:
            return
        target, _ = QFileDialog.getSaveFileName(self, "Save the preview", self.preview_file.name)
        if target:
            shutil.copyfile(self.preview_file, target)


def run(db_path: Path, tool: Path, cooked: Path, previews: Path, query: str | None = None) -> int:
    app = QApplication.instance() or QApplication([])
    previews.mkdir(parents=True, exist_ok=True)
    window = AssetBrowser(db_path, tool, cooked, previews)
    if query:
        window.search.setText(query)
    window.show()
    return app.exec()
