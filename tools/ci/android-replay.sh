#!/usr/bin/env bash
# This verifies replay, not an emulated claim of physical RAW camera support.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
export PATH="$ANDROID_HOME/platform-tools:$ANDROID_HOME/emulator:$PATH"
avd_home="${ANDROID_AVD_HOME:-${ANDROID_SDK_HOME:-$HOME/.android}/avd}"
if [ ! -f "$avd_home/latent-fixture.ini" ]; then
    echo "Android AVD latent-fixture is missing from $avd_home" >&2
    emulator -list-avds >&2 || true
    exit 1
fi
emulator -avd latent-fixture -no-window -no-snapshot -no-audio -no-boot-anim \
    -gpu swiftshader_indirect -camera-back none -camera-front none -memory 2048 -cores 2 \
    >build-host/android-emulator.log 2>&1 &
pid=$!
cleanup() {
    adb logcat -d >"$root/build-host/android-logcat.log" 2>&1 || true
    adb exec-out screencap -p >"$root/build-host/android-screenshot.png" 2>/dev/null || true
    adb emu kill >/dev/null 2>&1 || true
    kill "$pid" 2>/dev/null || true
}
trap cleanup EXIT
for attempt in $(seq 1 150); do
    if [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; then break; fi
    kill -0 "$pid"
    sleep 2
done
test "$(adb shell getprop sys.boot_completed | tr -d '\r')" = 1
adb shell settings put global window_animation_scale 0
adb shell settings put global transition_animation_scale 0
adb shell settings put global animator_duration_scale 0
adb shell input keyevent 82
cd android
./gradlew --no-daemon --stacktrace --max-workers=2 connectedDebugAndroidTest
