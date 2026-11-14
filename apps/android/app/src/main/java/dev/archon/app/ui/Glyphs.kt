package dev.archon.app.ui

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.unit.dp

/** Line glyphs drawn in-house (no icon font / extended icon pack: smaller APK). */
object Glyphs {
    private fun glyph(name: String, block: ImageVector.Builder.() -> Unit): ImageVector =
        ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f).apply(block).build()

    private fun ImageVector.Builder.stroke(d: androidx.compose.ui.graphics.vector.PathBuilder.() -> Unit) =
        path(
            stroke = SolidColor(Color.White), strokeLineWidth = 1.8f,
            strokeLineCap = StrokeCap.Round, strokeLineJoin = StrokeJoin.Round, pathBuilder = d,
        )

    val Speaker = glyph("speaker") {
        stroke { moveTo(4f, 9.5f); lineTo(8f, 9.5f); lineTo(12.5f, 5.5f); lineTo(12.5f, 18.5f); lineTo(8f, 14.5f); lineTo(4f, 14.5f); close() }
        stroke { moveTo(16f, 9f); curveTo(17.3f, 10.2f, 17.3f, 13.8f, 16f, 15f) }
        stroke { moveTo(18.5f, 6.5f); curveTo(21.2f, 9f, 21.2f, 15f, 18.5f, 17.5f) }
    }

    val SpeakerOff = glyph("speaker_off") {
        stroke { moveTo(4f, 9.5f); lineTo(8f, 9.5f); lineTo(12.5f, 5.5f); lineTo(12.5f, 18.5f); lineTo(8f, 14.5f); lineTo(4f, 14.5f); close() }
        stroke { moveTo(16f, 9.5f); lineTo(21f, 14.5f) }
        stroke { moveTo(21f, 9.5f); lineTo(16f, 14.5f) }
    }

    val Back = glyph("back") { stroke { moveTo(15f, 5f); lineTo(8f, 12f); lineTo(15f, 19f) } }

    val Plus = glyph("plus") {
        stroke { moveTo(12f, 5f); lineTo(12f, 19f) }
        stroke { moveTo(5f, 12f); lineTo(19f, 12f) }
    }

    val More = glyph("more") {
        stroke { moveTo(12f, 6f); lineTo(12f, 6.01f) }
        stroke { moveTo(12f, 12f); lineTo(12f, 12.01f) }
        stroke { moveTo(12f, 18f); lineTo(12f, 18.01f) }
    }

    val Chevron = glyph("chevron") { stroke { moveTo(7f, 10f); lineTo(12f, 15f); lineTo(17f, 10f) } }

    val Monitor = glyph("monitor") {
        stroke { moveTo(4f, 5f); lineTo(20f, 5f); lineTo(20f, 16f); lineTo(4f, 16f); close() }
        stroke { moveTo(9f, 20f); lineTo(15f, 20f) }
        stroke { moveTo(12f, 16f); lineTo(12f, 20f) }
    }

    val Bell = glyph("bell") {
        stroke { moveTo(6f, 16f); lineTo(6f, 11f); curveTo(6f, 7.7f, 8.7f, 5f, 12f, 5f); curveTo(15.3f, 5f, 18f, 7.7f, 18f, 11f); lineTo(18f, 16f); lineTo(19.5f, 17.5f); lineTo(4.5f, 17.5f); close() }
        stroke { moveTo(10f, 20f); lineTo(14f, 20f) }
    }
}
