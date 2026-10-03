#!/usr/bin/env python3
#
# SuperFW ROM manager: prepares an SD card for SuperFW from a ROM collection.
#
# Pick an input folder (your ROM collection, never modified) and an output
# folder (the SD card, or a folder to copy to it later). ROMs are identified
# (CRC32 against the No-Intro DATs), given their official names, sorted into one
# folder per console (GBA/, GB/, GBC/, NES/, ...), and copied together with their
# saves (to SAVEGAME/) and cheats; box art is generated in .superfw/art/.
#
# The work is done by superfw_romlib.Organizer (also used by rom-scraper.py
# --input/--output) in worker threads; progress reaches the UI via Qt signals.
#
# Requires PySide6 and Pillow.

import html
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import superfw_romlib as lib  # noqa: E402

from PySide6.QtCore import QObject, QSettings, Qt, QTimer, QUrl, Signal  # noqa: E402
from PySide6.QtGui import QDesktopServices, QFontDatabase  # noqa: E402
from PySide6.QtWidgets import (  # noqa: E402
  QApplication, QCheckBox, QComboBox, QFileDialog, QFrame, QGridLayout, QGroupBox,
  QHBoxLayout, QLabel, QLineEdit, QMainWindow, QMessageBox, QPlainTextEdit,
  QProgressBar, QPushButton, QScrollArea, QSizePolicy, QSpinBox, QVBoxLayout, QWidget)

APP_NAME = "SuperFW ROM Manager"
CACHE_DIR = os.environ.get("SUPERFW_SCRAPER_CACHE", lib.DEFAULT_CACHE)

# Counters shown in the progress panel: (key, caption)
TILES = [
  ("found", "ROMs found"), ("identified", "Identified"), ("unidentified", "Unidentified"),
  ("duplicates", "Duplicates (input)"), ("already", "Already in output"), ("copied", "Copied"),
  ("conflict", "Skipped (name taken)"), ("saves_copied", "Saves copied"),
  ("art_created", "Art created"), ("art_missing", "Art missing"), ("art_failed", "Art failed"),
  ("errors", "Errors"),
]

LOG_FILTERS = [("All messages", 0), ("Info and above", 1), ("Warnings and errors", 2), ("Errors only", 3)]
LOG_COLORS = {"debug": "#8a8a8a", "warn": "#c77700", "error": "#d32f2f"}


class Bridge(QObject, lib.Reporter):
  """Receives engine callbacks from any thread and forwards them as Qt signals
     (queued to the GUI thread). Log lines are batched."""
  progress = Signal(object)
  logs = Signal(object)
  finished = Signal(bool)

  def __init__(self):
    QObject.__init__(self)
    self._lock = threading.Lock()
    self._pending = []

  def on_log(self, level, msg):
    with self._lock:
      self._pending.append((level, msg))

  def on_progress(self, snap):
    self.progress.emit(snap)
    self.flush()

  def flush(self):
    with self._lock:
      batch, self._pending = self._pending, []
    if batch:
      self.logs.emit(batch)


class StatTile(QFrame):
  def __init__(self, caption):
    super().__init__()
    self.setObjectName("tile")
    lay = QVBoxLayout(self)
    lay.setContentsMargins(10, 6, 10, 6)
    lay.setSpacing(0)
    self.value = QLabel("0")
    f = self.value.font()
    f.setPointSizeF(f.pointSizeF() * 1.45)
    f.setBold(True)
    self.value.setFont(f)
    self.caption = QLabel(caption)
    self.caption.setObjectName("caption")
    lay.addWidget(self.value)
    lay.addWidget(self.caption)

  def set(self, n, alert=False):
    self.value.setText("{:,}".format(n))
    self.value.setStyleSheet("color: #d32f2f;" if alert and n else "")


