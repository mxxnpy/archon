package dev.archon.app.ui

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.draw.scale
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import dev.archon.app.R
import dev.archon.app.core.CoreError
import dev.archon.app.core.LinkStatus
import dev.archon.app.core.NativeClient
import kotlinx.coroutines.delay
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin

@Composable
fun ArchonApp(vm: ArchonUi) {
    val screen by vm.screen.collectAsState()
    val reduce = reduceMotion()
    Box(Modifier.fillMaxSize().background(Archon.Bg).safeDrawingPadding()) {
        AnimatedContent(
            targetState = screen,
            contentKey = { it::class },
            transitionSpec = {
                val d = if (reduce) 0 else 300
                fadeIn(tween(d, easing = Motion.Easing)) togetherWith fadeOut(tween(d, easing = Motion.Easing))
            },
            label = "screen",
        ) { s ->
            when (s) {
                Screen.Computers -> ComputersScreen(vm)
                is Screen.Pairing -> PairingScreen(vm, s.target)
                is Screen.Mixer -> MixerScreen(vm, s.target)
            }
        }
    }
}

/** Fade + 8 dp rise once, staggered (cap 240 ms). Fade only under reduce motion. */
@Composable
private fun Modifier.appear(index: Int, reduce: Boolean): Modifier {
    val p = remember { Animatable(if (reduce) 1f else 0f) }
    LaunchedEffect(Unit) {
        if (reduce) return@LaunchedEffect
        delay(minOf(index * Motion.STAGGER, Motion.STAGGER_CAP).toLong())
        p.animateTo(1f, tween(Motion.UI, easing = Motion.Easing))
    }
    val rise = with(LocalDensity.current) { 8.dp.toPx() }
    return this.alpha(p.value).offset { IntOffset(0, ((1f - p.value) * rise).roundToInt()) }
}

// ---- 1. Computers: radar ------------------------------------------------------------

@Composable
private fun ComputersScreen(vm: ArchonUi) {
    val rows by vm.computers.collectAsState()
    val link by vm.link.collectAsState()
    val notice by vm.notice.collectAsState()
    val reduce = reduceMotion()
    var adding by remember { mutableStateOf(false) }

    LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(horizontal = 20.dp, vertical = 16.dp)) {
        item {
            Text(stringResource(R.string.computers_title), style = Type.LargeTitle, color = Archon.Paper,
                modifier = Modifier.padding(top = 8.dp).semantics { heading() })
            Spacer(Modifier.height(8.dp))
            Radar(rows, active = link.status != LinkStatus.CONNECTED, reduce, onPick = { vm.open(it.target) })
            Spacer(Modifier.height(8.dp))
            if (notice == Notice.PAIRING_REMOVED) {
                Row(
                    Modifier.fillMaxWidth().background(Archon.Surface, RoundedCornerShape(20.dp))
                        .clickable { vm.dismissNotice() }.padding(16.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Dot(Archon.Error)
                    Spacer(Modifier.width(12.dp))
                    Text(stringResource(R.string.notice_pairing_removed), style = Type.Caption, color = Archon.Paper)
                }
                Spacer(Modifier.height(8.dp))
            }
            if (rows.isEmpty()) {
                Text(stringResource(R.string.computers_empty_title), style = Type.Body, color = Archon.Paper,
                    textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth())
                Spacer(Modifier.height(4.dp))
                Text(stringResource(R.string.computers_empty_body), style = Type.Caption, color = Archon.Muted,
                    textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth())
                Spacer(Modifier.height(16.dp))
            }
        }
        itemsIndexed(rows, key = { _, r -> r.target.agentId ?: r.target.host }) { i, row ->
            PcRowView(row, Modifier.appear(i, reduce)) { vm.open(row.target) }
            Spacer(Modifier.height(8.dp))
        }
        item {
            TextButton(onClick = { adding = true }, modifier = Modifier.fillMaxWidth().heightIn(min = 48.dp)) {
                Text(stringResource(R.string.add_by_address), style = Type.Body, color = Archon.Accent)
            }
        }
    }
    if (adding) AddressDialog(onDismiss = { adding = false }, onConnect = { vm.openAddress(it) })
}

/** Stable angle per PC so points don't jump between visits. */
private fun angleFor(key: String): Double = Math.toRadians(((key.hashCode() and 0x7fffffff) % 360).toDouble())

