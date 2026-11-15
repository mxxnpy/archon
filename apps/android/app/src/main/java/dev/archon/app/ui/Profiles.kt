package dev.archon.app.ui

import android.app.TimePickerDialog
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import dev.archon.app.R
import dev.archon.app.core.LinkStatus

/** 1.5: a profile as the phone sees it (the settings themselves stay on the PC). */
data class ProfileInfo(
    val id: String, val name: String, val active: Boolean = false, val automatic: Boolean = false,
    val restore: Boolean = false, val days: Int = 0, val startMin: Int = 0, val endMin: Int = 0,
    val network: String = "", val outputId: String = "", val outputName: String = "",
    val nEndpoints: Int = 0, val nApps: Int = 0,
) {
    val isNew: Boolean get() = id.startsWith("new:")
    val hasTime: Boolean get() = days != 0 || startMin != endMin

    companion object {
        fun new(name: String) = ProfileInfo(id = "new:" + java.util.UUID.randomUUID(), name = name)
        fun clock(m: Int) = "%02d:%02d".format(m / 60, m % 60)
    }
}

@Composable
fun profileSummary(p: ProfileInfo): String {
    if (!p.automatic) return stringResource(R.string.profile_manual)
    val parts = mutableListOf<String>()
    if (p.hasTime) {
        val letters = listOf(R.string.day_sun, R.string.day_mon, R.string.day_tue, R.string.day_wed, R.string.day_thu,
            R.string.day_fri, R.string.day_sat).map { stringResource(it) }
        val d = when (p.days) {
            0 -> stringResource(R.string.every_day)
            0b0111110 -> stringResource(R.string.weekdays)
            0b1000001 -> stringResource(R.string.weekends)
            else -> (0..6).filter { p.days and (1 shl it) != 0 }.joinToString(", ") { letters[it] }
        }
        parts += if (p.startMin == p.endMin) d else "$d ${ProfileInfo.clock(p.startMin)}–${ProfileInfo.clock(p.endMin)}"
    }
    if (p.network.isNotEmpty()) parts += stringResource(R.string.profile_on_network, p.network)
    if (p.outputId.isNotEmpty()) parts += stringResource(R.string.profile_with_output, p.outputName)
    return parts.joinToString(" · ")
}

@Composable
fun ProfilesScreen(vm: ArchonUi) {
    val link by vm.link.collectAsState()
    var editing by remember { mutableStateOf<ProfileInfo?>(null) }
    BackHandler { vm.setShowProfiles(false) }
    val live = link.status == LinkStatus.CONNECTED
    Column(Modifier.fillMaxSize().background(Archon.Bg)) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 4.dp, vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = { vm.setShowProfiles(false) }) {
                Icon(Glyphs.Back, contentDescription = stringResource(R.string.a11y_back), tint = Archon.Paper)
            }
            Text(stringResource(R.string.profiles), style = Type.Body, color = Archon.Paper,
                modifier = Modifier.semantics { heading() })
        }
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text(if (link.pcNetwork.isEmpty()) stringResource(R.string.pc_no_network)
                else stringResource(R.string.pc_network, link.pcNetwork),
                style = Type.Caption, color = Archon.Muted, modifier = Modifier.padding(horizontal = 4.dp))
            val list = link.profiles.values.sortedWith(compareByDescending<ProfileInfo> { it.active }.thenBy { it.name.lowercase() })
            if (list.isEmpty()) Text(stringResource(R.string.profiles_empty), style = Type.Caption, color = Archon.Muted,
                textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth().padding(24.dp))
            list.forEach { p -> ProfileCard(p, live, onApply = { vm.applyProfile(p.id) }, onEdit = { editing = p }) }
            val newName = stringResource(R.string.profile_n, link.profiles.size + 1)
            TextButton(
                onClick = { editing = ProfileInfo.new(newName) },
                enabled = live && link.profiles.size < 32,
                modifier = Modifier.fillMaxWidth().heightIn(min = 50.dp)
                    .background(if (live) Archon.Accent else Archon.Raised, CircleShape),
            ) { Text(stringResource(R.string.save_current_as_profile), style = Type.Body, color = Archon.Ink) }
            Spacer(Modifier.size(24.dp))
        }
    }
    editing?.let { p -> ProfileEditor(p, link, vm, onDone = { editing = null }) }
}

