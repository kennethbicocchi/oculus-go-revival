// GoVR desktop protocol v1, see docs/PROTOCOL.md.
#pragma once
#include <cstdint>

namespace proto {
constexpr uint16_t kPort = 9950;  // ALVR owns 9943/9944
constexpr uint32_t kVersion = 1;

enum Type : uint8_t {
    HELLO_SERVER = 1, VIDEO_CONFIG = 2, VIDEO_FRAME = 3, SCREEN = 4, PING = 5,
    AUDIO_CONFIG = 6, AUDIO_FRAME = 7, OSD = 8,  // OSD: UTF-8 text, empty = hide
    VR_POINTER = 9,
    HELLO_CLIENT = 64, FRAME_ACK = 65, HEAD_POSE = 66, PONG = 67, REQUEST_IDR = 68, CONTROL = 69, AUDIO_ACK = 70, GAMEPAD = 71,
};

#pragma pack(push, 1)
struct Header { uint8_t type; uint8_t pad[3]; uint32_t length; };
struct VideoConfig { uint32_t codec, width, height; };
struct VideoFrameHeader { uint64_t pcTimeNs; uint32_t flags; };
struct Screen { float radius, arcDeg, pitchDeg; uint32_t recenterSeq, flags; };
struct HelloClient { uint32_t version, eyeW, eyeH; float refreshHz; };
struct FrameAck { uint64_t pcTimeNs; uint32_t decodeUs; uint32_t onGoUs; };  // onGoUs: received -> decoded
struct AudioConfig { uint32_t codec, sampleRate, channels, frameMs; };  // codec 0 = Opus
struct AudioFrameHeader { uint64_t pcTimeNs; };
struct AudioAck { uint64_t pcTimeNs; uint32_t bufferedMs, underruns; };
// Full pad state, forwarded to the PC's virtual Xbox 360 pad while SteamVR streams.
// buttons: bit 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 Back, 7 Start, 8 Guide, 9 LS, 10 RS
struct Gamepad { uint32_t buttons; int16_t lx, ly, rx, ry; uint8_t lt, rt; int8_t hatX, hatY; };
// Virtual VR controllers driven by the PC's gamepad (pc/vr_pointer.py), sent ~60 Hz while active.
// flags: 1 right controller (dashboard / game menu pointer), 2 left controller too (gamepad used
// as a pair of VR controllers). yaw/pitch: right controller aim offset from the gaze (radians,
// + right / + up). buttons: VR_BTN_*. Sticks -1..1 (+y up), triggers and grips 0..1.
struct VrControllers {
    uint32_t flags; float yaw, pitch; uint32_t buttons;
    float rx, ry, lx, ly, rtrigger, ltrigger, rgrip, lgrip;
};
enum VrButton : uint32_t {
    VR_BTN_A = 1, VR_BTN_B = 2, VR_BTN_X = 4, VR_BTN_Y = 8, VR_BTN_MENU = 16, VR_BTN_SYSTEM = 32,
    VR_BTN_LSTICK = 64, VR_BTN_RSTICK = 128,
};
struct HeadPose { uint64_t goTimeNs; float qx, qy, qz, qw; };
#pragma pack(pop)

enum ControlCmd : uint32_t {
    CTL_RECENTER = 1, CTL_BIGGER, CTL_SMALLER, CTL_CLOSER, CTL_FARTHER, CTL_UP, CTL_DOWN,
    CTL_ENV, CTL_IDR, CTL_RESET, CTL_OVERLAY,
};
}  // namespace proto
