package dev.latent.camera

import androidx.compose.ui.test.assertExists
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
        compose.onNodeWithTag("result_image").assertExists()
    }
}
