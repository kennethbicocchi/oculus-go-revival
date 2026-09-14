# Insert this block at the TOP of /system/bin/init.oculus.properties.sh,
# immediately after `set -o nounset` (i.e. after line 3).
#
# It must go at the top: the stock script has `set -o errexit`, and the
# `setprop ro.*` calls further down fail on the second boot because ro.*
# properties are immutable once set. An appended block would never be reached.
#
#   sudo sed -i '3r autostart-block.sh' <mount>/system/bin/init.oculus.properties.sh
#   ./sysmod.sh relabel system/bin/init.oculus.properties.sh
#
# Change the component in `am start` to auto-launch a different app.
#
# Do NOT add LD_LIBRARY_PATH. Setting it to /vendor/lib:/system/lib breaks
# with "CANNOT LINK EXECUTABLE: libc++.so is 32-bit instead of 64-bit".
#
# Do NOT redirect output to /data/local/tmp: the shell domain has no
# dac_override and the write fails, taking the whole script down with it.

# autostart browser, invoked by init as a service
if [ "${1:-}" = "autostart" ]; then
  sleep 40
  export PATH=/sbin:/vendor/bin:/system/sbin:/system/bin:/system/xbin
  export ANDROID_DATA=/data
  export ANDROID_ROOT=/system
  export ANDROID_ASSETS=/system/app
  export ANDROID_STORAGE=/storage
  export BOOTCLASSPATH=/system/framework/core-oj.jar:/system/framework/core-libart.jar:/system/framework/conscrypt.jar:/system/framework/okhttp.jar:/system/framework/core-junit.jar:/system/framework/bouncycastle.jar:/system/framework/ext.jar:/system/framework/framework.jar:/system/framework/telephony-common.jar:/system/framework/voip-common.jar:/system/framework/ims-common.jar:/system/framework/apache-xml.jar:/system/framework/org.apache.http.legacy.boot.jar:/system/framework/com.oculus.os.platform.jar
  /system/bin/am start -n com.oculus.browser/.WebVRActivity
  exit 0
fi
