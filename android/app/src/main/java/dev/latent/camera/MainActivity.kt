package dev.latent.camera

import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.Image
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
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
    val state by model.state.collectAsStateWithLifecycle()
    var settings by remember { mutableStateOf(false) }
    var diagnostics by remember { mutableStateOf(false) }
    Surface(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize().safeDrawingPadding().padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.app_name), style = MaterialTheme.typography.headlineMedium)
                TextButton(onClick = { settings = true }, modifier = Modifier.sizeIn(minWidth = 48.dp, minHeight = 48.dp)) {
                    Text(stringResource(R.string.settings))
                }
            }
            Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                val image = state.result
                if (image != null) {
                    Image(image.asImageBitmap(), stringResource(R.string.result_description),
                        Modifier.fillMaxSize().testTag("result_image"), contentScale = ContentScale.Fit)
                } else {
                    Text(stringResource(R.string.fixture_notice))
                }
            }
            Text(state.stage, modifier = Modifier.testTag("processing_stage"))
            if (state.busy) {
                LinearProgressIndicator(progress = { state.progress }, modifier = Modifier.fillMaxWidth())
                TextButton(onClick = model::cancel, enabled = !state.cancelling) { Text(stringResource(R.string.cancel)) }
            }
            state.error?.let { Text(it, color = MaterialTheme.colorScheme.error, modifier = Modifier.testTag("processing_error")) }
            Button(onClick = model::replay, enabled = !state.busy,
                modifier = Modifier.fillMaxWidth().heightIn(min = 56.dp).testTag("replay")) {
                Text(stringResource(R.string.replay_fixture))
            }
            if (state.result != null) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceEvenly) {
                    TextButton(onClick = model::saveLatest, enabled = !state.busy && state.saved == null) { Text(stringResource(R.string.save)) }
                    TextButton(onClick = { diagnostics = true }) { Text(stringResource(R.string.diagnostics)) }
                }
            }
        }
    }
    if (settings) SettingsDialog(model) { settings = false }
    if (diagnostics) AlertDialog(
        onDismissRequest = { diagnostics = false }, title = { Text(stringResource(R.string.diagnostics)) },
        text = { Text(state.trace, Modifier.verticalScroll(rememberScrollState())) },
        confirmButton = { TextButton(onClick = { diagnostics = false }) { Text(stringResource(R.string.close)) } },
    )
}

@Composable
private fun SettingsDialog(model: ProcessingModel, close: () -> Unit) {
    var vulkan by remember { mutableStateOf(model.preferVulkan) }
    var ev by remember { mutableStateOf(model.renderExposureEv) }
    AlertDialog(onDismissRequest = close, title = { Text(stringResource(R.string.settings)) }, text = {
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.vulkan_fusion), Modifier.weight(1f))
                Switch(checked = vulkan, onCheckedChange = { vulkan = it; model.preferVulkan = it })
            }
            Text(stringResource(R.string.render_exposure, ev))
            Slider(value = ev, onValueChange = { ev = it; model.renderExposureEv = it }, valueRange = -2f..2f)
            Text(stringResource(R.string.fixture_notice), style = MaterialTheme.typography.bodySmall)
        }
    }, confirmButton = { TextButton(onClick = close) { Text(stringResource(R.string.close)) } })
}
