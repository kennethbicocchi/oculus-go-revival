# DECISIONS

## D-001 — User-space toolchain under ./toolchain (2026-10-01)
No passwordless sudo, so pacman is unavailable. NDK r28c, build-tools 34, platform-25,
JDK 25 (also satisfies Ghidra), CMake and jadx are unpacked into `toolchain/` (git-ignored).
No Gradle: APKs are built directly with aapt2/d8/apksigner from a shell script (fewer
moving parts, fully reproducible offline). Rejected: Android Studio / Gradle (heavy,
needs sdkmanager + licences, slower iteration).

## D-002 — Keep-awake via VrPowerManager automation intents (2026-10-01)
Decompiled `com.oculus.vrpowermanager` (odex in `/system/priv-app/VrPowerManager/oat/arm64`,
extracted with `scripts/oat2dex.py`, which also fixes the dex checksum/signature that
dex2oat leaves stale). Its runtime BroadcastReceiver handles:
- `com.oculus.vrpowermanager.prox_close`: sets `virtualProximitySensor=true`, stops the real
  mount sensor, forces state DON -> `sys.hmt.mounted=1`, sticky MOUNT_STATE_CHANGED.
- `prox_far`: virtual DOFF. `automation_disable`: back to the real sensor.
Autosleep only arms on SCREEN_OFF, so combined with `svc power stayon usb` and
`stay_on_while_plugged_in=7` the screen never turns off. The receiver is unprotected, so
`adb shell` (even non-root) can send it. State is lost when the process restarts (reboot).
Rejected: patching sepolicy/init or a wakelock app (unnecessary), editing Oculus settings
(`autosleep_time` lives in the Oculus `SettingsService` binder, not in Android settings).

## D-003 — Reverse-engineered binaries stay in ./re (git-ignored)
Meta binaries (odex, oat, apk, decompiled sources) are never committed.

## D-004 — Architecture: C (own desktop protocol first, then ALVR core) (2026-10-01)
One Go app (`com.govr.client`, native, VrApi, arm64) with a shared platform layer:
VrApi frame loop + head pose, MediaCodec decoder → SurfaceTexture → GL external texture,
rendering as a curved screen (desktop mode) or as a full-FOV eye layer (VR mode).
Two content sources:
1. **Desktop mode** (goal 1): own minimal TCP protocol over `adb reverse`; PC side captures one
   monitor (PipeWire / KWin), encodes with NVENC (ffmpeg), sends Annex-B HEVC/H.264 NALs.
2. **SteamVR mode** (goals 2/3): link a patched `alvr_client_core` (pinned v20.14.1, API-26
   bits removed) through its C ABI with an external decoder callback; ALVR server v20.14.1 on PC.
Rationale: goal 1 delivers value without SteamVR and de-risks the whole platform layer
(VrApi, decode, render, transport), which ALVR mode then reuses unchanged.
Rejected: A alone (blocks goal 1 on SteamVR install + Rust/ALVR porting); B alone (no SteamVR);
old polygraphene ALVRClient (protocol incompatible with any maintained server);
OpenXR (Go has no OpenXR runtime).

## D-005 — VrApi from Mobile SDK 1.50 + NativeActivity, built without Gradle
Use the SDK's `libvrapi.so` loader packed in the APK (standard for Go apps), headers from
`VrApi/Include`. Base the client on `VrCubeWorld_NativeActivity` structure. Java only where
unavoidable (SurfaceTexture for decoder output), compiled with javac+d8 from the build script.

## D-006 — Mobile SDK 1.35.0, not 1.50 (2026-10-01)
The Go's VrDriver ships `libvrapiimpl.so` "1.1.35.0 (Jul 2021)". The 1.50 loader asks for
API 1.1.50 and DriverLoader rejects it. SDK 1.35.0 (still downloadable from Meta) matches.

## D-007 — Decoder output into a VrApi Android-surface swapchain, shown as a cylinder layer
`vrapi_CreateAndroidSurfaceSwapChain` returns a Surface that AMediaCodec renders into; the
compositor samples it directly in a `ovrLayerCylinder2` (with CLIP_TO_TEXTURE_RECT).
Pros: zero copies, no GL work in the app for the video, single resampling (sharper text than
rendering into eye buffers first), compositor-side timewarp. The same mechanism will carry
SteamVR frames as a projection layer with the frame's render pose (ATW for free).
Rejected: SurfaceTexture + GL external texture rendered into eye buffers (extra resample,
extra GPU work, more latency); AImageReader (needs API 26 for GPU buffers).

