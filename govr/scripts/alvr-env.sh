# Source me: environment for cross-compiling ALVR's client core for the Go (API 25).
TC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/toolchain"
export RUSTUP_HOME=$TC/rustup CARGO_HOME=$TC/cargo
export PATH=$TC/cargo/bin:$PATH
NDK=$TC/android-ndk-r28c; NDKB=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
export ANDROID_NDK_HOME=$NDK ANDROID_NDK_ROOT=$NDK
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$NDKB/aarch64-linux-android25-clang
export CC_aarch64_linux_android=$NDKB/aarch64-linux-android25-clang
export CXX_aarch64_linux_android=$NDKB/aarch64-linux-android25-clang++
export AR_aarch64_linux_android=$NDKB/llvm-ar
export PATH=$NDKB:$PATH
