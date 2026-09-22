package dev.latent.camera

import android.graphics.ImageFormat
import android.graphics.Rect
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.media.Image
import android.util.Size
import java.nio.ByteOrder

/** Describes the default sensor coordinate system. Unknown resampling is rejected. */
internal class CameraRawAdapter(private val id: String, private val characteristics: CameraCharacteristics) {
    private val pixelArray = checkNotNull(characteristics.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE))
    private val active = Rect(checkNotNull(characteristics.get(CameraCharacteristics.SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE)))
    private val cfa = checkNotNull(characteristics.get(CameraCharacteristics.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT))
    private val white = checkNotNull(characteristics.get(CameraCharacteristics.SENSOR_INFO_WHITE_LEVEL)).toFloat()
    private val black = checkNotNull(characteristics.get(CameraCharacteristics.SENSOR_BLACK_LEVEL_PATTERN)).let { pattern ->
        FloatArray(4) { pattern.getOffsetForIndex(it % 2, it / 2).toFloat() }
    }

    init {
        require(cfa in 0..3 && active.left >= 0 && active.top >= 0 && active.width() >= 2 && active.height() >= 2)
        require(active.right <= pixelArray.width && active.bottom <= pixelArray.height)
    }

    fun canMap(size: Size): Boolean = size == pixelArray || (size.width == active.width() && size.height == active.height())
    val outputSize: Size get() = Size(active.width(), active.height())

    fun input(image: Image, result: TotalCaptureResult, memberId: Long): RawInput {
        require(image.format == ImageFormat.RAW_SENSOR && image.planes.size == 1) { "Expected one RAW_SENSOR plane" }
        val rawSize = Size(image.width, image.height)
        require(canMap(rawSize)) { "RAW sensor mode has no verified calibration-coordinate mapping" }
        require(image.cropRect == Rect(0, 0, image.width, image.height)) { "Unexpected RAW crop rectangle" }
        val timestamp = checkNotNull(result.get(CaptureResult.SENSOR_TIMESTAMP)) { "Missing sensor timestamp" }
        require(timestamp == image.timestamp) { "Image and metadata sensor timestamps differ" }
        val crop = if (rawSize == pixelArray) active else Rect(0, 0, active.width(), active.height())
        val plane = image.planes.single()
        require(plane.pixelStride == 2 && plane.rowStride % 2 == 0) { "Unsupported RAW16 stride" }
        val bytes = plane.buffer.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        val offset = crop.top.toLong() * plane.rowStride + crop.left.toLong() * 2
        val end = offset + (crop.height() - 1L) * plane.rowStride + crop.width() * 2L
        require(offset >= 0 && end <= bytes.limit() && end <= Int.MAX_VALUE) { "RAW plane is truncated" }
        bytes.position(offset.toInt()); bytes.limit(end.toInt())
        val pixels = bytes.slice().order(ByteOrder.LITTLE_ENDIAN)
        val gain = checkNotNull(result.get(CaptureResult.COLOR_CORRECTION_GAINS)) { "Missing sensor white balance" }
        val matrix = checkNotNull(result.get(CaptureResult.COLOR_CORRECTION_TRANSFORM)) { "Missing sensor-to-linear-sRGB transform" }
        val noise = result.get(CaptureResult.SENSOR_NOISE_PROFILE)?.let { pairs ->
            require(pairs.size == 4) { "Expected four CFA noise channels" }
            FloatArray(8) { if (it % 2 == 0) pairs[it / 2].first.toFloat() else pairs[it / 2].second.toFloat() }
        } ?: floatArrayOf()
        val shading = result.get(CaptureResult.STATISTICS_LENS_SHADING_CORRECTION_MAP)
        val columns = shading?.columnCount ?: 0
        val rows = shading?.rowCount ?: 0
        require(columns.toLong() * rows * 4 <= 65536) { "Lens shading map exceeds admission bound" }
        val gains = if (shading == null) floatArrayOf() else FloatArray(columns * rows * 4) {
            shading.getGainFactor(it % 4, (it / 4) % columns, it / (4 * columns))
        }
        val physical = result.get(CaptureResult.LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID)
        return RawInput(
            id = memberId, timestampNs = timestamp,
            exposureNs = checkNotNull(result.get(CaptureResult.SENSOR_EXPOSURE_TIME)) { "Missing sensor exposure" },
            iso = checkNotNull(result.get(CaptureResult.SENSOR_SENSITIVITY)) { "Missing sensor ISO observation" }.toFloat(),
            width = crop.width(), height = crop.height(), rowStrideBytes = plane.rowStride, cfa = cfa,
            phaseX = active.left and 1, phaseY = active.top and 1,
            cameraId = if (physical == null) id else "$id/physical:$physical",
            sensorMode = "default:${image.width}x${image.height}/active:${active.flattenToString()}", pixels = pixels,
            black = black.copyOf(), dynamicBlack = result.get(CaptureResult.SENSOR_DYNAMIC_BLACK_LEVEL)?.copyOf() ?: floatArrayOf(),
            white = white, dynamicWhite = result.get(CaptureResult.SENSOR_DYNAMIC_WHITE_LEVEL)?.toFloat() ?: 0f,
            noise = noise, shadingColumns = columns, shadingRows = rows, shading = gains,
            whiteBalance = floatArrayOf(gain.red, gain.greenEven, gain.greenOdd, gain.blue),
            sensorToLinearSrgb = FloatArray(9) { matrix.getElement(it % 3, it / 3).toFloat() },
        )
    }
}
