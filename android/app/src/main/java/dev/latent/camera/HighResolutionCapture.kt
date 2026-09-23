package dev.latent.camera

import android.Manifest
import android.annotation.SuppressLint
import android.app.ActivityManager
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.graphics.Rect
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureFailure
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.hardware.camera2.DngCreator
import android.media.Image
import android.media.ImageReader
import android.os.Build
import android.os.Debug
import android.os.Handler
import android.os.HandlerThread
import android.os.PowerManager
import android.os.Looper
import android.os.SystemClock
import android.util.Size
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.util.concurrent.CompletableFuture
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import org.json.JSONArray
import org.json.JSONObject

/** Opt-in developer backend. Raw-only session, one outstanding request, immediate
 * disk spooling: no burst of full-resolution Image leases or FP32 frames in RAM.
 * Invoke off the main/camera callback thread. The normal camera must be closed.
 */
internal class HighResolutionCapture(private val context: Context) {
    data class Options(
        val cameraId: String, val pixelMode: Int = 0, val size: Size,
        val frames: Int = 3, val exposureNs: Long = 10_000_000, val iso: Int = 100,
        val zoom: Float = 1f, val croppedRaw: Boolean = false,
        val preferVulkan: Boolean = true, val reconstructionBudgetBytes: Long = 128L * 1024 * 1024,
        val dumpDng: Boolean = false,
    )

    fun probe(): JSONArray {
        val manager = context.getSystemService(CameraManager::class.java)
        return JSONArray(manager.cameraIdList.map { SensorMode.probe(it, manager.getCameraCharacteristics(it)) })
    }