@Composable
private fun ProfileCard(p: ProfileInfo, live: Boolean, onApply: () -> Unit, onEdit: () -> Unit) {
    val inUse = stringResource(R.string.in_use)
    val summary = profileSummary(p)
    Row(
        Modifier.fillMaxWidth().background(Archon.Surface, RoundedCornerShape(24.dp)).padding(16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f).semantics(mergeDescendants = true) {
            if (p.active) stateDescription = inUse
        }) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (p.active) {
                    Dot(Archon.Accent)
                    Spacer(Modifier.width(6.dp))
                }
                Text(p.name, style = Type.Body, color = Archon.Paper, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
            Text(summary, style = Type.Caption, color = Archon.Muted, maxLines = 3, overflow = TextOverflow.Ellipsis)
        }
        Spacer(Modifier.width(12.dp))
        val applyLabel = stringResource(R.string.apply_profile, p.name)
        if (p.active) {
            // Already in force: a label, not another yellow button (one accent action per screen).
            Text(stringResource(R.string.in_use_label), style = Type.Caption, color = Archon.Accent,
                modifier = Modifier.padding(horizontal = 12.dp))
        } else {
            TextButton(onClick = onApply, enabled = live,
                modifier = Modifier.heightIn(min = 44.dp).background(Archon.Track, CircleShape)
                    .semantics { contentDescription = applyLabel }) {
                Text(stringResource(R.string.apply), style = Type.Caption, color = Archon.Paper)
            }
        }
        Spacer(Modifier.width(8.dp))
        val editLabel = stringResource(R.string.edit_profile_named, p.name)
        Box(Modifier.size(44.dp).background(Archon.Raised, CircleShape)
            .clickable(role = Role.Button, onClickLabel = editLabel, onClick = onEdit)
            .semantics { contentDescription = editLabel }, contentAlignment = Alignment.Center) {
            Text("⋯", style = Type.Body, color = Archon.Paper)
        }
    }
}

