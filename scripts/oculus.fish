# Oculus Go helper functions — fish shell
#
# Install:  source oculus.fish  (then each function is saved permanently)
# Or copy the individual functions into ~/.config/fish/functions/

function ocu --description "Live view + real-time keyboard for the Oculus Go"
    # --mouse=disabled is REQUIRED. Moving the mouse into the scrcpy window
    # sends events the VR compositor cannot handle: the screen goes black and
    # vrshell restarts into the three-dots loop.
    #
    # The keyboard works everywhere, including inside immersive WebXR
    # sessions, where `adb shell input keyevent` does not reach at all.
    scrcpy --no-audio --mouse=disabled
end
funcsave ocu

function ocubrowser --description "Launch the VR browser over the shell"
    adb shell am start -n com.oculus.browser/.WebVRActivity
end
funcsave ocubrowser

function ocuclean --description "Clear the browser and reboot — fixes a wedged headset"
    adb root
    sleep 2
    adb shell pm clear com.oculus.browser
    adb reboot
end
funcsave ocuclean

function ocushot --description "Screenshot what the headset sees (works in immersive mode)"
    set -l name (date +%Y%m%d-%H%M%S).png
    adb shell screencap -p /sdcard/$name
    adb pull /sdcard/$name .
    adb shell rm /sdcard/$name
    echo "Saved $name"
end
funcsave ocushot

function ocutype --description "Type a string into an already-focused text field"
    # Only needed when scrcpy is not running. `input text` drops characters
    # if sent too fast — 0.4 s per character is the tested minimum.
    for c in (string split '' $argv[1])
        adb shell input text "$c"
        sleep 0.4
    end
end
funcsave ocutype

function ocudenied --description "Show SELinux denials — the diagnostic that matters"
    adb shell "logcat -d | grep 'avc: denied'"
end
funcsave ocudenied

function ocuservice --description "Did the autostart service run? Check fork/exit timing"
    # A ~40 s gap between 'forked success' and 'exited' means the sleep ran and
    # `am` was reached. An exit within milliseconds means it never started:
    # run ocudenied.
    adb shell "dmesg | grep -i ocubrowser"
end
funcsave ocuservice
