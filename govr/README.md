# GoVR — the Oculus Go as a wired PC-VR headset on Linux

GoVR turns an unlocked Oculus Go (MH-A32, Android 7.1) into a **USB-tethered headset for a Linux
PC**, while keeping the Go's own VR compositor, timewarp and sensor stack. The PC does the heavy
work (rendering, NVENC encoding); the Go decodes, reprojects with its native timewarp and plays
the audio.

Tested on: Oculus Go 32 GB (`pacific`, official unlocked build) + CachyOS (Arch), KDE Plasma 6
on Wayland, NVIDIA RTX 3090. Nothing is flashed: everything runs as a normal app on the Go.

| What | Status |
|---|---|
| **PC desktop in the headset** (curved virtual screen, 3DoF, PC mouse/keyboard) | ✅ ~25 ms capture→decode |
| **PC audio in the headset speakers** (dedicated PipeWire sink, Opus, OpenSL ES) | ✅ ~48 ms, A/V sync within 20 ms |
| **SteamVR** via ALVR v20.14.1 (client core ported to Android 7.1) | ✅ 72 fps, 2304×1280 HEVC |
| **No Man's Sky in VR** (Proton), seated, Xbox gamepad on the PC | ✅ playable |
| **VR video player**: YouTube/Vimeo/any yt-dlp site/local files, 360, 360 3D, 180, VR180, flat | ✅ head tracking fully local |
| In-headset 2D menu, pause/seek, live format switching | ✅ |
| App starts by itself when the Go boots | ✅ (BOOT_COMPLETED receiver, no flashing) |
| One-window control panel + KDE shortcuts, no terminal needed | ✅ |
| WebXR from the PC's desktop browser | ❌ Chrome has no WebXR/OpenXR backend on Linux; Firefox dropped WebXR |

## Screenshots (captured on the Go with `adb exec-out screencap`)

| | |
|---|---|
| ![test pattern](screenshots/01-hello-vr-test-pattern.jpg) First native VrApi app: head-tracked stereo pattern | ![PC stream](screenshots/02-pc-stream-projection-layer.jpg) NVENC stream from the PC through the SteamVR layer path |
| ![NMS menu](screenshots/03-no-mans-sky-vr-menu.jpg) No Man's Sky in VR (SteamVR → ALVR → Go) | ![NMS cockpit](screenshots/04-no-mans-sky-vr-cockpit.jpg) In flight, gamepad on the PC |
| ![VR180](screenshots/05-vr180-stereo-test.jpg) VR180 side-by-side test: each eye gets its half | ![menu](screenshots/06-in-headset-player-menu.jpg) Head-locked 2D player menu over a 360 video |

![control panel](screenshots/07-govr-control-panel.jpg)

*The GoVR control panel on the PC (status, modes, video player, screen placement, settings).*

## Quick start

Prerequisites: the unlocked Go with `adb root` working (see the main [README](../README.md)),
`android-tools`, `python-gobject` + GStreamer with `nvcodec`, `pipewire`, `python-pyqt6`,
`python-dbus`, Steam + SteamVR (for VR games), a USB **data** cable.

```bash
cd govr
scripts/setup-toolchain.sh          # NDK, SDK 1.35, JDK, Rust, ALVR streamer → ./toolchain
scripts/build-alvr-core.sh          # ALVR v20.14.1 client core for Android 7.1 (patched)
scripts/build-client.sh --install   # the Go app (com.govr.client), installed over adb
python3 -m venv --system-site-packages .venv-srv && .venv-srv/bin/pip install evdev openvr websockets vdf
python3 -m venv .venv && .venv/bin/pip install yt-dlp pillow numpy
scripts/install-desktop.sh          # "GoVR" panel, menu entries, global shortcuts, KWin capture auth
```

Then: plug in the Go, turn it on (the app starts ~1 min after boot), open **GoVR** from the
menu or the desktop and click a mode.

## Daily use

**Control panel** (`pc/govr_panel.py`): headset status and battery, *Desktop / SteamVR /
No Man's Sky / Stop*, the video player (link or file, format, pause, ±5/±30 s, change format),
screen placement (recenter, size, distance, height, background) and settings (which monitor or a
virtual monitor, all audio vs. selected apps, mute PC speakers). Settings live in `govr.conf`.

**Global shortcuts** (registered only where free; Meta = logo key):

| Keys | Action |
|---|---|
| Meta+Alt+D / Meta+Alt+Q | desktop in the headset / stop everything |
| Meta+Alt+C, Meta+Alt++, Meta+Alt+- | recenter, bigger, smaller |
| Meta+Alt+Space, Meta+Alt+, / . | video pause/play, −5 / +5 s |
| Meta+Alt+F / Meta+Alt+M | change video format / in-headset menu |

**Gamepad during a video** (Xbox pad connected to the PC): A pause · LB/RB −5/+5 s · D-pad
−30/+30 s · X next format · Y recenter · View/Start/Xbox opens the menu (D-pad + A, B closes).

**Terminal equivalents:** `scripts/go-start [desktop|vr]`, `scripts/go-play nms`,
`scripts/go-360 <url|file> --layout 360|360tb|180|180sbs|flat`, `pc/govr-ctl <command>`,
`scripts/go-stop --steamvr`.

