package dev.latent.camera

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Base64

/** Bounded fixture transport; it does not contain expected image pixels or processing logic. */
object ReplayFixture {
    fun decode(encoded: String): Array<RawInput> {
        require(encoded.length <= 2_000_000) { "Fixture exceeds transport bound" }
        val raw = Base64.getMimeDecoder().decode(encoded)
        val input = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN)
        require(input.remaining() >= 28 && input.int == 0x3152424c) { "Unsupported fixture schema" }
        val width = input.int
        val height = input.int
        val count = input.int
        val cfa = input.int
        val black = input.int.toFloat()
        val white = input.int.toFloat()
        require(width in 2..256 && height in 2..256 && count in 1..16 && cfa in 0..3)
        require(black >= 0 && white > black && white <= 65535)
        val bytes = width * height * 2
        require(input.remaining() == count * (24 + bytes)) { "Truncated or trailing fixture data" }
        return Array(count) { index ->
            val timestamp = input.long
            val exposure = input.long
            val iso = input.float
            val variance = input.float
            require(timestamp > 0 && exposure > 0 && iso.isFinite() && iso > 0)
            require(variance.isFinite() && variance >= 0)
            val buffer = ByteBuffer.allocateDirect(bytes).order(ByteOrder.LITTLE_ENDIAN)
            val slice = input.slice()
            slice.limit(bytes)
            buffer.put(slice).flip()
            input.position(input.position() + bytes)
            RawInput(
                id = index + 1L, timestampNs = timestamp, exposureNs = exposure, iso = iso,
                width = width, height = height, rowStrideBytes = width * 2, cfa = cfa,
                cameraId = "synthetic-linear-sRGB", sensorMode = "fixture-v1", pixels = buffer,
                black = FloatArray(4) { black }, white = white,
                noise = FloatArray(8) { if (it % 2 == 0) 0f else variance },
                whiteBalance = floatArrayOf(1f, 1f, 1f, 1f),
                sensorToLinearSrgb = floatArrayOf(1f,0f,0f,0f,1f,0f,0f,0f,1f), synthetic = true,
            )
        }
    }
}
