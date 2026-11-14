package dev.archon.app.ui

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import dev.archon.app.R
import dev.archon.app.core.NativeClient
import kotlinx.coroutines.delay
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin

/** Arcs span 270°, starting at 135° (bottom-left), clockwise. */
const val ARC_START = 135f
const val ARC_SWEEP = 270f

/**
 * Draws a volume ring: unlit track + lit arc (0..1) with round caps and an
 * optional white thumb at the end of the lit part.
 */
@Composable
fun VolumeArc(
    fraction: Float,
    stroke: Dp,
    modifier: Modifier = Modifier,
    color: Color = Archon.Accent,
    thumbRadius: Dp = 0.dp,
) {
    Canvas(modifier) {
        val w = stroke.toPx()
        val inset = maxOf(w / 2f, thumbRadius.toPx())
        val d = size.minDimension - inset * 2
        val topLeft = Offset((size.width - d) / 2f, (size.height - d) / 2f)
        val s = Size(d, d)
        drawArc(Archon.Track, ARC_START, ARC_SWEEP, false, topLeft, s, style = Stroke(w, cap = StrokeCap.Round))
        val f = fraction.coerceIn(0f, 1f)
        if (f > 0.002f) drawArc(color, ARC_START, ARC_SWEEP * f, false, topLeft, s, style = Stroke(w, cap = StrokeCap.Round))
        if (thumbRadius > 0.dp) {
            val a = Math.toRadians((ARC_START + ARC_SWEEP * f).toDouble())
            val c = Offset(size.width / 2f, size.height / 2f)
            val r = d / 2f
            drawCircle(Archon.Paper, thumbRadius.toPx(), Offset(c.x + r * cos(a).toFloat(), c.y + r * sin(a).toFloat()))
        }
    }
}

/**
 * Value shown by a volume control. The finger always wins: while dragging the
 * local value is shown; PC echoes are ignored until [Motion.ECHO_HOLD] ms after
 * release; remote changes otherwise animate with a 240 ms ease-out.
 */
class VolumeDrag {
    var dragging by mutableStateOf(false)
    var holding by mutableStateOf(false)
    var local by mutableFloatStateOf(0f)
    var lastTick by mutableIntStateOf(-1)
}

@Composable
fun rememberShownVolume(remote: Int, drag: VolumeDrag, reduce: Boolean): Float {
    LaunchedEffect(drag.holding) {
        if (drag.holding) {
            delay(Motion.ECHO_HOLD.toLong())
            drag.holding = false
        }
    }
    val target = if (drag.dragging || drag.holding) drag.local else remote.toFloat() / NativeClient.MAX_VOLUME
    val animated by animateFloatAsState(
        target, if (drag.dragging || reduce) tween(0) else tween(Motion.ARC, easing = Motion.Easing), label = "arc",
    )
    return if (drag.dragging) drag.local else animated
}

/** Applies a relative drag delta (pixels) to the local value and emits it. */
@Composable
fun volumeDragHandler(
    drag: VolumeDrag,
    remote: Int,
    pxForFullRange: Float,
    onChange: (Int) -> Unit,
): Triple<() -> Unit, (Float) -> Unit, () -> Unit> {
    val haptics = LocalHapticFeedback.current
    val start = {
        drag.local = remote.toFloat() / NativeClient.MAX_VOLUME
        drag.dragging = true
        drag.lastTick = (drag.local * 10).roundToInt()
    }
    val move = { delta: Float ->
        drag.local = (drag.local + delta / pxForFullRange).coerceIn(0f, 1f)
        onChange((drag.local * NativeClient.MAX_VOLUME).roundToInt())
        val tick = (drag.local * 10).toInt()
        if (tick != drag.lastTick) {
            drag.lastTick = tick
            haptics.performHapticFeedback(
                if (drag.local <= 0f || drag.local >= 1f) HapticFeedbackType.LongPress else HapticFeedbackType.TextHandleMove
            )
        }
    }
    val end = {
        drag.dragging = false
        drag.holding = true
    }
    return Triple(start, move, end)
}

/** 8 dp status dot. Always accompanied by text or a semantics label. */
@Composable
fun Dot(color: Color, alpha: Float = 1f, modifier: Modifier = Modifier) {
    Box(modifier.size(8.dp).background(color.copy(alpha = alpha), CircleShape))
}

@Composable
fun StatusLine(color: Color, text: String) {
    // Polite live region: TalkBack announces state changes (connecting, wrong code…).
    Row(
        Modifier.semantics { liveRegion = androidx.compose.ui.semantics.LiveRegionMode.Polite },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Dot(color)
        Spacer(Modifier.width(8.dp))
        Text(text, style = Type.Caption, color = Archon.Muted)
    }
}

/** Round mute button: 36 dp visual, 48 dp hit area (Android minimum). Muted = error fill + slashed speaker. */
@Composable
fun MuteButton(label: String, muted: Boolean, onToggle: () -> Unit, visual: Dp = 36.dp) {
    val desc = stringResource(if (muted) R.string.a11y_unmute else R.string.a11y_mute, label)
    Box(
        Modifier.size(48.dp).clickable(onClick = onToggle).semantics {
            contentDescription = desc
            role = Role.Button
        },
        contentAlignment = Alignment.Center,
    ) {
        Box(
            Modifier.size(visual).background(if (muted) Archon.Error else Archon.Raised, CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                if (muted) Glyphs.SpeakerOff else Glyphs.Speaker, contentDescription = null,
                tint = if (muted) Archon.Ink else Archon.Paper, modifier = Modifier.size(visual * 0.5f),
            )
        }
    }
}

@Composable
fun Fill(content: @Composable () -> Unit) = Box(Modifier.fillMaxSize()) { content() }
