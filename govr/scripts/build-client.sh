#!/usr/bin/env bash
# build-client.sh [--install] [--run]: build the Go client APK without Gradle.
# Output: build/client/govr.apk
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TC="$ROOT/toolchain"; SRC="$ROOT/src/client"; OUT="$ROOT/build/client"
NDK="$TC/android-ndk-r28c"; LLVM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
BT="$TC/build-tools/android-14"; JAR="$TC/platforms/android-7.1.1/android.jar"
# Must match the Go runtime (VrApi 1.1.35, see DECISIONS.md D-006)
SDK="$TC/ovr_sdk_mobile_1.35"; VRAPI_LIB="$SDK/VrApi/Libs/Android/arm64-v8a/Release/libvrapi.so"
export JAVA_HOME="$TC/jdk" PATH="$TC/jdk/bin:$PATH"
CXX="$LLVM/aarch64-linux-android25-clang++"; CC="$LLVM/aarch64-linux-android25-clang"

rm -rf "$OUT"; mkdir -p "$OUT"/{obj,classes,apk/lib/arm64-v8a}
# --- native
"$ROOT/scripts/build-opus.sh" >/dev/null
OPUS="$ROOT/build/opus"
CFLAGS=(-O2 -g -fPIC -Wall -Wno-unused-function -I"$OPUS/include/opus" -I"$SDK/VrApi/Include" -I"$NDK/sources/android/native_app_glue" -I"$SRC/jni" ${EXTRA_CFLAGS:-})
"$CC" "${CFLAGS[@]}" -c "$NDK/sources/android/native_app_glue/android_native_app_glue.c" -o "$OUT/obj/glue.o"
objs=("$OUT/obj/glue.o")
for f in "$SRC"/jni/*.cpp; do
  o="$OUT/obj/$(basename "${f%.cpp}").o"; "$CXX" "${CFLAGS[@]}" -std=c++17 -c "$f" -o "$o"; objs+=("$o")
done
"$CXX" -shared -o "$OUT/apk/lib/arm64-v8a/libgovr.so" "${objs[@]}" \
  -L"$(dirname "$VRAPI_LIB")" -lvrapi "$OPUS/lib/libopus.a" -lOpenSLES -lEGL -lGLESv3 -landroid -llog -lmediandk \
  -u ANativeActivity_onCreate -static-libstdc++ -Wl,--no-undefined ${EXTRA_LDFLAGS:-}
cp "$VRAPI_LIB" "$OUT/apk/lib/arm64-v8a/"
# SteamVR mode (optional): ALVR client core, built by scripts/build-alvr-core.sh
[[ -f "$ROOT/build/alvr/libalvr_client_core.so" ]] && cp "$ROOT/build/alvr/libalvr_client_core.so" "$OUT/apk/lib/arm64-v8a/"
for extra in ${EXTRA_SOS:-}; do cp "$extra" "$OUT/apk/lib/arm64-v8a/"; done
# --- java
javac -nowarn --release 8 -cp "$JAR" -d "$OUT/classes" $(find "$SRC/java" -name '*.java') 2>&1 | grep -v "^warning\|^Note\|^1 warning" || true
"$BT/d8" --min-api 25 --lib "$JAR" --output "$OUT/apk" $(find "$OUT/classes" -name '*.class')
# --- package
"$BT/aapt2" link -o "$OUT/base.apk" --manifest "$SRC/AndroidManifest.xml" -I "$JAR" \
  --min-sdk-version 25 --target-sdk-version 25
python3 - "$OUT/base.apk" "$OUT/apk" <<'PY'
import sys, os, zipfile
apk, root = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(root):
        for f in files:
            full = os.path.join(d, f); z.write(full, os.path.relpath(full, root))
PY
"$BT/zipalign" -f -p 4 "$OUT/base.apk" "$OUT/aligned.apk"
KS="$ROOT/keystore/debug.keystore"
if [[ ! -f "$KS" ]]; then
  mkdir -p "$(dirname "$KS")"
  keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias govr \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=GoVR debug" >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --out "$OUT/govr.apk" "$OUT/aligned.apk"
ls -l "$OUT/govr.apk"
for a in "$@"; do case "$a" in
  --install) adb install -r "$OUT/govr.apk" ;;
  --run) "$ROOT/scripts/go-launch" com.govr.client/.MainActivity ;;
esac; done
