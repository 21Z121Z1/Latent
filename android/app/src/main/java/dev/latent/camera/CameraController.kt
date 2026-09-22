package dev.latent.camera

import android.Manifest
import android.app.ActivityManager
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.graphics.SurfaceTexture
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureFailure
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.Image
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Size
import android.view.Surface
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import org.json.JSONObject
import java.util.concurrent.CancellationException
import java.util.concurrent.Executor
import java.util.concurrent.TimeoutException
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.abs

internal data class CameraChoice(val id: String, val label: String, val raw: Boolean)
internal data class CameraState(
    val cameras: List<CameraChoice> = emptyList(),
    val selected: String? = null,
    val previewReady: Boolean = false,
    val rawSupported: Boolean = false,
    val capturing: Boolean = false,
    val status: String = "Camera closed",
    val error: String? = null,
    val previewWidth: Int = 0,
    val previewHeight: Int = 0,
    val sensorOrientation: Int = 0,
    val frontFacing: Boolean = false,
)

/** UI owns the initial lease. A pending/open camera retains the actual Surface. */
internal class PreviewTarget(val texture: SurfaceTexture) : AutoCloseable {
    val surface = Surface(texture)
    private val lifetime = SharedLifetime { try { surface.release() } finally { texture.release() } }
    fun retain(): AutoCloseable = lifetime.retain()
    override fun close() = lifetime.close()
}

internal class CaptureOperation(
    private val ticket: CaptureTicket<OwnedBurst>,
    private val cancelCapture: () -> Unit,
) {
    fun await(): OwnedBurst = try { ticket.await(12_000) } catch (error: Exception) { cancel(); throw error }
    fun cancel() { ticket.cancel(); cancelCapture() }
}

/** Camera callbacks are handler-confined. Images cross threads only as owned leases. */
internal class CameraController(context: Context) : AutoCloseable, SensorEventListener {
    private val app = context.applicationContext
    private val manager = app.getSystemService(CameraManager::class.java)
    private val sensors = app.getSystemService(SensorManager::class.java)
    private val thread = HandlerThread("Latent-Camera2").apply { start() }
    private val handler = Handler(thread.looper)
    private val executor = Executor { command -> handler.post(command) }
    private val mutable = MutableStateFlow(CameraState())
    val state = mutable.asStateFlow()
    private val closed = AtomicBoolean(false)
    private val gyro = GyroTimeline()
    private data class PendingPreview(val target: PreviewTarget, val camera: String?, val lease: AutoCloseable)
    private var pending: PendingPreview? = null
    private var current: CameraRun? = null

    init { handler.post { refreshChoices() } }

