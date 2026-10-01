#!/usr/bin/env bash
# setup-toolchain.sh: download everything needed to build GoVR into ./toolchain (no root).
# Android NDK r28c, build-tools 34, platform android-25, JDK 25, CMake, libopus source,
# Oculus Mobile SDK 1.35 (Oculus SDK licence: downloaded from Meta, never redistributed),
# Rust + aarch64-linux-android target, ALVR v20.14.1 Linux streamer.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; TC="$ROOT/toolchain"; mkdir -p "$TC"; cd "$TC"
G=https://dl.google.com/android/repository
fetch() { [[ -e "$2" ]] || { echo "downloading $2..."; curl -fsSL -o "$2.dl" "$1" && mv "$2.dl" "$2"; }; }
fetch $G/android-ndk-r28c-linux.zip ndk.zip;            [[ -d android-ndk-r28c ]] || unzip -q ndk.zip
fetch $G/build-tools_r34-linux.zip bt.zip;              [[ -d build-tools ]] || unzip -q bt.zip -d build-tools
fetch $G/platform-25_r03.zip p25.zip;                   [[ -d platforms ]] || unzip -q p25.zip -d platforms
fetch "https://api.adoptium.net/v3/binary/latest/25/ga/linux/x64/jdk/hotspot/normal/eclipse" jdk.tgz
[[ -d jdk ]] || { mkdir jdk; tar xzf jdk.tgz -C jdk --strip-components=1; }
fetch https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz cmake.tgz
[[ -d cmake ]] || { mkdir cmake; tar xzf cmake.tgz -C cmake --strip-components=1; }
fetch https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz opus.tgz; [[ -d opus-1.5.2 ]] || tar xzf opus.tgz
# Mobile SDK 1.35.0 matches the Go's VrApi runtime (1.1.35). Link from Meta's download page.
fetch "https://securecdn.oculus.com/binaries/download/?id=3324204000975916" ovr_sdk_mobile_1.35.0.zip
[[ -d ovr_sdk_mobile_1.35 ]] || unzip -q ovr_sdk_mobile_1.35.0.zip -d ovr_sdk_mobile_1.35
fetch https://github.com/alvr-org/ALVR/releases/download/v20.14.1/alvr_streamer_linux.tar.gz alvr.tgz
[[ -d alvr_streamer_linux ]] || tar xzf alvr.tgz
if [[ ! -x cargo/bin/cargo ]]; then
  curl -fsSL https://sh.rustup.rs -o rustup-init.sh
  RUSTUP_HOME=$TC/rustup CARGO_HOME=$TC/cargo sh rustup-init.sh -y --no-modify-path \
    --profile minimal -t aarch64-linux-android
fi
echo "toolchain ready in $TC"
