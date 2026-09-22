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
# AGP uninstalls both packages at the end of connected tests. Reinstall the
# exact built APKs and run the one UI test explicitly so its private screenshot
# can be retrieved before teardown. Keep the full AGP/JUnit report above.
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb install -r app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
adb shell run-as dev.latent.camera rm -f files/replay-result.png
adb shell am instrument -w -r -e class dev.latent.camera.ReplayUiTest \
    dev.latent.camera.test/androidx.test.runner.AndroidJUnitRunner \
    | tee "$root/build-host/android-visual-instrumentation.log"
grep -Eq '^OK \(1 test\)' "$root/build-host/android-visual-instrumentation.log"
adb exec-out run-as dev.latent.camera cat files/replay-result.png > "$root/build-host/android-replay-result.png"
python3 - "$root/build-host/android-replay-result.png" <<'PYPNG'
import pathlib, struct, sys, zlib
raw = pathlib.Path(sys.argv[1]).read_bytes()
assert raw[:8] == b"\x89PNG\r\n\x1a\n", "Not a PNG screenshot (adb errors may exit zero)"
pos, chunks = 8, []
while pos < len(raw):
    size, tag = struct.unpack_from(">I4s", raw, pos)
    end = pos + size + 12
    assert end <= len(raw), "Truncated PNG"
    payload = raw[pos+8:pos+8+size]
    expected, = struct.unpack_from(">I", raw, pos+8+size)
    assert zlib.crc32(tag+payload) == expected, "PNG CRC mismatch"
    chunks.append(tag)
    if tag == b"IHDR":
        width, height = struct.unpack_from(">II", payload)
        assert width >= 64 and height >= 64, "Empty result screenshot"
    pos = end
assert chunks[0] == b"IHDR" and b"IDAT" in chunks and chunks[-1] == b"IEND"
print(f"Verified result screenshot: {width}x{height}, {len(raw)} bytes")
PYPNG
