# Oculus Go Revival — MH-A32 (`pacific`)
[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-yellow?logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/kennethbicocchi)

Bringing a dead Oculus Go back to life in 2026: unlocked bootloader, root,
working SELinux, auto-starting browser, real-time keyboard input — **without
the Meta account, without the original controller, and without the phone app.**

Everything here was tested on a single device: **Oculus Go MH-A32, 32 GB,
manufactured 2018, codename `pacific`**, running the official Meta unlocked
build. Host machine: Arch Linux (CachyOS), fish shell.

> **Why this repo exists.** Public documentation for the Go stops around 2022.
> None of it mentions that Meta shipped an unused permissive SELinux policy
> inside the system image, or how to get an init service to survive on this
> device, or that pairing with the modern Meta Horizon app will wipe an
> unlocked headset. Those three facts cost a full night to discover. They are
> written down here so nobody has to rediscover them.

---

## What actually works after following this

| Capability | Status |
|---|---|
| Unlocked bootloader, flashable partitions | ✅ |
| Root via `adb root` | ✅ |
| SELinux manageable (`setenforce` works) | ✅ |
| Boots straight into the browser, no PC needed | ✅ |
| Real-time keyboard from the PC, **including inside WebXR sessions** | ✅ |
| Live view of what the headset sees | ✅ |
| Wi-Fi, YouTube, 360/180 video playback | ✅ |
| Menu navigation with an Xbox gamepad | ✅ |
| VR pointer / cursor | ❌ unsolved |
| Library / Store / Search tabs | ❌ dead (server-side, unfixable) |
| Magisk | ❌ does not work, see below |

---

## Quick start

You need `android-tools`, `android-udev`, and your user in the `adbusers`
group (log out and back in after adding yourself).

```bash
# 1. Get the official unlocked build from Meta
#    https://developers.meta.com/horizon/downloads/package/oculus-go-sw-unlock
#    Extract the OUTER archive only — keep unlocked_build.zip intact.
#    adb sideload verifies the signature over the archive as-is.

# 2. Enter the bootloader: VOLUME DOWN + power, cable unplugged.
#    (Volume Up does NOT work, despite Meta's own guide.)
#    Select "enable sideload update" with volume keys, confirm with power.

adb devices                       # must show state: sideload
adb sideload unlocked_build.zip   # ~3.5 min, DO NOT INTERRUPT

# 3. Back to the bootloader, then:
fastboot oem unlock               # factory resets, irreversible

# 4. Verify
adb root                          # -> "restarting adbd as root"

# 5. Skip the phone-app pairing requirement
adb shell am startservice -a firsttimenux.ota.SKIP_NUX \
  -n com.oculus.companion.server/com.oculus.firsttimenux.ota.OtaIntentService
```

`adb root` does **not** persist across reboots. Re-run it each session.

---

## The key discovery: `sepolicy.unlocked`

The system image ships **two** SELinux policies in its root:

```
sepolicy           364k   <- the one actually loaded
sepolicy.unlocked  395k   <- orphaned; nothing in the image references it
```

`grep -r "sepolicy.unlocked"` across the whole image returns nothing. Meta
built a permissive policy for unlocked devices and never wired it up.

Swapping them makes the device workable: `setenforce 0` starts succeeding
(it is denied even to uid 0 under the stock policy), and files like
`/data/misc/bluedroid/bt_config.conf` become readable.

```bash
sudo cp mnt/sepolicy mnt/sepolicy.stock      # keep the original
sudo cp mnt/sepolicy.unlocked mnt/sepolicy
```

See [`scripts/sysmod.sh`](scripts/sysmod.sh) for the full mount/edit/flash cycle.

**Caveat:** verified on this one build. If your image lacks
`sepolicy.unlocked`, this repo's central trick does not apply to you.

---

## Auto-starting an app at boot

### The problem

`vrshell` loops forever on:

```
OCPlatformModule: access token fetch returned error:
'user not logged in', retrying in 10.000000 seconds
```

This is the infamous three-dots screen. It is **not** a hang — the compositor
runs at 72 FPS, no ANR — but the shell never reaches its home screen.

Android's HOME mechanism is **removed** on this device:
`pm query-activities -c android.intent.category.HOME` returns **zero**
activities system-wide, even after installing a launcher.
`set-home-activity` refuses everything. So a normal launcher cannot be
installed as home.

### The solution

An init service that launches an app *over* the looping shell. See
[`patches/`](patches/). Two files change:

- `/init.oculus.properties.rc` — declares the service
- `/system/bin/init.oculus.properties.sh` — the payload, inserted at the **top**
  of the file (line 3), because `set -o errexit` plus the immutable
  `setprop ro.*` calls below would abort the script on the second boot

### The four walls, in order

Each one was found with `adb shell "logcat -d | grep 'avc: denied'"`.
This single command solved in minutes what eight blind flash cycles had not.

| Attempt | Failure | Cause |
|---|---|---|
| `seclabel u:r:su:s0` | *"Service does not have a SELinux domain defined"* | the `su` domain only exists while `adb root` is active |
| `seclabel u:r:init:s0` | `avc: denied { execute_no_trans }` | the init domain may not execute `system_file` without transitioning |
| `logwrapper` as wrapper | `avc: denied { entrypoint }` | only `/system/bin/sh` is a valid entrypoint for the shell domain |
| edited script | `avc: denied { read } ... tcontext=unlabeled` | **the SELinux label is lost on every edit** |

Two more traps found along the way:

- Setting `LD_LIBRARY_PATH=/vendor/lib:/system/lib` breaks everything with
  `CANNOT LINK EXECUTABLE "/system/bin/sh": libc++.so is 32-bit instead of
  64-bit`. **Do not set it.** Android resolves the right paths on its own.
