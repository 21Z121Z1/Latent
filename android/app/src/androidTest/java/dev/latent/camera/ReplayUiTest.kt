package dev.latent.camera

import android.graphics.Bitmap
import androidx.test.platform.app.InstrumentationRegistry
import androidx.compose.ui.test.assertIsDisplayed
import androidx.compose.ui.test.assertIsNotEnabled
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onAllNodesWithTag
import androidx.compose.ui.test.onNodeWithTag
import androidx.compose.ui.test.performClick
import org.junit.Rule
import org.junit.Test

class ReplayUiTest {
    @get:Rule val compose = createAndroidComposeRule<MainActivity>()

    @Test fun fixtureTravelsThroughNativeProcessingToResultUi() {
        compose.onNodeWithTag("shutter").assertIsNotEnabled()
        compose.onNodeWithTag("replay").performClick()
        compose.waitUntil(timeoutMillis = 60_000) {
            compose.onAllNodesWithTag("result_image").fetchSemanticsNodes().isNotEmpty() ||
                compose.onAllNodesWithTag("processing_error").fetchSemanticsNodes().isNotEmpty()
        }
        compose.onNodeWithTag("result_image").assertIsDisplayed()
        // Capture before ActivityScenario teardown; a later shell screenshot is
        // only the launcher and cannot establish what the result UI displayed.
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val screenshot = checkNotNull(instrumentation.uiAutomation.takeScreenshot())
        try {
            instrumentation.targetContext.openFileOutput("replay-result.png", 0).use {
                check(screenshot.compress(Bitmap.CompressFormat.PNG, 100, it))
            }
        } finally { screenshot.recycle() }
    }
}