@Composable
private fun Radar(rows: List<PcRow>, active: Boolean, reduce: Boolean, onPick: (PcRow) -> Unit) {
    // While no PC is connected the pulse keeps going as long as the radar is on
    // screen (Compose suspends it in the background). Reduce Motion stops it:
    // that is the user's way to pause moving content.
    val pulse = remember { Animatable(0f) }
    LaunchedEffect(active, reduce) {
        if (!active || reduce) {
            pulse.snapTo(0f)
            return@LaunchedEffect
        }
        while (true) {
            pulse.snapTo(0.001f)
            pulse.animateTo(1f, tween(2400, easing = LinearEasing))
        }
    }
    val desc = stringResource(R.string.a11y_radar, rows.size)
    BoxWithConstraints(Modifier.fillMaxWidth().height(300.dp).semantics { contentDescription = desc }) {
        val cx = maxWidth / 2
        val cy = 150.dp
        Canvas(Modifier.fillMaxSize()) {
            val c = Offset(size.width / 2f, 150.dp.toPx())
            drawCircle(Archon.Track, 70.dp.toPx(), c, style = Stroke(1.dp.toPx()))
            drawCircle(Archon.Track, 130.dp.toPx(), c, style = Stroke(1.dp.toPx()))
            if (pulse.value > 0f) {
                drawCircle(Archon.Accent.copy(alpha = 1f - pulse.value), 130.dp.toPx() * pulse.value, c,
                    style = Stroke(1.dp.toPx()))
            }
            // Phone glyph in the centre.
            val pw = 14.dp.toPx(); val ph = 24.dp.toPx()
            drawRoundRect(Archon.Paper, Offset(c.x - pw / 2, c.y - ph / 2), androidx.compose.ui.geometry.Size(pw, ph),
                androidx.compose.ui.geometry.CornerRadius(3.dp.toPx()), style = Stroke(1.5.dp.toPx(), cap = StrokeCap.Round))
        }
        // Each point: the dot sits exactly on its orbit and the name hangs toward
        // the centre, so labels never leave the screen.
        val points = rows.map { row ->
            val r = if (row.paired && row.online) 70.dp else 130.dp
            val a = angleFor(row.target.agentId ?: row.target.host)
            Triple(row, cx + r * cos(a).toFloat(), cy + r * sin(a).toFloat())
        }
        androidx.compose.ui.layout.Layout(
            modifier = Modifier.fillMaxSize(),
            content = {
                points.forEach { (row, x, _) ->
                    androidx.compose.runtime.key(row.target.agentId ?: row.target.host) {
                        val enter = remember { Animatable(if (reduce) 1f else 0f) }
                        LaunchedEffect(Unit) { if (!reduce) enter.animateTo(1f, tween(Motion.UI, easing = Motion.Easing)) }
                        // Outer orbit: toward the centre; inner orbit: outward (keeps the phone glyph clear).
                        val labelLeft = if (row.paired && row.online) x < cx else x > cx
                        Row(
                            Modifier.scale(0.6f + 0.4f * enter.value).alpha(enter.value).clickable { onPick(row) }
                                .clearAndSetSemantics { },
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            val name = @Composable {
                                // Decorative (rows below carry the names at full size): cap growth at 1.3×.
                                // Long names wrap to a second line toward the dot instead of
                                // running across the radar; beyond two lines they end with "…".
                                Text(row.target.name, style = Type.Caption.copy(fontSize = cappedSp(13f)), maxLines = 2, overflow = TextOverflow.Ellipsis,
                                    textAlign = if (labelLeft) TextAlign.End else TextAlign.Start,
                                    modifier = Modifier.widthIn(max = 112.dp),
                                    color = if (row.online) Archon.Paper else Archon.Muted)
                            }
                            if (labelLeft) { name(); Spacer(Modifier.width(6.dp)); Dot(statusColor(row)) }
                            else { Dot(statusColor(row)); Spacer(Modifier.width(6.dp)); name() }
                        }
                    }
                }
            },
        ) { measurables, constraints ->
            val maxLabel = (constraints.maxWidth * 0.45f).toInt()
            val placeables = measurables.map { it.measure(androidx.compose.ui.unit.Constraints(maxWidth = maxLabel)) }
            layout(constraints.maxWidth, constraints.maxHeight) {
                val half = 4.dp.roundToPx()
                val gap = 2.dp.roundToPx()
                val placed = mutableListOf<androidx.compose.ui.unit.IntRect>()
                placeables.forEachIndexed { i, p ->
                    val (_, x, y) = points[i]
                    val px = x.roundToPx()
                    val inner = points[i].first.let { it.paired && it.online }
                    val toLeft = if (inner) x < cx else x > cx
                    val left = (if (toLeft) px - p.width + half else px - half).coerceIn(0, constraints.maxWidth - p.width)
                    var top = y.roundToPx() - p.height / 2
                    // Greedy de-overlap: push a label below any label it would cover.
                    var moved = true
                    while (moved) {
                        moved = false
                        val r = androidx.compose.ui.unit.IntRect(left, top, left + p.width, top + p.height)
                        placed.firstOrNull { it.overlaps(r) }?.let { top = it.bottom + gap; moved = true }
                    }
                    top = top.coerceIn(0, constraints.maxHeight - p.height)
                    placed += androidx.compose.ui.unit.IntRect(left, top, left + p.width, top + p.height)
                    p.place(left, top)
                }
            }
        }
    }
}

