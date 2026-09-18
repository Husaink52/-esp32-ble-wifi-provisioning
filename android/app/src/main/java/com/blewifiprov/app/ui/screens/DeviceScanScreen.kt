/*
 * DeviceScanScreen.kt: Screen 2, lists nearby ESP32 devices in provisioning mode.
 *
 * Architecture: UI layer (docs/DESIGN.md §9, Screens table, row 2).
 * State: ProvisioningViewModel.scanState. Only devices whose BLE name starts
 * with "PROV_" are shown (the filter is in the repository).
 * Each row shows an ESTIMATED distance ("≈ 0.5 m") computed from the
 * smoothed RSSI; see data/DistanceEstimator.kt for how and how inaccurate it is.
 */
package com.blewifiprov.app.ui.screens

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.blewifiprov.app.R
import com.blewifiprov.app.data.DistanceEstimator
import com.blewifiprov.app.data.ScannedDevice
import java.util.Locale
import kotlin.math.roundToInt
import com.blewifiprov.app.ui.ProvisioningViewModel
import com.blewifiprov.app.ui.ScanError
import com.blewifiprov.app.ui.components.CenteredMessage
import com.blewifiprov.app.ui.components.CenteredText
import com.blewifiprov.app.ui.components.WizardTopBar
import com.blewifiprov.app.ui.components.signalIcon

/**
 * Device list with automatic scan on entry.
 *
 * @param vm Shared wizard ViewModel.
 * @param onDeviceSelected Called when the user taps a device.
 */
@Composable
fun DeviceScanScreen(vm: ProvisioningViewModel, onDeviceSelected: (ScannedDevice) -> Unit) {
    val state by vm.scanState.collectAsStateWithLifecycle()

    // Scan automatically the first time (or after the wizard restarts), but not
    // when the user comes Back from the connect screen and results are still shown.
    LaunchedEffect(Unit) {
        if (state.devices.isEmpty() && !state.isScanning) vm.startScan()
    }
    // Don't leave the radio scanning when this screen goes away.
    DisposableEffect(Unit) { onDispose { vm.stopScan() } }

    Scaffold(topBar = { WizardTopBar(title = stringResource(R.string.scan_title)) }) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {

            if (state.isScanning) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
            }

            Column(Modifier.weight(1f)) {
                when {
                    state.error != null -> CenteredMessage {
                        Text(
                            text = stringResource(
                                if (state.error == ScanError.BLUETOOTH_OFF) R.string.scan_error_bluetooth_off
                                else R.string.scan_error_failed
                            ),
                            color = MaterialTheme.colorScheme.error,
                        )
                    }

                    state.devices.isEmpty() -> CenteredMessage {
                        CenteredText(
                            stringResource(if (state.isScanning) R.string.scan_scanning else R.string.scan_empty)
                        )
                    }

                    else -> LazyColumn {
                        items(state.devices, key = { it.address }) { device ->
                            DeviceRow(device = device, onClick = { onDeviceSelected(device) })
                            HorizontalDivider()
                        }
                    }
                }
            }

            Button(
                onClick = vm::startScan,
                enabled = !state.isScanning,
                modifier = Modifier.fillMaxWidth().padding(16.dp),
            ) { Text(stringResource(R.string.scan_rescan)) }
        }
    }
}

/**
 * One device in the list: name, MAC address, estimated distance and a signal icon.
 */
@Composable
private fun DeviceRow(device: ScannedDevice, onClick: () -> Unit) {
    ListItem(
        modifier = Modifier.clickable(onClick = onClick),
        leadingContent = { Icon(Icons.Filled.Memory, contentDescription = null) },
        headlineContent = { Text(device.name) },
        supportingContent = { Text(device.address) },
        trailingContent = {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(distanceText(device.rssi), style = MaterialTheme.typography.labelMedium)
                Icon(signalIcon(device.rssi), contentDescription = null)
            }
        },
    )
}

/**
 * Human-readable estimated distance for an RSSI, e.g. "≈ 0.5 m", "≈ 3 m", "> 10 m".
 *
 * Rounding gets coarser with distance, because the estimate gets less reliable
 * further away (see DistanceEstimator):
 *  - under 1 m: one decimal (minimum 0.1 m, so it never shows "0.0 m"),
 *  - 1–10 m: whole metres,
 *  - beyond: just "> 10 m".
 */
@Composable
private fun distanceText(rssi: Int): String {
    val meters = DistanceEstimator.estimateMeters(rssi)
    return when {
        meters < 1.0 -> stringResource(
            R.string.distance_approx_m,
            String.format(Locale.getDefault(), "%.1f", meters.coerceAtLeast(0.1)),
        )
        meters < FAR_THRESHOLD_M -> stringResource(R.string.distance_approx_m, meters.roundToInt().toString())
        else -> stringResource(R.string.distance_far_m, FAR_THRESHOLD_M.toInt())
    }
}

/** Beyond this many metres the estimate is too rough to show a number. */
private const val FAR_THRESHOLD_M = 10.0