## D-008 — PC server in Python + GStreamer (nvcodec), own TCP protocol over adb reverse
GStreamer 1.28 on CachyOS has `nvh265enc`/`nvh264enc` and `pipewiresrc`, plus Python gi:
exact per-AU packets from appsink, upstream force-key-unit for IDR on demand, and the same
pipeline later takes PipeWire screencast input. `adb reverse` makes the client a plain TCP
client of 127.0.0.1 that retries forever, so either side can restart.
Note: GstVideoEncoder shifts output PTS by whole hours; the server strips that offset to get
capture timestamps on CLOCK_MONOTONIC. Rejected: ffmpeg subprocess (no packet boundaries,
no IDR on demand), PyAV (not installed, wheels may lack NVENC).

## D-009 — Screen capture through KWin's zkde_screencast, authorized per-binary (2026-10-01)
The xdg-desktop-portal ScreenCast needs a click in a consent dialog (at least once), which
cannot happen unattended. KWin exposes `zkde_screencast_unstable_v1` (stream an output, a
region, a window, or a *virtual output*) to executables whose `.desktop` entry lists it in
`X-KDE-Wayland-Interfaces` (as Spectacle, krdpserver and Sunshine do). A 170-line C helper
requests the stream and prints the PipeWire node id; GStreamer's `pipewiresrc path=<id>`
consumes it. Authorization is a user-level `.desktop` file naming only that binary.
Rejected: portal (dialog), kmsgrab/ffmpeg (needs CAP_SYS_ADMIN), injecting a click with
/dev/uinput (it is user-writable, but synthesizing input on the user's live desktop is too
intrusive), whitelisting /usr/bin/python (would grant every Python process screen capture).

## D-010 — Screen state owned by the PC; gamepad commands forwarded
The PC server owns radius/arc/pitch/recenter and pushes SCREEN messages; the headset only
renders. The gamepad on the headset sends CONTROL commands to the PC. One source of truth,
persisted in state/screen.json, and identical behaviour from keyboard, govr-ctl and gamepad.

## D-011 — ADDENDUM.md applied (2026-10-01 09:35)
Read ~/go-vr/ADDENDUM.md (user instructions) and applied it:
- Phase 6 acceptance test is now **No Man's Sky (Proton) in VR, seated, Xbox gamepad (+ mouse/
  keyboard where the game allows), Go presented to SteamVR as a head only, menus navigable with
  the gamepad**. ProtonDB/VR-on-Linux findings logged in FEASIBILITY.md §5.
- Desktop-in-VR: evaluated wlx-overlay-s/WayVR (FEASIBILITY.md §6): needs VR controllers →
  not adopted; our own desktop layer composited on the Go over the SteamVR stream is the plan.
- Reverse engineering rules (Ghidra/jadx, ./re/, never commit Meta binaries) were already being
  followed (D-002, D-003, D-006).
- I will re-read ADDENDUM.md at the start of every phase.

## D-012 — ADDENDUM "Audio" section applied (2026-10-01 09:40)
Re-read ADDENDUM.md (new "Audio" section, required from Phase 4 on). Plan:
- **PC**: a dedicated PipeWire virtual sink `govr` ("GoVR Headset", `module-null-sink` via
  pipewire-pulse, created at server start, removed at exit). Apps are routed to it individually
  (pavucontrol / KDE volume applet), or all at once with `--audio-default` (makes it the
  default sink and moves current streams, restoring on exit). `--mute-pc` mutes the physical
  sink for the session. Capture `govr.monitor` with GStreamer, **Opus**, 10 ms frames,
  `restricted-lowdelay`, 128 kbit/s stereo 48 kHz.
- **Transport**: AUDIO_CONFIG / AUDIO_FRAME messages on the same TCP connection over adb as video.
- **Go**: libopus cross-compiled with the NDK, **OpenSL ES** buffer-queue player (Android 7.1
  has no AAudio, which is API 26; OpenSL ES is the low-latency path on N), small jitter buffer.
- **Metrics**: audio packets carry the PC capture timestamp; the Go reports when each one is
  queued for output; the PC logs audio latency next to video latency and their difference
  (A/V sync) in the stats; PROGRESS.md records them.
