package dev.latent.camera

import java.util.concurrent.CancellationException
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/** A close request detaches the owner, but cannot invalidate outstanding leases. */
internal class SharedLifetime(private val release: () -> Unit) : AutoCloseable {
    private var references = 1
    private var detached = false
    private val lock = Any()

    fun retain(): AutoCloseable {
        synchronized(lock) {
            check(!detached) { "Resource owner already detached" }
            references++
        }
        val returned = AtomicBoolean(false)
        return AutoCloseable { if (returned.compareAndSet(false, true)) drop() }
    }

    private fun drop() {
        val last = synchronized(lock) { check(references > 0); --references == 0 }
        if (last) release()
    }

    override fun close() {
        val detach = synchronized(lock) {
            if (detached) false else { detached = true; true }
        }
        if (detach) drop()
    }
}

/** Linearizable single-consumer ownership transfer. Cancellation never closes a taken value. */
internal class CaptureTicket<T : AutoCloseable> {
    private val lock = ReentrantLock()
    private val changed = lock.newCondition()
    private var value: T? = null
    private var failure: Throwable? = null
    private var taken = false

    fun offer(item: T) {
        val keep = lock.withLock {
            if (failure != null || taken || value != null) false
            else { value = item; changed.signalAll(); true }
        }
        if (!keep) item.close()
    }

    fun fail(cause: Throwable) {
        val abandoned = lock.withLock {
            if (taken || failure != null) null
            else { failure = cause; val current = value; value = null; changed.signalAll(); current }
        }
        abandoned?.close()
    }

    fun cancel() = fail(CancellationException("Capture cancelled"))

    fun await(timeoutMillis: Long): T {
        require(timeoutMillis > 0)
        lock.withLock {
            check(!taken) { "Capture lease was already transferred" }
            var remaining = TimeUnit.MILLISECONDS.toNanos(timeoutMillis)
            while (value == null && failure == null) {
                if (remaining <= 0) { failure = TimeoutException("RAW capture timed out"); break }
                try { remaining = changed.awaitNanos(remaining) }
                catch (interrupted: InterruptedException) {
                    Thread.currentThread().interrupt()
                    failure = CancellationException("Capture wait interrupted").apply { initCause(interrupted) }
                }
            }
            failure?.let { throw it }
            val result = checkNotNull(value)
            value = null
            taken = true
            return result
        }
    }
}

/** Timestamp keys are sensor timestamps, not callback arrival time or sequence position. */
internal class TimestampMatcher<I : AutoCloseable, R>(private val maximum: Int) : AutoCloseable {
    private val images = sortedMapOf<Long, I>()
    private val results = sortedMapOf<Long, R>()
    private val seenImages = mutableSetOf<Long>()
    private val seenResults = mutableSetOf<Long>()
    private var closed = false

    init { require(maximum > 0) }

    fun image(timestamp: Long, image: I) {
        if (closed || timestamp <= 0 || timestamp in seenImages || seenImages.size >= maximum) {
            image.close()
            throw IllegalArgumentException("Invalid, duplicate, or excessive RAW timestamp")
        }
        seenImages += timestamp
        images[timestamp] = image
    }

    fun result(timestamp: Long, result: R) {
        check(!closed)
        require(timestamp > 0 && timestamp !in seenResults && seenResults.size < maximum) {
            "Invalid, duplicate, or excessive result timestamp"
        }
        seenResults += timestamp
        results[timestamp] = result
    }

    val pairedCount: Int get() = images.keys.count { it in results }

    fun take(): List<Pair<I,R>> {
        check(!closed && images.size == maximum && results.size == maximum && images.keys == results.keys) {
            "RAW images and capture results do not form a complete timestamp bijection"
        }
        val output = images.map { (timestamp, image) -> image to checkNotNull(results[timestamp]) }
        images.clear(); results.clear(); closed = true
        return output
    }

    override fun close() {
        if (closed) return
        closed = true
        var first: Throwable? = null
        images.values.forEach {
            try { it.close() } catch (error: Throwable) { if (first == null) first = error else first?.addSuppressed(error) }
        }
        images.clear(); results.clear()
        first?.let { throw it }
    }
}

/** Release every lease even if one platform cleanup fails. */
internal fun closeAll(resources: Iterable<AutoCloseable>) {
    var first: Exception? = null
    resources.forEach {
        try { it.close() } catch (error: Exception) { if (first == null) first = error else first?.addSuppressed(error) }
    }
    first?.let { throw it }
}
