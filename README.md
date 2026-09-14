# Oculus Go Revival — MH-A32 (`pacific`)
[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-support-yellow?logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/kennethbicocchi)

Bringing a dead Oculus Go back to life in 2026: unlocked bootloader, root,
working SELinux, an app launcher you can navigate with a gamepad, and
real-time keyboard input — **without the Meta account, without the original
controller, and without the phone app.**

Everything here was tested on a single device: **Oculus Go MH-A32, 32 GB,
manufactured 2018, codename `pacific`**, running the official Meta unlocked
build. Host machine: Arch Linux (CachyOS), fish shell.

> **Why this repo exists.** Public documentation for the Go stops around 2022.
> None of it mentions that Meta shipped an unused permissive SELinux policy
> inside the system image, or how to get an init service to survive on this
> device, or that pairing with the modern Meta Horizon app will wipe an
> unlocked headset. Those facts cost several nights to discover. They are
> written down here so nobody has to rediscover them.

---

## What actually works after following this

| Capability | Status |
|---|---|
| Unlocked bootloader, flashable partitions | ✅ |
| Root via `adb root` | ✅ |
| SELinux manageable (`setenforce` works) | ✅ |
| Boots straight into a working app, no PC needed | ✅ |
| Real-time keyboard from the PC, **including inside WebXR sessions** | ✅ |
| Live view of what the headset sees | ✅ |
| Wi-Fi, YouTube, 360/180 video playback | ✅ |
| Xbox gamepad navigation of system menus | ✅ |
| **App launcher + gamepad button to return to it** | ✅ |
| **Real Android settings panel** | ✅ |
| **Taps via `adb shell input tap`** (fullscreen apps only) | ✅ |
| VR pointer / cursor | ❌ unsolved |
| Alternative browsers | ❌ all fail, see below |
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
Note it restarts adbd, which drops scrcpy and kicks vrshell back into its
login loop — do it *before* starting scrcpy, never during.

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

## Launching apps: the rules

This took a long time to work out, and none of it is obvious.

### Privileged system apps: `monkey`

The **real Android settings panel** is not reachable from the Oculus shell,
but it is still installed:

```bash
adb shell monkey -p com.android.settings -c android.intent.category.LAUNCHER 1
```

Wi-Fi, Bluetooth, Display, Apps, Notifications and Users are all there.
Account, Security and Developer Options have been stripped out by Meta.

This works because Settings is a privileged system app. The same command on a
sideloaded app injects the event and starts nothing useful.

### Sideloaded apps: `adb root` is mandatory

From the shell user, `am start` on a sideloaded app fails:

```
java.lang.SecurityException: Permission Denial: starting Intent ...
from null (pid=21334, uid=2000) not exported from uid 10054
```

With root it works, and the app comes up **fullscreen with its own focus**,
outside the vrshell panel:

```bash
adb root
sleep 3
adb shell am start -n com.example.app/.MainActivity
```

### When scrcpy is running: `am stack start 0`

scrcpy creates a **secondary display** (id 66 here), and some apps launch onto
it — they run but you never see them. The log gives it away:

```
WindowManager: Secondary display focus changed from null to Window{... on display 66}
```

Android 7.1 has no `am start --display`. The equivalent is:

```bash
adb shell am stack start 0 -n com.example.app/.MainActivity
```

Closing scrcpy first also works.

### Taps work — under two conditions

`adb shell input tap` reaches an app when it is **fullscreen with its own
focus** (not inside the vrshell panel) **and the headset display is awake**.
A sleeping display swallows every event silently, which looks exactly like a
broken coordinate. Check focus before blaming coordinates:

```bash
adb shell dumpsys window windows | grep mCurrentFocus
```

Coordinate conversion from a scrcpy screenshot: the panel is 2560x1440, so
multiply screenshot coordinates by `2560 / screenshot_width`.

### Loading a URL from the command line

Works on any browser that registers a `VIEW` intent:

