package dev.archon.app.ui

import android.provider.Settings
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp

/** Halo v2 tokens — .creative/direction.md is the source of truth. */
object Archon {
    val Bg = Color(0xFF000000)
    val Surface = Color(0xFF111111)
    val Raised = Color(0xFF1C1C1C)
    val Track = Color(0xFF262626)
    val Edge = Color(0xFF6B6B6B)
    val Paper = Color(0xFFFFFFFF)
    val Muted = Color(0xFF9A9A9A)
    val Accent = Color(0xFFF6E7A1)
    val Ink = Color(0xFF000000)
    val Ok = Color(0xFF6FD39B)
    val Warn = Color(0xFFF2CF5B)
    val Error = Color(0xFFF07A7A)
}

/** Shared motion tokens (.creative/storyboards/motion.md). */
object Motion {
    const val UI = 220          // list/card appear, sheet
    const val ARC = 240         // remote value change, progress arc
    const val POP = 180         // code orb fill
    const val STAGGER = 35      // per item, capped
    const val STAGGER_CAP = 240
    const val ECHO_HOLD = 400   // after release, ignore PC echoes this long
    val Easing = androidx.compose.animation.core.CubicBezierEasing(0.2f, 0f, 0f, 1f)
}

/** Tabular figures for every number. */
val Tnum = TextStyle(fontFeatureSettings = "tnum")

object Type {
    val Caption = TextStyle(fontSize = 13.sp, fontWeight = FontWeight.Normal)
    val Body = TextStyle(fontSize = 16.sp, fontWeight = FontWeight.Medium)
    val Title = TextStyle(fontSize = 26.sp, fontWeight = FontWeight.SemiBold)
    val LargeTitle = TextStyle(fontSize = 34.sp, fontWeight = FontWeight.SemiBold)
    val CardPct = TextStyle(fontSize = 26.sp, fontWeight = FontWeight.SemiBold, fontFeatureSettings = "tnum")
    val Orb = TextStyle(fontSize = 24.sp, fontWeight = FontWeight.SemiBold, fontFeatureSettings = "tnum")
    val Counter = TextStyle(fontSize = 44.sp, fontWeight = FontWeight.SemiBold, fontFeatureSettings = "tnum")
    val Master = TextStyle(fontSize = 64.sp, fontWeight = FontWeight.SemiBold, letterSpacing = (-0.03).em,
        fontFeatureSettings = "tnum")
}

private val scheme = darkColorScheme(
    primary = Archon.Accent, onPrimary = Archon.Ink,
    secondary = Archon.Paper, onSecondary = Archon.Ink,
    background = Archon.Bg, onBackground = Archon.Paper,
    surface = Archon.Bg, onSurface = Archon.Paper,
    surfaceVariant = Archon.Surface, onSurfaceVariant = Archon.Muted,
    surfaceContainer = Archon.Surface, surfaceContainerHigh = Archon.Raised, surfaceContainerHighest = Archon.Raised,
    surfaceContainerLow = Archon.Surface, surfaceContainerLowest = Archon.Bg,
    outline = Archon.Edge, outlineVariant = Archon.Track,
    error = Archon.Error, onError = Archon.Ink,
)

@Composable
fun ArchonTheme(content: @Composable () -> Unit) =
    MaterialTheme(colorScheme = scheme, typography = Typography(), content = content)

/** sp that grows with the system font scale only up to [cap] (numerals inside fixed rings). */
@Composable
fun cappedSp(value: Float, cap: Float = 1.3f): androidx.compose.ui.unit.TextUnit {
    // Go through dp so Android 14's non-linear font scaling can't shrink it:
    // the result renders at exactly value × min(scale, cap) dp.
    val d = androidx.compose.ui.platform.LocalDensity.current
    return with(d) { (value * minOf(d.fontScale, cap)).dp.toSp() }
}

/** True when the user turned off animations (Android "Remove animations"). */
@Composable
fun reduceMotion(): Boolean {
    val ctx = LocalContext.current
    return remember {
        Settings.Global.getFloat(ctx.contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f
    }
}