private fun statusColor(row: PcRow) = if (row.online) Archon.Ok else Archon.Error

@Composable
private fun PcRowView(row: PcRow, modifier: Modifier, onClick: () -> Unit) {
    val state = stringResource(
        when { !row.paired -> R.string.pc_new; row.online -> R.string.pc_paired_online; else -> R.string.pc_paired_offline }
    )
    Row(
        modifier.fillMaxWidth().heightIn(min = 64.dp).background(Archon.Surface, RoundedCornerShape(20.dp))
            .clickable(onClick = onClick).padding(horizontal = 16.dp)
            .semantics { contentDescription = "${row.target.name}, $state" },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Dot(statusColor(row))
        Spacer(Modifier.width(14.dp))
        // State in words too (colour is never the only cue).
        Column(Modifier.weight(1f).padding(vertical = 10.dp)) {
            Text(row.target.name, style = Type.Body, color = if (row.online) Archon.Paper else Archon.Muted,
                maxLines = 2, overflow = TextOverflow.Ellipsis)
            Text(stringResource(when { !row.paired -> R.string.pc_state_new; row.online -> R.string.pc_state_online
                else -> R.string.pc_state_offline }), style = Type.Caption, color = Archon.Muted)
        }
        if (!row.paired) {
            Box(
                Modifier.background(Archon.Accent, CircleShape).padding(horizontal = 16.dp, vertical = 8.dp),
            ) { Text(stringResource(R.string.pair_button), style = Type.Caption, color = Archon.Ink) }
        } else {
            Icon(Glyphs.Chevron, contentDescription = null, tint = Archon.Muted, modifier = Modifier.size(18.dp).rotate(-90f))
        }
    }
}

@Composable
private fun AddressDialog(onDismiss: () -> Unit, onConnect: (String) -> Boolean) {
    var text by remember { mutableStateOf("") }
    var invalid by remember { mutableStateOf(false) }
    AlertDialog(
        onDismissRequest = onDismiss,
        containerColor = Archon.Surface,
        shape = RoundedCornerShape(24.dp),
        title = { Text(stringResource(R.string.add_by_address), style = Type.Body) },
        text = {
            Column {
                Text(stringResource(R.string.address_help), style = Type.Caption, color = Archon.Muted)
                Spacer(Modifier.height(12.dp))
                OutlinedTextField(
                    value = text, onValueChange = { text = it; invalid = false }, singleLine = true,
                    placeholder = { Text("192.168.0.10", color = Archon.Muted) }, isError = invalid,
                    supportingText = if (invalid) ({ Text(stringResource(R.string.address_invalid)) }) else null,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                    shape = RoundedCornerShape(12.dp),
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = Archon.Accent, unfocusedBorderColor = Archon.Edge, cursorColor = Archon.Accent,
                    ),
                )
            }
        },
        confirmButton = {
            TextButton(onClick = { if (onConnect(text)) onDismiss() else invalid = true }) {
                Text(stringResource(R.string.connect), color = Archon.Accent)
            }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel), color = Archon.Paper) } },
    )
}

// ---- 2. Pairing: code ring --------------------------------------------------------------

