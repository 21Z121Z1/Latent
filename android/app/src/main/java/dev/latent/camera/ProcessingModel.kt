package dev.latent.camera

import android.app.Application
import android.graphics.Bitmap
import android.graphics.Matrix
import android.net.Uri
import androidx.core.content.edit
import androidx.core.graphics.createBitmap
import androidx.lifecycle.AndroidViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import org.json.JSONObject
import java.util.Locale
import java.util.concurrent.CancellationException
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/** Only the worker owns unpublished bitmaps and borrowed RAW payloads during JNI. */
data class ProcessingState(
    val busy: Boolean = false,
    val cancelling: Boolean = false,
    val canCancel: Boolean = false,
    val progress: Float = 0f,
    val stage: String = "Ready",
    val result: Bitmap? = null,
    val trace: String = "",
    val diagnostics: String = "",
    val error: String? = null,
    val saved: Uri? = null,
)

class ProcessingModel(application: Application) : AndroidViewModel(application) {
    private val mutable = MutableStateFlow(ProcessingState())
    val state = mutable.asStateFlow()
    private val worker = Executors.newSingleThreadExecutor()
    private val cancel = AtomicBoolean(false)
    private val closed = AtomicBoolean(false)
    private val saving = AtomicBoolean(false)
    private val cancelAcquisition = AtomicReference<(() -> Unit)?>(null)
    private val preferences = application.getSharedPreferences("camera-policy", Application.MODE_PRIVATE)
    var preferVulkan = preferences.getBoolean("vulkan", true)
        set(value) { field = value; preferences.edit { putBoolean("vulkan", value) } }
    var renderExposureEv = preferences.getFloat("render-ev", 0f).coerceIn(-2f, 2f)
        set(value) { field = value.coerceIn(-2f, 2f); preferences.edit { putFloat("render-ev", field) } }
    var targetNoise = preferences.getFloat("target-noise", 0.006f).coerceIn(0.004f, 0.015f)
        set(value) { field = value.coerceIn(0.004f, 0.015f); preferences.edit { putFloat("target-noise", field) } }

    fun replay() {
        submit(acquire = {
            val encoded = getApplication<Application>().assets.open("static_burst.b64").bufferedReader().use { it.readText() }
            OwnedBurst(ReplayFixture.decode(encoded), {}, synthetic = true)
        })
    }

    internal fun capture(controller: CameraController, displayRotation: Int) {
        if (mutable.value.busy || closed.get()) return
        val operation = controller.capture(CaptureIntentInput(targetStandardDeviation = targetNoise), preferVulkan,
            displayRotation) { label, done, total ->
            mutable.update {
                if (it.busy && !it.cancelling) it.copy(stage = label, progress = done.toFloat() / total.coerceAtLeast(1)) else it
            }
        }
        submit(acquire = operation::await, cancelAcquire = operation::cancel)
    }

    fun submit(acquire: () -> OwnedBurst, cancelAcquire: () -> Unit = {}) {
        synchronized(this) {
            if (mutable.value.busy || closed.get()) { cancelAcquire(); return }
            cancel.set(false); saving.set(false); cancelAcquisition.set(cancelAcquire)
            mutable.update { it.copy(busy = true, cancelling = false, canCancel = true, progress = 0f, stage = "Acquiring RAW", error = null) }
            val useVk = preferVulkan
            val ev = renderExposureEv
            worker.execute { process(acquire, useVk, ev) }
        }
    }

    private fun checkCancellation() {
        if (cancel.get() || closed.get()) throw CancellationException("Processing cancelled")
    }

    private fun process(acquire: () -> OwnedBurst, useVk: Boolean, ev: Float) {
        var candidate: Bitmap? = null
        try {
            val acquired = acquire()
            val rotation = acquired.rotationDegrees
            val synthetic = acquired.synthetic
            val capturedAt = acquired.capturedAtMillis
            val diagnostics = acquired.use { burst ->
                checkCancellation()
                val first = burst.frames.first()
                val output = createBitmap(first.width, first.height)
                candidate = output
                val trace = NativeBridge.processRaw(burst.frames, output, useVk, burst.processingBudgetBytes, ev) { stage, done, total ->
                    val names = arrayOf("Selecting reference", "Normalizing RAW", "Aligning frames", "Fusing RAW", "Reconstructing scene")
                    mutable.update { if (it.cancelling) it else it.copy(stage = names.getOrElse(stage) { "Processing" },
                        progress = (done.toFloat() / total.coerceAtLeast(1)).coerceIn(0f, 1f)) }
                    !cancel.get() && !closed.get()
                }
                val json = JSONObject(trace).put("renderExposureEv", ev)
                if (burst.captureTrace.isNotEmpty()) json.put("capture", JSONObject(burst.captureTrace))
                json.toString()
            } // Release every Image/reader lease before orientation, JPEG or MediaStore work.
            checkCancellation()
            if (rotation != 0) {
                val source = checkNotNull(candidate)
                val oriented = Bitmap.createBitmap(source, 0, 0, source.width, source.height,
                    Matrix().apply { postRotate(rotation.toFloat()) }, true)
                if (oriented !== source) source.recycle()
                candidate = oriented
            }
            checkCancellation()
            val summary = diagnosticSummary(diagnostics)
            var uri: Uri? = null
            if (!synthetic) {
                saving.set(true)
                mutable.update { it.copy(stage = "Saving image", canCancel = false) }
                checkCancellation()
                // Publication is a non-cancellable transaction once encoding starts.
                uri = MediaStoreWriter.save(getApplication<Application>().contentResolver,
                    checkNotNull(candidate), diagnostics, capturedAt)
            }
            if (!closed.get()) {
                val output = checkNotNull(candidate)
                mutable.update { it.copy(result = output, trace = diagnostics, diagnostics = summary, saved = uri, progress = 1f, stage = "Ready") }
                candidate = null // UI/Bitmap GC owns the result. Never recycle a displayed bitmap.
            }
        } catch (_: CancellationException) {
            mutable.update { it.copy(stage = "Cancelled") }
        } catch (failure: Exception) {
            mutable.update { it.copy(error = failure.message ?: failure.javaClass.simpleName, stage = "Failed") }
        } catch (_: OutOfMemoryError) {
            mutable.update { it.copy(error = "Memory allocation failed; no partial image was published", stage = "Failed") }
        } finally {
            candidate?.recycle()
            cancelAcquisition.set(null)
            saving.set(false)
            mutable.update { it.copy(busy = false, cancelling = false, canCancel = false) }
        }
    }

