# SELinux on the Oculus Go — error reference

Every wall hit on this device turned out to be SELinux. None of them announced
itself clearly; they showed up as exit code 127, as silence, or as a service
that died in five milliseconds.

## The one command that matters

```bash
adb shell "logcat -d | grep 'avc: denied'"
```

Run this *before* changing anything. Eight blind flash cycles were spent
guessing at header bytes and service syntax; the first run of this command
named the exact problem.

Note the `permissive=0` at the end of each denial: that means the action was
actually blocked, not just logged.

## Denials encountered, and what they mean

### `Service does not have a SELinux domain defined`

```
init: Service ocubrowser does not have a SELinux domain defined.
```

Not an `avc: denied` — init refuses before SELinux is consulted. The
`seclabel` you gave is not a domain that exists at init time.

`u:r:su:s0` looks reasonable because `adb root` puts you in it, but that
domain is created by the adb root transition. It does not exist during boot.

### `execute_no_trans`

```
avc: denied { execute_no_trans } for path="/system/bin/logwrapper"
scontext=u:r:init:s0 tcontext=u:object_r:system_file:s0 tclass=file
```

The init domain may not execute a `system_file` and stay in its own domain.
It would have to transition into another domain, and the policy has no rule
allowing it. Using `seclabel u:r:init:s0` on a service that runs a
`/system/bin/` binary always hits this.

### `entrypoint`

```
avc: denied { entrypoint } for path="/system/bin/logwrapper"
scontext=u:r:shell:s0 tcontext=u:object_r:system_file:s0
```

Progress: the domain is now valid, but the *binary* is not an authorised entry
point into it. For `u:r:shell:s0` only `/system/bin/sh` qualifies. Wrapping
the command in `logwrapper` — useful for getting output into logcat — breaks
this. Debug with it, then remove it.

### `read` on an `unlabeled` file

```
avc: denied { read } for name="init.oculus.properties.sh"
scontext=u:r:shell:s0 tcontext=u:object_r:unlabeled:s0
```

**The most expensive mistake in this project.** Editing a file inside a
loop-mounted image with `sed`, `tee` or a redirect destroys its
`security.selinux` extended attribute. The file keeps its Unix permissions and
looks fine, but SELinux sees it as `unlabeled` and refuses everything.

Compare against a known-good file:

```bash
sudo getfattr -n security.selinux <mount>/system/bin/am
# security.selinux="u:object_r:system_file:s0"

sudo getfattr -n security.selinux <mount>/system/bin/your-edited-file
# No such attribute        <- this is the bug
```

Fix, including the trailing NUL that the kernel expects:

```bash
sudo setfattr -n security.selinux -v "u:object_r:system_file:s0\0" <file>
```

Verify byte-for-byte against a stock file:

```bash
sudo getfattr --only-values -n security.selinux <file> | xxd | tail -2
```

Both should end `...6d5f 6669 6c65 3a73 3000` — `m_file:s0.`

### `dac_override`

```
avc: denied { dac_override } for capability=1
scontext=u:r:shell:s0 tcontext=u:r:shell:s0 tclass=capability
```

The service runs as root but SELinux withholds the capability that makes root
useful: it cannot bypass Unix permission checks. Writing to
`/data/local/tmp` from an init service in the shell domain fails for this
reason. Don't put log redirects there.

## Things SELinux is *not* responsible for

Two dead ends that look like SELinux and aren't:

**`/sys/block/sde/sde21/ro`** stays at `1` and rejects writes even with
`setenforce 0`. That is the kernel's write protection on the UFS partition,
not policy. The only way to write to system is to flash it via fastboot.

**`CANNOT LINK EXECUTABLE "/system/bin/sh": libc++.so is 32-bit instead of
64-bit`** is a linker problem caused by setting `LD_LIBRARY_PATH` by hand.
Unset it and the error disappears.

## `setenforce 0` and why it fails on a stock unlocked build

Under the shipped policy, `setenforce` is denied even to uid 0 with SELinux
context `u:r:su:s0`. The policy does not grant the domain permission to change
its own enforcement state.

After swapping in `sepolicy.unlocked` (see the main README), `setenforce 0`
succeeds. It does **not** persist across reboots — the device comes up
`Enforcing` every time. If you need permissive at boot, `write
/sys/fs/selinux/enforce 0` inside an init `on property:sys.boot_completed=1`
stanza is the init.rc-valid form (`setenforce` is not an init command), though
on this device it appeared to have no effect and was dropped in favour of
simply using the unlocked policy.
