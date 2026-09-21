package dev.latent.camera

import kotlin.math.sqrt

/** Bounded observation history. It does not infer camera/IMU clock comparability. */
internal class GyroTimeline {
    private data class Sample(val timestamp: Long, val speed: Float)
    private val samples = ArrayDeque<Sample>()

    @Synchronized fun add(timestampNs: Long, x: Float, y: Float, z: Float) {
        if (timestampNs <= 0 || !x.isFinite() || !y.isFinite() || !z.isFinite()) return
        val speed = sqrt(x.toDouble()*x + y.toDouble()*y + z.toDouble()*z).toFloat()
        if (!speed.isFinite()) return
        if (samples.isNotEmpty() && samples.last().timestamp >= timestampNs) samples.clear()
        samples.addLast(Sample(timestampNs, speed))
        while (samples.size > 256 || samples.first().timestamp < timestampNs - 500_000_000L) samples.removeFirst()
    }

    @Synchronized fun speedAt(timestampNs: Long, timestampComparable: Boolean): Float? {
        if (!timestampComparable || timestampNs <= 0) return null
        // Do not silently extrapolate stale or future motion across capture gaps.
        val eligible = samples.filter { it.timestamp in (timestampNs-80_000_000L)..timestampNs }
        if (eligible.isEmpty() || timestampNs-eligible.last().timestamp > 50_000_000L) return null
        return eligible.maxOf { it.speed }
    }
}
