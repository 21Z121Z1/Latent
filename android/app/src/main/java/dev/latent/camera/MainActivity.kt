package dev.latent.camera

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.view.Surface as AndroidSurface
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
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
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
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
        enableEdgeToEdge()
        setContent {
            val dark = isSystemInDarkTheme()
            val scheme = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                if (dark) dynamicDarkColorScheme(this) else dynamicLightColorScheme(this)
            } else if (dark) darkColorScheme() else lightColorScheme()
            MaterialTheme(colorScheme = scheme) { LatentScreen() }
        }
    }
}

@Composable
private fun LatentScreen(model: ProcessingModel = viewModel()) {
    val context = LocalContext.current
    val view = LocalView.current
    val controller = remember(context.applicationContext) { CameraController(context.applicationContext) }
    val camera by controller.state.collectAsStateWithLifecycle()
    val state by model.state.collectAsStateWithLifecycle()
    var selected by remember { mutableStateOf<String?>(null) }
    var permission by remember { mutableStateOf(context.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) }
    val requestPermission = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { permission = it }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(controller, lifecycle) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME)
                permission = context.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer); controller.close() }
    }
    var settings by remember { mutableStateOf(false) }
    var diagnostics by remember { mutableStateOf(false) }
    var resultOpen by remember { mutableStateOf(false) }
    var lensMenu by remember { mutableStateOf(false) }
    LaunchedEffect(state.result) { if (state.result != null) resultOpen = true }
    Surface(Modifier.fillMaxSize()) {
        Box(Modifier.fillMaxSize()) {
            CameraPreview(controller, selected, permission && !resultOpen, camera, Modifier.fillMaxSize())
            if (!permission) {
                Surface(Modifier.align(Alignment.Center).padding(24.dp), shape = MaterialTheme.shapes.large) {
                    Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                        Text(stringResource(R.string.grant_permission))
                        Button(onClick = { requestPermission.launch(Manifest.permission.CAMERA) }) {
                            Text(stringResource(R.string.permission))
                        }
                    }
                }
            }
            Column(Modifier.fillMaxSize().safeDrawingPadding().padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Surface(shape = MaterialTheme.shapes.large, color = MaterialTheme.colorScheme.surface.copy(alpha = 0.9f)) {
                    Row(Modifier.fillMaxWidth().padding(horizontal = 12.dp), horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically) {
                        Text(stringResource(R.string.app_name), style = MaterialTheme.typography.titleLarge)
                        TextButton(onClick = { settings = true }, enabled = !state.busy,
                            modifier = Modifier.sizeIn(minWidth = 48.dp, minHeight = 48.dp)) { Text(stringResource(R.string.settings)) }
                    }
                }
                Spacer(Modifier.weight(1f))
                Surface(shape = MaterialTheme.shapes.large, color = MaterialTheme.colorScheme.surface.copy(alpha = 0.93f)) {
                    Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(if (state.busy) stringResource(R.string.processing) else stringResource(R.string.ready),
                            style = MaterialTheme.typography.titleMedium)
                        Text(if (state.busy) state.stage else camera.status, modifier = Modifier.testTag("processing_stage"),
                            style = MaterialTheme.typography.bodySmall)
                        if (state.busy) {
                            LinearProgressIndicator(progress = { state.progress }, modifier = Modifier.fillMaxWidth())
                            TextButton(onClick = model::cancel, enabled = state.canCancel && !state.cancelling) { Text(stringResource(R.string.cancel)) }
                        }
                        state.error?.let { Text(it, color = MaterialTheme.colorScheme.error, modifier = Modifier.testTag("processing_error")) }
                        if (!state.busy && state.error == null) camera.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                            Box(Modifier.size(56.dp), contentAlignment = Alignment.Center) {
                                state.result?.let { image ->
                                    Image(image.asImageBitmap(), stringResource(R.string.latest_capture),
                                        Modifier.fillMaxSize().clip(RoundedCornerShape(12.dp)).clickable { resultOpen = true }, contentScale = ContentScale.Crop)
                                }
                            }
                            FilledIconButton(onClick = { model.capture(controller, view.display?.rotation ?: AndroidSurface.ROTATION_0) },
                                enabled = permission && camera.previewReady && camera.rawSupported && !camera.capturing && !state.busy,
                                modifier = Modifier.size(76.dp).testTag("shutter")) {
                                Icon(painterResource(R.drawable.shutter), stringResource(R.string.shutter), Modifier.size(42.dp))
                            }
                            Box {
                                TextButton(onClick = { lensMenu = true }, enabled = camera.cameras.isNotEmpty() && !state.busy,
                                    modifier = Modifier.sizeIn(minWidth = 56.dp, minHeight = 56.dp)) { Text(stringResource(R.string.switch_camera)) }
                                DropdownMenu(expanded = lensMenu, onDismissRequest = { lensMenu = false }) {
                                    camera.cameras.forEach { lens -> DropdownMenuItem(
                                        text = { Text(lens.label + if (lens.raw) " · RAW" else "") },
                                        onClick = { selected = lens.id; lensMenu = false },
                                    ) }
                                }
                            }
                        }
                        TextButton(onClick = model::replay, enabled = !state.busy,
                            modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp).testTag("replay")) {
                            Text(stringResource(R.string.replay_fixture))
                        }
                    }
                }
            }
        }
    }
    if (settings) SettingsDialog(model) { settings = false }
    if (resultOpen && state.result != null) {
        Dialog(onDismissRequest = { resultOpen = false }, properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)) {
            Surface(Modifier.fillMaxSize()) {
                Column(Modifier.fillMaxSize().safeDrawingPadding().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text(state.stage, style = MaterialTheme.typography.titleMedium)
                    Image(checkNotNull(state.result).asImageBitmap(), stringResource(R.string.result_description),
                        Modifier.weight(1f).fillMaxWidth().testTag("result_image"), contentScale = ContentScale.Fit)
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        TextButton(onClick = model::saveLatest, enabled = !state.busy && state.saved == null) { Text(stringResource(R.string.save)) }
                        TextButton(onClick = { diagnostics = true }) { Text(stringResource(R.string.diagnostics)) }
                        TextButton(onClick = { resultOpen = false }) { Text(stringResource(R.string.close)) }
                    }
                }
            }
        }
    }
    if (diagnostics) AlertDialog(
        onDismissRequest = { diagnostics = false }, title = { Text(stringResource(R.string.diagnostics)) },
        text = { Text(state.diagnostics, Modifier.verticalScroll(rememberScrollState())) },
        confirmButton = { TextButton(onClick = { diagnostics = false }) { Text(stringResource(R.string.close)) } },
    )
}

