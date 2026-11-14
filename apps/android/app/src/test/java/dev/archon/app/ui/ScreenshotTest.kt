package dev.archon.app.ui

import android.graphics.Bitmap
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.captureToImage
import androidx.compose.ui.test.hasSetTextAction
import androidx.activity.ComponentActivity
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import android.graphics.Canvas
import androidx.compose.ui.test.onRoot
import androidx.compose.ui.test.performTextInput
import dev.archon.app.core.LinkStatus
import kotlinx.coroutines.flow.MutableStateFlow
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File

/**
 * Renders every screen to PNG on the JVM (Robolectric native graphics) so the
 * UI can be reviewed without a device. Output: $ARCHON_SHOTS or build/screenshots.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = [34], qualifiers = "w430dp-h932dp-xhdpi")
class ScreenshotTest {
    @get:Rule val rule = createAndroidComposeRule<ComponentActivity>()

    private class FakeUi : ArchonUi {
        override val screen = MutableStateFlow<Screen>(Screen.Computers)
        override val link = MutableStateFlow(LinkState())
        override val icons = MutableStateFlow<Map<String, ImageBitmap>>(emptyMap())
        override val levels = MutableStateFlow<Map<Int, Float>>(emptyMap())
        override val selectedEndpoint = MutableStateFlow<String?>(null)
        override val notice = MutableStateFlow<Notice?>(null)
        override val searching = MutableStateFlow(true)
        override val computers = MutableStateFlow<List<PcRow>>(emptyList())
        override fun open(target: Target) {}
        override fun openAddress(input: String) = true
        override fun back() {}
        override fun forget(target: Target) {}
        override fun dismissNotice() {}
        override fun submitCode(code: String) {}
        override fun retryPairing() {}
        override fun reconnect() {}
        override fun repair(target: Target) {}
        override fun selectEndpoint(id: String) {}
        override fun setVolume(endpoint: Boolean, id: String, volume: Int) {}
        override fun setMute(endpoint: Boolean, id: String, muted: Boolean) {}
        override fun setMetersVisible(visible: Boolean) {}
    }

    private val pc = Target("a".repeat(32), "DESKTOP-01", "192.168.0.10", 47321)

    private fun shoot(name: String, ui: FakeUi, afterSetup: () -> Unit = {}) {
        // Drive the clock by hand: entrance animations finish, loops are frozen mid-way.
        rule.mainClock.autoAdvance = false
        rule.setContent { ArchonTheme { ArchonApp(ui) } }
        rule.mainClock.advanceTimeBy(100)
        afterSetup()
        repeat(12) { rule.mainClock.advanceTimeBy(100) }
        val dir = File(System.getenv("ARCHON_SHOTS") ?: "build/screenshots").apply { mkdirs() }
        // Draw the real view hierarchy into a bitmap (works under Robolectric native graphics).
        val root = rule.activity.window.decorView.rootView
        val bmp = Bitmap.createBitmap(root.width, root.height, Bitmap.Config.ARGB_8888)
        rule.runOnUiThread { root.draw(Canvas(bmp)) }
        File(dir, "$name.png").outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }

    @Test fun computers() = shoot("1-computers", FakeUi().apply {
        computers.value = listOf(
            PcRow(pc, paired = true, online = true),
            PcRow(Target("b".repeat(32), "NOTEBOOK-SALA", "192.168.0.22", 47321), paired = true, online = false),
            PcRow(Target("c".repeat(32), "ESTUDIO-PC", "192.168.0.31", 47321), paired = false, online = true),
        )
    })

    @Test fun computersEmpty() = shoot("1b-computers-empty", FakeUi())

    // Long PC name: wraps to a second line on the radar instead of crossing it.
    @Test fun computersLongName() = shoot("1c-computers-long-name", FakeUi().apply {
        computers.value = listOf(
            PcRow(Target("d".repeat(32), "ESTACAO-DE-TRABALHO-DO-ESCRITORIO", "192.168.0.40", 47321), paired = false, online = true),
            PcRow(Target("e".repeat(32), "Ayu", "192.168.0.41", 47321), paired = true, online = false),
        )
    })

    @Test fun pairing() {
        val ui = FakeUi().apply {
            screen.value = Screen.Pairing(pc)
            link.value = LinkState(status = LinkStatus.AWAITING_CODE)
        }
        shoot("2-pairing", ui) { rule.onNode(hasSetTextAction()).performTextInput("4829") }
    }

    @Test fun pairingWrong() = shoot("2b-pairing-wrong", FakeUi().apply {
        screen.value = Screen.Pairing(pc)
        link.value = LinkState(status = LinkStatus.FAILED, error = dev.archon.app.core.CoreError.WRONG_CODE)
    })

    private fun mixerUi() = FakeUi().apply {
        screen.value = Screen.Mixer(pc)
        link.value = LinkState(
            status = LinkStatus.CONNECTED, synced = true,
            endpoints = mapOf("e1" to Endpoint("e1", "SteelSeries Sonar - Gaming", 6400, false, true, 1)),
            sources = listOf(
                Source("s1", "e1", "Spotify", "", 7200, false, true, false, 2),
                Source("s2", "e1", "Discord", "", 8000, false, true, false, 3),
                Source("s3", "e1", "Counter-Strike 2", "", 5800, false, false, false, 4),
                Source("s4", "e1", "Brave", "", 4500, true, false, false, 5),
                Source("s5", "e1", "System sounds", "", 4000, false, false, true, 6),
            ).associateBy { it.id },
        )
        levels.value = mapOf(2 to 0.9f, 3 to 0.4f)
    }

    @Test fun mixer() = shoot("3-mixer", mixerUi())

    @Test fun mixerReconnecting() = shoot("3b-mixer-connecting", FakeUi().apply {
        screen.value = Screen.Mixer(pc)
        link.value = LinkState(status = LinkStatus.RECONNECTING)
    })

    @Test fun mixerReconnectingWithData() = shoot("3c-mixer-reconnecting", mixerUi().apply {
        link.value = link.value.copy(status = LinkStatus.RECONNECTING)
    })

    private fun computersUi() = FakeUi().apply {
        computers.value = listOf(
            PcRow(pc, paired = true, online = true),
            PcRow(Target("b".repeat(32), "NOTEBOOK-SALA", "192.168.0.22", 47321), paired = true, online = false),
            PcRow(Target("c".repeat(32), "ESTUDIO-PC", "192.168.0.31", 47321), paired = false, online = true),
        )
    }

    @Test @Config(qualifiers = "w360dp-h800dp-xhdpi")
    fun narrowComputers() = shoot("4-360-computers", computersUi())

    @Test @Config(qualifiers = "w360dp-h800dp-xhdpi")
    fun narrowMixer() = shoot("4-360-mixer", mixerUi())

    @Test @Config(qualifiers = "w360dp-h800dp-xhdpi")
    fun narrowPairing() {
        val ui = FakeUi().apply {
            screen.value = Screen.Pairing(pc)
            link.value = LinkState(status = LinkStatus.AWAITING_CODE)
        }
        shoot("4-360-pairing", ui) { rule.onNode(hasSetTextAction()).performTextInput("48") }
    }

    @Test fun fontScale2Mixer() {
        org.robolectric.RuntimeEnvironment.setFontScale(2f)
        shoot("5-font2-mixer", mixerUi())
    }

    @Test fun fontScale2Computers() {
        org.robolectric.RuntimeEnvironment.setFontScale(2f)
        shoot("5-font2-computers", computersUi())
    }

    @Test fun mixerNotRecognized() = shoot("3d-mixer-not-recognized", FakeUi().apply {
        screen.value = Screen.Mixer(pc)
        link.value = LinkState(status = LinkStatus.FAILED, error = dev.archon.app.core.CoreError.NOT_RECOGNIZED)
    })
}