@Composable
private fun PairingScreen(vm: ArchonUi, target: Target) {
    val link by vm.link.collectAsState()
    val reduce = reduceMotion()
    var code by remember(target) { mutableStateOf("") }
    var wrongFlash by remember { mutableStateOf(false) }
    val focus = remember { FocusRequester() }
    BackHandler { vm.back() }

    val failed = link.status == LinkStatus.FAILED
    LaunchedEffect(link.status, link.error) {
        if (failed && link.error == CoreError.WRONG_CODE) {
            wrongFlash = true
            delay(600)
            wrongFlash = false
        }
        if (failed) code = ""
        if (link.status == LinkStatus.AWAITING_CODE) runCatching { focus.requestFocus() }
    }

    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).imePadding().padding(horizontal = 20.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        TopBar(onBack = { vm.back() })
        Text(stringResource(R.string.code_title), style = Type.Title, color = Archon.Paper,
            modifier = Modifier.semantics { heading() })
        Text(target.name, style = Type.Caption, color = Archon.Muted)
        Spacer(Modifier.height(28.dp))

        val canType = link.status == LinkStatus.AWAITING_CODE
        val a11y = stringResource(R.string.a11y_code_field)
        Box(contentAlignment = Alignment.Center) {
            BoxWithConstraints { // ring ≤ 310 dp, smaller on narrow phones (orbs scale with it)
                CodeRing(code, link.status, wrongFlash, reduce, ringSize = minOf(310.dp, maxWidth),
                    onTap = { runCatching { focus.requestFocus() } })
            }
            // Invisible input that drives the ring (system number pad, OTP paste works).
            BasicTextField(
                value = code,
                onValueChange = { v ->
                    val digits = v.filter { it.isDigit() }.take(6)
                    code = digits
                    if (digits.length == 6) vm.submitCode(digits)
                },
                enabled = canType,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword),
                modifier = Modifier.size(1.dp).alpha(0f).focusRequester(focus).semantics { contentDescription = a11y },
            )
        }
        Spacer(Modifier.height(28.dp))
        PairingStatus(link)
        Spacer(Modifier.height(32.dp))
        if (failed && link.error != CoreError.PAIRING_REJECTED) {
            Box(
                Modifier.fillMaxWidth().heightIn(min = 52.dp).background(Archon.Accent, CircleShape)
                    .clickable { vm.retryPairing() },
                contentAlignment = Alignment.Center,
            ) { Text(stringResource(R.string.try_again), style = Type.Body, color = Archon.Ink) }
            Spacer(Modifier.height(20.dp))
        }
    }
}

@Composable
private fun CodeRing(
    code: String, status: LinkStatus, wrong: Boolean, reduce: Boolean,
    ringSize: androidx.compose.ui.unit.Dp = 310.dp, onTap: () -> Unit,
) {
    val orb = ringSize * (52f / 310f)
    val n = code.length
    val progress by animateFloatAsState(n / 6f, if (reduce) tween(0) else tween(Motion.ARC, easing = Motion.Easing), label = "p")
    // Verifying: the progress arc turns slowly (stops by itself after 5 s).
    val spin = remember { Animatable(0f) }
    LaunchedEffect(status, reduce) {
        if (status == LinkStatus.VERIFYING && !reduce) spin.animateTo(360f, tween(5000, easing = LinearEasing))
        else spin.snapTo(0f)
    }
    // Wrong code: a short ±8° shake.
    val shake = remember { Animatable(0f) }
    LaunchedEffect(wrong) {
        if (wrong && !reduce) for (t in listOf(8f, -8f, 5f, -3f, 0f)) shake.animateTo(t, tween(70))
    }
    Box(
        Modifier.size(ringSize).rotate(shake.value).clickable(onClick = onTap,
            interactionSource = remember { androidx.compose.foundation.interaction.MutableInteractionSource() },
            indication = null),
        contentAlignment = Alignment.Center,
    ) {
        Canvas(Modifier.size(ringSize - orb)) {
            val w = 3.dp.toPx()
            drawCircle(Archon.Track, size.minDimension / 2f, style = Stroke(w))
            if (progress > 0f) drawArc(
                if (wrong) Archon.Error else Archon.Accent, -90f + spin.value, 360f * progress, false,
                style = Stroke(w, cap = StrokeCap.Round),
            )
        }
        for (i in 0 until 6) {
            val a = Math.toRadians(-90.0 + 60.0 * i)
            val r = (ringSize - orb).value / 2f
            val filled = i < n
            val current = i == n && status == LinkStatus.AWAITING_CODE
            val pop by animateFloatAsState(if (filled) 1f else 0.85f,
                if (reduce) tween(0) else tween(Motion.POP, easing = Motion.Easing), label = "orb")
            Box(
                Modifier.offset((r * cos(a)).dp, (r * sin(a)).dp).size(orb).scale(if (filled) pop else 1f)
                    .background(
                        when { wrong -> Archon.Error; filled -> Archon.Accent; else -> Archon.Raised }, CircleShape,
                    )
                    .then(if (current) Modifier.border(1.5.dp, Archon.Accent, CircleShape) else Modifier),
                contentAlignment = Alignment.Center,
            ) {
                if (filled) Text(code[i].toString(), style = Type.Orb.copy(fontSize = cappedSp(24f)), color = Archon.Ink)
                else if (current) Box(Modifier.width(2.dp).height(22.dp).background(Archon.Accent))
            }
        }
        Text("$n/6", style = Type.Counter.copy(fontSize = cappedSp(44f)), color = Archon.Paper)
    }
}

