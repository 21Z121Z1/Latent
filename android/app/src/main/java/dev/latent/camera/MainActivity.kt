package dev.latent.camera

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.Surface as AndroidSurface
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
        )
        setContent {
            MaterialTheme(colorScheme = cameraColorScheme) { LatentScreen() }
        }
    }
}

private val cameraColorScheme = darkColorScheme(
    primary = Color(0xFFFFD45B),
    onPrimary = Color(0xFF261C00),
    background = Color.Black,
    onBackground = Color.White,
    surface = Color.Black,
    onSurface = Color.White,
    surfaceContainerHigh = Color(0xFF242427),
    surfaceContainerHighest = Color(0xFF303034),
    onSurfaceVariant = Color(0xFFD1D1D6),
    outline = Color(0xFF8E8E93),
)

@Composable
private fun LatentScreen(model: ProcessingModel = viewModel()) {
    val context = LocalContext.current
    val view = LocalView.current
    val controller = remember(context.applicationContext) { CameraController(context.applicationContext) }
    val camera by controller.state.collectAsStateWithLifecycle()
    val state by model.state.collectAsStateWithLifecycle()
    var selected by remember { mutableStateOf<String?>(null) }
    var permission by remember {
        mutableStateOf(context.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED)
    }
    val requestPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {
        permission = it
    }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(controller, lifecycle) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) {
                permission = context.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
            }
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer); controller.close() }
    }
    var settingsOpen by remember { mutableStateOf(false) }
    var resultOpen by remember { mutableStateOf(false) }
    var diagnosticsOpen by remember { mutableStateOf(false) }
    LaunchedEffect(state.result) {
        if (state.result != null) {
            diagnosticsOpen = false
            resultOpen = true
        }
    }

    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        BoxWithConstraints(Modifier.fillMaxSize().safeDrawingPadding()) {
            val sideControls = maxWidth >= 600.dp && maxHeight < 520.dp
            if (sideControls) {
                Row(Modifier.fillMaxSize()) {
                    Viewfinder(controller, selected, permission, permission && !resultOpen, camera, state,
                        onPermission = { requestPermission.launch(Manifest.permission.CAMERA) },
                        onCancel = model::cancel,
                        modifier = Modifier.weight(1f).fillMaxHeight())
                    CameraControls(camera, state, permission, selected,
                        onCapture = { model.capture(controller, view.display?.rotation ?: AndroidSurface.ROTATION_0) },
                        onSelectCamera = { selected = it }, onOpenResult = { resultOpen = true },
                        onReplay = model::replay, onSettings = { settingsOpen = true },
                        sideControls = true, modifier = Modifier.width(280.dp).fillMaxHeight())
                }
            } else {
                Column(Modifier.fillMaxSize(),
                    horizontalAlignment = Alignment.CenterHorizontally) {
                    CameraHeader(Modifier.fillMaxWidth())
                    Viewfinder(controller, selected, permission, permission && !resultOpen, camera, state,
                        onPermission = { requestPermission.launch(Manifest.permission.CAMERA) },
                        onCancel = model::cancel,
                        modifier = Modifier.weight(1f).fillMaxWidth())
                    CameraControls(camera, state, permission, selected,
                        onCapture = { model.capture(controller, view.display?.rotation ?: AndroidSurface.ROTATION_0) },
                        onSelectCamera = { selected = it }, onOpenResult = { resultOpen = true },
                        onReplay = model::replay, onSettings = { settingsOpen = true },
                        sideControls = false, modifier = Modifier.widthIn(max = 560.dp).fillMaxWidth())
                }
            }
        }
    }

    if (settingsOpen) SettingsSheet(model) { settingsOpen = false }
    if (resultOpen && state.result != null) {
        ResultViewer(state, diagnosticsOpen,
            onDiagnostics = { diagnosticsOpen = true }, onBack = { diagnosticsOpen = false },
            onClose = { resultOpen = false; diagnosticsOpen = false }, onSave = model::saveLatest)
    }
}

