#!/usr/bin/env python3
"""GoVR control panel: one window with every GoVR action (modes, video player, screen
placement, settings) and the headset status. Italian UI; everything it does goes through the
same scripts as the menu entries and shortcuts (scripts/govr-launch, pc/govr-ctl)."""
import os
import re
import subprocess
import sys
import threading
import time

from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QFont, QIcon
from PyQt6.QtWidgets import (QApplication, QCheckBox, QComboBox, QFileDialog, QGridLayout,
                             QGroupBox, QHBoxLayout, QLabel, QLineEdit, QMessageBox,
                             QPushButton, QRadioButton, QVBoxLayout, QWidget)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCH = os.path.join(ROOT, "scripts", "govr-launch")
CTL = os.path.join(ROOT, "pc", "govr-ctl")
CONF = os.path.join(ROOT, "govr.conf")
CAPTURE = os.path.join(ROOT, "build", "kwin-capture", "govr-kwin-capture")
LAYOUTS = [("360", "360°"), ("360tb", "360° 3D (sopra/sotto)"), ("180", "180°"),
           ("180sbs", "180° 3D / VR180 (affiancato)"), ("flat", "Video normale (schermo cinema)")]


def run_detached(*cmd):
    subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=True)


def ctl(cmd):
    subprocess.run([CTL, cmd], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read_conf():
    conf = {}
    try:
        for line in open(CONF):
            m = re.match(r"^([A-Z_]+)=(.*)$", line.strip())
            if m:
                conf[m.group(1)] = m.group(2)
    except OSError:
        pass
    return conf


def write_conf(updates):
    lines = open(CONF).read().splitlines()
    done = set()
    for i, line in enumerate(lines):
        m = re.match(r"^([A-Z_]+)=", line)
        if m and m.group(1) in updates:
            lines[i] = f"{m.group(1)}={updates[m.group(1)]}"
            done.add(m.group(1))
    lines += [f"{k}={v}" for k, v in updates.items() if k not in done]
    open(CONF, "w").write("\n".join(lines) + "\n")


class Status:
    """Polled in a background thread so the window never freezes on adb."""

    def __init__(self):
        self.connected = False
        self.battery = None
        self.mode = "—"
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        n = 0
        while True:
            try:
                r = subprocess.run(["adb", "get-state"], capture_output=True, text=True, timeout=5)
                self.connected = r.stdout.strip() == "device"
                if self.connected and n % 5 == 0:
                    b = subprocess.run(["adb", "shell", "dumpsys", "battery"], capture_output=True,
                                       text=True, timeout=5).stdout
                    m = re.search(r"\n\s+level: (\d+)", b)
                    self.battery = int(m.group(1)) if m else None
                elif not self.connected:
                    self.battery = None
                ps = subprocess.run(["ps", "-eo", "comm,args"], capture_output=True, text=True).stdout
                server = [l for l in ps.splitlines() if "govr_server.py" in l and "python" in l.split()[0]]
                vr = any(l.split()[0] == "vrserver" for l in ps.splitlines() if l.strip())
                nms = any(l.split()[0] == "NMS.exe" for l in ps.splitlines() if l.strip())
                if vr:
                    self.mode = "No Man's Sky (VR)" if nms else "SteamVR"
                elif any("--source url" in l for l in server):
                    self.mode = "Video VR"
                elif server:
                    self.mode = "Desktop"
                else:
                    self.mode = "spento"
            except Exception:
                pass
            n += 1
            time.sleep(2)


class Panel(QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("GoVR")
        self.setWindowIcon(QIcon.fromTheme("video-display"))
        self.status = Status()
        root = QVBoxLayout(self)

        # --- status
        self.lbl_status = QLabel("…")
        f = QFont(); f.setPointSize(11); f.setBold(True)
        self.lbl_status.setFont(f)
        root.addWidget(self.lbl_status)

        # --- modes
        box = QGroupBox("Modalità")
        g = QGridLayout(box)
        for i, (text, icon, mode) in enumerate([
                ("Desktop nel visore", "video-display", "desktop"),
                ("SteamVR (giochi VR)", "applications-games", "vr"),
                ("No Man's Sky VR", "applications-games", "nms"),
                ("Stop (chiudi tutto)", "process-stop", "stop")]):
            b = self.big_button(text, icon, lambda _, m=mode: run_detached(LAUNCH, m))
            g.addWidget(b, i // 2, i % 2)
        root.addWidget(box)

        # --- video
        box = QGroupBox("Video VR (YouTube, Vimeo, altri siti, file sul PC)")
        v = QVBoxLayout(box)
        row = QHBoxLayout()
        self.url = QLineEdit(); self.url.setPlaceholderText("Incolla un link o scegli un file…")
        row.addWidget(self.url)
        row.addWidget(self.button("Sfoglia…", "document-open", self.browse))
        v.addLayout(row)
        row = QHBoxLayout()
        row.addWidget(QLabel("Formato:"))
        self.layout_combo = QComboBox()
        for key, name in LAYOUTS:
            self.layout_combo.addItem(name, key)
        row.addWidget(self.layout_combo, 1)
        row.addWidget(self.button("Riproduci", "media-playback-start", self.play))
        row.addWidget(self.button("Applica formato al video in corso", "view-refresh",
                                  lambda: ctl("layout" + self.layout_combo.currentData())))
        v.addLayout(row)
        row = QHBoxLayout()
        for text, icon, cmd in [("−30 s", "media-skip-backward", "back30"),
                                ("−5 s", "media-seek-backward", "back"),
                                ("Pausa / Play", "media-playback-pause", "pause"),
                                ("+5 s", "media-seek-forward", "fwd"),
                                ("+30 s", "media-skip-forward", "fwd30")]:
            row.addWidget(self.button(text, icon, lambda _, c=cmd: ctl(c)))
        v.addLayout(row)
        root.addWidget(box)

        # --- screen
        box = QGroupBox("Schermo / vista nel visore")
        g = QGridLayout(box)
        for i, (text, icon, cmd) in enumerate([
                ("Ricentra", "zoom-fit-best", "recenter"), ("Più grande", "zoom-in", "bigger"),
                ("Più piccolo", "zoom-out", "smaller"), ("Più vicino", "go-down", "closer"),
                ("Più lontano", "go-up", "farther"), ("Più in alto", "arrow-up", "up"),
                ("Più in basso", "arrow-down", "down"), ("Sfondo sì/no", "games-config-background", "env"),
                ("Desktop sopra la VR", "window-duplicate", "overlay"),
                ("Posizione predefinita", "edit-undo", "reset")]):
            g.addWidget(self.button(text, icon, lambda _, c=cmd: ctl(c)), i // 5, i % 5)
        root.addWidget(box)

        # --- settings
        box = QGroupBox("Impostazioni (valgono dal prossimo avvio di una modalità)")
        v = QVBoxLayout(box)
        conf = read_conf()
        row = QHBoxLayout()
        row.addWidget(QLabel("Monitor da mostrare:"))
        self.monitor = QComboBox()
        for name, label in self.list_outputs():
            self.monitor.addItem(label, ("OUTPUT", name))
        self.monitor.addItem("Monitor virtuale 1920×1080 (solo per il visore)", ("VIRTUAL", "1920x1080"))
        cur = ("VIRTUAL", conf["VIRTUAL"]) if conf.get("VIRTUAL") else ("OUTPUT", conf.get("OUTPUT", "DP-3"))
        for i in range(self.monitor.count()):
            if self.monitor.itemData(i) == cur:
                self.monitor.setCurrentIndex(i)
        row.addWidget(self.monitor, 1)
        v.addLayout(row)
        self.audio_all = QRadioButton("Tutto l'audio del PC nel visore")
        self.audio_choose = QRadioButton("Solo le app che sposto su \"GoVR-Headset\"")
        (self.audio_all if conf.get("AUDIO", "all") == "all" else self.audio_choose).setChecked(True)
        row = QHBoxLayout(); row.addWidget(self.audio_all); row.addWidget(self.audio_choose)
        v.addLayout(row)
        self.mute = QCheckBox("Silenzia le casse del PC durante l'uso")
        self.mute.setChecked(conf.get("MUTE_PC") == "yes")
        v.addWidget(self.mute)
        v.addWidget(self.button("Salva impostazioni", "document-save", self.save_settings))
        root.addWidget(box)

        # --- footer
        row = QHBoxLayout()
        row.addWidget(self.button("Guida", "help-contents",
                                  lambda: run_detached("xdg-open", os.path.join(ROOT, "HOW_TO_USE.md"))))
        row.addWidget(self.button("Registro (log)", "text-x-log",
                                  lambda: run_detached("xdg-open", os.path.join(ROOT, "logs", "launcher.log"))))
        row.addStretch(1)
        root.addLayout(row)

        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(1000)
        self.refresh()

    # --- helpers
    def button(self, text, icon, slot):
        b = QPushButton(QIcon.fromTheme(icon), text)
        b.clicked.connect(slot)
        return b

    def big_button(self, text, icon, slot):
        b = self.button(text, icon, slot)
        b.setMinimumHeight(48)
        f = b.font(); f.setPointSize(f.pointSize() + 2); b.setFont(f)
        return b

    def list_outputs(self):
        try:
            out = subprocess.run([CAPTURE, "--list"], capture_output=True, text=True, timeout=5).stdout
        except Exception:
            out = ""
        res = []
        for line in out.splitlines():
            parts = line.split(None, 2)
            if len(parts) >= 2 and parts[0] != "screencast":
                res.append((parts[0], f"{parts[0]} — {parts[2] if len(parts) > 2 else ''} ({parts[1]})"))
        return res or [("DP-3", "DP-3")]

    def refresh(self):
        s = self.status
        if not s.connected:
            text = "Visore: NON collegato (accendilo e collega il cavo USB)"
        else:
            bat = f"{s.battery}%" if s.battery is not None else "?"
            warn = "  ⚠ batteria bassa" if s.battery is not None and s.battery < 25 else ""
            text = f"Visore: collegato · Batteria: {bat}{warn} · Modalità: {s.mode}"
        self.lbl_status.setText(text)

    # --- actions
    def browse(self):
        f, _ = QFileDialog.getOpenFileName(self, "Scegli un video", os.path.expanduser("~"),
                                           "Video (*.mp4 *.mkv *.webm *.mov *.avi);;Tutti i file (*)")
        if f:
            self.url.setText(f)

    def play(self):
        url = self.url.text().strip()
        if not url:
            QMessageBox.information(self, "GoVR", "Incolla un link o scegli un file video.")
            return
        run_detached(LAUNCH, "video", url, self.layout_combo.currentData())

    def save_settings(self):
        kind, value = self.monitor.currentData()
        write_conf({"OUTPUT": value if kind == "OUTPUT" else read_conf().get("OUTPUT", "DP-3"),
                    "VIRTUAL": value if kind == "VIRTUAL" else "",
                    "AUDIO": "all" if self.audio_all.isChecked() else "choose",
                    "MUTE_PC": "yes" if self.mute.isChecked() else "no"})
        QMessageBox.information(self, "GoVR", "Impostazioni salvate.\nValgono dal prossimo avvio "
                                              "di una modalità (es. clic su \"Desktop nel visore\").")


if __name__ == "__main__":
    app = QApplication(sys.argv)
    app.setDesktopFileName("govr-panel")
    w = Panel()
    w.resize(760, 640)
    w.show()
    sys.exit(app.exec())
