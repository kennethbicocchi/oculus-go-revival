"""Xbox pad -> virtual VR controllers (SteamVR dashboard pointer, game menus, VR controllers).

The SteamVR dashboard (library, game launch, Steam dialogs) and games made for motion
controllers only accept VR-controller input, and the Go has none in SteamVR mode. This drives
virtual controllers in the headset (protocol VR_POINTER, sent to ALVR by the Go client):

SteamVR dashboard open (automatic):
  look at it        the laser follows your gaze
  left stick        nudges the laser away from the gaze;  Y / X: back to the gaze
  A                 click (trigger)
  right stick, D-pad  scroll
  B                 close the dashboard
  View + Menu       open / close the dashboard (also during a game)

In a game, LS + RS (press both sticks) cycles:
  off               the game sees only the gamepad (default)
  pointer           right controller as a laser pointer, as in the dashboard, for game menus
                    that need one (e.g. The Forest's main menu)
  VR controllers    the gamepad becomes a pair of Touch controllers, for games without gamepad
                    support in VR (e.g. The Forest): sticks -> thumbsticks, A/B -> right A/B,
                    X/Y -> left X/Y, RT/LT -> triggers, RB/LB -> grips, Menu -> left menu,
                    stick clicks -> thumbstick clicks, D-pad -> right thumbstick
The current mode is shown on the headset for a few seconds.

When nothing is active the controllers disconnect, so games only see the gamepad. When a game
starts, the dashboard is closed, the controllers stay off for GAME_QUIET_S (so the game
initializes its input without VR controllers, else it may ignore the pad) and the game window
gets the keyboard focus (games ignore the pad while unfocused). Games listed in GAME_MODES
start directly in their mode instead (none at the moment).
Works with the pad on the PC (evdev, read-only) or paired to the headset (GAMEPAD messages).
Dashboard visibility comes from OpenVR (pyopenvr, background application).
"""
import glob
import math
import os
import struct
import subprocess
import threading
import time

# flags, yaw, pitch, buttons, rx, ry, lx, ly, rtrigger, ltrigger, rgrip, lgrip (docs/PROTOCOL.md)
FORMAT = "<IffI8f"
BTN = {"A": 1, "B": 2, "X": 4, "Y": 8, "MENU": 16, "SYSTEM": 32, "LS": 64, "RS": 128}
RATE_HZ = 60
DEADZONE = 0.2
AIM_SPEED = 1.3                  # rad/s at full deflection
YAW_LIMIT, PITCH_LIMIT = 1.2, 0.9
GAME_QUIET_S = 20
MODES = ["off", "pointer", "controllers"]
MODE_TEXT = {"off": "Joypad: comandi del gioco\nLS+RS: puntatore",
             "pointer": "Joypad: PUNTATORE\nstick sx mira, A clic\nLS+RS: controller VR",
             "controllers": "Joypad: CONTROLLER VR\nmenu: croce mira, RT clic\nStart: pausa"}
# Games that start in a given mode (OpenVR application key). Empty: The Forest gets real
# gamepad bindings instead (pc/vr_bindings.py).
GAME_MODES = {}
FOCUS_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "kwin-focus-game.js")


def find_vrcmd():
    """SteamVR's vrcmd (GOVR_STEAMVR from go-steamvr, else the default Steam library)."""
    roots = [os.environ.get("GOVR_STEAMVR", "")] + glob.glob(
        os.path.expanduser("~/.local/share/Steam/steamapps/common/SteamVR"))
    for r in roots:
        exe = os.path.join(r, "bin", "linux64", "vrcmd")
        if r and os.access(exe, os.X_OK):
            return exe
    return None


def focus_game_window():
    """Give the keyboard focus to the running Steam game's window (KWin script over D-Bus)."""
    for qdbus in ("qdbus6", "qdbus"):
        try:
            subprocess.run([qdbus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.unloadScript",
                            "govr-focus"], capture_output=True, timeout=5)
            sid = subprocess.run([qdbus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.loadScript",
                                  FOCUS_SCRIPT, "govr-focus"], capture_output=True, text=True,
                                 timeout=5).stdout.strip()
            if sid:
                subprocess.run([qdbus, "org.kde.KWin", f"/Scripting/Script{sid}", "org.kde.kwin.Script.run"],
                               capture_output=True, timeout=5)
            subprocess.run([qdbus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.unloadScript",
                            "govr-focus"], capture_output=True, timeout=5)
            return
        except (OSError, subprocess.TimeoutExpired):
            continue