@Composable
private fun PairingStatus(link: LinkState) {
    val (color, text) = when (link.status) {
        LinkStatus.IDLE, LinkStatus.CONNECTING, LinkStatus.RECONNECTING -> Archon.Warn to R.string.status_connecting
        LinkStatus.AWAITING_CODE ->
            if (link.error == CoreError.WRONG_CODE) Archon.Error to R.string.err_code_format
            else Archon.Warn to R.string.status_awaiting_code
        LinkStatus.VERIFYING -> Archon.Warn to R.string.status_verifying
        LinkStatus.CONNECTED -> Archon.Ok to R.string.status_connected
        LinkStatus.FAILED -> Archon.Error to errorText(link.error)
    }
    StatusLine(color, stringResource(text))
}

fun errorText(e: Int): Int = when (e) {
    CoreError.WRONG_CODE -> R.string.err_wrong_code
    CoreError.PAIRING_LOCKED -> R.string.err_locked
    CoreError.PAIRING_REJECTED -> R.string.err_rejected
    CoreError.BUSY -> R.string.err_busy
    CoreError.TIMEOUT -> R.string.err_timeout
    CoreError.UNSUPPORTED_VERSION -> R.string.err_version
    CoreError.CONNECT_FAILED -> R.string.err_connect
    CoreError.REVOKED -> R.string.notice_pairing_removed
    CoreError.NOT_RECOGNIZED, CoreError.UNKNOWN_CONTROLLER, CoreError.AUTH_FAILED -> R.string.err_not_recognized
    CoreError.WRONG_AGENT -> R.string.err_wrong_agent
    else -> R.string.err_lost
}

// ---- 3. Mixer: master arc + app cards -----------------------------------------------------