@Composable
private fun CameraHeader(modifier: Modifier = Modifier) {
    Surface(modifier, color = MaterialTheme.colorScheme.background) {
        Row(Modifier.padding(horizontal = 24.dp, vertical = 16.dp).sizeIn(minHeight = 48.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.SpaceBetween) {
            Text(stringResource(R.string.app_name).uppercase(), style = MaterialTheme.typography.labelMedium,
                letterSpacing = 2.sp)
            Text(stringResource(R.string.raw_badge), color = MaterialTheme.colorScheme.primary,
                style = MaterialTheme.typography.labelMedium, letterSpacing = 1.5.sp)
        }
    }
}

@Composable
private fun Viewfinder(
    controller: CameraController,
    selected: String?,
    permission: Boolean,
    previewActive: Boolean,
    camera: CameraState,
    state: ProcessingState,
    onPermission: () -> Unit,
    onCancel: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Box(modifier.clipToBounds().background(MaterialTheme.colorScheme.surfaceContainerHigh)) {
        CameraPreview(controller, selected, previewActive, camera, Modifier.fillMaxSize())
        if (!permission) {
            PermissionPrompt(onRequest = onPermission,
                modifier = Modifier.align(Alignment.Center).padding(horizontal = 24.dp))
        }
        CameraStatus(camera, state, permission, onCancel,
            Modifier.align(Alignment.BottomCenter).padding(12.dp))
    }
}

@Composable
private fun CameraStatus(camera: CameraState, state: ProcessingState, permission: Boolean,
                         onCancel: () -> Unit, modifier: Modifier = Modifier) {
    val message = when {
        state.busy -> state.stage
        state.error != null -> state.error
        !permission -> null
        camera.error != null -> camera.error
        camera.previewReady && !camera.rawSupported -> stringResource(R.string.capture_unavailable)
        !camera.previewReady -> stringResource(R.string.opening_camera)
        else -> null
    } ?: return
    Surface(modifier.widthIn(max = 420.dp), shape = MaterialTheme.shapes.medium,
        color = MaterialTheme.colorScheme.surfaceContainerHighest.copy(alpha = 0.94f)) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 10.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(message, style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.testTag(if (state.error != null) "processing_error" else "processing_stage")
                    .semantics { liveRegion = LiveRegionMode.Polite })
            if (state.busy) {
                LinearProgressIndicator(progress = { state.progress.coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxWidth())
                TextButton(onClick = onCancel, enabled = state.canCancel && !state.cancelling,
                    modifier = Modifier.sizeIn(minHeight = 48.dp)) {
                    Text(stringResource(R.string.cancel))
                }
            }
        }
    }
}