@Composable
private fun SettingsDialog(model: ProcessingModel, close: () -> Unit) {
    var vulkan by remember { mutableStateOf(model.preferVulkan) }
    var ev by remember { mutableFloatStateOf(model.renderExposureEv) }
    var noise by remember { mutableFloatStateOf(model.targetNoise) }
    val exposureDescription = stringResource(R.string.render_exposure, ev)
    val noiseDescription = stringResource(R.string.noise_target, noise)
    AlertDialog(onDismissRequest = close, title = { Text(stringResource(R.string.settings)) }, text = {
        Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(Modifier.fillMaxWidth().toggleable(value = vulkan, role = Role.Switch,
                onValueChange = { vulkan = it; model.preferVulkan = it }), verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.vulkan_fusion), Modifier.weight(1f))
                Switch(checked = vulkan, onCheckedChange = null)
            }
            Text(exposureDescription)
            Slider(value = ev, onValueChange = { ev = it; model.renderExposureEv = it }, valueRange = -2f..2f,
                modifier = Modifier.semantics { contentDescription = exposureDescription })
            Text(noiseDescription)
            Slider(value = noise, onValueChange = { noise = it; model.targetNoise = it }, valueRange = 0.004f..0.015f,
                modifier = Modifier.semantics { contentDescription = noiseDescription })
            Text(stringResource(R.string.fixture_notice), style = MaterialTheme.typography.bodySmall)
        }
    }, confirmButton = { TextButton(onClick = close) { Text(stringResource(R.string.close)) } })
}