    @SuppressLint("NewApi", "MissingPermission") // pixel-mode and use-case calls have explicit API guards.
    fun capture(o: Options, directory: File): JSONObject {
        check(Looper.myLooper() != Looper.getMainLooper()) { "Capture must not block the main thread" }
        check(BuildConfig.DEBUG) { "High-resolution evidence capture is a developer-only feature" }
        check(context.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED)
        require(o.pixelMode in 0..1 && o.frames in 1..8 && o.zoom.isFinite() && o.zoom >= 1f)
        require(o.size.width > 0 && o.size.height > 0 && o.size.width <= 65535 && o.size.height <= 65535)
        require(o.reconstructionBudgetBytes >= 8L * 1024 * 1024 && o.reconstructionBudgetBytes <= 512L * 1024 * 1024)
        require(o.pixelMode == 0 || Build.VERSION.SDK_INT >= 31)
        require(!o.croppedRaw || Build.VERSION.SDK_INT >= 34)
        check(!directory.exists() && directory.mkdirs()) { "Use a new capture bundle directory" }
        val manager = context.getSystemService(CameraManager::class.java)
        val c = manager.getCameraCharacteristics(o.cameraId)
        val manifest = JSONObject().put("schema", 1).put("algorithm", "latent.direct-cfa.1")
            .put("deviceVerified", false).put("buildFingerprint", Build.FINGERPRINT)
            .put("probe", SensorMode.probe(o.cameraId, c)).put("frames", JSONArray())
        val manifestFile = File(directory, "capture.json")
        fun save() { val temp = File(directory, "capture.json.partial"); temp.writeText(manifest.toString(2)); check(temp.renameTo(manifestFile)) }
        save()
        val thread = HandlerThread("latent-highres-capture").apply { start() }
        val handler = Handler(thread.looper)
        val executor = Executor { command -> check(handler.post(command)) }
        var camera: CameraDevice? = null
        var session: CameraCaptureSession? = null
        var reader: ImageReader? = null
        val outstandingImage = AtomicReference<CompletableFuture<Image>?>(null)
        val ownedImage = AtomicReference<Image?>(null)
        val start = SystemClock.elapsedRealtimeNanos()
        try {
            val caps = c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: intArrayOf()
            require(caps.contains(CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_RAW))
            require(caps.contains(CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)) { "Developer burst requires explicit manual exposure" }
            val map = checkNotNull(if (o.pixelMode == 1) c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP_MAXIMUM_RESOLUTION)
                else c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP))
            require(map.getOutputSizes(ImageFormat.RAW_SENSOR)?.contains(o.size) == true) { "RAW size absent from selected pixel-mode map" }
            if (o.pixelMode == 1) require(c.availableCaptureRequestKeys.contains(CaptureRequest.SENSOR_PIXEL_MODE))
            if (o.croppedRaw) require(c.get(CameraCharacteristics.SCALER_AVAILABLE_STREAM_USE_CASES)?.contains(CameraMetadata.SCALER_AVAILABLE_STREAM_USE_CASES_CROPPED_RAW.toLong()) == true)
            require(checkNotNull(c.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)).contains(o.exposureNs))
            require(checkNotNull(c.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)).contains(o.iso))
            val memory = ActivityManager.MemoryInfo()
            context.getSystemService(ActivityManager::class.java).getMemoryInfo(memory)
            val rawBytes = o.size.width.toLong() * o.size.height * 2
            // Camera/HAL private pools are not observable from maxImages. This is
            // conservative admission, followed by measured PSS; not a zero-copy claim.
            require(!memory.lowMemory && rawBytes * 4 + o.reconstructionBudgetBytes < memory.availMem / 2) { "Insufficient capture + reconstruction headroom" }
            val diskBytes = rawBytes * o.frames * 2 + o.size.width.toLong() * o.size.height * 64 + if (o.dumpDng) rawBytes * o.frames else 0
            require(directory.usableSpace > diskBytes + 64L * 1024 * 1024) { "Insufficient debug bundle disk space" }
            manifest.put("admission", JSONObject().put("availableRamBytes", memory.availMem)
                .put("rawPlaneBytes", rawBytes).put("imageReaderMaxImages", 2).put("diskEstimateBytes", diskBytes)
                .put("reconstructionBudgetBytes", o.reconstructionBudgetBytes))
            val rawReader = ImageReader.newInstance(o.size.width, o.size.height, ImageFormat.RAW_SENSOR, 2)
            reader = rawReader
            rawReader.setOnImageAvailableListener({ input ->
                val image = input.acquireNextImage() ?: return@setOnImageAvailableListener
                val waiting = outstandingImage.getAndSet(null)
                if (waiting == null || !ownedImage.compareAndSet(null, image)) image.close()
                else if (!waiting.complete(image)) ownedImage.getAndSet(null)?.close()
            }, handler)
            val opened = CompletableFuture<CameraDevice>()
            manager.openCamera(o.cameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(value: CameraDevice) { if (!opened.complete(value)) value.close() }
                override fun onDisconnected(value: CameraDevice) { value.close(); opened.completeExceptionally(IllegalStateException("camera disconnected")) }
                override fun onError(value: CameraDevice, error: Int) { value.close(); opened.completeExceptionally(IllegalStateException("camera error $error")) }
            }, handler)
            val device = try { opened.get(15, TimeUnit.SECONDS) } catch (e: Exception) { opened.cancel(false); throw e }
            camera = device
            val output = OutputConfiguration(rawReader.surface)
            if (Build.VERSION.SDK_INT >= 31) output.addSensorPixelModeUsed(o.pixelMode)
            if (o.croppedRaw) output.streamUseCase = CameraMetadata.SCALER_AVAILABLE_STREAM_USE_CASES_CROPPED_RAW.toLong()
            val configured = CompletableFuture<CameraCaptureSession>()
            device.createCaptureSession(SessionConfiguration(SessionConfiguration.SESSION_REGULAR, listOf(output), executor,
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(value: CameraCaptureSession) { if (!configured.complete(value)) value.close() }
                    override fun onConfigureFailed(value: CameraCaptureSession) { value.close(); configured.completeExceptionally(IllegalStateException("RAW-only session rejected")) }
                }))
            val activeSession = try { configured.get(20, TimeUnit.SECONDS) } catch (e: Exception) { configured.cancel(false); throw e }
            session = activeSession
            val raws = ArrayList<RawInput>()
            val modes = ArrayList<SensorModeInput>()
            val paths = ArrayList<String>()
            for (index in 0 until o.frames) {
                val request = device.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE).apply {
                    addTarget(rawReader.surface)
                    set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_OFF)
                    set(CaptureRequest.SENSOR_EXPOSURE_TIME, o.exposureNs)
                    set(CaptureRequest.SENSOR_SENSITIVITY, o.iso)
                    set(CaptureRequest.SENSOR_FRAME_DURATION, maxOf(o.exposureNs, map.getOutputMinFrameDuration(ImageFormat.RAW_SENSOR, o.size)))
                    if (Build.VERSION.SDK_INT >= 31 && c.availableCaptureRequestKeys.contains(CaptureRequest.SENSOR_PIXEL_MODE)) set(CaptureRequest.SENSOR_PIXEL_MODE, o.pixelMode)
                    if (Build.VERSION.SDK_INT >= 30 && c.availableCaptureRequestKeys.contains(CaptureRequest.CONTROL_ZOOM_RATIO)) {
                        require(checkNotNull(c.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)).contains(o.zoom))
                        set(CaptureRequest.CONTROL_ZOOM_RATIO, o.zoom)
                    } else require(o.zoom == 1f) { "No public zoom ratio control" }
                    if (c.availableCaptureRequestKeys.contains(CaptureRequest.STATISTICS_LENS_SHADING_MAP_MODE)) set(CaptureRequest.STATISTICS_LENS_SHADING_MAP_MODE, CameraMetadata.STATISTICS_LENS_SHADING_MAP_MODE_ON)
                    if (c.get(CameraCharacteristics.HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES)?.contains(CameraMetadata.HOT_PIXEL_MODE_OFF) == true) set(CaptureRequest.HOT_PIXEL_MODE, CameraMetadata.HOT_PIXEL_MODE_OFF)
                }.build()
                val imageFuture = CompletableFuture<Image>()
                check(outstandingImage.compareAndSet(null, imageFuture))
                val resultFuture = CompletableFuture<TotalCaptureResult>()
                val captureStart = SystemClock.elapsedRealtimeNanos()
                activeSession.capture(request, object : CameraCaptureSession.CaptureCallback() {
                    override fun onCaptureCompleted(s: CameraCaptureSession, r: CaptureRequest, result: TotalCaptureResult) { resultFuture.complete(result) }
                    override fun onCaptureFailed(s: CameraCaptureSession, r: CaptureRequest, failure: CaptureFailure) { resultFuture.completeExceptionally(IllegalStateException("capture failed: ${failure.reason}")) }
                }, handler)
                val timeout = 30 + TimeUnit.NANOSECONDS.toSeconds(o.exposureNs + map.getOutputStallDuration(ImageFormat.RAW_SENSOR, o.size))
                val result = resultFuture.get(timeout, TimeUnit.SECONDS)
                val image = imageFuture.get(timeout, TimeUnit.SECONDS)
                try {
                    require(image.timestamp == result.get(CaptureResult.SENSOR_TIMESTAMP)) { "RAW/result timestamp mismatch" }
                    require(image.cropRect == Rect(0, 0, image.width, image.height)) { "Unexpected Image crop requires a profile" }
                    val frame = JSONObject().put("id", index + 1).put("request", SensorMode.request(request)).put("result", SensorMode.result(result))
                        .put("captureWallNs", SystemClock.elapsedRealtimeNanos() - captureStart).put("rawExtent", JSONArray(listOf(image.width, image.height)))
                        .put("rowStrideBytes", image.planes.single().rowStride).put("imageCrop", JSONArray(SensorMode.rect(image.cropRect).toList()))
                    manifest.getJSONArray("frames").put(frame)
                    // Original observations survive an interpretation failure.
                    save()
                    // Preserve uninterpreted sensor evidence even when topology/calibration is rejected.
                    require(image.format == ImageFormat.RAW_SENSOR && image.planes.size == 1)
                    val originalPlane = image.planes.single()
                    require(originalPlane.pixelStride == 2 && originalPlane.rowStride >= image.width * 2)
                    val originalFile = File(directory, "sensor-${index + 1}.raw16")
                    FileOutputStream(originalFile).channel.use { channel ->
                        val bytes = originalPlane.buffer.duplicate()
                        val available = bytes.limit()
                        for (y in 0 until image.height) {
                            val offset = y.toLong() * originalPlane.rowStride
                            val end = offset + image.width.toLong() * 2
                            require(end <= available && end <= Int.MAX_VALUE)
                            bytes.limit(available); bytes.position(offset.toInt()); bytes.limit(end.toInt())
                            while (bytes.hasRemaining()) check(channel.write(bytes) > 0)
                        }
                        channel.force(false)
                    }
                    frame.put("originalRawFile", originalFile.name).put("originalPackedRowStrideBytes", image.width * 2)
                    save()
                    val pixel = checkNotNull(if (o.pixelMode == 1) c.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE_MAXIMUM_RESOLUTION) else c.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE))
                    val pre = checkNotNull(if (o.pixelMode == 1) c.get(CameraCharacteristics.SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE_MAXIMUM_RESOLUTION) else c.get(CameraCharacteristics.SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE))
                    val crop = if (!o.croppedRaw && o.size == pixel) pre else Rect(0, 0, image.width, image.height)
                    val mode = SensorMode.observe(o.cameraId, c, result, o.pixelMode, o.size, crop, o.croppedRaw)
                    val semantics = JSONObject(NativeBridge.interpretSensorMode(mode))
                    frame.put("sampling", semantics)
                    val raw = projectRawInput(o.cameraId, c, image, result, index + 1L, crop, 0, 0, "direct-mode:${o.pixelMode}")
                    val file = File(directory, "frame-${index + 1}.raw16")
                    FileOutputStream(file).channel.use { channel ->
                        val pixels = raw.pixels.duplicate()
                        for (y in 0 until raw.height) {
                            pixels.limit(pixels.capacity()); pixels.position(y * raw.rowStrideBytes); pixels.limit(y * raw.rowStrideBytes + raw.width * 2)
                            while (pixels.hasRemaining()) check(channel.write(pixels) > 0)
                        }
                        channel.force(false)
                    }
                    frame.put("rawFile", file.name).put("packedRowStrideBytes", raw.width * 2)
                    if (o.dumpDng && semantics.getInt("representation") == 1) {
                        DngCreator(c, result).use { dng -> FileOutputStream(File(directory, "frame-${index + 1}.dng")).use { dng.writeImage(it, image) } }
                    }
                    raws.add(raw.copy(pixels = ByteBuffer.allocateDirect(0), rowStrideBytes = raw.width * 2))
                    paths.add(file.absolutePath); modes.add(mode)
                } finally { if (ownedImage.compareAndSet(image, null)) image.close() }
                save()
            }
            // Release HAL/ImageReader resources BEFORE native reconstruction.
            activeSession.close(); session = null; device.close(); camera = null; rawReader.close(); reader = null
            val power = context.getSystemService(PowerManager::class.java)
            val thermal = power.currentThermalStatus
            val severity = if (thermal >= PowerManager.THERMAL_STATUS_SEVERE) 2 else if (thermal >= PowerManager.THERMAL_STATUS_MODERATE) 1 else 0
            manifest.put("thermalStatus", thermal).put("powerSaveMode", power.isPowerSaveMode)
            val report = NativeBridge.reconstructRawFiles(raws.toTypedArray(), modes.toTypedArray(), paths.toTypedArray(),
                File(directory, "reconstruction.lrgb").absolutePath, o.preferVulkan, o.reconstructionBudgetBytes, severity)
            manifest.put("reconstruction", JSONObject(report)).put("status", "software-pipeline-completed-device-quality-review-required")
            return manifest
        } catch (e: Exception) { manifest.put("status", "failed-closed").put("error", e.toString()); throw e }
        finally {
            outstandingImage.getAndSet(null)?.cancel(false)
            session?.close(); camera?.close()
            // Callback shutdown before closing an image it could still publish.
            val drained = CompletableFuture<Unit>(); handler.post { ownedImage.getAndSet(null)?.close(); reader?.close(); drained.complete(Unit) }
            try { drained.get(5, TimeUnit.SECONDS) } finally { thread.quitSafely(); thread.join(5000) }
            val mem = Debug.MemoryInfo(); Debug.getMemoryInfo(mem)
            manifest.put("totalWallNs", SystemClock.elapsedRealtimeNanos() - start).put("pssBytesAtClose", mem.totalPss.toLong() * 1024)
                .put("nativeHeapBytesAtClose", Debug.getNativeHeapAllocatedSize())
            save()
        }
    }
}
