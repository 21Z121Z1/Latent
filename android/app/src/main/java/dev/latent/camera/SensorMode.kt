package dev.latent.camera

import android.annotation.SuppressLint
import android.graphics.ImageFormat
import android.graphics.Rect
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.os.Build
import android.util.Size
import androidx.annotation.Keep
import org.json.JSONArray
import org.json.JSONObject

/** Observation transport only: interpretCamera2Sampling in latent_core is the sole topology authority. */
@Keep
class SensorModeInput(
    @JvmField val cfa: Int,
    @JvmField val groupWidth: Int,
    @JvmField val groupHeight: Int,
    @JvmField val groupingUsed: Int, // -1 absent, 0 false, 1 true
    @JvmField val ultraHighResolution: Boolean,
    @JvmField val remosaicReprocessing: Boolean,
    @JvmField val pixelModeAvailable: Boolean,
    @JvmField val requestedMode: Int,
    @JvmField val actualMode: Int, // -1 absent
    @JvmField val pixelWidth: Int,
    @JvmField val pixelHeight: Int,
    @JvmField val rawWidth: Int,
    @JvmField val rawHeight: Int,
    @JvmField val active: IntArray,
    @JvmField val preCorrection: IntArray,
    @JvmField val deliveredCrop: IntArray,
    @JvmField val croppedRaw: Boolean,
    @JvmField val rawCrop: IntArray,
    @JvmField val zoom: Float,
    @JvmField val coordinateSpace: String,
)

internal object SensorMode {
    fun rect(r: Rect) = intArrayOf(r.left, r.top, r.width(), r.height())

    @SuppressLint("NewApi") // max-mode access is guarded by the SDK precondition below.
    fun observe(id: String, c: CameraCharacteristics, r: CaptureResult, requestedMode: Int,
                raw: Size, delivered: Rect, croppedRaw: Boolean = false): SensorModeInput {
        val caps = c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf()
        val actual = if (Build.VERSION.SDK_INT >= 31) r.get(CaptureResult.SENSOR_PIXEL_MODE) else null
        val max = (actual ?: requestedMode) == CameraMetadata.SENSOR_PIXEL_MODE_MAXIMUM_RESOLUTION
        require(!max || Build.VERSION.SDK_INT >= 31) { "Maximum-resolution API unavailable" }
        val pixel = if (max) c.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE_MAXIMUM_RESOLUTION)
            else c.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE)
        val active = if (max) c.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE_MAXIMUM_RESOLUTION)
            else c.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
        val pre = if (max) c.get(CameraCharacteristics.SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE_MAXIMUM_RESOLUTION)
            else c.get(CameraCharacteristics.SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE)
        val group = if (Build.VERSION.SDK_INT >= 31) c.get(CameraCharacteristics.SENSOR_INFO_BINNING_FACTOR) else null
        val used = if (Build.VERSION.SDK_INT >= 31) r.get(CaptureResult.SENSOR_RAW_BINNING_FACTOR_USED) else null
        val crop = if (Build.VERSION.SDK_INT >= 34) r.get(CaptureResult.SCALER_RAW_CROP_REGION) else null
        val physical = r.get(CaptureResult.LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID)
        return SensorModeInput(
            checkNotNull(c.get(CameraCharacteristics.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT)),
            group?.width ?: 0, group?.height ?: 0, used?.let { if (it) 1 else 0 } ?: -1,
            caps.contains(CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_ULTRA_HIGH_RESOLUTION_SENSOR),
            caps.contains(CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_REMOSAIC_REPROCESSING),
            c.availableCaptureRequestKeys.any { it.name == "android.sensor.pixelMode" },
            requestedMode, actual ?: -1, checkNotNull(pixel).width, pixel.height, raw.width, raw.height,
            rect(checkNotNull(active)), rect(checkNotNull(pre)), rect(delivered), croppedRaw,
            crop?.let(::rect) ?: intArrayOf(),
            if (Build.VERSION.SDK_INT >= 30) r.get(CaptureResult.CONTROL_ZOOM_RATIO) ?: 1f else 1f,
            "$id/physical:${physical ?: "unspecified"}/mode:$requestedMode",
        )
    }

    fun probe(id: String, c: CameraCharacteristics): JSONObject {
        val modes = JSONArray()
        for (mode in 0..1) {
            val map = if (mode == 0) c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                else if (Build.VERSION.SDK_INT >= 31) c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP_MAXIMUM_RESOLUTION) else null
            val sizes = JSONArray()
            map?.getOutputSizes(ImageFormat.RAW_SENSOR)?.forEach { size ->
                sizes.put(JSONObject().put("width", size.width).put("height", size.height)
                    .put("minimumFrameDurationNs", map.getOutputMinFrameDuration(ImageFormat.RAW_SENSOR, size))
                    .put("stallDurationNs", map.getOutputStallDuration(ImageFormat.RAW_SENSOR, size)))
            }
            modes.put(JSONObject().put("pixelMode", mode).put("raw16", sizes))
        }
        return JSONObject().put("cameraId", id).put("physicalIds", JSONArray(c.physicalCameraIds.toList()))
            .put("modes", modes).put("characteristics", characteristics(c))
            .put("requestKeys", JSONArray(c.availableCaptureRequestKeys.map { it.name }))
            .put("resultKeys", JSONArray(c.availableCaptureResultKeys.map { it.name }))
    }

    // Capture all present public/vendor values for diagnosis. Semantic interpretation
    // never parses this human/debug projection. Result omissions stay explicit null.
    private fun value(v: Any?): Any = when (v) {
        null -> JSONObject.NULL
        is Number, is Boolean, is String -> v
        is Rect -> JSONArray(rect(v).toList())
        is Size -> JSONArray(listOf(v.width, v.height))
        is FloatArray -> JSONArray(v.toList())
        is IntArray -> JSONArray(v.toList())
        is LongArray -> JSONArray(v.toList())
        is DoubleArray -> JSONArray(v.toList())
        is BooleanArray -> JSONArray(v.toList())
        is Array<*> -> JSONArray(v.map(::value))
        is Iterable<*> -> JSONArray(v.map(::value))
        else -> v.toString()
    }
    fun characteristics(c: CameraCharacteristics): JSONObject = JSONObject().also { out ->
        c.keys.forEach { key -> out.put(key.name, value(c.get(key))) }
    }
    fun result(r: CaptureResult): JSONObject = JSONObject().also { out ->
        r.keys.forEach { key -> out.put(key.name, value(r.get(key))) }
    }
    fun request(r: CaptureRequest): JSONObject = JSONObject().also { out ->
        r.keys.forEach { key -> out.put(key.name, value(r.get(key))) }
    }
}