@Composable
private fun PermissionPrompt(onRequest: () -> Unit, modifier: Modifier = Modifier) {
    Surface(modifier.widthIn(max = 360.dp), shape = MaterialTheme.shapes.extraLarge,
        color = MaterialTheme.colorScheme.surfaceContainerHighest) {
        Column(Modifier.padding(24.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(stringResource(R.string.camera_access_needed), style = MaterialTheme.typography.titleLarge)
            Text(stringResource(R.string.grant_permission), style = MaterialTheme.typography.bodyMedium)
            Button(onClick = onRequest, modifier = Modifier.sizeIn(minHeight = 48.dp)) {
                Text(stringResource(R.string.permission))
            }
        }
    }
}

@Composable
private fun CameraControls(
    camera: CameraState,
    state: ProcessingState,
    permission: Boolean,
    selected: String?,
    onCapture: () -> Unit,
    onSelectCamera: (String) -> Unit,
    onOpenResult: () -> Unit,
    onReplay: () -> Unit,
    onSettings: () -> Unit,
    sideControls: Boolean,
    modifier: Modifier = Modifier,
) {
    val captureEnabled = permission && camera.previewReady && camera.rawSupported &&
        !camera.capturing && !state.busy
    val currentLens = camera.cameras.firstOrNull { it.id == (selected ?: camera.selected) }
    val switchCameraLabel = stringResource(R.string.switch_camera) +
        (currentLens?.let { ": ${it.label}" } ?: "")
    val shutterLabel = stringResource(R.string.shutter)
    val noCaptureLabel = stringResource(R.string.no_capture)
    Surface(modifier, color = MaterialTheme.colorScheme.background) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp)) {
            if (sideControls) {
                CameraHeader(Modifier.fillMaxWidth())
                Spacer(Modifier.weight(1f))
            }
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically) {
                Column(horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    CameraAction(onReplay, !state.busy, stringResource(R.string.replay_fixture),
                        Modifier.testTag("replay")) {
                        Icon(painterResource(R.drawable.ic_sample), null, Modifier.size(24.dp))
                    }
                    Text(stringResource(R.string.sample_short),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Surface(onClick = onCapture, enabled = captureEnabled, shape = CircleShape,
                    color = MaterialTheme.colorScheme.surfaceContainerHighest,
                    modifier = Modifier.size(84.dp).testTag("shutter").semantics {
                        contentDescription = shutterLabel
                    }) {
                    Box(contentAlignment = Alignment.Center) {
                        Box(Modifier.size(66.dp).background(
                            if (captureEnabled) Color.White else MaterialTheme.colorScheme.outline,
                            CircleShape))
                    }
                }
                Column(horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    CameraAction(onSettings, !state.busy, stringResource(R.string.settings)) {
                        Icon(painterResource(R.drawable.ic_controls), null, Modifier.size(24.dp))
                    }
                    Text(stringResource(R.string.controls_short),
                        style = MaterialTheme.typography.labelSmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(52.dp), contentAlignment = Alignment.Center) {
                    val image = state.result
                    if (image != null) {
                        Image(image.asImageBitmap(), stringResource(R.string.latest_capture),
                            Modifier.size(48.dp).clip(CircleShape)
                                .clickable(onClickLabel = stringResource(R.string.latest_capture), onClick = onOpenResult),
                            contentScale = ContentScale.Crop)
                    } else {
                        Surface(shape = CircleShape, color = MaterialTheme.colorScheme.surfaceContainerHigh,
                            modifier = Modifier.size(48.dp).semantics {
                                contentDescription = noCaptureLabel
                            }) {
                            Box(contentAlignment = Alignment.Center) {
                                Text("—", color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                }
                Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                    Surface(shape = CircleShape, color = MaterialTheme.colorScheme.surfaceContainerHigh) {
                        Text(stringResource(R.string.camera_mode),
                            Modifier.padding(horizontal = 16.dp, vertical = 10.dp),
                            color = MaterialTheme.colorScheme.primary,
                            style = MaterialTheme.typography.labelMedium, letterSpacing = 1.sp)
                    }
                }
                CameraAction(onClick = {
                    val cameras = camera.cameras
                    if (cameras.size > 1) {
                        val currentIndex = cameras.indexOfFirst { it.id == (selected ?: camera.selected) }
                        onSelectCamera(cameras[(currentIndex + 1) % cameras.size].id)
                    }
                }, enabled = camera.cameras.size > 1 && !state.busy, label = switchCameraLabel) {
                    Text("↻", style = MaterialTheme.typography.headlineSmall)
                }
            }
            if (sideControls) Spacer(Modifier.weight(1f))
        }
    }
}

@Composable
private fun CameraAction(onClick: () -> Unit, enabled: Boolean, label: String,
                         modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    Surface(onClick = onClick, enabled = enabled, shape = CircleShape,
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        contentColor = if (enabled) MaterialTheme.colorScheme.onSurface
            else MaterialTheme.colorScheme.outline,
        modifier = modifier.size(52.dp).semantics { contentDescription = label }) {
        Box(contentAlignment = Alignment.Center) { content() }
    }
}

@Composable
private fun SettingsSheet(model: ProcessingModel, close: () -> Unit) {
    var vulkan by remember { mutableStateOf(model.preferVulkan) }
    var ev by remember { mutableFloatStateOf(model.renderExposureEv) }
    var noise by remember { mutableFloatStateOf(model.targetNoise) }
    val exposureDescription = stringResource(R.string.render_exposure, ev)
    val noiseDescription = stringResource(R.string.noise_target, noise)
    Dialog(onDismissRequest = close,
        properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        BoxWithConstraints(Modifier.fillMaxSize().safeDrawingPadding(),
            contentAlignment = Alignment.BottomCenter) {
            Surface(Modifier.fillMaxWidth().widthIn(max = 600.dp).height(maxHeight * 0.68f),
                shape = RoundedCornerShape(topStart = 28.dp, topEnd = 28.dp),
                color = MaterialTheme.colorScheme.surfaceContainerHigh) {
                Column {
                    Row(Modifier.fillMaxWidth().padding(start = 24.dp, end = 16.dp, top = 12.dp),
                        verticalAlignment = Alignment.CenterVertically) {
                        Text(stringResource(R.string.settings), Modifier.weight(1f),
                            style = MaterialTheme.typography.titleLarge)
                        TextButton(onClick = close, modifier = Modifier.sizeIn(minHeight = 48.dp)) {
                            Text(stringResource(R.string.done))
                        }
                    }
                    Column(Modifier.fillMaxWidth().weight(1f).verticalScroll(rememberScrollState())
                        .padding(start = 24.dp, end = 24.dp, top = 16.dp, bottom = 24.dp),
                        verticalArrangement = Arrangement.spacedBy(16.dp)) {
                        Row(Modifier.fillMaxWidth().toggleable(value = vulkan, role = Role.Switch,
                            onValueChange = { vulkan = it; model.preferVulkan = it }),
                            verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f)) {
                                Text(stringResource(R.string.vulkan_fusion),
                                    style = MaterialTheme.typography.titleMedium)
                                Text(stringResource(R.string.vulkan_help),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                            Switch(checked = vulkan, onCheckedChange = null)
                        }
                        HorizontalDivider()
                        Column {
                            Text(exposureDescription, style = MaterialTheme.typography.titleSmall)
                            Slider(value = ev, onValueChange = { ev = it; model.renderExposureEv = it },
                                valueRange = -2f..2f,
                                modifier = Modifier.semantics { contentDescription = exposureDescription })
                        }
                        Column {
                            Text(noiseDescription, style = MaterialTheme.typography.titleSmall)
                            Slider(value = noise, onValueChange = { noise = it; model.targetNoise = it },
                                valueRange = 0.004f..0.015f,
                                modifier = Modifier.semantics { contentDescription = noiseDescription })
                        }
                        Text(stringResource(R.string.fixture_notice),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
            }
        }
    }
}

@Composable
private fun ResultViewer(
    state: ProcessingState,
    diagnosticsOpen: Boolean,
    onDiagnostics: () -> Unit,
    onBack: () -> Unit,
    onClose: () -> Unit,
    onSave: () -> Unit,
) {
    val image = checkNotNull(state.result)
    Dialog(onDismissRequest = { if (diagnosticsOpen) onBack() else onClose() },
        properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
        Surface(Modifier.fillMaxSize()) {
            BoxWithConstraints(Modifier.fillMaxSize().safeDrawingPadding()) {
                val wide = maxWidth >= 700.dp
                Column(Modifier.fillMaxSize().padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        Text(if (diagnosticsOpen) stringResource(R.string.diagnostics)
                            else stringResource(R.string.result_description),
                            modifier = Modifier.weight(1f), style = MaterialTheme.typography.titleLarge)
                        TextButton(onClick = { if (diagnosticsOpen) onBack() else onClose() },
                            modifier = Modifier.sizeIn(minHeight = 48.dp)) {
                            Text(if (diagnosticsOpen) stringResource(R.string.back)
                                else stringResource(R.string.close))
                        }
                    }
                    if (diagnosticsOpen) {
                        Text(state.diagnostics, Modifier.weight(1f).verticalScroll(rememberScrollState()),
                            style = MaterialTheme.typography.bodyMedium)
                    } else if (wide) {
                        Row(Modifier.weight(1f), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                            Image(image.asImageBitmap(), stringResource(R.string.result_description),
                                Modifier.weight(1f).fillMaxSize().testTag("result_image"),
                                contentScale = ContentScale.Fit)
                            ResultActions(state, onDiagnostics, onSave, Modifier.width(300.dp))
                        }
                    } else {
                        Image(image.asImageBitmap(), stringResource(R.string.result_description),
                            Modifier.weight(1f).fillMaxWidth().testTag("result_image"),
                            contentScale = ContentScale.Fit)
                        ResultActions(state, onDiagnostics, onSave, Modifier.fillMaxWidth())
                    }
                }
            }
        }
    }
}

@Composable
private fun ResultActions(state: ProcessingState, onDiagnostics: () -> Unit,
                          onSave: () -> Unit, modifier: Modifier = Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(12.dp)) {
        if (state.busy) {
            Text(state.stage, style = MaterialTheme.typography.bodyMedium)
            LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        }
        state.error?.let { error ->
            Text(error, color = MaterialTheme.colorScheme.error,
                modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite })
        }
        OutlinedButton(onClick = onDiagnostics, modifier = Modifier.fillMaxWidth().sizeIn(minHeight = 48.dp)) {
            Text(stringResource(R.string.diagnostics))
        }
        Button(onClick = onSave, enabled = !state.busy && state.saved == null,
            modifier = Modifier.fillMaxWidth().sizeIn(minHeight = 48.dp)) {
            Text(stringResource(if (state.saved == null) R.string.save else R.string.saved))
        }
    }
}