- **ALVR (Phase 6)**: ALVR on Linux creates its own PipeWire sink and sends raw PCM; its client
  plays through cpal/AAudio, which does not exist on 7.1. Decision: disable ALVR's game audio and
  route SteamVR/game audio to the `govr` sink, so one audio path serves both desktop and VR
  modes (also avoids porting ALVR's audio backend). Re-evaluate if A/V sync in VR suffers.
- Acceptance additions: Phase 4 test tone verified on the headset via logcat + `dumpsys audio`
  / `dumpsys media.audio_flinger`; Phase 6 NMS audio from the headset.
- Optional later: headset microphone to the PC.

## D-013 — Keep-awake, part 2: Oculus autosleep_time = -1 (2026-10-01 10:14)
The soak test showed the headset still fell asleep ~3 min after each wake-up while lying
motionless. VrPowerManager exposes `notifyDeviceIdle()` (called by the VR runtime when the IMU
sees no motion): it dims the screen and 5 s later forces `goToSleep`, bypassing Android's
stay-on. Both paths return early when `isAutosleepDisabled()`, i.e. Oculus setting
`autosleep_time == -1` (default 15). The setting lives in the `SettingsService` binder
(`oculus.internal.ISettingsService`, decompiled: txn 5 = setUserSetting(key, value, userId),
txn 6 = getUserSetting). Set with `service call SettingsService 5 s16 autosleep_time s16 -1 i32 0`
as root; persisted in the Oculus settings DB. go-up re-applies it every time.

## D-014 — Boot autostart through a BOOT_COMPLETED receiver, not the system image
The app's BootReceiver sends `prox_close` and starts MainActivity 55 s after boot (after the stock
init service opens the browser at ~40 s, so GoVR ends on top). No flashing, no SELinux labels,
survives OS image changes. Verified with a real reboot (start from uid 10056 at boot+55 s).
Rejected: editing init.oculus.properties.{rc,sh} in system_b (needs a flash cycle, 48 MB free,
label pitfalls) — kept as the documented fallback.

## D-015 — SteamVR on Linux: started via vrmonitor.sh, QT_QPA_PLATFORM=xcb
Launched by Steam, SteamVR runs inside the Steam Runtime container and vrserver times out waiting
for vrcompositor ("vrcompositor process is not running", then quits). go-steamvr starts
`SteamVR/bin/vrmonitor.sh` directly (same effect as the ALVR-recommended launch option, without
editing Steam's config), with QT_QPA_PLATFORM=xcb for vrmonitor's Qt UI on KDE Wayland.
ALVR's `adb kill-server` on shutdown is handled by the GoVR server re-creating its adb reverse.

## D-016 — Stock power behaviour by default; forced keep-awake only for testing (2026-10-01 10:56)
A USB 2.0 data port supplies less than the Go draws with the display on and the decoder
running; with D-002/D-013 forcing the display on for ~2 h 15 min the battery emptied and the
headset shut down during the NMS test. Now go-up defaults to stock behaviour (real proximity
sensor, autosleep 15 s, no stay-on), reports battery and warns below 25 %; keep-awake is opt-in
(`--keep-awake` / GOVR_KEEP_AWAKE=1 / KEEP_AWAKE=yes) and go-stop reverts it.

## D-017 — Launch VR games with their VR arguments (go-play)
Steam's library button and steam://rungameid use a game's default launch option. For No Man's
Sky that is the flat one; the VR option adds `-HmdEnable 1` (Steam appinfo). Launched flat,
SteamVR shows the game on a theater screen. go-play passes the VR arguments via `steam -applaunch`.

## D-018 — Reproject ALVR frames with our own pose history, not the server's pose tag (2026-10-01 15:10)
Symptom (NMS, user wearing the headset): turning the head showed black that filled in only after
several seconds. Measured on the Go: the render pose ALVR attached to frames was up to ~5 s old
(frame yaw -5.5 deg while the head was at -69 deg), then jumped to fresh, repeatedly.
Cause: ALVR's Linux server tags each frame by GetBestPoseMatch() — the history entry whose rotation
best matches the pose the SteamVR compositor presented. Its buffer holds 360 entries (3 s at 120 Hz =
5 s at our 72 Hz); on this setup the match fails and falls back to old entries, so the client
reprojected correct images by a wrong (stale) rotation.
Fix: the client records every orientation it sends and reprojects each frame with the one sent
~60 ms (pipeline latency) before the frame was decoded. Verified by the user: no more black.
Possible upstream follow-up: investigate why the match fails (universe/seated transform?).