class MainWindow(QMainWindow):
  def __init__(self):
    super().__init__()
    self.setWindowTitle(APP_NAME)
    self.settings = QSettings("superfw", "rom-manager")
    self.org = None
    self.thread = None
    self.bridge = None
    self.log_entries = []
    self.report_file = None
    self.t_start = None
    self._build()
    self._load_settings()
    self._set_running(False)
    self.timer = QTimer(self)
    self.timer.setInterval(200)
    self.timer.timeout.connect(self._tick)

  # -- UI construction --

  def _build(self):
    central = QWidget()
    root = QVBoxLayout(central)
    root.setContentsMargins(16, 14, 16, 14)
    root.setSpacing(10)

    title = QLabel("Prepare an SD card for SuperFW")
    tf = title.font()
    tf.setPointSizeF(tf.pointSizeF() * 1.35)
    tf.setBold(True)
    title.setFont(tf)
    sub = QLabel("Identifies your ROMs, gives them official No-Intro names, sorts them into one folder "
                 "per console, copies saves and cheats along and generates box art.")
    sub.setWordWrap(True)
    sub.setObjectName("caption")
    root.addWidget(title)
    root.addWidget(sub)

    # Folders
    gb = QGroupBox("Folders")
    g = QGridLayout(gb)
    g.setColumnStretch(1, 1)
    self.in_edit = QLineEdit()
    self.in_edit.setPlaceholderText("Folder with your ROM collection (read only)")
    self.out_edit = QLineEdit()
    self.out_edit.setPlaceholderText("SD card root, or an empty folder to copy to the card later")
    self.in_btn = QPushButton("Browse…")
    self.out_btn = QPushButton("Browse…")
    self.in_btn.clicked.connect(lambda: self._browse(self.in_edit, "Select the ROM collection folder"))
    self.out_btn.clicked.connect(lambda: self._browse(self.out_edit, "Select the output / SD card folder"))
    g.addWidget(QLabel("Input (ROMs):"), 0, 0)
    g.addWidget(self.in_edit, 0, 1)
    g.addWidget(self.in_btn, 0, 2)
    g.addWidget(QLabel("Output (SD card):"), 1, 0)
    g.addWidget(self.out_edit, 1, 1)
    g.addWidget(self.out_btn, 1, 2)
    root.addWidget(gb)

    # Options
    ob = QGroupBox("Options")
    og = QGridLayout(ob)
    og.setHorizontalSpacing(24)
    self.cb_art = QCheckBox("Download box art")
    self.cb_saves = QCheckBox("Copy saves")
    self.cb_cheats = QCheckBox("Copy cheats")
    self.cb_sfw = QCheckBox("Copy SuperFW emulators && cheat database from input .superfw")
    self.cb_clean = QCheckBox("Clean names of unidentified ROMs")
    self.cb_clean.setToolTip("Strips collection numbering such as “0145 - ”, “003 ” or “391.” from the "
                             "names of ROMs that could not be identified. Nothing else is changed.")
    self.cb_replace = QCheckBox("Replace existing files in output")
    self.cb_replace_saves = QCheckBox("Also replace existing save files")
    self.cb_dry = QCheckBox("Preview only (don't write anything)")
    self.cb_replace.setToolTip("ROMs, cheats, SuperFW files and box art whose name already exists in the "
                               "output with different content are overwritten.")
    self.cb_replace_saves.setToolTip("Saves are precious: existing save files are only overwritten "
                                     "when this is checked too.")
    self.cb_replace.toggled.connect(self._sync_replace)
    og.addWidget(self.cb_art, 0, 0)
    og.addWidget(self.cb_saves, 1, 0)
    og.addWidget(self.cb_cheats, 2, 0)
    og.addWidget(self.cb_sfw, 3, 0)
    og.addWidget(self.cb_clean, 0, 1)
    og.addWidget(self.cb_replace, 1, 1)
    ind = QHBoxLayout()
    ind.setContentsMargins(22, 0, 0, 0)
    ind.addWidget(self.cb_replace_saves)
    og.addLayout(ind, 2, 1)
    og.addWidget(self.cb_dry, 3, 1)
    row = QHBoxLayout()
    row.addWidget(QLabel("Threads:"))
    self.threads = QSpinBox()
    self.threads.setRange(1, 64)
    self.threads.setValue(lib.default_threads())
    row.addWidget(self.threads)
    row.addSpacing(24)
    row.addWidget(QLabel("Skip input folders:"))
    self.exclude_edit = QLineEdit("EMU")
    self.exclude_edit.setToolTip("Comma separated top-level input folders to ignore")
    self.exclude_edit.setMaximumWidth(220)
    row.addWidget(self.exclude_edit)
    row.addStretch(1)
    og.addLayout(row, 4, 0, 1, 2)
    root.addWidget(ob)

    # Progress
    pb = QGroupBox("Progress")
    pl = QVBoxLayout(pb)
    top = QHBoxLayout()
    self.phase_lbl = QLabel("Idle")
    pf = self.phase_lbl.font()
    pf.setBold(True)
    self.phase_lbl.setFont(pf)
    self.elapsed_lbl = QLabel("")
    self.elapsed_lbl.setObjectName("caption")
    top.addWidget(self.phase_lbl)
    top.addStretch(1)
    top.addWidget(self.elapsed_lbl)
    pl.addLayout(top)
    self.bar = QProgressBar()
    self.bar.setRange(0, 1000)
    self.bar.setValue(0)
    self.bar.setFormat("%p%")
    self.bar.setTextVisible(True)
    pl.addWidget(self.bar)
    self.item_lbl = QLabel("Choose the folders and press Start.")
    self.item_lbl.setObjectName("caption")
    self.item_lbl.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
    pl.addWidget(self.item_lbl)
    tiles = QGridLayout()
    tiles.setSpacing(6)
    self.tiles = {}
    for i, (key, cap) in enumerate(TILES):
      t = StatTile(cap)
      self.tiles[key] = t
      tiles.addWidget(t, i // 6, i % 6)
    pl.addLayout(tiles)
    root.addWidget(pb)

    # Log
    lb = QGroupBox("Log")
    ll = QVBoxLayout(lb)
    lt = QHBoxLayout()
    self.filter_cb = QComboBox()
    for name, _ in LOG_FILTERS:
      self.filter_cb.addItem(name)
    self.filter_cb.setCurrentIndex(1)
    self.filter_cb.currentIndexChanged.connect(self._rebuild_log)
    self.search_edit = QLineEdit()
    self.search_edit.setPlaceholderText("Filter log text…")
    self.search_edit.setClearButtonEnabled(True)
    self.search_edit.textChanged.connect(self._rebuild_log)
    self.report_btn = QPushButton("Open report")
    self.report_btn.setEnabled(False)
    self.report_btn.clicked.connect(self._open_report)
    lt.addWidget(QLabel("Show:"))
    lt.addWidget(self.filter_cb)
    lt.addWidget(self.search_edit, 1)
    lt.addWidget(self.report_btn)
    ll.addLayout(lt)
    self.log_view = QPlainTextEdit()
    self.log_view.setReadOnly(True)
    self.log_view.setLineWrapMode(QPlainTextEdit.NoWrap)
    self.log_view.setFont(QFontDatabase.systemFont(QFontDatabase.FixedFont))
    self.log_view.setMinimumHeight(90)
    ll.addWidget(self.log_view)
    root.addWidget(lb, 1)

    # Buttons
    br = QHBoxLayout()
    self.status_lbl = QLabel("")
    self.status_lbl.setWordWrap(True)
    self.status_lbl.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
    self.start_btn = QPushButton("Start")
    self.start_btn.setDefault(True)
    self.start_btn.setMinimumWidth(110)
    self.cancel_btn = QPushButton("Cancel")
    self.cancel_btn.setMinimumWidth(110)
    self.start_btn.clicked.connect(self.start)
    self.cancel_btn.clicked.connect(self.cancel)
    br.addWidget(self.status_lbl, 1)
    br.addWidget(self.cancel_btn)
    br.addWidget(self.start_btn)
    root.addLayout(br)

    # Scrollable, so that the window stays usable when tiled small.
    scroll = QScrollArea()
    scroll.setWidget(central)
    scroll.setWidgetResizable(True)
    scroll.setFrameShape(QFrame.NoFrame)
    self.setCentralWidget(scroll)
    self.setStyleSheet("""
      QLabel#caption { color: palette(placeholder-text); }
      QFrame#tile { border: 1px solid palette(mid); border-radius: 6px; background: palette(base); }
      QGroupBox { font-weight: bold; margin-top: 8px; }
      QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
      QGroupBox QWidget { font-weight: normal; }
    """)
    # Default size: what the content wants, within the available screen area.
    hint = central.sizeHint()
    w, h = max(1000, hint.width() + 24), hint.height() + 24
    scr = QApplication.primaryScreen()
    if scr is not None:
      avail = scr.availableGeometry()
      w, h = min(w, avail.width()), min(h, avail.height())
    self.resize(w, h)

  def _browse(self, edit, title):
    d = QFileDialog.getExistingDirectory(self, title, edit.text() or os.path.expanduser("~"))
    if d:
      edit.setText(d)

  def _sync_replace(self, on=None):
    on = self.cb_replace.isChecked()
    self.cb_replace_saves.setEnabled(on and not self._running)
    if not on:
      self.cb_replace_saves.setChecked(False)

  # -- settings --

  def _load_settings(self):
    s = self.settings
    self.in_edit.setText(s.value("input", ""))
    self.out_edit.setText(s.value("output", ""))
    for key, cb, default in self._option_boxes():
      cb.setChecked(s.value(key, default, type=bool))
    self.exclude_edit.setText(s.value("exclude", "EMU"))
    geo = s.value("geometry")
    if geo is not None:
      self.restoreGeometry(geo)

  def _save_settings(self):
    s = self.settings
    s.setValue("input", self.in_edit.text())
    s.setValue("output", self.out_edit.text())
    for key, cb, _ in self._option_boxes():
      s.setValue(key, cb.isChecked())
    s.setValue("exclude", self.exclude_edit.text())
    s.setValue("geometry", self.saveGeometry())

  def _option_boxes(self):
    # Replace options are deliberately not remembered (always start off).
    return [("art", self.cb_art, True), ("saves", self.cb_saves, True),
            ("cheats", self.cb_cheats, True), ("superfw", self.cb_sfw, True),
            ("clean", self.cb_clean, True), ("dry_run", self.cb_dry, False)]

  # -- running --

  _running = False

  def _set_running(self, running):
    self._running = running
    for w in (self.in_edit, self.out_edit, self.in_btn, self.out_btn, self.cb_art, self.cb_saves,
              self.cb_cheats, self.cb_sfw, self.cb_clean, self.cb_replace, self.cb_dry,
              self.threads, self.exclude_edit):
      w.setEnabled(not running)
    self._sync_replace()
    self.start_btn.setEnabled(not running)
    self.cancel_btn.setEnabled(running)

  def options(self):
    return lib.Options(
      self.in_edit.text().strip(), self.out_edit.text().strip(),
      replace=self.cb_replace.isChecked(), replace_saves=self.cb_replace_saves.isChecked(),
      art=self.cb_art.isChecked(), saves=self.cb_saves.isChecked(), cheats=self.cb_cheats.isChecked(),
      superfw_files=self.cb_sfw.isChecked(), clean_names=self.cb_clean.isChecked(),
      dry_run=self.cb_dry.isChecked(), threads=self.threads.value(), cache_dir=CACHE_DIR,
      exclude=[x for x in self.exclude_edit.text().split(",")])

  def _error(self, msg):
    self.status_lbl.setText(msg)
    self.status_lbl.setStyleSheet("color: #d32f2f;")
    if QApplication.platformName() != "offscreen":
      QMessageBox.warning(self, APP_NAME, msg)

  def start(self):
    if self._running:
      return
    inp, out = self.in_edit.text().strip(), self.out_edit.text().strip()
    if not inp or not os.path.isdir(inp):
      return self._error("Please choose an existing input folder.")
    if not out:
      return self._error("Please choose an output folder.")
    ai, ao = os.path.abspath(inp), os.path.abspath(out)
    if ai == ao:
      return self._error("Input and output must be different folders.")
    if (ai + os.sep).startswith(ao + os.sep):
      return self._error("The input folder cannot be inside the output folder.")
    self._save_settings()
    self.status_lbl.setStyleSheet("")
    self.status_lbl.setText("Running…")
    self.log_entries = []
    self.log_view.clear()
    self.report_btn.setEnabled(False)
    for t in self.tiles.values():
      t.set(0)
    self.bar.setValue(0)
    self.phase_lbl.setText("Starting…")

    self.bridge = Bridge()
    self.bridge.progress.connect(self._on_progress)
    self.bridge.logs.connect(self._on_logs)
    self.bridge.finished.connect(self._on_finished)
    self.org = lib.Organizer(self.options(), self.bridge)
    org, bridge = self.org, self.bridge

    def work():
      ok = False
      try:
        ok = org.run()
      finally:
        bridge.flush()
        bridge.finished.emit(ok)

    self.t_start = time.time()
    self._set_running(True)
    self.thread = threading.Thread(target=work, name="organizer", daemon=True)
    self.thread.start()
    self.timer.start()

  def cancel(self):
    if self.org and self._running:
      self.org.cancel()
      self.cancel_btn.setEnabled(False)
      self.phase_lbl.setText("Cancelling…")
      self.status_lbl.setText("Cancelling: finishing the files in progress…")

  def _tick(self):
    if self.bridge:
      self.bridge.flush()
    if self.t_start and self._running:
      self.elapsed_lbl.setText(_fmt_time(time.time() - self.t_start))

  def _on_progress(self, snap):
    if not self._running:
      return
    idx = snap["phase_idx"]
    if idx >= 0 and not (self.org and self.org.cancel_event.is_set()):
      self.phase_lbl.setText("Step %d of %d: %s" % (idx + 1, len(lib.PHASES), snap["phase"]))
    self.bar.setValue(int(snap["overall"] * 1000))
    item = snap["item"] or " "
    fm = self.item_lbl.fontMetrics()
    self.item_lbl.setText(fm.elidedText(item, Qt.ElideMiddle, max(100, self.item_lbl.width())))
    self._update_tiles(snap["counters"])

  def _update_tiles(self, c):
    for key, t in self.tiles.items():
      t.set(c.get(key, 0), alert=key in ("errors", "art_failed"))

  def _on_logs(self, batch):
    self.log_entries.extend(batch)
    lvl = LOG_FILTERS[self.filter_cb.currentIndex()][1]
    needle = self.search_edit.text().lower()
    lines = [self._fmt_entry(level, msg) for level, msg in batch
             if lib.LEVELS[level] >= lvl and (not needle or needle in msg.lower())]
    if lines:
      sb = self.log_view.verticalScrollBar()
      at_end = sb.value() >= sb.maximum() - 4
      self.log_view.appendHtml("<br>".join(lines))
      if at_end:
        sb.setValue(sb.maximum())

  @staticmethod
  def _fmt_entry(level, msg):
    text = html.escape(msg).replace(" ", "&nbsp;") or "&nbsp;"
    if level in ("warn", "error"):
      text = "<b>%s:</b> %s" % ("Warning" if level == "warn" else "Error", text)
    color = LOG_COLORS.get(level)
    return '<span style="color:%s">%s</span>' % (color, text) if color else text

  def _rebuild_log(self):
    lvl = LOG_FILTERS[self.filter_cb.currentIndex()][1]
    needle = self.search_edit.text().lower()
    lines = [self._fmt_entry(level, msg) for level, msg in self.log_entries
             if lib.LEVELS[level] >= lvl and (not needle or needle in msg.lower())]
    self.log_view.clear()
    for i in range(0, len(lines), 500):
      self.log_view.appendHtml("<br>".join(lines[i:i + 500]))
    self.log_view.verticalScrollBar().setValue(self.log_view.verticalScrollBar().maximum())

  def _on_finished(self, ok):
    org = self.org
    self.timer.stop()
    self._tick()
    snap = org.snapshot()
    self._update_tiles(snap["counters"])
    self._set_running(False)
    c = snap["counters"]
    elapsed = _fmt_time(time.time() - self.t_start)
    self.elapsed_lbl.setText(elapsed)
    if org.cancelled:
      self.phase_lbl.setText("Cancelled")
      self.status_lbl.setText("Cancelled after %s. Files already copied were kept; "
                              "no partial files were left behind." % elapsed)
    elif org.cancelled is None:
      self.phase_lbl.setText("Failed")
      self._error("The job failed, see the log for details.")
    else:
      self.bar.setValue(1000)
      self.phase_lbl.setText("Finished" + (" (preview only, nothing written)" if org.o.dry_run else ""))
      verb = "would copy" if org.o.dry_run else "copied"
      self.status_lbl.setText(
        "Done in %s: %d unique ROMs, %s %d, %d already in output, %d saves, %d box art created%s." % (
          elapsed, c.get("identified", 0) + c.get("unidentified", 0), verb, c.get("copied", 0),
          c.get("already", 0), c.get("saves_copied", 0), c.get("art_created", 0),
          (", %d errors" % c["errors"]) if c.get("errors") else ""))
      self.status_lbl.setStyleSheet("color: #d32f2f;" if c.get("errors") else "")
    self.item_lbl.setText(" ")
    self.report_file = org.report_file
    self.report_btn.setEnabled(bool(self.report_file))
    if self.report_file:
      self.report_btn.setToolTip(self.report_file)

  def _open_report(self):
    if self.report_file and os.path.exists(self.report_file):
      if not QDesktopServices.openUrl(QUrl.fromLocalFile(self.report_file)):
        try:
          subprocess.Popen(["xdg-open", self.report_file])
        except OSError:
          pass

  def closeEvent(self, ev):
    if self._running:
      if QApplication.platformName() != "offscreen":
        r = QMessageBox.question(self, APP_NAME, "A job is running. Cancel it and quit?")
        if r != QMessageBox.Yes:
          ev.ignore()
          return
      self.org.cancel()
      self.thread.join(15)
    self._save_settings()
    ev.accept()


def _fmt_time(sec):
  sec = int(sec)
  return "%d:%02d:%02d" % (sec // 3600, sec // 60 % 60, sec % 60) if sec >= 3600 else "%d:%02d" % (sec // 60, sec % 60)


def main():
  app = QApplication(sys.argv)
  app.setApplicationName(APP_NAME)
  w = MainWindow()
  w.show()
  return app.exec()


if __name__ == "__main__":
  sys.exit(main())
