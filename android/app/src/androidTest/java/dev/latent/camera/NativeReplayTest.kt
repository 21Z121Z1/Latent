package dev.latent.camera

import android.graphics.Bitmap
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
    private fun image() = Bitmap.createBitmap(33, 25, Bitmap.Config.ARGB_8888)
    private fun process(vulkan: Boolean, output: Bitmap): String = NativeBridge.processRaw(frames(), output, vulkan, 64L*1024*1024, 0f) { _,_,_ -> true }

    @Test fun nativeLoadReferenceReplayAndProductionLowering() {
        val reference = image()
        val trace = JSONObject(process(false, reference))
        assertEquals("synthetic-fixture", trace.getString("source"))
        assertEquals(4, trace.getInt("frameCount"))
        assertTrue(trace.getDouble("effectiveN") > 1.0)
        assertEquals(4, trace.getJSONArray("frames").length())
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

    @Test fun cancellationAndInvalidBitmapAreExplicit() {
        val output = image()
        assertThrows(CancellationException::class.java) {
            NativeBridge.processRaw(frames(), output, false, 64L*1024*1024, 0f) { _,_,_ -> false }
        }
        val wrong = Bitmap.createBitmap(4,4,Bitmap.Config.ARGB_8888)
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
