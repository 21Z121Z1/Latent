package dev.latent.camera

import android.graphics.Bitmap
import androidx.annotation.Keep
import java.nio.ByteBuffer

/** Transport projection only. Native RawBurst validation owns imaging semantics. */
@Keep
class RawInput(
    @JvmField val id: Long,
    @JvmField val timestampNs: Long,
    @JvmField val exposureNs: Long,
    @JvmField val iso: Float,
    @JvmField val width: Int,
    @JvmField val height: Int,
    @JvmField val rowStrideBytes: Int,
    @JvmField val cfa: Int,
    @JvmField val cameraId: String,
    @JvmField val sensorMode: String,
    @JvmField val pixels: ByteBuffer,
    @JvmField val black: FloatArray,
    @JvmField val dynamicBlack: FloatArray = floatArrayOf(),
    @JvmField val white: Float,
    @JvmField val dynamicWhite: Float = 0f,
    @JvmField val noise: FloatArray = floatArrayOf(),
    @JvmField val shadingColumns: Int = 0,
    @JvmField val shadingRows: Int = 0,
    @JvmField val shading: FloatArray = floatArrayOf(),
    @JvmField val whiteBalance: FloatArray,
    @JvmField val sensorToLinearSrgb: FloatArray,
    @JvmField val synthetic: Boolean = false,
)

@Keep
fun interface NativeProgress {
    /** Called on the processing thread. Return false to cancel at a stage boundary. */
    fun onProgress(stage: Int, completed: Int, total: Int): Boolean
}

@Keep
class CaptureObservationInput(
    @JvmField val exposureNs: Long,
    @JvmField val frameDurationNs: Long,
    @JvmField val iso: Int,
    @JvmField val aeConverged: Boolean,
    @JvmField val noiseVariance: Float = -1f,
    @JvmField val angularSpeed: Float = -1f,
    @JvmField val gyroTimestampComparable: Boolean = false,
)

@Keep
class CaptureIntentInput(
    @JvmField val targetStandardDeviation: Float = 0.006f,
    @JvmField val latencyBudgetNs: Long = 1_500_000_000L,
    @JvmField val integrationBudgetNs: Long = 1_000_000_000L,
    @JvmField val maximumAngularTravel: Float = 0.0015f,
    @JvmField val highlightExposureEv: Float = -0.5f,
    @JvmField val maximumFrames: Int = 8,
)

@Keep
class CaptureCapabilityInput(
    @JvmField val raw: Boolean,
    @JvmField val manualSensor: Boolean,
    @JvmField val aeLock: Boolean,
    @JvmField val minimumExposureNs: Long,
    @JvmField val maximumExposureNs: Long,
    @JvmField val minimumRawFrameDurationNs: Long,
    @JvmField val maximumFrameDurationNs: Long,
    @JvmField val minimumIso: Int,
    @JvmField val maximumIso: Int,
    @JvmField val retainedRawBudgetBytes: Long,
    @JvmField val bytesPerRawFrame: Long,
    @JvmField val maximumRetainedFrames: Int,
)

/** This is a synchronous borrowing boundary. Keep every Image alive until return. */
@Keep
object NativeBridge {
    init { System.loadLibrary("latent_android") }

    external fun processRaw(
        frames: Array<RawInput>,
        output: Bitmap,
        preferVulkan: Boolean,
        memoryBudgetBytes: Long,
        renderExposureEv: Float,
        progress: NativeProgress,
    ): String

    external fun capturePlan(
        observation: CaptureObservationInput,
        intent: CaptureIntentInput,
        capability: CaptureCapabilityInput,
    ): String
}
