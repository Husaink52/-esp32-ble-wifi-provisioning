/*
 * WifiListScreen.kt: Screen 4, Wi-Fi networks found by the ESP32-C6's own scan.
 *
 * Architecture: UI layer (docs/DESIGN.md §9, Screens table, row 4).
 * State: ProvisioningViewModel.wifiState. The data comes from the device's
 * `prov-scan` endpoint, so it shows what the DEVICE can reach (2.4 GHz only),
 * not what the phone sees.
 */
package com.blewifiprov.app.ui.screens

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.blewifiprov.app.R
import com.blewifiprov.app.data.WifiNetwork
import com.blewifiprov.app.ui.ProvisioningViewModel
import com.blewifiprov.app.ui.components.CenteredMessage
import com.blewifiprov.app.ui.components.CenteredText
import com.blewifiprov.app.ui.components.DeviceLostDialog
import com.blewifiprov.app.ui.components.signalIcon
import androidx.compose.material.icons.automirrored.filled.ArrowBack

/**
 * Network picker.
 *
 * @param vm Shared wizard ViewModel.
 * @param onNetworkChosen Called after the ViewModel stores the choice; navigate to the password screen.
 * @param onExit Leave the wizard (Back or device lost): disconnect and return to scan.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun WifiListScreen(vm: ProvisioningViewModel, onNetworkChosen: () -> Unit, onExit: () -> Unit) {
    val state by vm.wifiState.collectAsStateWithLifecycle()
    val deviceLost by vm.deviceLost.collectAsStateWithLifecycle()

    // Back from here means "abandon this device": going back to Connect makes no sense.
    BackHandler(onBack = onExit)

    // Load the list on first entry (it stays cached when returning from the password screen).
    LaunchedEffect(Unit) {
        if (state.networks.isEmpty() && !state.isLoading) vm.loadWifiNetworks()
    }

    if (deviceLost) {
        DeviceLostDialog(onDismiss = { vm.acknowledgeDeviceLost(); onExit() })
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text(stringResource(R.string.wifi_title))
                        Text(stringResource(R.string.wifi_subtitle), style = MaterialTheme.typography.labelMedium)
                    }
                },
                navigationIcon = {
                    IconButton(onClick = onExit) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.action_back))
                    }
                },
                actions = {
                    IconButton(onClick = vm::loadWifiNetworks, enabled = !state.isLoading) {
                        Icon(Icons.Filled.Refresh, contentDescription = stringResource(R.string.wifi_refresh))
                    }
                },
            )
        }
    ) { padding ->
        Column(Modifier.padding(padding).fillMaxSize()) {
            if (state.isLoading && state.networks.isNotEmpty()) {
                // Refreshing: keep the old list visible and show a thin progress bar.
                LinearProgressIndicator(Modifier.fillMaxWidth())
            }

            when {
                state.isLoading && state.networks.isEmpty() -> CenteredMessage {
                    CircularProgressIndicator()
                    CenteredText(stringResource(R.string.wifi_loading))
                }

                state.loadFailed && state.networks.isEmpty() -> CenteredMessage {
                    Text(stringResource(R.string.wifi_load_failed), color = MaterialTheme.colorScheme.error)
                    Button(onClick = vm::loadWifiNetworks) { Text(stringResource(R.string.action_retry)) }
                }

                else -> LazyColumn {
                    if (state.networks.isEmpty()) {
                        item { ListItem(headlineContent = { Text(stringResource(R.string.wifi_empty)) }) }
                    }
                    items(state.networks, key = { it.ssid }) { network ->
                        NetworkRow(network) {
                            vm.selectNetwork(network)
                            onNetworkChosen()
                        }
                        HorizontalDivider()
                    }
                    // Hidden networks don't show up in scans, so let the user type the SSID.
                    item {
                        ListItem(
                            modifier = Modifier.clickable {
                                vm.selectManualNetwork()
                                onNetworkChosen()
                            },
                            leadingContent = { Icon(Icons.Filled.Add, contentDescription = null) },
                            headlineContent = { Text(stringResource(R.string.wifi_other)) },
                        )
                    }
                }
            }
        }
    }
}

/** One network row: signal icon, SSID, and "Secured"/"Open" with a lock icon. */
@Composable
private fun NetworkRow(network: WifiNetwork, onClick: () -> Unit) {
    ListItem(
        modifier = Modifier.clickable(onClick = onClick),
        leadingContent = { Icon(signalIcon(network.rssi), contentDescription = null) },
        headlineContent = { Text(network.ssid) },
        supportingContent = {
            Text(stringResource(if (network.isOpen) R.string.wifi_open else R.string.wifi_secured))
        },
        trailingContent = {
            if (!network.isOpen) Icon(Icons.Filled.Lock, contentDescription = stringResource(R.string.wifi_secured))
        },
    )
}