## How it works

```
PC (Linux, KDE Wayland, NVIDIA)                              Oculus Go (Android 7.1, VrApi 1.1.35)
KWin zkde_screencast → PipeWire ─┐                           com.govr.client (NativeActivity, C++)
yt-dlp / files → GStreamer ──────┤ pc/govr_server.py          ├─ TCP over `adb reverse` :9950
PipeWire sink "govr" → Opus ─────┤ (NVENC HEVC, own protocol) ├─ MediaCodec → VrApi Android-surface
gamepad / keys / panel → ctl ────┘                            │  swapchain → cylinder / equirect layer
SteamVR → ALVR v20.14.1 driver → NVENC → `adb forward` ─────► ├─ libalvr_client_core.so (patched)
                                                              │  → projection layer + render pose
                                                              └─ libopus → OpenSL ES; 2D OSD panel
```

Key decisions (details in [DECISIONS.md](DECISIONS.md), research in [FEASIBILITY.md](FEASIBILITY.md),
wire format in [docs/PROTOCOL.md](docs/PROTOCOL.md)):

- **VrApi, not OpenXR**: the Go only has the legacy Mobile SDK runtime. Its VrDriver reports
  **VrApi 1.1.35**, so the app must be built with **Mobile SDK 1.35** (1.50 is rejected at init).
  The APK is built without Gradle (clang + aapt2 + d8 + apksigner).
- **Zero-copy video**: MediaCodec decodes straight into a VrApi *Android-surface swapchain*
  sampled by the compositor (cylinder layer for the desktop, equirect layer for 360/180,
  projection layer for SteamVR). One resample, native timewarp, sharp text.
- **Desktop capture without the portal dialog**: KWin's privileged `zkde_screencast_unstable_v1`,
  granted to one small helper binary (`pc/kwin-capture`) through a user-level `.desktop` file.
- **SteamVR**: ALVR's Rust client core is reused through its C API with an external decoder.
  Only the Android-8-only parts are patched out (AAudio, AImageReader decoder):
  [`patches/alvr-v20.14.1-go.patch`](patches/alvr-v20.14.1-go.patch).
- **Audio**: one path for every mode (dedicated `govr` PipeWire sink → Opus → OpenSL ES), so
  ALVR's own game audio is disabled.

## Things worth knowing (each cost hours)

- **Keeping the Go awake without wearing it** (for testing): VrPowerManager has hidden
  automation intents — `am broadcast -a com.oculus.vrpowermanager.prox_close` fakes "on face" —
  and the Oculus setting `autosleep_time=-1` (`service call SettingsService 5 s16 autosleep_time
  s16 -1 i32 0`) disables the "motionless → sleep" path. **But a USB 2.0 port cannot power the
  Go with the display on**: forced keep-awake drained the battery to shutdown in ~2 h. GoVR
  therefore uses stock power behaviour by default (`go-up --keep-awake` for tests only).
- **SteamVR on Linux**: launched by Steam it runs in the Steam Runtime container and vrserver
  quits after "vrcompositor process is not running"; GoVR starts `SteamVR/bin/vrmonitor.sh`
  directly with `QT_QPA_PLATFORM=xcb`. ALVR runs `adb kill-server` on shutdown, so the GoVR
  server re-creates its `adb reverse` every few seconds.
- **ALVR on Linux mislabels frame poses**: its server matches each frame to a tracking sample by
  rotation similarity (`PoseHistory::GetBestPoseMatch`, 360 entries); here it often returned
  samples up to 5 s old, so turning the head showed black for seconds. The Go client now
  reprojects each frame with its own record of the pose sent ~60 ms before decode (D-018).
- **No Man's Sky in VR**: start it with its VR launch option `-HmdEnable 1` (`scripts/go-play nms`),
  otherwise SteamVR shows it on a flat theater screen. With a gamepad, disable Steam Input for the
  game; in VR menus LB/RB do not switch tabs by design — enable NMS's *controller cursor in VR*.
  The SteamVR dashboard ignores gamepads: `vrcmd --hidedashboard` closes it.
- **YouTube 360**: DASH formats are EAC cubemaps ("mesh"); the HLS formats are equirectangular —
  GoVR asks yt-dlp for HLS.
- **An Xbox controller remembers one Bluetooth host**: pairing it with the Go removes the PC.
  For PC games keep it on the PC (`scripts/pair-xbox <MAC>` re-pairs it).

## Layout

```
govr/
  src/client/   Go app: C++ (VrApi, MediaCodec, ALVR glue, Opus/OpenSL, OSD), Java shell
  pc/           govr_server.py (capture/encode/stream/player), govr_panel.py, govr-ctl,
                gamepad.py, kwin-capture/ (C helper), alvr/ (session generator)
  scripts/      go-* commands, builds, setup-toolchain.sh, install-desktop.sh
  patches/      ALVR v20.14.1 patch for Android 7.1
  docs/         protocol
```

## Licences

GoVR code: MIT (this repository). ALVR: MIT. The Oculus Mobile SDK (Oculus SDK licence) and any
files extracted from the headset are downloaded locally and never redistributed.