    private fun refreshChoices() {
        try {
            val choices = manager.cameraIdList.map { id ->
                val info = manager.getCameraCharacteristics(id)
                val facing = when (info.get(CameraCharacteristics.LENS_FACING)) {
                    CameraCharacteristics.LENS_FACING_BACK -> "Rear"
                    CameraCharacteristics.LENS_FACING_FRONT -> "Front"
                    else -> "External"
                }
                CameraChoice(id, "$facing · $id", info.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
                    ?.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_RAW) == true)
            }.sortedWith(compareByDescending<CameraChoice> { it.raw }.thenBy { it.id })
            mutable.update { it.copy(cameras = choices, status = if (choices.isEmpty()) "No camera is available; fixture replay remains available" else it.status) }
        } catch (error: Exception) { mutable.update { it.copy(error = error.message ?: "Camera enumeration failed") } }
    }

    fun show(target: PreviewTarget, camera: String?) {
        if (closed.get()) return
        val lease = try { target.retain() } catch (_: IllegalStateException) { return }
        if (!handler.post {
                pending?.lease?.close()
                pending = if (closed.get()) { lease.close(); null } else PendingPreview(target, camera, lease)
                current?.close()
                startPending()
            }) lease.close()
    }

    fun stop() {
        handler.post {
            pending?.lease?.close(); pending = null
            current?.close()
            mutable.update { it.copy(previewReady = false, capturing = false, status = "Camera closed") }
        }
    }

    private fun startPending() {
        if (current != null) return
        val next = pending
        pending = null
        if (closed.get()) { next?.lease?.close(); thread.quitSafely(); return }
        if (next == null) return
        if (app.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            next.lease.close()
            mutable.update { it.copy(error = "Camera permission is not granted", previewReady = false) }
            return
        }
        try {
            val id = next.camera ?: mutable.value.cameras.firstOrNull()?.id ?: error("No camera is available")
            val info = manager.getCameraCharacteristics(id)
            val run = CameraRun(next, id, info)
            current = run
            mutable.update { it.copy(selected = id, previewReady = false, rawSupported = run.rawSize != null,
                status = "Opening camera", error = null, sensorOrientation = run.orientation, frontFacing = run.front) }
            manager.openCamera(id, run.deviceCallback, handler)
        } catch (error: Exception) {
            val failed = current
            if (failed != null) failed.dispose() else next.lease.close()
            mutable.update { it.copy(error = error.message ?: "Camera open failed", previewReady = false) }
        }
    }

    fun capture(intent: CaptureIntentInput, preferVulkan: Boolean, rotation: Int,
                progress: (String, Int, Int) -> Unit): CaptureOperation {
        val ticket = CaptureTicket<OwnedBurst>()
        val operation = CaptureOperation(ticket) { handler.post { current?.cancel(ticket) } }
        if (closed.get()) { ticket.fail(IllegalStateException("Camera controller is closed")); return operation }
        handler.post {
            val run = current
            if (run == null || run.closing || !mutable.value.previewReady || run.rawSize == null) {
                ticket.fail(IllegalStateException(app.getString(R.string.raw_unsupported)))
            } else try { run.beginCapture(ticket, intent, preferVulkan, rotation, progress) }
            catch (error: Exception) { ticket.fail(error); run.cancel(ticket) }
        }
        return operation
    }

    override fun close() {
        if (!closed.compareAndSet(false, true)) return
        handler.post {
            pending?.lease?.close(); pending = null
            current?.close()
            if (current == null) thread.quitSafely()
        }
    }

    override fun onSensorChanged(event: SensorEvent) {
        if (event.sensor.type == Sensor.TYPE_GYROSCOPE && event.values.size >= 3)
            gyro.add(event.timestamp, event.values[0], event.values[1], event.values[2])
    }
    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    private class ImageLease(val image: Image, private val lease: AutoCloseable) : AutoCloseable {
        private val closed = AtomicBoolean(false)
        override fun close() { if (closed.compareAndSet(false, true)) try { image.close() } finally { lease.close() } }
    }
    private enum class Phase { WaitingAe, WaitingLock, Capturing }
    private data class FrameTag(val capture: CaptureRun, val ordinal: Int)
    private class CaptureRun(
        val ticket: CaptureTicket<OwnedBurst>, val intent: CaptureIntentInput, val preferVulkan: Boolean,
        val rotation: Int, val progress: (String, Int, Int) -> Unit,
    ) {
        var phase = Phase.WaitingAe
        var matcher: TimestampMatcher<ImageLease, TotalCaptureResult>? = null
        var count = 0
        var sequenceId = -1
        var retainedBytes = 0L
        var retainedBudget = 0L
        var processingBudget = 0L
        var plan = JSONObject()
        val createdAt = System.currentTimeMillis()
        val startedNs = SystemClock.elapsedRealtimeNanos()
        var timeout: Runnable? = null
    }

    private inner class CameraRun(private val preview: PendingPreview, val id: String,
                                  private val info: CameraCharacteristics) {
        var closing = false
            private set
        private var disposed = false
        private var device: CameraDevice? = null
        private var session: CameraCaptureSession? = null
        private var reader: ImageReader? = null
        private var readerLifetime: SharedLifetime? = null
        private var active: CaptureRun? = null
        private val outputs = checkNotNull(info.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP))
        private val manual = info.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
            ?.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR) == true
        private val aeLock = info.get(CameraCharacteristics.CONTROL_AE_LOCK_AVAILABLE) == true
        private val awbLock = info.get(CameraCharacteristics.CONTROL_AWB_LOCK_AVAILABLE) == true
        private val adapter = try {
            if (info.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
                    ?.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_RAW) == true)
                CameraRawAdapter(id, info) else null
        } catch (_: Exception) { null }
        val rawSize: Size? = if ((manual || aeLock) && adapter != null) outputs.getOutputSizes(ImageFormat.RAW_SENSOR)
            ?.filter(adapter::canMap)?.maxByOrNull { it.width.toLong() * it.height } else null
        val orientation = info.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
        val front = info.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
        private val comparable = info.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE) ==
            CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME
        private val previewSize: Size = checkNotNull(outputs.getOutputSizes(SurfaceTexture::class.java)).let { sizes ->
            val suitable = sizes.filter { it.width <= 1920 && it.height <= 1080 }.ifEmpty { sizes.toList() }
            val desired = rawSize?.let { it.width.toDouble() / it.height } ?: (4.0 / 3.0)
            checkNotNull(suitable.minWithOrNull(compareBy<Size> { abs(it.width.toDouble() / it.height - desired) }
                .thenByDescending { it.width.toLong() * it.height }))
        }
        private val lockTag = Any()

        val deviceCallback = object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                device = camera
                if (closing || closed.get() || current !== this@CameraRun) { camera.close(); return }
                try { configure(camera) } catch (error: Exception) { fatal(error) }
            }
            override fun onDisconnected(camera: CameraDevice) {
                device = camera
                if (!closing) mutable.update { it.copy(error = "Camera disconnected") }
                if (closing) camera.close() else close()
            }
            override fun onError(camera: CameraDevice, error: Int) {
                device = camera
                if (!closing) mutable.update { it.copy(error = "Camera2 device error $error") }
                if (closing) camera.close() else close()
            }
            override fun onClosed(camera: CameraDevice) { dispose() }
        }

        private fun configure(camera: CameraDevice) {
            preview.target.texture.setDefaultBufferSize(previewSize.width, previewSize.height)
            val surfaces = mutableListOf(preview.target.surface)
            rawSize?.let { size ->
                val imageReader = ImageReader.newInstance(size.width, size.height, ImageFormat.RAW_SENSOR, 10)
                reader = imageReader
                readerLifetime = SharedLifetime { imageReader.close() }
                imageReader.setOnImageAvailableListener({ source -> receiveImages(source) }, handler)
                surfaces += imageReader.surface
            }
            camera.createCaptureSession(SessionConfiguration(SessionConfiguration.SESSION_REGULAR,
                surfaces.map { OutputConfiguration(it) }, executor, object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(configured: CameraCaptureSession) {
                        if (closing || current !== this@CameraRun) { configured.close(); return }
                        session = configured
                        try {
                            repeatPreview(false)
                            sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE)?.let {
                                sensors.registerListener(this@CameraController, it, SensorManager.SENSOR_DELAY_GAME, handler)
                            }
                            mutable.update { it.copy(previewReady = true, rawSupported = rawSize != null,
                                status = if (rawSize == null) app.getString(R.string.raw_unsupported) else "Night RAW ready",
                                previewWidth = previewSize.width, previewHeight = previewSize.height) }
                        } catch (error: Exception) { fatal(error) }
                    }
                    override fun onConfigureFailed(configured: CameraCaptureSession) {
                        configured.close(); fatal(IllegalStateException("Camera does not support this preview + RAW stream combination"))
                    }
                }))
        }

        private fun baseRequest(template: Int, locked: Boolean): CaptureRequest.Builder {
            val request = checkNotNull(device).createCaptureRequest(template)
            request.addTarget(preview.target.surface)
            request.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
            request.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
            request.set(CaptureRequest.FLASH_MODE, CaptureRequest.FLASH_MODE_OFF)
            request.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)
            if (aeLock) request.set(CaptureRequest.CONTROL_AE_LOCK, locked)
            if (awbLock) request.set(CaptureRequest.CONTROL_AWB_LOCK, locked)
            val focus = info.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES)
            if (focus?.contains(CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE) == true)
                request.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
            if (info.availableCaptureRequestKeys.contains(CaptureRequest.STATISTICS_LENS_SHADING_MAP_MODE))
                request.set(CaptureRequest.STATISTICS_LENS_SHADING_MAP_MODE, CaptureRequest.STATISTICS_LENS_SHADING_MAP_MODE_ON)
            return request
        }

        private fun repeatPreview(locked: Boolean) {
            if (closing) return
            val request = baseRequest(CameraDevice.TEMPLATE_PREVIEW, locked)
            if (locked) request.setTag(lockTag)
            checkNotNull(session).setRepeatingRequest(request.build(), previewCallback, handler)
        }

        private val previewCallback = object : CameraCaptureSession.CaptureCallback() {
            override fun onCaptureCompleted(camera: CameraCaptureSession, request: CaptureRequest, result: TotalCaptureResult) {
                if (closing || camera !== session) return
                val capture = active ?: return
                try {
                    val ae = result.get(CaptureResult.CONTROL_AE_STATE)
                    if (capture.phase == Phase.WaitingAe && (ae == CaptureResult.CONTROL_AE_STATE_CONVERGED ||
                            ae == CaptureResult.CONTROL_AE_STATE_FLASH_REQUIRED || ae == CaptureResult.CONTROL_AE_STATE_LOCKED)) {
                        if (aeLock) {
                            capture.phase = Phase.WaitingLock
                            capture.progress("Locking exposure", 0, 1)
                            repeatPreview(true)
                        } else startBurst(capture, result)
                    } else if (capture.phase == Phase.WaitingLock && request.tag === lockTag &&
                        (ae == CaptureResult.CONTROL_AE_STATE_LOCKED || result.get(CaptureResult.CONTROL_AE_LOCK) == true)) {
                        startBurst(capture, result)
                    }
                } catch (error: Exception) { failCapture(error) }
            }
        }

        fun beginCapture(ticket: CaptureTicket<OwnedBurst>, intent: CaptureIntentInput, preferVk: Boolean,
                         displayRotation: Int, progress: (String, Int, Int) -> Unit) {
            check(!closing && active == null && rawSize != null && session != null) { "Camera is not ready for a RAW burst" }
            require(intent.maximumFrames in 1..8) { "Capture reader supports at most eight retained frames" }
            val degrees = when (displayRotation) {
                Surface.ROTATION_90 -> 90
                Surface.ROTATION_180 -> 180
                Surface.ROTATION_270 -> 270
                else -> 0
            }
            val rotation = (orientation + (if (front) degrees else -degrees) + 360) % 360
            val capture = CaptureRun(ticket, intent, preferVk, rotation, progress)
            active = capture
            val timeout = Runnable { if (active === capture) failCapture(TimeoutException("AE lock or RAW burst delivery timed out")) }
            capture.timeout = timeout
            handler.postDelayed(timeout, 10_000)
            mutable.update { it.copy(capturing = true, error = null, status = "Waiting for converged AE") }
            progress("Waiting for converged AE", 0, 1)
        }

        private fun startBurst(capture: CaptureRun, result: TotalCaptureResult) {
            check(active === capture && capture.phase != Phase.Capturing)
            val size = checkNotNull(rawSize)
            val output = checkNotNull(adapter).outputSize
            val exposure = checkNotNull(result.get(CaptureResult.SENSOR_EXPOSURE_TIME))
            val duration = checkNotNull(result.get(CaptureResult.SENSOR_FRAME_DURATION))
            val sensitivity = checkNotNull(result.get(CaptureResult.SENSOR_SENSITIVITY))
            val time = checkNotNull(result.get(CaptureResult.SENSOR_TIMESTAMP))
            val motion = gyro.speedAt(time, comparable)
            val noise = result.get(CaptureResult.SENSOR_NOISE_PROFILE)?.takeIf { it.size == 4 }?.maxOf {
                (it.first * 0.18 + it.second).toFloat()
            }
            val observation = CaptureObservationInput(exposure, duration, sensitivity, true,
                noise ?: -1f, motion ?: -1f, comparable && motion != null)
            val limits = checkNotNull(info.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE))
            val isoRange = checkNotNull(info.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE))
            val maximumDuration = checkNotNull(info.get(CameraCharacteristics.SENSOR_INFO_MAX_FRAME_DURATION))
            val minimumDuration = outputs.getOutputMinFrameDuration(ImageFormat.RAW_SENSOR, size)
            val stallDuration = outputs.getOutputStallDuration(ImageFormat.RAW_SENSOR, size)
            require(minimumDuration > 0 && stallDuration >= 0) { "RAW cadence is unknown; capture admission cannot assume an FPS" }
            val cadence = Math.addExact(minimumDuration, stallDuration)
            val memoryInfo = ActivityManager.MemoryInfo()
            app.getSystemService(ActivityManager::class.java).getMemoryInfo(memoryInfo)
            // Budget policy, not an assertion of measured peak usage or available VRAM.
            val totalBudget = minOf(2L * 1024 * 1024 * 1024, memoryInfo.availMem / 2)
            val processing = NativeBridge.processingBound(output.width, output.height, capture.intent.maximumFrames, capture.preferVulkan)
            val presentationAndDriverReserve = output.width.toLong() * output.height * 16 + 128L * 1024 * 1024
            val retainedBudget = totalBudget - processing - presentationAndDriverReserve
            val rowEstimate = ((size.width.toLong() * 2 + 4095) / 4096) * 4096
            val rawBytes = rowEstimate * size.height
            require(retainedBudget >= rawBytes) { "Insufficient memory budget for this RAW sensor mode" }
            val capability = CaptureCapabilityInput(true, manual, aeLock, limits.lower, limits.upper,
                cadence, maximumDuration, isoRange.lower, isoRange.upper, retainedBudget, rawBytes, 8)
            val plan = JSONObject(NativeBridge.capturePlan(observation, capture.intent, capability))
            val frames = plan.getJSONArray("frames")
            require(frames.length() in 1..8)
            capture.plan = JSONObject().put("policy", plan).put("gyroTimestampComparable", comparable)
                .put("gyroAvailable", motion != null).put("totalBudgetBytes", totalBudget)
                .put("advertisedRawStallNs", stallDuration)
            if (motion != null) capture.plan.put("angularSpeedRadiansPerSecond", motion)
            capture.count = frames.length()
            capture.processingBudget = processing
            capture.retainedBudget = retainedBudget
            capture.matcher = TimestampMatcher(capture.count)
            capture.phase = Phase.Capturing
            val requests = List(capture.count) { index ->
                val frame = frames.getJSONObject(index)
                baseRequest(CameraDevice.TEMPLATE_STILL_CAPTURE, true).apply {
                    addTarget(checkNotNull(reader).surface)
                    if (plan.getBoolean("manual")) {
                        set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                        set(CaptureRequest.SENSOR_EXPOSURE_TIME, frame.getLong("exposureNs"))
                        set(CaptureRequest.SENSOR_FRAME_DURATION, frame.getLong("durationNs"))
                        set(CaptureRequest.SENSOR_SENSITIVITY, frame.getInt("iso"))
                    }
                    setTag(FrameTag(capture, index))
                }.build()
            }
            capture.progress("Capturing RAW", 0, capture.count)
            mutable.update { it.copy(status = "Capturing ${capture.count} RAW frames") }
            checkNotNull(session).stopRepeating()
            capture.sequenceId = checkNotNull(session).captureBurst(requests, burstCallback, handler)
        }

        private val burstCallback = object : CameraCaptureSession.CaptureCallback() {
            override fun onCaptureCompleted(camera: CameraCaptureSession, request: CaptureRequest, result: TotalCaptureResult) {
                val tag = request.tag as? FrameTag ?: return
                if (closing || active !== tag.capture || camera !== session) return
                try {
                    checkNotNull(tag.capture.matcher).result(checkNotNull(result.get(CaptureResult.SENSOR_TIMESTAMP)), result)
                    deliverIfComplete(tag.capture)
                } catch (error: Exception) { failCapture(error) }
            }
            override fun onCaptureFailed(camera: CameraCaptureSession, request: CaptureRequest, failure: CaptureFailure) {
                val tag = request.tag as? FrameTag ?: return
                if (active === tag.capture) failCapture(IllegalStateException("RAW frame ${tag.ordinal} failed: ${failure.reason}"))
            }
            override fun onCaptureSequenceAborted(camera: CameraCaptureSession, sequenceId: Int) {
                if (!closing && camera === session && active?.phase == Phase.Capturing && active?.sequenceId == sequenceId)
                    failCapture(CancellationException("Camera aborted the RAW sequence"))
            }
        }

        private fun receiveImages(source: ImageReader) {
            try {
                while (true) {
                    val image = source.acquireNextImage() ?: break
                    val capture = active
                    if (closing || source !== reader || capture == null || capture.phase != Phase.Capturing) { image.close(); continue }
                    val lease = try { ImageLease(image, checkNotNull(readerLifetime).retain()) }
                    catch (error: Exception) { image.close(); throw error }
                    try {
                        capture.retainedBytes += image.planes.single().buffer.capacity().toLong()
                        require(capture.retainedBytes <= capture.retainedBudget) { "Actual RAW allocation exceeds retained-frame budget" }
                        checkNotNull(capture.matcher).image(image.timestamp, lease)
                    } catch (error: Exception) { lease.close(); throw error }
                    deliverIfComplete(capture)
                }
            } catch (error: Exception) { if (!closing) failCapture(error) }
        }

        private fun deliverIfComplete(capture: CaptureRun) {
            val matcher = checkNotNull(capture.matcher)
            capture.progress("Capturing RAW", matcher.pairedCount, capture.count)
            if (matcher.pairedCount != capture.count) return
            val pairs = matcher.take()
            val burst = try {
                val inputs = pairs.map { (lease, result) ->
                    val tag = result.request.tag as FrameTag
                    checkNotNull(adapter).input(lease.image, result, tag.ordinal + 1L)
                }.toTypedArray()
                capture.plan.put("deliveryDurationNs", SystemClock.elapsedRealtimeNanos() - capture.startedNs)
                capture.plan.put("retainedRawBytes", capture.retainedBytes)
                OwnedBurst(inputs, { closeAll(pairs.map { it.first }) }, capture.rotation,
                    capturedAtMillis = capture.createdAt, processingBudgetBytes = capture.processingBudget,
                    captureTrace = capture.plan.toString())
            } catch (error: Exception) {
                try { closeAll(pairs.map { it.first }) } catch (cleanup: Exception) { error.addSuppressed(cleanup) }
                failCapture(error)
                return
            }
            // Ownership crosses exactly once. No subsequent preview failure can
            // reclaim buffers after the processing worker has taken the ticket.
            capture.ticket.offer(burst)
            finishCapture(capture)
        }

        fun cancel(ticket: CaptureTicket<OwnedBurst>) {
            if (active?.ticket === ticket) failCapture(CancellationException("RAW capture cancelled"))
        }

        fun failCapture(error: Exception) {
            val capture = active ?: return
            active = null
            capture.timeout?.let(handler::removeCallbacks)
            try { capture.matcher?.close() } catch (cleanup: Exception) { error.addSuppressed(cleanup) }
            try { capture.ticket.fail(error) } catch (cleanup: Exception) { error.addSuppressed(cleanup) }
            mutable.update { it.copy(capturing = false, previewReady = false,
                error = if (error is CancellationException) it.error else error.message) }
            if (!closing) {
                // A new ImageReader/session generation prevents late images from
                // an aborted sequence from entering the next capture's matcher.
                if (!closed.get() && pending == null) {
                    try { pending = PendingPreview(preview.target, id, preview.target.retain()) }
                    catch (_: IllegalStateException) { /* UI has already released the target. */ }
                }
                close()
            }
        }

        private fun finishCapture(capture: CaptureRun) {
            capture.timeout?.let(handler::removeCallbacks)
            if (active === capture) active = null
            mutable.update { it.copy(capturing = false, status = if (closing) "Camera closed" else "Night RAW ready") }
            if (!closing) try { repeatPreview(false) } catch (error: Exception) { fatal(error) }
        }

        private fun fatal(error: Exception) {
            mutable.update { it.copy(error = error.message ?: "Camera2 failure", previewReady = false) }
            close()
        }

        fun close() {
            if (closing) return
            closing = true
            cleanup { sensors.unregisterListener(this@CameraController) }
            failCapture(CancellationException("Camera closed"))
            cleanup { reader?.setOnImageAvailableListener(null, null) }
            val lifetime = readerLifetime
            readerLifetime = null; reader = null
            cleanup { lifetime?.close() }
            try { session?.stopRepeating() } catch (_: Exception) { /* Device can already be disconnected. */ }
            val previousSession = session
            session = null
            cleanup { previousSession?.close() }
            cleanup { device?.close() }
            // If openCamera is pending, its callback closes the device and calls dispose.
        }

        private fun cleanup(action: () -> Unit) {
            try { action() } catch (error: Exception) {
                mutable.update { it.copy(error = it.error ?: "Camera resource cleanup failed: ${error.message}") }
            }
        }

        fun dispose() {
            if (disposed) return
            disposed = true
            close()
            cleanup { preview.lease.close() }
            if (current === this) current = null
            startPending()
        }
    }
}
