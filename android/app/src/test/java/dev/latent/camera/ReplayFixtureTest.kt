package dev.latent.camera

import org.junit.Assert.*
import org.junit.Test
import java.nio.ByteOrder

class ReplayFixtureTest {
    private fun fixture() = checkNotNull(javaClass.classLoader?.getResourceAsStream("static_burst.b64")).bufferedReader().use { it.readText() }

    @Test fun parsesRealSensorSamplesWithoutLoadingNativeLibrary() {
        val frames = ReplayFixture.decode(fixture())
        assertEquals(4, frames.size)
        assertEquals(33, frames[0].width)
        assertEquals(25, frames[0].height)
        assertTrue(frames.all { it.synthetic && it.pixels.isDirect })
        assertTrue(frames.asList().zipWithNext().all { (a,b) -> a.timestampNs < b.timestampNs })
        val a = frames[0].pixels.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        val b = frames[1].pixels.duplicate().order(ByteOrder.LITTLE_ENDIAN)
        assertTrue((0 until 825).any { a.getShort(it*2) != b.getShort(it*2) })
    }

    @Test fun rejectsTruncationAndUnknownSchema() {
        assertThrows(IllegalArgumentException::class.java) { ReplayFixture.decode("AAAA") }
        assertThrows(IllegalArgumentException::class.java) { ReplayFixture.decode(fixture().take(200)) }
    }
}