- The `shell` domain has no `dac_override`, so it cannot write to
  `/data/local/tmp`. Don't redirect logs there from inside the service.

### ⚠️ The golden rule

**After every `sed`, `tee` or any other write to a file inside the mounted
image, restore its SELinux label:**

```bash
sudo setfattr -n security.selinux -v "u:object_r:system_file:s0\0" <file>
sudo getfattr -n security.selinux <file>   # verify
```

Forgetting this costs a flash cycle every time. It cost three.

---

## Input

### What works

**Keyboard via scrcpy.** This is the single most useful discovery after root.

```bash
scrcpy --no-audio --mouse=disabled
```

Typing on the PC keyboard reaches the headset in real time — **including
inside immersive WebXR sessions**, where `adb shell input keyevent` does not
reach at all. Arrow keys scrub the timeline, Enter confirms, Esc exits.

`--mouse=disabled` is not optional: moving the mouse into the scrcpy window
sends events the VR compositor cannot handle, the screen goes black and
`vrshell` restarts into the three-dots loop.

**Xbox gamepad** over Bluetooth navigates system menus. It does not scroll web
pages (it moves focus between elements; there is no pointer) and does nothing
inside WebXR sessions.

**Screenshots**, including in immersive mode:

```bash
adb shell screencap -p /sdcard/shot.png && adb pull /sdcard/shot.png .
```

### What does not work

- **`adb shell input tap`** — focus stays on `com.oculus.vrshell/.MainActivity`
  even with a browser activity started, because the browser runs as a
  `PanelService` *inside* vrshell. They are not competing apps; one is nested
  in the other.
- **`sendevent` / `/dev/input`** — the Oculus controller has no device node.
  `getevent -pl` lists only onboard hardware. Writes to `event1`
  (`qbt1000_key_input`, which exposes ABS_X/ABS_Y and looks like a pointer)
  never arrive. Note `/dev/input/mouse0` and `mice` *do* exist.
- **`scrcpy --mouse=uhid`** — fails with `EACCES` on `/dev/uhid` (owned by
  `system:net_bt_stack`). `chmod 666` clears that, but no virtual device is
  ever created: `dumpsys input` shows nothing new. Android 7.1 predates the
  path scrcpy 4.x uses.
- **Bluetooth HID from the PC** — gets *remarkably* far (see
  [`docs/bluetooth-hid.md`](docs/bluetooth-hid.md)); the headset registers the
  PC as a connected input device, but movement reports produce no events.
  Unfinished.
- **`pm disable com.oculus.vrshell`** — **black screen**. It is the compositor.

### ⚠️ Never pair with the Meta Horizon app

On an unlocked headset the app triggers **a factory reset, a "your device
can't be checked for corruption" message, and a shutdown.** The unlocked
bootloader fails the integrity check and the app responds by restoring the
device. This is not a bug to work around — it is the designed behaviour.

There is no way to log in an account on an unlocked Go. The in-headset
Profile panel spins forever on the same token the shell cannot fetch.

---

## Magisk does not work here

Magisk 27.0 patches `boot.img` cleanly and flashes fine. It then does nothing:
`/data/adb/` stays empty, nothing appears in `/sbin/`, `su` does not exist.

The cause is in the kernel cmdline:

```
skip_initramfs rootwait ro init=/init root=/dev/sde21
```

**`skip_initramfs`** tells the kernel to ignore the ramdisk and mount the
system partition directly as root. The ramdisk Magisk patches is never loaded.
This is legacy system-as-root, not a SELinux or version problem.

Untried theoretical fix: strip `skip_initramfs` from the cmdline with
`magiskboot`. Risky — the entire boot is built around mounting `sde21`
directly. Back up `boot_b` first.

**You do not need Magisk** for anything in this repo. The `sepolicy.unlocked`
swap covers the SELinux side, and flashing covers the write side.

---

## Device reference

```
Model            MH-A32 (32 GB)        Codename    pacific
FCC ID           2AGOZMH-A             USB id      2833:0082
Display          2560x1440, density 480
Android          7.1.1, Snapdragon 821, 3 GB RAM, 3DoF
Active slot      _b
  boot_b         /dev/block/sde18 / sde19
  system_b       /dev/block/sde21   (1.8 GB, ~48 MB free)
Bootloader       VOLUME DOWN + power, cable unplugged
```

Relevant packages: `com.oculus.browser` (Chromium — `.WebVRActivity`,
`.PanelService`, plus `MediaLauncherActivity`/`AudioLauncherActivity` which
accept `VIEW` + `file` scheme), `com.oculus.os.settings`,
`com.android.gallery3d`, `com.oculus.horizon`.

Tried and rejected: **Firefox Reality** (`org.mozilla.vrbrowser`) installs and
launches but renders at wrong scale — it is a native VR app that wants its own
compositor and cannot have one inside vrshell. **TV Bro**
(`com.phlox.tvwebbrowser` 2.0.1 arm-v7a) launches but is not rendered in
stereo; the right eye shows the left eye's panel split.

---

## Repository layout

```
scripts/    fish functions, the system-image modification helper,
            btmouse.py (unfinished Bluetooth HID mouse)
patches/    the init service and the autostart payload
docs/       SELinux error reference, Bluetooth HID notes
```

---

## Disclaimer

Unlocking wipes your device and voids whatever warranty a 2018 headset still
has. Flashing the wrong partition can brick it. **Back up `boot_b` and
`system_b` before touching anything** — `./scripts/sysmod.sh backup` does this.

Tested on exactly one device. Your mileage will vary.
