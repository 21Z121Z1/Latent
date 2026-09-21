package dev.latent.camera

import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.CancellationException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread

class CaptureResourcesTest {
    private class Resource : AutoCloseable {
        val closes = AtomicInteger()
        override fun close() { check(closes.incrementAndGet() == 1) }
    }

    @Test fun readerCloseIsDeferredUntilTheLastNativeLease() {
        val reader = Resource()
        val owner = SharedLifetime(reader::close)
        val a = owner.retain(); val b = owner.retain()
        owner.close(); owner.close()
        assertEquals(0, reader.closes.get())
        a.close(); a.close()
        assertEquals(0, reader.closes.get())
        b.close()
        assertEquals(1, reader.closes.get())
        assertThrows(IllegalStateException::class.java) { owner.retain() }
    }

    @Test fun cancellationDoesNotFreeTransferredRawWhileJniBorrowsIt() {
        val ticket = CaptureTicket<Resource>()
        val image = Resource()
        ticket.offer(image)
        val acquired = ticket.await(100)
        ticket.cancel()
        assertEquals(0, image.closes.get())
        acquired.close()
        assertEquals(1, image.closes.get())
    }

    @Test fun cancelledLateArrivalAndTimedOutCaptureReleaseExactlyOnce() {
        val ticket = CaptureTicket<Resource>()
        ticket.cancel()
        val image = Resource(); ticket.offer(image)
        assertEquals(1, image.closes.get())
        assertThrows(CancellationException::class.java) { ticket.await(100) }
        val timed = CaptureTicket<Resource>()
        assertThrows(TimeoutException::class.java) { timed.await(1) }
        val late = Resource(); timed.offer(late)
        assertEquals(1, late.closes.get())
    }

    @Test fun concurrentCancellationAndCompletionNeverDuplicateOwnership() {
        repeat(100) {
            val ticket = CaptureTicket<Resource>()
            val image = Resource()
            val start = CountDownLatch(1)
            val done = CountDownLatch(2)
            val a = thread { start.await(); try { ticket.offer(image) } finally { done.countDown() } }
            val b = thread { start.await(); try { ticket.cancel() } finally { done.countDown() } }
            start.countDown()
            assertTrue(done.await(3, TimeUnit.SECONDS))
            a.join(); b.join()
            assertThrows(CancellationException::class.java) { ticket.await(100) }
            assertEquals(1, image.closes.get())
        }
    }

    @Test fun timestampPairingIsIndependentOfDeliveryOrderAndFailsClosed() {
        val matcher = TimestampMatcher<Resource,String>(2)
        val a = Resource(); val b = Resource()
        matcher.image(20,b); matcher.result(10,"a"); matcher.image(10,a); matcher.result(20,"b")
        assertEquals(2,matcher.pairedCount)
        val ordered = matcher.take()
        assertSame(a,ordered[0].first); assertEquals("a",ordered[0].second)
        assertSame(b,ordered[1].first); assertEquals("b",ordered[1].second)
        matcher.close()
        assertEquals(0,a.closes.get())
        ordered.forEach { it.first.close() }
        val invalid=TimestampMatcher<Resource,String>(1)
        val first=Resource(); val duplicate=Resource()
        invalid.image(4,first)
        assertThrows(IllegalArgumentException::class.java) { invalid.image(4,duplicate) }
        invalid.result(5,"wrong timestamp")
        assertThrows(IllegalStateException::class.java) { invalid.take() }
        invalid.close()
        assertEquals(1,first.closes.get()); assertEquals(1,duplicate.closes.get())
    }

    @Test fun gyroClockComparabilityAndFreshnessAreExplicit() {
        val timeline=GyroTimeline()
        timeline.add(100_000_000,0f,3f,4f)
        assertNull(timeline.speedAt(110_000_000,false))
        assertEquals(5f,checkNotNull(timeline.speedAt(110_000_000,true)),0f)
        assertNull(timeline.speedAt(200_000_000,true))
        assertNull(timeline.speedAt(90_000_000,true))
        timeline.add(105_000_000,Float.NaN,0f,0f)
        assertEquals(5f,checkNotNull(timeline.speedAt(110_000_000,true)),0f)
    }
}
