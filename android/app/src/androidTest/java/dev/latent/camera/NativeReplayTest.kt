package dev.latent.camera

import android.graphics.Bitmap
import androidx.core.graphics.createBitmap
import android.graphics.BitmapFactory
import android.provider.MediaStore
import androidx.test.platform.app.InstrumentationRegistry
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.io.IOException
import java.util.concurrent.CancellationException

class NativeReplayTest {
    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext
    private fun frames() = context.assets.open("static_burst.b64").bufferedReader().use { ReplayFixture.decode(it.readText()) }
    private fun image() = createBitmap(33, 25)
    private fun process(vulkan: Boolean, output: Bitmap): String = NativeBridge.processRaw(frames(), output, vulkan, 64L*1024*1024, 0f) { _,_,_ -> true }

    @Test fun nativeLoadReferenceReplayAndProductionLowering() {
        val reference = image()
        val trace = JSONObject(process(false, reference))
        assertEquals("synthetic-fixture", trace.getString("source"))
        assertEquals(4, trace.getInt("frameCount"))
        assertTrue(trace.getDouble("effectiveN") > 1.0)
        assertEquals(4, trace.getJSONArray("frames").length())
        for (index in 0 until 4) {
            val frame = trace.getJSONArray("frames").getJSONObject(index)
            val geometry = frame.getJSONObject("geometry")
            assertEquals(2, geometry.getInt("schemaVersion"))
            assertTrue(geometry.getInt("status") in 0..4)
            assertTrue(geometry.getInt("prior") in 0..2)
            assertTrue(geometry.getLong("supportedGuideSamples") >= 0)
            for (name in arrayOf("localizationStdDevPx", "cycleErrorPx")) {
                assertTrue(geometry.has(name))
                assertTrue(geometry.isNull(name) || geometry.getDouble(name).isFinite())
            }
            assertEquals(2, geometry.getJSONArray("tiles").length())
            if (frame.getLong("id") == trace.getLong("referenceId")) {
                assertEquals(0, geometry.getInt("status"))
                assertEquals(0, geometry.getInt("prior"))
                assertEquals(0.0, geometry.getDouble("localizationStdDevPx"), 0.0)
                assertEquals(0.0, geometry.getDouble("dx"), 0.0)
                assertEquals(0.0, geometry.getDouble("dy"), 0.0)
            }
        }
        val replay = image()
        process(false, replay)
        assertTrue(reference.sameAs(replay))
        val optimized = image()
        val production = JSONObject(process(true, optimized))
        assertTrue(production.getString("backend").isNotBlank())
        var minimum = 255
        var maximum = 0
        for (y in 0 until 25) for (x in 0 until 33) {
            val a = reference.getPixel(x,y)
            val b = optimized.getPixel(x,y)
            for (shift in intArrayOf(0,8,16)) {
                val av = (a ushr shift) and 255
                val bv = (b ushr shift) and 255
                assertTrue("Display quantization differential", kotlin.math.abs(av-bv) <= 2)
                minimum = minOf(minimum, av); maximum = maxOf(maximum, av)
            }
        }
        assertTrue(maximum-minimum > 10)
        reference.recycle(); replay.recycle(); optimized.recycle()
    }

    @Test fun unobservableGeometryUsesNullUncertaintyAndExplicitPrior() {
        val inputs = frames()
        for (frame in inputs) {
            val level = (frame.black[0] + 0.2f * (frame.white - frame.black[0])).toInt().toShort()
            for (y in 0 until frame.height) for (x in 0 until frame.width)
                frame.pixels.putShort(y * frame.rowStrideBytes + x * 2, level)
        }
        val output = image()
        try {
            val trace = JSONObject(NativeBridge.processRaw(inputs, output, false, 64L*1024*1024, 0f) { _,_,_ -> true })
            val frames = trace.getJSONArray("frames")
            for (index in 0 until frames.length()) {
                val frame = frames.getJSONObject(index)
                if (frame.getLong("id") == trace.getLong("referenceId")) continue
                val geometry = frame.getJSONObject("geometry")
                assertEquals(2, geometry.getInt("status"))
                assertEquals(1, geometry.getInt("prior"))
                assertEquals(0.0, geometry.getDouble("dx"), 0.0)
                assertEquals(0.0, geometry.getDouble("dy"), 0.0)
                assertTrue(geometry.isNull("localizationStdDevPx"))
                assertTrue(geometry.isNull("cycleErrorPx"))
            }
        } finally { output.recycle() }
    }

    @Test fun cancellationAndInvalidBitmapAreExplicit() {
        val output = image()
        assertThrows(CancellationException::class.java) {
            NativeBridge.processRaw(frames(), output, false, 64L*1024*1024, 0f) { _,_,_ -> false }
        }
        val wrong = createBitmap(4,4)
        assertThrows(IllegalArgumentException::class.java) { process(false, wrong) }
        assertTrue(JSONObject(process(false, output)).getDouble("effectiveN") > 1)
        output.recycle(); wrong.recycle()
    }

    @Test fun mediaStorePublishesJpegAndRollsBackFailure() {
        val output = image()
        val trace = process(false, output)
        val resolver = context.contentResolver
        fun pendingCount(): Int = resolver.query(MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
            arrayOf(MediaStore.Images.Media._ID), "${MediaStore.Images.Media.IS_PENDING}=1", null, null)?.use { it.count } ?: 0
        val before = pendingCount()
        assertThrows(IOException::class.java) {
            MediaStoreWriter.save(resolver, output, trace, System.currentTimeMillis()) { _,_ -> throw IOException("injected write failure") }
        }
        assertEquals(before, pendingCount())
        val uri = MediaStoreWriter.save(resolver, output, trace, System.currentTimeMillis())
        try {
            resolver.query(uri, arrayOf(MediaStore.Images.Media.IS_PENDING), null, null, null)!!.use {
                assertTrue(it.moveToFirst()); assertEquals(0, it.getInt(0))
            }
            resolver.openInputStream(uri)!!.use { stream ->
                val decoded = BitmapFactory.decodeStream(stream)
                assertNotNull(decoded); assertEquals(33, decoded.width); assertEquals(25, decoded.height)
                decoded.recycle()
            }
        } finally { resolver.delete(uri,null,null); output.recycle() }
    }
}
