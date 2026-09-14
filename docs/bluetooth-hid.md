# Bluetooth HID mouse — unfinished

**Status: incomplete.** The headset accepts the PC as a connected HID input
device, but movement reports produce no events. This document exists so the
next person starts from the wall instead of from zero.

If you finish this, please open a PR. A working VR pointer is the single
missing piece that would make a controller-less Go fully usable.

## How far it gets

This is further than it sounds. The Go registers the PC in its input device
table:

```
$ adb shell dumpsys bluetooth_manager | grep -A 4 HidService
Profile: HidService
  mTargetDevice: null
  mInputDevices:
    58:A0:23:71:7C:84 : 2        <- 2 = STATE_CONNECTED
```

Both L2CAP channels open cleanly — PSM 17 (control) and PSM 19 (interrupt) —
and the SDP profile registers over D-Bus without error.

## Setup that produces the above

```bash
sudo pacman -S python-pybluez python-dbus python-gobject bluez-deprecated-tools

sudo systemctl stop bluetooth
sudo /usr/lib/bluetooth/bluetoothd --compat --noplugin=input &
sudo chmod 777 /var/run/sdp

sudo hciconfig hci0 up
sudo hciconfig hci0 piscan
sudo hciconfig hci0 class 0x002580   # Peripheral, Pointing device

sudo python3 btmouse.py
```

`--compat` re-enables the legacy SDP interface; `--noplugin=input` stops BlueZ
from claiming the HID role for itself. Without both, `RegisterProfile`
succeeds but the record is never usable.

This takes over the system Bluetooth adapter — anything currently paired will
drop until you `systemctl start bluetooth` again.

## Pairing

The headset must initiate. On the Go: **Settings → Experimental Features →
Bluetooth pairing**. On the PC, in `bluetoothctl`:

```
agent NoInputNoOutput
default-agent
pairable on
discoverable on
```

`NoInputNoOutput` is what makes this work: it declares the PC has no way to
show or enter a PIN, so the pairing falls back to Just Works. Any agent that
wants a PIN deadlocks — the headset asks you to type a code you have no way to
type without a controller.

Tips for getting the PC to the top of the headset's device list, which cannot
be scrolled without a pointer: turn off nearby TVs and speakers (the list
appears to be sorted by RSSI), and put the adapter physically close. If your
adapter advertises a random address, `btmgmt -i hci0 privacy off` makes it use
its stable public one.

## Where it fails

`connect` without the HID profile registered gives the diagnosis cleanly:

```
Failed to connect: org.bluez.Error.BREDR.ProfileUnavailable
No more profiles to connect to
```

With the profile registered, the connection holds and the device shows as
connected — but sending movement reports produces **nothing** in the
headset's logcat.

Report header variants tried, both without effect:

| Header | Descriptor declares report ID? | Result |
|---|---|---|
| `0xA1 0x01` | no | nothing |
| `0xA1` | no | nothing |
| `0xA1 0x01` | yes (`0x85 0x01`) | nothing |

For the third variant the bond must be removed on **both** sides and redone:
the HID descriptor is read once at pairing time and cached, so changing it
without re-pairing means the headset keeps validating against the old one.

## The diagnostic that was never run

**Capture the L2CAP traffic while sending reports:**

```bash
sudo btmon -w hid-test.btsnoop
# ... send movement from the btmouse.py prompt ...
btmon -r hid-test.btsnoop | grep -B 2 -A 8 -i "acl data"
```

This separates the two hypotheses that were never distinguished:

1. The reports never leave the socket → the bug is in `btmouse.py`
2. The reports go out and the headset discards them → the bug is in the
   descriptor or the report format

**Do not change the script before running this.** Every variant tried above
was a guess, and guessing is what made this take a night without resolving.

## Notes on the device side

The Go's SDP record advertises only `0x1200` (PnP), `0x1800` (GAP), `0x1801`
(GATT) and `0xfeb8` (vendor Oculus), all on PSM 31 (ATT). **HID is not
advertised** — yet `HidService` is loaded and responds. Do not use SDP to
decide what this device supports.

The headset presents itself as `Class: 0x005a020c`, `Icon: phone`.

## The original controller, for reference

Lost controllers stay bonded through factory resets. From
`/data/misc/bluedroid/bt_config.conf`:

```
[2c:26:17:16:4f:ef]
Name = OMVR-V190
DevType = 2      (BLE)
AddrType = 0     (public)
LE_KEY_PENC  = ...
LE_KEY_LCSRK = ...
```

Its BLE service UUID is `4F63756C-7573-2054-6872-65656D6F7465`, which is ASCII
for **`Oculus Threemote`**. The Gear VR controller protocol it derives from was
reverse engineered years ago: https://jsyang.ca/hacks/gear-vr-rev-eng/

Emulating the controller itself — rather than a generic mouse — is the other
open avenue. Advertising with that UUID alone produces no reaction from the
headset (verified), so it would need a full GATT server exposing the
documented characteristics. Since the bond survives, impersonating the
controller's MAC is possible (`btmgmt` reports `supported options:
public-address`), but the stored encryption keys are in Android's format and
would need converting into BlueZ's.
