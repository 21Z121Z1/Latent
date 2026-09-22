from pathlib import Path
p=Path.cwd()
def edit(name,a,b):
 f=p/name;s=f.read_text();assert a in s,name;f.write_text(s.replace(a,b))
edit('android/app/src/main/cpp/NativePipeline.cpp',
 '<< ",\\\"reason\\\":\\\"" << escape(plan.reason) << "\\\",\\\"frames\\\":[";',
 '<< ",\\\"reason\\\":\\\"" << escape(plan.reason) << "\\\""\n         << ",\\\"motionConstraintUsed\\\":" << (plan.motionConstraintUsed ? "true" : "false")\n         << ",\\\"noiseEstimated\\\":" << (plan.noiseEstimated ? "true" : "false")\n         << ",\\\"qualityLimited\\\":" << (plan.qualityLimited ? "true" : "false") << ",\\\"frames\\\":[";')
edit('android/app/src/main/java/dev/latent/camera/CameraController.kt', '.put("gyroUsed", motion != null)', '.put("gyroAvailable", motion != null)')
edit('android/app/src/main/java/dev/latent/camera/ProcessingModel.kt',
 'text.appendLine("Gyro used: ${capture.getBoolean("gyroUsed")}. Physical synchronization is not calibrated by this app.")',
 'text.appendLine("Gyro available: ${capture.getBoolean("gyroAvailable")}; exposure constrained: ${capture.getJSONObject("policy").getBoolean("motionConstraintUsed")}. Physical synchronization is not calibrated by this app.")')
f=p/'android/app/src/androidTest/java/dev/latent/camera/SensorTransportTest.kt'
s=f.read_text();at=s.rindex('\n}');s=s[:at]+'''

    @Test fun captureTraceDistinguishesAvailableMotionFromAppliedPolicy() {
        val caps = CaptureCapabilityInput(true,true,true,100_000,200_000_000,33_333_333,1_000_000_000,
            50,3200,128_000_000,8_000_000,8)
        fun plan(speed: Float, comparable: Boolean) = JSONObject(NativeBridge.capturePlan(
            CaptureObservationInput(20_000_000,33_333_333,400,true,0.0001f,speed,comparable),
            CaptureIntentInput(),caps))
        assertFalse(plan(0.001f,true).getBoolean("motionConstraintUsed"))
        assertFalse(plan(1f,false).getBoolean("motionConstraintUsed"))
        val constrained = plan(1f,true)
        assertTrue(constrained.getBoolean("motionConstraintUsed"))
        assertFalse(constrained.getBoolean("noiseEstimated"))
        assertTrue(constrained.getBoolean("qualityLimited"))
        assertTrue(constrained.getJSONArray("frames").getJSONObject(0).getLong("exposureNs") <= 1_500_001)
    }
'''+s[at:];f.write_text(s)
f=p/'android/app/src/androidTest/java/dev/latent/camera/ReplayUiTest.kt';s=f.read_text().replace('import androidx.compose.ui.test.assertIsDisplayed','import android.graphics.Bitmap\nimport androidx.test.platform.app.InstrumentationRegistry\nimport androidx.compose.ui.test.assertIsDisplayed');s=s.replace('        compose.onNodeWithTag("result_image").assertIsDisplayed()','''        compose.onNodeWithTag("result_image").assertIsDisplayed()
        // Capture before ActivityScenario teardown; a later shell screenshot is
        // only the launcher and cannot establish what the result UI displayed.
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val screenshot = checkNotNull(instrumentation.uiAutomation.takeScreenshot())
        try {
            instrumentation.targetContext.openFileOutput("replay-result.png", 0).use {
                check(screenshot.compress(Bitmap.CompressFormat.PNG, 100, it))
            }
        } finally { screenshot.recycle() }''');f.write_text(s)
edit('tools/ci/android-replay.sh',
 './gradlew --no-daemon --stacktrace --max-workers=2 connectedDebugAndroidTest',
 './gradlew --no-daemon --stacktrace --max-workers=2 connectedDebugAndroidTest\nadb exec-out run-as dev.latent.camera cat files/replay-result.png > "$root/build-host/android-replay-result.png"\ntest -s "$root/build-host/android-replay-result.png"')
edit('.github/workflows/android.yml','            build-host/android-screenshot.png','            build-host/android-screenshot.png\n            build-host/android-replay-result.png')
# Restore the pre-PR nonlinear-texture fractional case, without replacing the
# independently added band-limited gate or weakening the original 0.6 px bound.
f=p/'tests/test_temporal.cpp';s=f.read_text();anchor='    const auto bandLimited = [](float x, float y, std::size_t) {';assert anchor in s;s=s.replace(anchor,'''    {
        auto r = test::frame(1, {161,129}, imaging::CfaPattern::RGGB,0,0,0.001F);
        auto s = test::frame(2, {161,129}, imaging::CfaPattern::RGGB,0.75F,-1.25F,0.001F);
        const auto rn = reference::normalizeTemporalRaw(runtime::viewRawFrame(r), r, {}, false);
        const auto sn = reference::normalizeTemporalRaw(runtime::viewRawFrame(s), r, {}, false);
        const auto a = reference::alignTemporalRaw(rn,sn,imaging::FrameId{1},imaging::FrameId{2},{});
        std::cout << "nonlinear texture subpixel=" << a.global.dx << ',' << a.global.dy << '\\n';
        near(a.global.dx,0.75F,0.6F,"nonlinear-texture horizontal displacement");
        near(a.global.dy,-1.25F,0.6F,"nonlinear-texture vertical displacement");
    }
'''+anchor);f.write_text(s)
