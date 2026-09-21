package dev.latent.camera

import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.view.Surface
import android.view.TextureView
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner

/** Preview stays a Camera2 surface; it never runs the temporal reconstruction graph. */
@Composable
internal fun CameraPreview(controller: CameraController, camera: String?, permission: Boolean,
                           state: CameraState, modifier: Modifier = Modifier) {
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    var resumed by remember(lifecycle) { mutableStateOf(lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) }
    var target by remember { mutableStateOf<PreviewTarget?>(null) }
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, _ -> resumed = lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED) }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }
    DisposableEffect(target, camera, resumed, permission) {
        val surface = target
        if (surface != null && permission && resumed) controller.show(surface, camera) else controller.stop()
        onDispose { controller.stop() }
    }
    AndroidView(modifier = modifier, factory = { context ->
        TextureView(context).apply {
            surfaceTextureListener = object : TextureView.SurfaceTextureListener {
                override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
                    target = PreviewTarget(texture)
                    updateTransform(this@apply, controller.state.value)
                }
                override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {
                    updateTransform(this@apply, controller.state.value)
                }
                override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
                    controller.stop()
                    target?.close(); target = null
                    // The controller releases the texture only after CameraDevice.onClosed.
                    return false
                }
                override fun onSurfaceTextureUpdated(texture: SurfaceTexture) = Unit
            }
        }
    }, update = { updateTransform(it, state) }, onRelease = { _ ->
        controller.stop()
        val old = target
        target = null
        old?.close()
        // Keep the listener until TextureView destruction; its false return
        // prevents the framework from releasing a texture still held by Camera2.
    })
}

private fun updateTransform(view: TextureView, state: CameraState) {
    if (view.width <= 0 || view.height <= 0 || state.previewWidth <= 0 || state.previewHeight <= 0) return
    val degrees = when (view.display?.rotation) {
        Surface.ROTATION_90 -> 90
        Surface.ROTATION_180 -> 180
        Surface.ROTATION_270 -> 270
        else -> 0
    }
    val rotation = (state.sensorOrientation + (if (state.frontFacing) degrees else -degrees) + 360) % 360
    val rotatedWidth = if (rotation % 180 == 0) state.previewWidth else state.previewHeight
    val rotatedHeight = if (rotation % 180 == 0) state.previewHeight else state.previewWidth
    val scale = maxOf(view.width.toFloat() / rotatedWidth, view.height.toFloat() / rotatedHeight)
    val cx = view.width / 2f
    val cy = view.height / 2f
    val matrix = Matrix().apply {
        // Undo TextureView's implicit non-uniform stretch, then rotate and crop.
        setScale(state.previewWidth.toFloat() / view.width, state.previewHeight.toFloat() / view.height, cx, cy)
        postRotate(rotation.toFloat(), cx, cy)
        postScale(scale, scale, cx, cy)
        if (state.frontFacing) postScale(-1f, 1f, cx, cy)
    }
    view.setTransform(matrix)
}
