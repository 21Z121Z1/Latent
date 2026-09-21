package dev.latent.camera

import android.app.Application
import android.graphics.Bitmap
import android.graphics.Matrix
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.util.concurrent.CancellationException
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/** Immutable UI state; only the worker owns borrowed RAW payloads during JNI calls. */
data class ProcessingState(
    val busy: Boolean = false,
    val cancelling: Boolean = false,
    val progress: Float = 0f,
    val stage: String = "Ready",
    val result: Bitmap? = null,
    val trace: String = "",
    val error: String? = null,
    val saved: Uri? = null,
)

class ProcessingModel(application: Application) : AndroidViewModel(application) {
    private val mutable = MutableStateFlow(ProcessingState())
    val state = mutable.asStateFlow()
    private val worker = Executors.newSingleThreadExecutor()
    private val cancel = AtomicBoolean(false)
    private val closed = AtomicBoolean(false)
    var preferVulkan = true
    var renderExposureEv = 0f
    var targetNoise = 0.006f

    fun replay() {
        submit(acquire = {
            val encoded = getApplication<Application>().assets.open("static_burst.b64").bufferedReader().use { it.readText() }
            OwnedBurst(ReplayFixture.decode(encoded), {}, synthetic = true)
        })
    }

    fun submit(acquire: () -> OwnedBurst) {
        synchronized(this) {
            if (mutable.value.busy || closed.get()) return
            cancel.set(false)
            mutable.update { it.copy(busy = true, cancelling = false, progress = 0f, stage = "Acquiring RAW", error = null) }
        }
        val useVk = preferVulkan
        val ev = renderExposureEv
        worker.execute {
            try {
                acquire().use { burst ->
                    if (cancel.get()) throw CancellationException()
                    val first = burst.frames.first()
                    val output = Bitmap.createBitmap(first.width, first.height, Bitmap.Config.ARGB_8888)
                    val diagnostics = NativeBridge.processRaw(burst.frames, output, useVk, burst.processingBudgetBytes, ev) { stage, done, total ->
                        val names = arrayOf("Selecting reference", "Normalizing RAW", "Aligning frames", "Fusing RAW", "Reconstructing scene")
                        mutable.update { it.copy(stage = names.getOrElse(stage) { "Processing" }, progress = (done.toFloat() / total.coerceAtLeast(1)).coerceIn(0f, 1f)) }
                        !cancel.get() && !closed.get()
                    }
                    if (cancel.get() || closed.get()) throw CancellationException()
                    val oriented = if (burst.rotationDegrees == 0) output else Bitmap.createBitmap(output, 0, 0, output.width, output.height,
                        Matrix().apply { postRotate(burst.rotationDegrees.toFloat()) }, true)
                    var uri: Uri? = null
                    if (!burst.synthetic) {
                        mutable.update { it.copy(stage = "Saving image") }
                        uri = MediaStoreWriter.save(getApplication<Application>().contentResolver, oriented, diagnostics, burst.capturedAtMillis)
                    }
                    if (!closed.get()) mutable.update { it.copy(result = oriented, trace = diagnostics, saved = uri, progress = 1f, stage = "Ready") }
                }
            } catch (_: CancellationException) {
                mutable.update { it.copy(stage = "Cancelled") }
            } catch (failure: Exception) {
                mutable.update { it.copy(error = failure.message ?: failure.javaClass.simpleName, stage = "Failed") }
            } finally {
                mutable.update { it.copy(busy = false, cancelling = false) }
            }
        }
    }

    fun cancel() {
        cancel.set(true)
        mutable.update { if (it.busy) it.copy(cancelling = true, stage = "Cancelling") else it }
    }

    fun saveLatest() {
        val latest = mutable.value
        val bitmap = latest.result ?: return
        if (latest.busy || latest.saved != null) return
        mutable.update { it.copy(busy = true, stage = "Saving image", error = null) }
        worker.execute {
            try {
                val uri = MediaStoreWriter.save(getApplication<Application>().contentResolver, bitmap, latest.trace, System.currentTimeMillis())
                mutable.update { it.copy(saved = uri, stage = "Saved") }
            } catch (failure: Exception) {
                mutable.update { it.copy(error = failure.message, stage = "Save failed") }
            } finally { mutable.update { it.copy(busy = false) } }
        }
    }

    override fun onCleared() {
        closed.set(true)
        cancel.set(true)
        // Do not interrupt JNI or invalidate its direct buffers. The active task
        // releases its capture lease after the next cooperative stage boundary.
        worker.shutdown()
        super.onCleared()
    }
}

class OwnedBurst(
    val frames: Array<RawInput>,
    private val release: () -> Unit,
    val rotationDegrees: Int = 0,
    val synthetic: Boolean = false,
    val capturedAtMillis: Long = System.currentTimeMillis(),
    val processingBudgetBytes: Long = 256L * 1024L * 1024L,
) : AutoCloseable {
    private val closed = AtomicBoolean(false)
    override fun close() { if (closed.compareAndSet(false, true)) release() }
}
