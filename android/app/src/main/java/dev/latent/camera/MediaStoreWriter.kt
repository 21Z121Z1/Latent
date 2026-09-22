package dev.latent.camera

import android.content.ContentResolver
import android.content.ContentValues
import android.graphics.Bitmap
import android.net.Uri
import android.os.Environment
import android.provider.MediaStore
import androidx.exifinterface.media.ExifInterface
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.UUID

/** A pending row is published only after both image and metadata writes succeed. */
object MediaStoreWriter {
    fun save(
        resolver: ContentResolver,
        bitmap: Bitmap,
        trace: String,
        capturedAtMillis: Long,
        encode: (Bitmap, java.io.OutputStream) -> Boolean = { image, stream ->
            image.compress(Bitmap.CompressFormat.JPEG, 95, stream)
        },
    ): Uri {
        val metadata = JSONObject(trace)
        val fixture = metadata.optString("source") == "synthetic-fixture"
        val date = Date(capturedAtMillis)
        val name = (if (fixture) "Latent_fixture_" else "Latent_") +
            SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(date) + "_${UUID.randomUUID()}.jpg"
        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, name)
            put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
            put(MediaStore.Images.Media.RELATIVE_PATH, Environment.DIRECTORY_PICTURES + "/Latent")
            put(MediaStore.Images.Media.DATE_TAKEN, capturedAtMillis)
            put(MediaStore.Images.Media.IS_PENDING, 1)
            put(MediaStore.Images.Media.WIDTH, bitmap.width)
            put(MediaStore.Images.Media.HEIGHT, bitmap.height)
        }
        val uri = checkNotNull(resolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)) { "MediaStore insert failed" }
        try {
            checkNotNull(resolver.openOutputStream(uri, "w")).use { stream ->
                check(encode(bitmap, stream)) { "JPEG encoding failed" }
                stream.flush()
            }
            checkNotNull(resolver.openFileDescriptor(uri, "rw")).use { descriptor ->
                val exif = ExifInterface(descriptor.fileDescriptor)
                exif.setAttribute(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL.toString())
                exif.setAttribute(ExifInterface.TAG_SOFTWARE, "Latent ${BuildConfig.VERSION_NAME}")
                exif.setAttribute(ExifInterface.TAG_DATETIME_ORIGINAL, SimpleDateFormat("yyyy:MM:dd HH:mm:ss", Locale.US).format(date))
                exif.setAttribute(ExifInterface.TAG_COLOR_SPACE, "1")
                exif.setAttribute(ExifInterface.TAG_IMAGE_WIDTH, bitmap.width.toString())
                exif.setAttribute(ExifInterface.TAG_IMAGE_LENGTH, bitmap.height.toString())
                // The fused result's radiometric reference supplies exposure metadata.
                exif.setAttribute(ExifInterface.TAG_EXPOSURE_TIME, (metadata.getLong("exposureNs") / 1e9).toString())
                exif.setAttribute(ExifInterface.TAG_PHOTOGRAPHIC_SENSITIVITY, metadata.getDouble("iso").toInt().toString())
                if (fixture) exif.setAttribute(ExifInterface.TAG_IMAGE_DESCRIPTION, "Synthetic fixture, not a live camera capture")
                exif.saveAttributes()
            }
            check(resolver.update(uri, ContentValues().apply { put(MediaStore.Images.Media.IS_PENDING, 0) }, null, null) == 1) {
                "MediaStore publication failed"
            }
            return uri
        } catch (failure: Throwable) {
            try { resolver.delete(uri, null, null) } catch (cleanup: Throwable) { failure.addSuppressed(cleanup) }
            throw failure
        }
    }
}