```bash
adb shell am start -a android.intent.action.VIEW -d "https://example.com" \
  -n org.mozilla.vrbrowser/org.mozilla.vrbrowser.VRBrowserActivity
```

---

## An actual launcher, navigable with a gamepad

Android's HOME mechanism is **removed** on this device:
`pm query-activities -c android.intent.category.HOME` returns **zero**
activities system-wide, even after installing a launcher, and
`set-home-activity` refuses everything. So a launcher cannot be installed as
home in the normal way.

What works instead is a plain 2D launcher app plus **Key Mapper** bound to a
gamepad button. That gives a complete loop with no PC attached: press the
button, the launcher appears, pick an app, it opens fullscreen.

### Setting it up

1. Sideload a 2D launcher and Key Mapper (`io.github.sds100.keymapper`).
2. Launch the launcher once with root, add the apps you want via its `+`
   button (use `input tap` if the gamepad can't reach it).
3. Enable Key Mapper's accessibility service. It cannot be enabled from the
   stripped Settings menu, but `settings put` works — find the exact class
   first:

```bash
adb shell dumpsys package io.github.sds100.keymapper | grep -i accessibilityservice

adb shell settings put secure enabled_accessibility_services \
  io.github.sds100.keymapper/io.github.sds100.keymapper.system.accessibility.MyAccessibilityService
adb shell settings put secure accessibility_enabled 1
```

4. In Key Mapper: record a trigger (Start is a good choice — it has no default
   Android function, unlike B which is BACK), add action "open app" → your
   launcher, Done.

Key Mapper itself responds to the gamepad, so step 4 onward can be done in
the headset.

### Two gotchas

Some launchers force **portrait orientation**, which renders sideways in VR.
Force landscape system-wide:

```bash
adb shell settings put system accelerometer_rotation 0
adb shell settings put system user_rotation 0      # try 1 if 0 is wrong
```

And check what you're installing: the launcher used during this work requests
consent for **311 advertising partners** on first run, including precise
location. On a VR headset that is absurd. Prefer an open-source launcher.

---

## Alternative browsers: all of them fail

The stock **Oculus Browser** is the only usable one, and the logs explain why.
It is Chromium, but with its engine wired directly into the VR compositor —
visible in logcat as `panel_compositor`, `panel_app`, `PanelNavUiInterface`.
It is infrastructure, not an app that happens to work.

| Browser | Outcome |
|---|---|
| **TV Bro** (`com.phlox.tvwebbrowser`) | Blank white panel. It uses the *system* WebView, which is `com.android.webview` **version 61.0.3163.98** — Chromium 61, from 2017. Dies at `initWebEngineStuff`. |
| **Firefox Reality**, generic build | Installs, launches, renders at the wrong scale. |
| **Firefox Reality `_go` build** | Renders *correctly* in stereo, then dies: "Firefox Reality does not have permission to run on this device" — the Oculus Store entitlement check, which fails with no account. |
| **Firefox Reality `_nostore` build** | Passes the entitlement check, renders beautifully, loads `https://example.com` with a valid padlock. But some sites fail with "Secure Connection Failed" (2021 root store), and **it responds to nothing** — not the gamepad, not `input tap`, not `input keyevent`. It expects the Oculus controller. |
| **Wolvic** | See below. |

Anything 2D (Cromite, Fennec, …) will load pages fine — they bundle their own
engine — but lands in the vrshell panel where there is no pointer. Predicted,
not tested.

### Wolvic: the one real lead

Wolvic is Igalia's continuation of Firefox Reality, and **its source still
references the Oculus Go**: the README documents that the native debugger
stops on each input event on Go, and gives
`setprop debug.oculus.enableVideoCapture 1` as the Go screen-recording trick.

More importantly, its Oculus build target needs `third_party/ovr_mobile/`
containing a **VrApi** folder — that is the *legacy Oculus Mobile SDK*, the
one the Go uses, not OpenXR. The build infrastructure for this device is
still there.

The obstacles are real but not fatal: `Igalia/wolvic-third-parties` is
members-only (the SDKs can be placed manually), GeckoView must be built from
source with patches or WebXR sessions won't work, and the Go has not been a
tested target for years.

**If you want a modern browser on this device, this is the path.** Nobody has
walked it recently. If you do, please open an issue here.

---

## Auto-starting an app at boot

`vrshell` loops forever on:

```
OCPlatformModule: access token fetch returned error:
'user not logged in', retrying in 10.000000 seconds
```

This is the infamous three-dots screen. It is **not** a hang — the compositor
runs at 72 FPS, no ANR — but the shell never reaches its home screen.
Disabling `com.oculus.horizon` changes the error to "Horizon isn't installed"
but the loop continues: it lives inside vrshell.

The fix is an init service that launches an app *over* the looping shell. See
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

Two more traps:

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
`scripts/sysmod.sh relabel <path>` wraps it.

---

## Input

### scrcpy: live view and a real keyboard

```bash
scrcpy --no-audio --mouse=disabled
```

Typing on the PC keyboard reaches the headset in real time — **including
inside immersive WebXR sessions**, where `adb shell input keyevent` does not
reach at all. Arrow keys scrub the timeline, Enter confirms, Esc exits.

`--mouse=disabled` is not optional: moving the mouse into the scrcpy window
sends events the VR compositor cannot handle, the screen goes black and
`vrshell` restarts into the three-dots loop.

`scrcpy --mouse=uhid` fails with `EACCES` on `/dev/uhid` (owned by
`system:net_bt_stack`). `chmod 666` clears that, but no virtual device is ever
created — `dumpsys input` shows nothing new. Android 7.1 predates the path
scrcpy 4.x uses.

### Xbox gamepad

Navigates system menus, Key Mapper, and the launcher. Does **not** scroll web
pages (it moves focus between elements; there is no pointer) and does nothing
inside WebXR sessions or native VR apps.

### Screenshots, including in immersive mode

```bash
adb shell screencap -p /sdcard/shot.png && adb pull /sdcard/shot.png .
```

### Still missing: a VR pointer

- **`sendevent` / `/dev/input`** — the Oculus controller has no device node.
  `getevent -pl` lists only onboard hardware. Writes to `event1`
  (`qbt1000_key_input`, which exposes ABS_X/ABS_Y and looks like a pointer)
  never arrive. Note `/dev/input/mouse0` and `mice` *do* exist.
- **Bluetooth HID from the PC** — gets *remarkably* far (see
  [`docs/bluetooth-hid.md`](docs/bluetooth-hid.md)); the headset registers the
  PC as a connected input device, but movement reports produce no events.
  Unfinished, and the highest-value thing left to solve: it would unlock every
  app at once.

### ⚠️ Never pair with the Meta Horizon app

On an unlocked headset the app triggers **a factory reset, a "your device
can't be checked for corruption" message, and a shutdown.** The unlocked
bootloader fails the integrity check and the app responds by restoring the
device. This is the designed behaviour, not a bug to work around.

There is no way to log in an account on an unlocked Go. The in-headset Profile
panel spins forever on the same token the shell cannot fetch. Library, Store
and Search are storefronts for a service that no longer exists — they will
never work, account or no account.

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
System WebView   Chromium 61.0.3163.98 (2017)
Active slot      _b
  boot_b         /dev/block/sde18 / sde19
  system_b       /dev/block/sde21   (1.8 GB, ~48 MB free)
Bootloader       VOLUME DOWN + power, cable unplugged
Max users        4 (multi-user is intact)
```

Relevant packages: `com.oculus.browser` (Chromium — `.WebVRActivity`,
`.PanelService`), `com.oculus.os.settings`, `com.android.settings`,
`com.android.gallery3d`, `com.oculus.horizon`, `com.oculus.socialplatform`.

An activity called **"Oculus Home"** (Gear VR icon) shows up in app pickers
and has never been explored.

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