@Composable
private fun MixerScreen(vm: ArchonUi, target: Target) {
    val link by vm.link.collectAsState()
    val icons by vm.icons.collectAsState()
    val levels by vm.levels.collectAsState()
    val selected by vm.selectedEndpoint.collectAsState()
    val reduce = reduceMotion()
    var menu by remember { mutableStateOf(false) }
    var devices by remember { mutableStateOf(false) }
    var confirmForget by remember { mutableStateOf(false) }
    BackHandler { vm.back() }

    // Live levels only while the mixer is on screen (and never under reduce motion).
    LaunchedEffect(reduce) { vm.setMetersVisible(!reduce) }
    androidx.compose.runtime.DisposableEffect(Unit) { onDispose { vm.setMetersVisible(false) } }

    val endpoint = link.endpoints[selected] ?: link.endpoints.values.firstOrNull { it.isDefault }
        ?: link.endpoints.values.minByOrNull { it.name }
    val sources = link.sources.values.filter { it.endpointId == endpoint?.id }
        .sortedWith(compareBy<Source> { it.system }.thenByDescending { it.playing }.thenBy { it.name.lowercase() })

    Column(Modifier.fillMaxSize()) {
        // Top bar: back · device ▾ · connection dot · more
        Row(Modifier.fillMaxWidth().padding(horizontal = 4.dp, vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = { vm.back() }) {
                Icon(Glyphs.Back, contentDescription = stringResource(R.string.a11y_back), tint = Archon.Paper)
            }
            Box(Modifier.weight(1f), contentAlignment = Alignment.Center) {
                Row(
                    Modifier.heightIn(min = 48.dp).background(Archon.Surface, CircleShape)  // 48 dp touch target
                        .clickable(enabled = link.endpoints.size > 1) { devices = true }
                        .padding(horizontal = 14.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(endpoint?.name ?: target.name, style = Type.Caption, color = Archon.Paper,
                        maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                    if (link.endpoints.size > 1) {
                        Spacer(Modifier.width(4.dp))
                        Icon(Glyphs.Chevron, contentDescription = stringResource(R.string.a11y_choose_device),
                            tint = Archon.Muted, modifier = Modifier.size(14.dp))
                    }
                }
                DropdownMenu(expanded = devices, onDismissRequest = { devices = false }, containerColor = Archon.Surface,
                    shape = RoundedCornerShape(20.dp)) {
                    link.endpoints.values.sortedWith(compareByDescending<Endpoint> { it.isDefault }.thenBy { it.name.lowercase() })
                        .forEach { e ->
                            DropdownMenuItem(
                                text = {
                                    Text(e.name + if (e.isDefault) "  ·  " + stringResource(R.string.default_device) else "",
                                        style = Type.Caption, color = if (e.id == endpoint?.id) Archon.Accent else Archon.Paper)
                                },
                                onClick = { vm.selectEndpoint(e.id); devices = false },
                            )
                        }
                }
            }
            val (dotColor, dotText) = linkDot(link.status)
            val dotLabel = stringResource(dotText)
            Box(
                Modifier.size(48.dp).semantics {
                    contentDescription = dotLabel
                    liveRegion = androidx.compose.ui.semantics.LiveRegionMode.Polite  // TalkBack: "Connected" again
                },
                contentAlignment = Alignment.Center,
            ) { Dot(dotColor) }
            Box {
                IconButton(onClick = { menu = true }) {
                    Icon(Glyphs.More, contentDescription = stringResource(R.string.a11y_more), tint = Archon.Paper)
                }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }, containerColor = Archon.Surface) {
                    DropdownMenuItem(text = { Text(stringResource(R.string.forget_pc)) }, onClick = {
                        menu = false; confirmForget = true
                    })
                }
            }
        }

        if (link.status == LinkStatus.FAILED || !link.synced) {
            Column(Modifier.fillMaxSize().padding(24.dp), horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.Center) {
                VolumeArc(0f, 8.dp, Modifier.size(180.dp), color = Archon.Edge)
                Spacer(Modifier.height(20.dp))
                if (link.status == LinkStatus.FAILED) {
                    Text(stringResource(errorText(link.error)), style = Type.Body, color = Archon.Paper, textAlign = TextAlign.Center)
                    Spacer(Modifier.height(20.dp))
                    Box(
                        Modifier.heightIn(min = 48.dp).background(Archon.Accent, CircleShape).clickable { vm.reconnect() }
                            .padding(horizontal = 28.dp, vertical = 14.dp),
                    ) { Text(stringResource(R.string.try_again), style = Type.Body, color = Archon.Ink) }
                    if (link.error in setOf(CoreError.NOT_RECOGNIZED, CoreError.UNKNOWN_CONTROLLER, CoreError.AUTH_FAILED)) {
                        // Only the user decides to drop the old pairing (the refusal is unverified).
                        TextButton(onClick = { vm.repair(target) }, modifier = Modifier.heightIn(min = 48.dp)) {
                            Text(stringResource(R.string.pair_again), style = Type.Body, color = Archon.Accent)
                        }
                    }
                } else {
                    StatusLine(Archon.Warn, stringResource(R.string.status_connecting))
                }
            }
            return@Column
        }

        val live = link.status == LinkStatus.CONNECTED
        val reconnectingText = stringResource(R.string.status_reconnecting)
        Box(Modifier.fillMaxSize()) {
        LazyVerticalGrid(
            columns = GridCells.Fixed(2),
            // Extra bottom room while reconnecting so the status pill never covers a card.
            contentPadding = PaddingValues(start = 16.dp, end = 16.dp, bottom = if (live) 24.dp else 96.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
            modifier = Modifier.fillMaxSize().alpha(if (live) 1f else 0.5f)
                .then(if (live) Modifier else Modifier.clearAndSetSemantics { contentDescription = reconnectingText }),
            userScrollEnabled = live,
        ) {
            if (endpoint != null) item(span = { GridItemSpan(2) }, key = "master") {
                MasterArc(endpoint, reduce, vm)
            }
            itemsIndexed(sources, key = { _, s -> s.id }) { i, s ->
                AppCard(s, icons[s.iconKey], levels[s.meter] ?: 0f, reduce, vm, Modifier.appear(i, reduce))
            }
            if (sources.isEmpty()) item(span = { GridItemSpan(2) }) {
                Text(stringResource(R.string.no_sources), style = Type.Caption, color = Archon.Muted,
                    textAlign = TextAlign.Center, modifier = Modifier.padding(24.dp))
            }
        }
        if (!live) {
            // Link lost but last state known: freeze the mixer (no input) and say why.
            Box(
                Modifier.fillMaxSize().pointerInput(Unit) {
                    awaitPointerEventScope { while (true) awaitPointerEvent().changes.forEach { it.consume() } }
                },
                contentAlignment = Alignment.BottomCenter,  // never covers the master arc
            ) {
                Box(Modifier.padding(bottom = 28.dp).background(Archon.Raised, CircleShape).padding(horizontal = 16.dp, vertical = 10.dp)) {
                    StatusLine(Archon.Warn, stringResource(R.string.status_reconnecting))
                }
            }
        }
        }
    }

    if (confirmForget) AlertDialog(
        onDismissRequest = { confirmForget = false },
        containerColor = Archon.Surface,
        shape = RoundedCornerShape(24.dp),
        title = { Text(stringResource(R.string.forget_pc), style = Type.Body) },
        text = { Text(stringResource(R.string.forget_pc_body, target.name), style = Type.Caption, color = Archon.Muted) },
        confirmButton = {
            TextButton(onClick = { confirmForget = false; vm.forget(target) }) {
                Text(stringResource(R.string.forget), color = Archon.Error)
            }
        },
        dismissButton = {
            TextButton(onClick = { confirmForget = false }) { Text(stringResource(R.string.cancel), color = Archon.Paper) }
        },
    )
}

private fun linkDot(s: LinkStatus) = when (s) {
    LinkStatus.CONNECTED -> Archon.Ok to R.string.status_connected
    LinkStatus.FAILED, LinkStatus.IDLE -> Archon.Error to R.string.status_disconnected
    else -> Archon.Warn to R.string.status_reconnecting
}

@Composable
private fun MasterArc(e: Endpoint, reduce: Boolean, vm: ArchonUi) {
    val drag = remember(e.id) { VolumeDrag() }
    val shown = rememberShownVolume(e.volume, drag, reduce)
    val label = stringResource(R.string.master_volume)
    val pct = (shown * 100).roundToInt()
    val px = with(LocalDensity.current) { 260.dp.toPx() }
    val (start0, move0, end0) = volumeDragHandler(drag, e.volume, px) { vm.setVolume(true, e.id, it) }
    val start by androidx.compose.runtime.rememberUpdatedState(start0)
    val move by androidx.compose.runtime.rememberUpdatedState(move0)
    val end by androidx.compose.runtime.rememberUpdatedState(end0)
    Column(Modifier.fillMaxWidth().padding(top = 8.dp, bottom = 8.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        Box(
            Modifier.size(220.dp)
                .pointerInput(e.id) {
                    detectDragGestures(onDragStart = { start() }, onDragEnd = { end() }, onDragCancel = { end() }) { change, d ->
                        change.consume()
                        move(d.x - d.y)  // right or up = louder
                    }
                }
                .semantics {
                    contentDescription = label
                    stateDescription = "$pct%"
                    progressBarRangeInfo = ProgressBarRangeInfo(shown, 0f..1f)
                    setProgress { v -> vm.setVolume(true, e.id, (v * NativeClient.MAX_VOLUME).roundToInt()); true }
                },
            contentAlignment = Alignment.Center,
        ) {
            VolumeArc(shown, 8.dp, Modifier.fillMaxSize(), color = if (e.muted) Archon.Edge else Archon.Accent,
                thumbRadius = 9.dp)
            Row(verticalAlignment = Alignment.Bottom) {
                Text("$pct", style = Type.Master.copy(fontSize = cappedSp(64f)), color = if (e.muted) Archon.Muted else Archon.Paper)
                Text("%", style = Type.Body, color = Archon.Muted, modifier = Modifier.padding(bottom = 14.dp))
            }
        }
        MuteButton(label, e.muted, { vm.setMute(true, e.id, !e.muted) }, visual = 40.dp)
    }
}

@Composable
private fun AppCard(s: Source, icon: ImageBitmap?, level: Float, reduce: Boolean, vm: ArchonUi, modifier: Modifier) {
    val name = if (s.system) stringResource(R.string.system_sounds) else s.name
    val drag = remember(s.id) { VolumeDrag() }
    val shown = rememberShownVolume(s.volume, drag, reduce)
    val pct = (shown * 100).roundToInt()
    val active = drag.dragging || drag.holding
    val px = with(LocalDensity.current) { 220.dp.toPx() }
    val (start0, move0, end0) = volumeDragHandler(drag, s.volume, px) { vm.setVolume(false, s.id, it) }
    val start by androidx.compose.runtime.rememberUpdatedState(start0)
    val move by androidx.compose.runtime.rememberUpdatedState(move0)
    val end by androidx.compose.runtime.rememberUpdatedState(end0)
    val strokeW by animateFloatAsState(if (active) 6f else 4f, tween(if (reduce) 0 else 120), label = "w")
    // Playing dot opacity follows the live peak, smoothed into 55–100 %.
    val dotAlpha by animateFloatAsState(if (reduce) 1f else 0.55f + 0.45f * level.coerceIn(0f, 1f), tween(50), label = "lvl")
    val playing = stringResource(R.string.a11y_playing)
    val muted = stringResource(R.string.a11y_muted)

    Box(
        modifier.fillMaxWidth().background(Archon.Surface, RoundedCornerShape(24.dp))
            .pointerInput(s.id) {
                detectHorizontalDragGestures(onDragStart = { start() }, onDragEnd = { end() }, onDragCancel = { end() }) { change, dx ->
                    change.consume()
                    move(dx)
                }
            }
            .semantics {
                contentDescription = name + (if (s.playing) ", $playing" else "") + (if (s.muted) ", $muted" else "")
                stateDescription = "$pct%"
                progressBarRangeInfo = ProgressBarRangeInfo(shown, 0f..1f)
                setProgress { v -> vm.setVolume(false, s.id, (v * NativeClient.MAX_VOLUME).roundToInt()); true }
            },
    ) {
        BoxWithConstraints(Modifier.fillMaxWidth()) {
        // Keep 44 dp free on each side for the mute button: 96 dp on phones ≥ 390 dp, smaller below.
        val ring = (maxWidth - 88.dp).coerceIn(72.dp, 96.dp)
        val tile = ring / 2
        Column(Modifier.fillMaxWidth().padding(top = 16.dp, bottom = 18.dp), horizontalAlignment = Alignment.CenterHorizontally) {
            Box(Modifier.size(ring), contentAlignment = Alignment.Center) {
                VolumeArc(shown, strokeW.dp, Modifier.fillMaxSize(), color = if (s.muted) Archon.Edge else Archon.Accent,
                    thumbRadius = if (active) 7.dp else 0.dp)
                if (active) {
                    Text("$pct", style = Type.CardPct.copy(fontSize = cappedSp(26f)), color = Archon.Paper)
                } else {
                    Box(
                        Modifier.size(tile).background(Archon.Raised, RoundedCornerShape(14.dp)),
                        contentAlignment = Alignment.Center,
                    ) {
                        if (icon != null) Image(icon, contentDescription = null,
                            modifier = Modifier.size(tile * 0.66f).alpha(if (s.muted) 0.45f else 1f))
                        else Text(name.take(1).uppercase(), style = Type.Body, color = Archon.Muted)
                    }
                }
            }
            Spacer(Modifier.height(10.dp))
            Row(Modifier.padding(horizontal = 14.dp), verticalAlignment = Alignment.CenterVertically) {
                if (s.playing && !s.muted) {
                    Dot(Archon.Ok, alpha = dotAlpha)
                    Spacer(Modifier.width(6.dp))
                }
                Text(name, style = Type.Caption, color = if (s.muted) Archon.Muted else Archon.Paper,
                    maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
        }
        Box(Modifier.align(Alignment.TopEnd).padding(4.dp)) {
            MuteButton(name, s.muted, { vm.setMute(false, s.id, !s.muted) })
        }
    }
}

// ---- shared ----------------------------------------------------------------------------

@Composable
private fun TopBar(onBack: () -> Unit) {
    Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        IconButton(onClick = onBack) {
            Icon(Glyphs.Back, contentDescription = stringResource(R.string.a11y_back), tint = Archon.Paper)
        }
    }
}
