# GoVR desktop protocol v1

Transport: one TCP connection. The Go client connects to `127.0.0.1:9950` on the headset,
which `adb reverse tcp:9950 tcp:9950` tunnels to the PC server. The client retries forever,
so the PC side can start/stop at any time.

Every message: `u8 type | u8 pad[3] | u32 length (LE) | payload[length]`. All integers and
floats little-endian.

## PC -> Go
| type | name | payload |
|---|---|---|
| 1 | HELLO_SERVER | `u32 version` |
| 2 | VIDEO_CONFIG | `u32 codec (0=H.264, 1=HEVC)`, `u32 width`, `u32 height` |
| 3 | VIDEO_FRAME | `u64 pc_time_ns`, `u32 flags (1=keyframe)`, Annex-B access unit |
| 4 | SCREEN | `f32 radius_m`, `f32 arc_deg`, `f32 pitch_deg`, `u32 recenter_seq`, `u32 flags` (1 show environment, 2 desktop over SteamVR, 4 debug: stream via projection layer) |
| 5 | PING | `u64 pc_time_ns` |
| 6 | AUDIO_CONFIG | `u32 codec (0=Opus)`, `u32 sample_rate`, `u32 channels`, `u32 frame_ms` |
| 8 | OSD | UTF-8 text for the head-locked 2D panel (menu, player status); empty = hide. Lines separated by `\n`, a line starting with `▸` is highlighted |
| 7 | AUDIO_FRAME | `u64 pc_time_ns` (capture), one Opus packet |
| 9 | VR_POINTER | `u32 flags` (1 right controller, 2 left controller too), `f32 yaw, pitch` (right controller aim offset from the gaze, radians, + right / + up), `u32 buttons` (1 A, 2 B, 4 X, 8 Y, 16 left menu, 32 right system, 64 left stick click, 128 right stick click), `f32 rx, ry, lx, ly` (thumbsticks -1..1, + up), `f32 rtrigger, ltrigger, rgrip, lgrip` (0..1) — ~60 Hz while virtual controllers are active (pc/vr_pointer.py: SteamVR dashboard pointer, game menu pointer, gamepad as VR controllers); the Go sends them to ALVR as Touch controllers, which disconnect 0.5 s after the last active message |

## Go -> PC
| type | name | payload |
|---|---|---|
| 64 | HELLO_CLIENT | `u32 version`, `u32 eye_w`, `u32 eye_h`, `f32 refresh_hz` |
| 65 | FRAME_ACK | `u64 pc_time_ns` (from VIDEO_FRAME), `u32 decode_us` (submit->decoded), `u32 on_go_us` (received->decoded) — sent when the decoder releases the frame to the compositor surface |
| 66 | HEAD_POSE | `u64 go_time_ns`, `f32 qx, qy, qz, qw` (~72 Hz) |
| 67 | PONG | `u64 pc_time_ns` |
| 68 | REQUEST_IDR | — (decoder (re)started or error) |
| 70 | AUDIO_ACK | `u64 pc_time_ns` of the packet just queued to OpenSL ES, `u32 buffered_ms` (jitter buffer), `u32 underruns` (every 10th packet) |
| 71 | GAMEPAD | `u32 buttons` (bit 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 Back, 7 Start, 8 Guide, 9 LS, 10 RS), `i16 lx, ly, rx, ry`, `u8 lt, rt`, `i8 hat_x, hat_y` — sent on change while SteamVR streams; the PC exposes it as a virtual Xbox 360 pad (uinput) |
| 69 | CONTROL | `u32 cmd`: 1 recenter, 2 bigger, 3 smaller, 4 closer, 5 farther, 6 up, 7 down, 8 toggle environment, 9 keyframe, 10 reset, 11 toggle desktop over SteamVR (gamepad on the headset) |

Latency = PC clock at FRAME_ACK receipt − `pc_time_ns` (capture/encode start). It includes
encode, USB transfer, decode and the return trip of the ACK (~1 ms), but not the
compositor's last ~14 ms to photons.
