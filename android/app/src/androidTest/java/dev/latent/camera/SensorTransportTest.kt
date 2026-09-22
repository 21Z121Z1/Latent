package dev.latent.camera

import android.graphics.Bitmap
import androidx.core.graphics.createBitmap
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.roundToInt

class SensorTransportTest {
    // An independent reading-order fixture table, not a production CFA mapper.
    private val patterns = arrayOf(intArrayOf(0,1,2,3), intArrayOf(1,0,3,2), intArrayOf(1,3,0,2), intArrayOf(3,1,2,0))
    private fun frame(pattern: Int, ox: Int, oy: Int, direct: Boolean = true): RawInput {
        val width = 13; val height = 11; val stride = 32
        val black = floatArrayOf(68f,86f,102f,120f)
        val shading = floatArrayOf(1.1f,1.2f,1.4f,1.8f)
        val color = floatArrayOf(0.2f,0.3f,0.3f,0.4f)
        val bytes = (if (direct) ByteBuffer.allocateDirect(stride*height) else ByteBuffer.allocate(stride*height)).order(ByteOrder.LITTLE_ENDIAN)
        for (y in 0 until height) for (x in 0 until width) {
            val layout = ((y+oy) and 1)*2 + ((x+ox) and 1)
            val channel = patterns[pattern][layout]
            val value = (black[layout] + color[channel]/shading[channel]*(4000-black[layout])).roundToInt()
            bytes.putShort(y*stride+x*2, value.toShort())
        }
        return RawInput(1,1_000_000_000,20_000_000,100f,width,height,stride,pattern,
            phaseX = ox, phaseY = oy, cameraId = "synthetic-sensor", sensorMode = "odd-strided-crop", pixels = bytes,
            black = floatArrayOf(64f,80f,96f,112f), dynamicBlack = black, white = 4095f, dynamicWhite = 4000f,
            noise = floatArrayOf(0f,0.0001f,0f,0.0001f,0f,0.0001f,0f,0.0001f),
            shadingColumns = 2, shadingRows = 2, shading = FloatArray(16) { shading[it%4] },
            whiteBalance = floatArrayOf(1f,1f,1f,1f), sensorToLinearSrgb = floatArrayOf(1f,0f,0f,0f,1f,0f,0f,0f,1f), synthetic = true)
    }
    private fun process(input: RawInput): Bitmap {
        val image = createBitmap(13,11)
        NativeBridge.processRaw(arrayOf(input), image, false, 64L*1024*1024, 0f) { _,_,_ -> true }
        return image
    }

    @Test fun allCfaPhasesReorderBlackNoiseAndShadingWithoutChangingColor() {
        val expected = process(frame(0,0,0))
        try {
            for (pattern in 0..3) for (oy in 0..1) for (ox in 0..1) {
                val actual = process(frame(pattern,ox,oy))
                try {
                    // Malvar's 5x5 filter clamps outside-frame taps; compare the full-kernel interior.
                    for (y in 2 until 9) for (x in 2 until 11) for (shift in intArrayOf(0,8,16)) {
                        val a = (actual.getPixel(x,y) ushr shift) and 255
                        val b = (expected.getPixel(x,y) ushr shift) and 255
                        assertTrue("CFA=$pattern crop=$ox,$oy pixel=$x,$y channel=$shift", kotlin.math.abs(a-b) <= 2)
                    }
                } finally { actual.recycle() }
            }
        } finally { expected.recycle() }
    }

    @Test fun invalidNativeOwnershipAndResourceAdmissionFailExplicitly() {
        val image = createBitmap(13,11)
        try {
            assertThrows(IllegalArgumentException::class.java) {
                NativeBridge.processRaw(arrayOf(frame(0,0,0,false)), image, false, 64L*1024*1024, 0f) { _,_,_ -> true }
            }
            assertThrows(IllegalArgumentException::class.java) {
                NativeBridge.processRaw(arrayOf(frame(0,0,0)), image, false, 1, 0f) { _,_,_ -> true }
            }
            assertTrue(NativeBridge.processingBound(13,11,4,false) > 13*11*4)
            assertThrows(IllegalArgumentException::class.java) { NativeBridge.processingBound(0,11,4,false) }
        } finally { image.recycle() }
    }

    @Test fun capturePlanningUsesTheSameNativePolicyAndConstantExposure() {
        val observations = CaptureObservationInput(20_000_000,33_333_333,400,true,0.0001f)
        val capabilities = CaptureCapabilityInput(true,true,true,100_000,200_000_000,33_333_333,1_000_000_000,
            50,3200,128_000_000,8_000_000,8)
        val plan = JSONObject(NativeBridge.capturePlan(observations,CaptureIntentInput(),capabilities))
        assertTrue(plan.getBoolean("manual"))
        val frames = plan.getJSONArray("frames")
        assertTrue(frames.length() in 2..8)
        for (index in 0 until frames.length()) {
            assertEquals(frames.getJSONObject(0).getLong("exposureNs"), frames.getJSONObject(index).getLong("exposureNs"))
            assertEquals(400,frames.getJSONObject(index).getInt("iso"))
        }
    }

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

}
