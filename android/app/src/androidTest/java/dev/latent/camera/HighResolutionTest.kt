package dev.latent.camera

import android.Manifest
import android.util.Size
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.rule.GrantPermissionRule
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class HighResolutionTest {
    @get:Rule val permission: GrantPermissionRule = GrantPermissionRule.grant(Manifest.permission.CAMERA)
    private fun mode(group: Int, phase: Int, size: Int) = SensorModeInput(
        cfa = phase, groupWidth = group, groupHeight = group, groupingUsed = 1,
        ultraHighResolution = true, remosaicReprocessing = true, pixelModeAvailable = true,
        requestedMode = 1, actualMode = 1, pixelWidth = size, pixelHeight = size, rawWidth = size, rawHeight = size,
        active = intArrayOf(0, 0, size, size), preCorrection = intArrayOf(0, 0, size, size),
        deliveredCrop = intArrayOf(0, 0, size, size), croppedRaw = false, rawCrop = intArrayOf(),
        zoom = 1f, coordinateSpace = "instrumentation/mode:1",
    )

    @Test fun canonicalModeProjectionAndFileReconstruction() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val directory = File(context.cacheDir, "highres-fixture-${System.nanoTime()}").apply { check(mkdirs()) }
        try {
            for (group in 1..4) for (phase in 0..3) {
                val mode = mode(group, phase, 32)
                val sampling = JSONObject(NativeBridge.interpretSensorMode(mode))
                assertEquals(group, sampling.getJSONArray("group").getInt(0))
                assertEquals(if (group == 1) 1 else 2, sampling.getInt("representation"))
                val input = RawInput(1, 1_000_000_000, 10_000_000, 100f, 32, 32, 64, phase,
                    cameraId = "instrumentation", sensorMode = "grouped", pixels = ByteBuffer.allocateDirect(0),
                    black = floatArrayOf(64f, 64f, 64f, 64f), white = 4095f,
                    noise = floatArrayOf(0f, 0.00001f, 0f, 0.00001f, 0f, 0.00001f, 0f, 0.00001f),
                    whiteBalance = floatArrayOf(1f, 1f, 1f, 1f),
                    sensorToLinearSrgb = floatArrayOf(1f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 1f), synthetic = true)
                val file = File(directory, "$group-$phase.raw16")
                val data = ByteBuffer.allocate(32 * 32 * 2).order(ByteOrder.LITTLE_ENDIAN)
                repeat(32 * 32) { data.putShort(1273.toShort()) }
                FileOutputStream(file).use { it.write(data.array()) }
                val output = File(directory, "$group-$phase.lrgb")
                val trace = JSONObject(NativeBridge.reconstructRawFiles(arrayOf(input), arrayOf(mode), arrayOf(file.absolutePath),
                    output.absolutePath, false, 32L * 1024 * 1024, 0))
                assertEquals("reference-camera-linear-green-balanced", trace.getString("output_domain"))
                assertEquals(1024, trace.getJSONObject("trace").getInt("output_pixels"))
                assertTrue(output.length() >= 32L * 32 * 64)
                assertTrue(!File(output.path + ".partial").exists())
                val bytes = output.readBytes()
                val header = bytes.indexOf('\n'.code.toByte())
                val end = (header + 1 until bytes.size).first { bytes[it] == '\n'.code.toByte() } + 1
                val values = ByteBuffer.wrap(bytes, end, bytes.size - end).order(ByteOrder.LITTLE_ENDIAN)
                repeat(1024) {
                    val pixel = FloatArray(16) { values.float }
                    for (c in 0..2) {
                        assertEquals((1273f - 64f) / (4095f - 64f), pixel[c], 0.00001f)
                        assertTrue(pixel[12 + c] > 0f)
                    }
                }
            }
        } finally { directory.deleteRecursively() }
    }

    @Test fun unknownGroupingFailsClosed() {
        val m = mode(2, 0, 32)
        // JNI observation corruption is rejected before any reconstruction allocation.
        val unknown = SensorModeInput(m.cfa, m.groupWidth, m.groupHeight, -1, true, true, true, 1, 1,
            32, 32, 32, 32, m.active, m.preCorrection, m.deliveredCrop, false, intArrayOf(), 1f, "unknown")
        var rejected = false
        try { NativeBridge.interpretSensorMode(unknown) } catch (_: IllegalArgumentException) { rejected = true }
        assertTrue(rejected)
    }

    /** Explicit opt-in: emulators cannot verify a real grouped/UHR sensor. */
    @Test fun deviceCaptureBundle() {
        val args = InstrumentationRegistry.getArguments()
        assumeTrue("Physical Camera2 opt-in required", args.getString("latentHighresDevice") == "true")
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val backend = HighResolutionCapture(context)
        val root = checkNotNull(context.getExternalFilesDir(null))
        File(root, "highres-probe.json").writeText(backend.probe().toString(2))
        val id = args.getString("cameraId") ?: error("Specify cameraId from probe")
        val width = args.getString("rawWidth")?.toInt() ?: error("Specify rawWidth from probe")
        val height = args.getString("rawHeight")?.toInt() ?: error("Specify rawHeight from probe")
        val directory = File(root, "highres-${System.currentTimeMillis()}")
        val report = backend.capture(HighResolutionCapture.Options(id,
            pixelMode = args.getString("pixelMode")?.toInt() ?: 0, size = Size(width, height),
            frames = args.getString("frames")?.toInt() ?: 3,
            exposureNs = args.getString("exposureNs")?.toLong() ?: 10_000_000,
            iso = args.getString("iso")?.toInt() ?: 100,
            zoom = args.getString("zoom")?.toFloat() ?: 1f,
            croppedRaw = args.getString("croppedRaw") == "true",
            preferVulkan = args.getString("backend") != "cpu", dumpDng = args.getString("dumpDng") == "true"), directory)
        assertTrue(report.has("reconstruction"))
    }

    @Test fun writeCapabilityProbe() {
        val args = InstrumentationRegistry.getArguments()
        assumeTrue("Physical Camera2 probe opt-in required", args.getString("latentHighresProbe") == "true")
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        File(checkNotNull(context.getExternalFilesDir(null)), "highres-probe.json")
            .writeText(HighResolutionCapture(context).probe().toString(2))
    }
}
