# FEASIBILITY — Oculus Go as a wired PC-VR headset (2026-10-01)

## 1. Headset side
- Android 7.1.1 (API 25), arm64 + armeabi-v7a, Snapdragon 821 (Adreno 530, HW H.264/HEVC decode).
- VR stack: legacy **Oculus Mobile SDK / VrApi** (not OpenXR). `/system/lib64/libvrapi.so` is a
  loader; `/system/vendor/bin/vrapiserver` + `libvrapiservice.so` implement the compositor/ATW.
  vrshell renders at 72 FPS through it (`VrApi: FPS=72` in logcat).
- **Mobile SDK 1.50.0 is still downloadable from Meta** (`ovr_sdk_mobile_1.50.0.zip`,
  `securecdn.oculus.com/binaries/download/?id=4643347799061523`). It contains VrApi headers,
  arm64/armv7 `libvrapi.so` loader (Aug 2021, older than the Go's Oct 2021 OS) and
  `VrCubeWorld_NativeActivity` (pure native, no Java). => VrApi toolchain: **available**.
  Licence: Oculus SDK licence → kept in `toolchain/`, never committed.
- `screencap` captures the final distorted stereo frame (verified with Oculus Browser), so we
  can verify everything without wearing the headset.

## 2. ALVR (alvr-org/ALVR)
- Latest stable release: **v20.14.1** (2025-07-14); master is 21.0.0-dev. Client and server must
  match the same protocol version → pin v20.14.1 for both.
- Linux server: SteamVR driver + dashboard; NVENC via ffmpeg/Vulkan on NVIDIA; works on
  Wayland because ALVR is a virtual HMD driver (no DRM lease / direct mode needed).
  Wired mode: the dashboard drives `adb forward/reverse` itself (alvr_adb crate).
- **Client core reuse**: `alvr_client_core` is Rust with a C ABI (`c_api.rs`):
  `alvr_initialize`, `alvr_poll_event`, `alvr_send_tracking`, `alvr_send_view_params`,
  and crucially **`alvr_set_decoder_input_callback` + `alvr_report_frame_decoded` +
  `alvr_report_compositor_start` + `alvr_report_submit`**: the app can supply its own decoder
  and compositor. Networking/protocol/stats/tracking-prediction are platform independent.
- What blocks API 25 (all *contained*, not structural):
  - `video_decoder/android.rs`: `AImageReader_newWithUsage` + `AHardwareBuffer` (API 26).
    Replace with our own decoder (MediaCodec → Surface from a Java `SurfaceTexture` →
    `GL_TEXTURE_EXTERNAL_OES`), fed by `alvr_set_decoder_input_callback`.
  - `graphics` crate: `eglGetNativeClientBufferANDROID` (API 26) → not used; we render ourselves.
  - `ndk` crate with `api-level-28` feature and `audio` (AAudio, API 26); `alvr_audio` uses
    cpal (AAudio backend on Android). Android's linker resolves all symbols at load time, so
    these must be cfg'd out / stubbed or the .so will fail to load on 7.1. Audio can be
    disabled at first (seated desktop/games still useful), later done with OpenSL ES.
  Estimated patch: a few hundred lines, mostly removals behind a `go` feature.
- Official Go support was dropped (client is OpenXR-only, min Android 8). The old
  polygraphene/ALVRClient (VrApi) is protocol-incompatible with modern servers: reference only.

## 3. PC side
- RTX 3090, driver 615.71, ffmpeg 9 with `h264_nvenc`/`hevc_nvenc`/`av1_nvenc`.
  Go decodes H.264 and HEVC (no AV1).
- KDE Plasma 6 Wayland: screen capture needs the xdg-desktop-portal ScreenCast (user consent
  dialog, can be persisted with a restore token) or KWin's privileged
  `zkde_screencast_unstable_v1`. X11 session not needed for ALVR.
- Steam installed, **SteamVR not installed** (needs a click in Steam → NEEDS_USER.md).
- No passwordless sudo: everything user-space.

## 4. Transport
- `adb forward tcp:X tcp:Y` (PC connects to Go) / `adb reverse` (Go connects to PC).
  USB 2.0 + adb: expected ~20-35 MB/s, i.e. far above the 10-30 Mbit/s a good HEVC stream needs.

## Verdict
- Goal 1 (3D desktop): feasible with our own protocol + VrApi client. Low risk.
- Goal 2/3 (SteamVR, browser WebXR via SteamVR): feasible with a patched ALVR v20.14.1
  client core inside the same Go app; needs SteamVR installed. Medium risk (Rust cross build,
  ALVR protocol details, NVIDIA + SteamVR on Linux quirks).
- Goal 4 (autostart): feasible with the existing init-service mechanism.

## 5. ADDENDUM use case: No Man's Sky in VR, seated, gamepad (researched 2026-10-01)
- NMS is installed (app 275850).
- ProtonDB / VR-on-Linux DB (db.vronlinux.org/games/275850): NMS VR runs on Linux through
  Proton with **SteamVR** (most reliable), WiVRn+xrizer (good), ALVR (slower, black-screen
  crashes on some systems), Monado/OpenComposite (glyph problems). NVIDIA: mixed (one RTX 3070 Ti
  crash report; others fine). Proton GE 10.x performs better than Proton 9.
  Valve fixed "NMS fails to boot in VR mode" in Proton Experimental (Feb 2025, GamingOnLinux),
  and an earlier VR + controller crash (Feb 2023).
- **Gamepad in VR**: NMS does not natively offer gamepad play in VR; it works through SteamVR's
  per-app binding: SteamVR → Settings → Controllers → Show more applications → No Man's Sky →
  Current controller = Gamepad → community/default binding (Steam discussion 2019; quirks:
  multitool aim, VR controllers override the gamepad when present). With the Go there are no VR
  controllers, so no override conflict. Menus reported usable.
- Risks: (1) NMS may show the "no controllers" path or put the HMD-only user in a broken state;
  (2) ALVR is the runtime most associated with black screens in reports.
- Fallbacks, in order: (a) SteamVR gamepad binding (community); (b) ALVR client sends *virtual
  controllers* (fixed poses relative to the head, buttons from the Xbox pad) so NMS sees
  "Touch-like" controllers; (c) launch NMS flat (non-VR) and show it on the Go's virtual screen
  (goal 1 path) — always works, no stereo.

## 6. Desktop-in-VR while in SteamVR: wlx-overlay-s (now "WayVR")
- OpenVR + OpenXR overlay for Wayland/X11, PipeWire capture through the portal (popup at first
  start), AppImage/AUR. Interaction is **laser-pointer from VR controllers**; no head-only mode;
  no longer auto-registers with SteamVR.
- Verdict for a head-only Go: could at best display a screen; positioning and interaction need
  controllers. **Not adopted.**
- Chosen instead: the Go client composites our own desktop cylinder layer (already working,
  goal 1) on top of the SteamVR stream, toggled from the gamepad or `govr-ctl`. Two hardware
  decoders in parallel are within the Snapdragon 821's budget. Mouse/keyboard act on the real
  PC desktop as usual.