    fun cancel() {
        if (saving.get()) return
        cancel.set(true)
        cancelAcquisition.get()?.invoke()
        mutable.update { if (it.busy) it.copy(cancelling = true, canCancel = false, stage = "Cancelling") else it }
    }

    fun saveLatest() {
        synchronized(this) {
            val latest = mutable.value
            val bitmap = latest.result ?: return
            if (latest.busy || latest.saved != null || closed.get()) return
            saving.set(true)
            mutable.update { it.copy(busy = true, canCancel = false, stage = "Saving image", error = null) }
            worker.execute {
                try {
                    val uri = MediaStoreWriter.save(getApplication<Application>().contentResolver, bitmap, latest.trace, System.currentTimeMillis())
                    mutable.update { it.copy(saved = uri, stage = "Saved") }
                } catch (failure: Exception) {
                    mutable.update { it.copy(error = failure.message, stage = "Save failed") }
                } finally { saving.set(false); mutable.update { it.copy(busy = false) } }
            }
        }
    }

    override fun onCleared() {
        synchronized(this) {
            closed.set(true)
            if (!saving.get()) { cancel.set(true); cancelAcquisition.get()?.invoke() }
            // Do not interrupt JNI or invalidate its direct buffers. The active
            // task releases its capture lease at a cooperative stage boundary.
            worker.shutdown()
        }
    }
}

private fun diagnosticSummary(trace: String): String {
    val json = JSONObject(trace)
    val text = StringBuilder()
    text.appendLine("Source: ${json.getString("source")}")
    text.appendLine("Burst ${json.getLong("burstId")}: ${json.getInt("frameCount")} frames; reference ${json.getLong("referenceId")}")
    text.appendLine("Backend: ${json.getString("backend")}")
    if (json.getInt("fallback") != 0) text.appendLine("Fallback: Vulkan fusion is unavailable for this extent/device")
    text.appendLine(String.format(Locale.ROOT, "Effective N: %.2f; alignment: %.3f; robustness: %.3f",
        json.getDouble("effectiveN"), json.getDouble("alignmentConfidence"), json.getDouble("robustnessConfidence")))
    text.appendLine(String.format(Locale.ROOT, "Processing: %.1f ms; working-set admission bound: %.1f MiB",
        json.getDouble("processingMs"), json.getLong("workingSetBoundBytes") / 1048576.0))
    text.appendLine("Color: ${json.getString("colorPath")}")
    text.appendLine("Variance is conditional aleatoric uncertainty, not total error.")
    val frames = json.getJSONArray("frames")
    for (index in 0 until frames.length()) {
        val frame = frames.getJSONObject(index)
        text.appendLine("Frame ${frame.getLong("id")}: ${frame.getLong("accepted")} accepted samples; gain estimated=${frame.getBoolean("gainEstimated")}; noise estimated=${frame.getBoolean("noiseEstimated")}")
        frame.optJSONObject("geometry")?.let { geometry ->
            val status = when (geometry.getInt("status")) {
                0 -> "reference"
                1 -> "estimated"
                2 -> "unobservable"
                3 -> "ambiguous"
                4 -> "inconsistent"
                else -> "unknown"
            }
            val prior = when (geometry.getInt("prior")) {
                0 -> "none"
                1 -> "identity"
                2 -> "global"
                else -> "unknown"
            }
            val cycle = if (geometry.isNull("cycleErrorPx")) "unavailable"
                else String.format(Locale.ROOT, "%.3f px", geometry.getDouble("cycleErrorPx"))
            text.appendLine("Geometry: $status; prior: $prior; cycle error: $cycle")
        }
    }
    json.optJSONObject("capture")?.let { capture ->
        text.appendLine(capture.getJSONObject("policy").getString("reason"))
        text.appendLine("Gyro available: ${capture.getBoolean("gyroAvailable")}; exposure constrained: ${capture.getJSONObject("policy").getBoolean("motionConstraintUsed")}. Physical synchronization is not calibrated by this app.")
    }
    return text.toString()
}

/** Android physical capture lease, not the storage-independent native RawBurst. */
class OwnedBurst(
    val frames: Array<RawInput>,
    private val release: () -> Unit,
    val rotationDegrees: Int = 0,
    val synthetic: Boolean = false,
    val capturedAtMillis: Long = System.currentTimeMillis(),
    val processingBudgetBytes: Long = 256L * 1024L * 1024L,
    val captureTrace: String = "",
) : AutoCloseable {
    private val closed = AtomicBoolean(false)
    override fun close() { if (closed.compareAndSet(false, true)) release() }
}
