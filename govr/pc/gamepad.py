"""Virtual Xbox 360 controller on the PC, fed by the gamepad paired to the Oculus Go.

Uses /dev/uinput (python-evdev). The device mimics the xpad driver's layout (same vendor/product,
buttons and axes), so Steam Input, SDL and games see an ordinary Xbox 360 pad.
"""
import struct

from evdev import AbsInfo, UInput, ecodes as e

# GAMEPAD payload (docs/PROTOCOL.md): u32 buttons, i16 lx ly rx ry, u8 lt rt, i8 hatx haty
FORMAT = "<IhhhhBBbb"
BUTTONS = [e.BTN_SOUTH, e.BTN_EAST, e.BTN_NORTH, e.BTN_WEST, e.BTN_TL, e.BTN_TR,
           e.BTN_SELECT, e.BTN_START, e.BTN_MODE, e.BTN_THUMBL, e.BTN_THUMBR]


class VirtualPad:
    def __init__(self):
        stick = AbsInfo(0, -32768, 32767, 16, 128, 0)
        trigger = AbsInfo(0, 0, 255, 0, 0, 0)
        hat = AbsInfo(0, -1, 1, 0, 0, 0)
        caps = {
            e.EV_KEY: BUTTONS,
            e.EV_ABS: [(e.ABS_X, stick), (e.ABS_Y, stick), (e.ABS_RX, stick), (e.ABS_RY, stick),
                       (e.ABS_Z, trigger), (e.ABS_RZ, trigger),
                       (e.ABS_HAT0X, hat), (e.ABS_HAT0Y, hat)],
        }
        self.dev = UInput(caps, name="Microsoft X-Box 360 pad (GoVR)", vendor=0x045E,
                          product=0x028E, version=0x110, bustype=e.BUS_USB)
        self.last = None

    def update(self, payload):
        state = struct.unpack(FORMAT, payload[:struct.calcsize(FORMAT)])
        if state == self.last:
            return
        buttons, lx, ly, rx, ry, lt, rt, hx, hy = state
        prev = self.last
        for i, code in enumerate(BUTTONS):
            v = (buttons >> i) & 1
            if prev is None or ((prev[0] >> i) & 1) != v:
                self.dev.write(e.EV_KEY, code, v)
        for idx, code, val in ((1, e.ABS_X, lx), (2, e.ABS_Y, ly), (3, e.ABS_RX, rx),
                               (4, e.ABS_RY, ry), (5, e.ABS_Z, lt), (6, e.ABS_RZ, rt),
                               (7, e.ABS_HAT0X, hx), (8, e.ABS_HAT0Y, hy)):
            if prev is None or prev[idx] != val:
                self.dev.write(e.EV_ABS, code, val)
        self.dev.syn()
        self.last = state

    def close(self):
        self.dev.close()