@Composable
private fun ProfileEditor(start: ProfileInfo, link: LinkState, vm: ArchonUi, onDone: () -> Unit) {
    var p by remember(start.id) { mutableStateOf(start) }
    var useTime by remember(start.id) { mutableStateOf(start.hasTime) }
    var from by remember(start.id) { mutableIntStateOf(if (start.startMin == start.endMin) 9 * 60 else start.startMin) }
    var until by remember(start.id) { mutableIntStateOf(if (start.startMin == start.endMin) 18 * 60 else start.endMin) }
    var useNetwork by remember(start.id) { mutableStateOf(start.network.isNotEmpty()) }
    var outputs by remember { mutableStateOf(false) }
    var confirmDelete by remember { mutableStateOf(false) }
    val ctx = LocalContext.current
    val networkName = p.network.ifEmpty { link.pcNetwork }
    val live = link.status == LinkStatus.CONNECTED
    val dayNames = listOf(R.string.day_sun_long, R.string.day_mon_long, R.string.day_tue_long, R.string.day_wed_long,
        R.string.day_thu_long, R.string.day_fri_long, R.string.day_sat_long).map { stringResource(it) }
    val letters = stringResource(R.string.day_letters)

    fun save(capture: Boolean) {
        val out = p.copy(
            name = p.name.trim(),
            days = if (p.automatic && useTime) p.days else 0,
            startMin = if (p.automatic && useTime) from else 0,
            endMin = if (p.automatic && useTime) until else 0,
            network = if (p.automatic && useNetwork) networkName else "",
            outputId = if (p.automatic) p.outputId else "",
            outputName = if (p.automatic) (link.endpoints[p.outputId]?.name ?: p.outputName) else "",
        )
        vm.saveProfile(out, capture)
        onDone()
    }

    AlertDialog(
        onDismissRequest = onDone,
        containerColor = Archon.Surface,
        shape = RoundedCornerShape(24.dp),
        title = { Text(stringResource(if (p.isNew) R.string.new_profile else R.string.edit_profile), style = Type.Body) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedTextField(
                    value = p.name, onValueChange = { p = p.copy(name = it.take(64)) }, singleLine = true,
                    label = { Text(stringResource(R.string.name)) },
                    keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Sentences),
                    colors = OutlinedTextFieldDefaults.colors(focusedBorderColor = Archon.Accent, cursorColor = Archon.Accent,
                        focusedLabelColor = Archon.Accent),
                    modifier = Modifier.fillMaxWidth(),
                )
                if (!p.isNew) Text(stringResource(R.string.profile_holds, p.nEndpoints, p.nApps), style = Type.Caption,
                    color = Archon.Muted)
                SwitchRow(stringResource(R.string.apply_by_itself), p.automatic) { p = p.copy(automatic = it) }
                if (p.automatic) {
                    Text(stringResource(R.string.when_all_conditions), style = Type.Caption, color = Archon.Muted)
                    SwitchRow(stringResource(R.string.days_and_hours), useTime) { useTime = it }
                    if (useTime) {
                        Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                            for (d in 0..6) {
                                val on = p.days and (1 shl d) != 0
                                Box(
                                    Modifier.size(36.dp).background(if (on) Archon.Accent else Archon.Raised, CircleShape)
                                        .clickable(role = Role.Checkbox) { p = p.copy(days = p.days xor (1 shl d)) }
                                        .semantics { contentDescription = dayNames[d]; selected = on },
                                    contentAlignment = Alignment.Center,
                                ) { Text(letters[d].toString(), style = Type.Caption, color = if (on) Archon.Ink else Archon.Paper) }
                            }
                        }
                        Text(stringResource(if (p.days == 0) R.string.every_day_long else R.string.only_marked_days),
                            style = Type.Caption, color = Archon.Muted)
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            TimeChip(stringResource(R.string.time_from), from) {
                                TimePickerDialog(ctx, { _, h, m -> from = h * 60 + m }, from / 60, from % 60, true).show()
                            }
                            TimeChip(stringResource(R.string.time_until), until) {
                                TimePickerDialog(ctx, { _, h, m -> until = h * 60 + m }, until / 60, until % 60, true).show()
                            }
                        }
                    }
                    SwitchRow(stringResource(R.string.only_this_network),
                        useNetwork, enabled = networkName.isNotEmpty() || useNetwork,
                        detail = networkName.ifEmpty { stringResource(R.string.pc_no_network_short) }) { useNetwork = it }
                    Box {
                        val current = link.endpoints[p.outputId]?.name ?: p.outputName
                        Column(Modifier.fillMaxWidth().clickable(role = Role.DropdownList) { outputs = true }.padding(vertical = 6.dp)) {
                            Text(stringResource(R.string.when_output_connected), style = Type.Body, color = Archon.Paper)
                            Text(if (p.outputId.isEmpty()) stringResource(R.string.any_output) else current,
                                style = Type.Caption, color = Archon.Accent)
                        }
                        DropdownMenu(expanded = outputs, onDismissRequest = { outputs = false }, containerColor = Archon.Raised) {
                            DropdownMenuItem(text = { Text(stringResource(R.string.any_output)) },
                                onClick = { p = p.copy(outputId = "", outputName = ""); outputs = false })
                            link.outputs.forEach { e ->
                                DropdownMenuItem(text = { Text(e.name) },
                                    onClick = { p = p.copy(outputId = e.id, outputName = e.name); outputs = false })
                            }
                        }
                    }
                    SwitchRow(stringResource(R.string.restore_when_ends), p.restore) { p = p.copy(restore = it) }
                }
                if (!p.isNew) {
                    TextButton(onClick = { save(capture = true) }, enabled = live,
                        modifier = Modifier.fillMaxWidth().background(Archon.Raised, CircleShape)) {
                        Text(stringResource(R.string.update_with_current), style = Type.Caption, color = Archon.Paper)
                    }
                    TextButton(onClick = { confirmDelete = true }, modifier = Modifier.fillMaxWidth()) {
                        Text(stringResource(R.string.delete_profile), style = Type.Caption, color = Archon.Error)
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { save(capture = p.isNew) }, enabled = live && p.name.isNotBlank()) {
                Text(stringResource(R.string.save), color = Archon.Accent)
            }
        },
        dismissButton = { TextButton(onClick = onDone) { Text(stringResource(R.string.cancel), color = Archon.Paper) } },
    )
    if (confirmDelete) AlertDialog(
        onDismissRequest = { confirmDelete = false },
        containerColor = Archon.Surface,
        shape = RoundedCornerShape(24.dp),
        title = { Text(stringResource(R.string.delete_profile_q, p.name), style = Type.Body) },
        confirmButton = {
            TextButton(onClick = { confirmDelete = false; vm.deleteProfile(p.id); onDone() }) {
                Text(stringResource(R.string.delete), color = Archon.Error)
            }
        },
        dismissButton = { TextButton(onClick = { confirmDelete = false }) { Text(stringResource(R.string.cancel), color = Archon.Paper) } },
    )
}

@Composable
private fun SwitchRow(label: String, on: Boolean, enabled: Boolean = true, detail: String? = null, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().heightIn(min = 48.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(label, style = Type.Body, color = if (enabled) Archon.Paper else Archon.Muted)
            if (detail != null) Text(detail, style = Type.Caption, color = Archon.Muted)
        }
        Switch(checked = on, onCheckedChange = onChange, enabled = enabled,
            colors = SwitchDefaults.colors(checkedTrackColor = Archon.Accent, checkedThumbColor = Archon.Ink))
    }
}

@Composable
private fun TimeChip(label: String, minutes: Int, onClick: () -> Unit) {
    Column(Modifier.background(Archon.Raised, RoundedCornerShape(16.dp)).clickable(role = Role.Button, onClick = onClick)
        .padding(horizontal = 16.dp, vertical = 8.dp)) {
        Text(label, style = Type.Caption, color = Archon.Muted)
        Text(ProfileInfo.clock(minutes), style = Type.Body, color = Archon.Paper)
    }
}