class PadState:
    """Normalized pad: sticks -1..1 (+y down, like evdev), triggers 0..1, buttons by name."""

    def __init__(self):
        self.lx = self.ly = self.rx = self.ry = 0.0
        self.lt = self.rt = 0.0
        self.hx = self.hy = 0
        self.buttons = set()


class VrPointer:
    def __init__(self, send, log, osd=None):
        self.send = send          # send(payload: bytes) -> None, to the headset
        self.log = log
        self.osd = osd or (lambda text: None)
        self.pad = PadState()
        self.lock = threading.Lock()
        self.yaw = self.pitch = 0.0
        self.dashboard = None     # None: OpenVR unavailable
        self.mode = "off"         # in games: off / pointer / controllers (GAME_MODES)
        self.quiet_until = 0.0    # controllers kept off while a game starts
        self.osd_until = 0.0
        self.vrcmd = find_vrcmd()
        self.was_active = False
        self.prev_buttons = set()
        self.menu_alone = False   # Menu pressed without View (not the dashboard combo)
        self.keyboard = None

    def start(self):
        threading.Thread(target=self._evdev_loop, daemon=True).start()
        threading.Thread(target=self._openvr_loop, daemon=True).start()
        threading.Thread(target=self._loop, daemon=True).start()

    # ---- inputs
    def feed_headset_pad(self, payload):
        """GAMEPAD message from the Go (pad paired to the headset)."""
        b, lx, ly, rx, ry, lt, rt, hx, hy = struct.unpack("<IhhhhBBbb", payload[:16])
        names = ["A", "B", "X", "Y", "LB", "RB", "VIEW", "MENU", "GUIDE", "LS", "RS"]
        with self.lock:
            p = self.pad
            p.lx, p.ly, p.rx, p.ry = lx / 32767, ly / 32767, rx / 32767, ry / 32767
            p.lt, p.rt = lt / 255, rt / 255
            p.hx, p.hy = hx, hy
            p.buttons = {n for i, n in enumerate(names) if b >> i & 1}

    def _evdev_loop(self):
        try:
            import evdev
            from evdev import ecodes as e
        except ImportError:
            return
        keys = {e.BTN_SOUTH: "A", e.BTN_EAST: "B", e.BTN_NORTH: "X", e.BTN_WEST: "Y",
                e.BTN_TL: "LB", e.BTN_TR: "RB", e.BTN_SELECT: "VIEW", e.BTN_START: "MENU",
                e.KEY_BACK: "VIEW", e.KEY_MENU: "MENU", e.BTN_THUMBL: "LS", e.BTN_THUMBR: "RS",
                e.BTN_MODE: "GUIDE"}
        while True:
            pads = [evdev.InputDevice(p) for p in evdev.list_devices()]
            pads = [d for d in pads if "xbox" in d.name.lower() and "govr" not in d.name.lower()]
            if not pads:
                time.sleep(3)
                continue
            dev = pads[0]
            caps = dict(dev.capabilities().get(e.EV_ABS, []))
            # xpad (USB): right stick RX/RY, triggers Z/RZ. hid-microsoft (Bluetooth): right
            # stick Z/RZ, triggers BRAKE/GAS.
            if e.ABS_RX in caps:
                axes = {e.ABS_X: "lx", e.ABS_Y: "ly", e.ABS_RX: "rx", e.ABS_RY: "ry",
                        e.ABS_Z: "lt", e.ABS_RZ: "rt"}
            else:
                axes = {e.ABS_X: "lx", e.ABS_Y: "ly", e.ABS_Z: "rx", e.ABS_RZ: "ry",
                        e.ABS_BRAKE: "lt", e.ABS_GAS: "rt"}

            def norm(code, v):
                i = caps[code]
                if axes[code] in ("lt", "rt"):
                    return max(0.0, min(1.0, (v - i.min) / max(1, i.max - i.min)))
                mid = (i.min + i.max) / 2
                return max(-1.0, min(1.0, (v - mid) / ((i.max - i.min) / 2)))

            self.log(f"vr-pointer: gamepad {dev.name} (dashboard: gaze+left stick aim, A click, "
                     "right stick scroll, B close, View+Menu open)")
            try:
                for ev in dev.read_loop():
                    with self.lock:
                        p = self.pad
                        if ev.type == e.EV_KEY and ev.code in keys:
                            (p.buttons.add if ev.value else p.buttons.discard)(keys[ev.code])
                        elif ev.type == e.EV_ABS and ev.code in axes and ev.code in caps:
                            setattr(p, axes[ev.code], norm(ev.code, ev.value))
                        elif ev.type == e.EV_ABS and ev.code == e.ABS_HAT0X:
                            p.hx = ev.value
                        elif ev.type == e.EV_ABS and ev.code == e.ABS_HAT0Y:
                            p.hy = ev.value
            except OSError:
                time.sleep(2)  # pad disconnected: look again

    def _openvr_loop(self):
        try:
            import openvr
        except ImportError:
            self.log("vr-pointer: pyopenvr missing, the dashboard state is unknown")
            return
        while True:
            try:
                openvr.init(openvr.VRApplication_Background)
            except Exception:
                self.dashboard = None
                time.sleep(3)  # SteamVR not running (yet)
                continue
            self.log("vr-pointer: connected to SteamVR")
            try:
                overlay = openvr.VROverlay()
                apps = openvr.VRApplications()
                scene = apps.getCurrentSceneProcessId()
                while True:
                    pid = apps.getCurrentSceneProcessId()
                    try:
                        key = apps.getApplicationKeyByProcessId(pid) if pid else ""
                    except Exception:
                        key = ""
                    if key.startswith(("openvr.", "system.")):
                        pid = 0  # SteamVR's own scenes (Home environment...), not a game
                    if pid and pid != scene:
                        self._dashboard(False)
                        if key in GAME_MODES:
                            self.log(f"vr-pointer: game {key} started: {GAME_MODES[key]} mode")
                            threading.Timer(3, self._set_mode, (GAME_MODES[key],)).start()
                        else:
                            self.log(f"vr-pointer: game {key or pid} started: dashboard closed, "
                                     f"controllers off for {GAME_QUIET_S} s")
                            self.quiet_until = time.monotonic() + GAME_QUIET_S
                            self.mode = "off"
                        for delay in (2, 10, 25, 45, 70):  # the window may appear well after the VR session
                            threading.Timer(delay, focus_game_window).start()
                    elif scene and not pid:
                        self.log("vr-pointer: game closed")
                        self.mode = "off"
                    scene = pid
                    visible = bool(overlay.isDashboardVisible())
                    if visible != self.dashboard:
                        self.log(f"vr-pointer: dashboard {'open' if visible else 'closed'}")
                        if visible:
                            self.yaw = self.pitch = 0.0
                        elif pid:
                            threading.Timer(0.5, focus_game_window).start()  # back to the game
                    self.dashboard = visible
                    ev = openvr.VREvent_t()
                    quit_ = False
                    while openvr.VRSystem().pollNextEvent(ev):
                        quit_ |= ev.eventType == openvr.VREvent_Quit
                    if quit_:
                        break
                    time.sleep(0.1)
            except Exception as ex:
                self.log(f"vr-pointer: OpenVR: {ex!r}")
            self.dashboard = None
            try:
                openvr.shutdown()
            except Exception:
                pass
            time.sleep(3)

    # ---- output
    def _dashboard(self, show):
        if not self.vrcmd:
            return
        self.log(f"vr-pointer: {'opening' if show else 'closing'} the dashboard")
        d = os.path.dirname(self.vrcmd)
        subprocess.Popen([self.vrcmd, "--showdashboard" if show else "--hidedashboard"], cwd=d,
                         env=dict(os.environ, LD_LIBRARY_PATH=d), stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)

    def _escape(self):
        """Esc on a virtual keyboard to the focused game: the pause menu of games whose VR
        controller bindings have none (The Forest binds Esc to no Touch button)."""
        try:
            if self.keyboard is None:
                from evdev import UInput, ecodes as e
                self.keyboard = UInput({e.EV_KEY: [e.KEY_ESC]}, name="GoVR keyboard")
                time.sleep(0.3)  # let the compositor pick up the new device
            from evdev import ecodes as e
            focus_game_window()
            self.keyboard.write(e.EV_KEY, e.KEY_ESC, 1)
            self.keyboard.syn()
            time.sleep(0.08)
            self.keyboard.write(e.EV_KEY, e.KEY_ESC, 0)
            self.keyboard.syn()
            self.log("vr-pointer: Esc sent to the game")
        except Exception as ex:
            self.log(f"vr-pointer: cannot send Esc: {ex!r}")

    def _set_mode(self, mode):
        self.mode = mode
        self.yaw = self.pitch = 0.0
        self.quiet_until = 0.0
        self.log(f"vr-pointer: game mode {mode}")
        self.osd(MODE_TEXT[mode])
        self.osd_until = time.monotonic() + 6

    def _loop(self):
        dt = 1 / RATE_HZ

        def curve(v):  # aiming: deadzone + quadratic for fine control
            if abs(v) < DEADZONE:
                return 0.0
            v = (abs(v) - DEADZONE) / (1 - DEADZONE) * math.copysign(1, v)
            return v * abs(v)

        def stick(v):  # thumbsticks: deadzone only, games apply their own response curve
            if abs(v) < DEADZONE:
                return 0.0
            return (abs(v) - DEADZONE) / (1 - DEADZONE) * math.copysign(1, v)

        while True:
            time.sleep(dt)
            now = time.monotonic()
            if self.osd_until and now > self.osd_until:
                self.osd_until = 0.0
                self.osd("")
            with self.lock:
                p = self.pad
                lx, ly, rx, ry, lt, rt = p.lx, p.ly, p.rx, p.ry, p.lt, p.rt
                hx, hy = p.hx, p.hy
                buttons = set(p.buttons)
            pressed = buttons - self.prev_buttons
            released = self.prev_buttons - buttons
            self.prev_buttons = buttons
            dash = self.dashboard

            if {"VIEW", "MENU"} <= buttons and pressed & {"VIEW", "MENU"} and dash is not None:
                self.quiet_until = 0.0
                self._dashboard(not dash)
            quiet = now < self.quiet_until
            if dash and not quiet:
                state = "dashboard"
            elif self.mode != "off" and not quiet and not dash:
                state = self.mode
            else:
                state = None

            if state is None:
                if self.was_active:
                    self.send(struct.pack(FORMAT, 0, 0, 0, 0, *[0.0] * 8))
                    self.was_active = False
                continue
            self.was_active = True

            if state in ("dashboard", "pointer"):
                if pressed & {"X", "Y"}:
                    self.yaw = self.pitch = 0.0
                if state == "dashboard" and "B" in pressed:
                    self._dashboard(False)
                self.yaw = max(-YAW_LIMIT, min(YAW_LIMIT, self.yaw + curve(lx) * AIM_SPEED * dt))
                self.pitch = max(-PITCH_LIMIT, min(PITCH_LIMIT, self.pitch - curve(ly) * AIM_SPEED * dt))
                sx = curve(rx) or float(hx)
                sy = -curve(ry) or float(-hy)
                trigger = 1.0 if "A" in buttons else 0.0
                self.send(struct.pack(FORMAT, 1, self.yaw, self.pitch, 0,
                                      sx, sy, 0.0, 0.0, trigger, 0.0, 0.0, 0.0))
                continue

            # VR controllers: the whole pad becomes a pair of Touch controllers. The right one
            # aims along the gaze, nudged with the D-pad (game menus that follow the head need it).
            if "MENU" in pressed:
                self.menu_alone = "VIEW" not in buttons
            elif "VIEW" in buttons:
                self.menu_alone = False
            if "MENU" in released and self.menu_alone:
                threading.Thread(target=self._escape, daemon=True).start()
            self.yaw = max(-YAW_LIMIT, min(YAW_LIMIT, self.yaw + hx * AIM_SPEED * 0.6 * dt))
            self.pitch = max(-PITCH_LIMIT, min(PITCH_LIMIT, self.pitch - hy * AIM_SPEED * 0.6 * dt))
            bits = 0
            for name in ("A", "B", "X", "Y", "LS", "RS"):
                if name in buttons:
                    bits |= BTN[name]
            if {"LS", "RS"} <= buttons:  # the mode-switch combo is not a stick click
                bits &= ~(BTN["LS"] | BTN["RS"])
            self.send(struct.pack(FORMAT, 3, self.yaw, self.pitch, bits,
                                  stick(rx), -stick(ry), stick(lx), -stick(ly), rt, lt,
                                  1.0 if "RB" in buttons else 0.0, 1.0 if "LB" in buttons else 0.0))
