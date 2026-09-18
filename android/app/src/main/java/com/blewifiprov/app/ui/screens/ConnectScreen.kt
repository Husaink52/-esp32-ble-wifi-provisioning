/*
 * ConnectScreen.kt: Screen 3, BLE connection and Security 1 session with the chosen device.
 *
 * Architecture: UI layer (docs/DESIGN.md §9, Screens table, row 3).
 * State: ProvisioningViewModel.connectState and .pop.
 *
 * Connects automatically on entry with the default PoP. If the secure session
 * fails (usually a wrong PoP), the "Advanced" section opens so the user can
 * fix the PoP and try again.
 */
package com.blewifiprov.app.ui.screens

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ErrorOutline
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.blewifiprov.app.R
import com.blewifiprov.app.data.ConnectError
import com.blewifiprov.app.data.ProvisioningConfig
import com.blewifiprov.app.ui.ConnectUiState
import com.blewifiprov.app.ui.ProvisioningViewModel
import com.blewifiprov.app.ui.components.WizardTopBar

/**
 * Connection progress and error screen.
 *
 * @param vm Shared wizard ViewModel.
 * @param onConnected Called once the secure session is set up.
 * @param onBack Called for Back/Cancel; the caller disconnects and pops the screen.
 */
@Composable
fun ConnectScreen(vm: ProvisioningViewModel, onConnected: () -> Unit, onBack: () -> Unit) {
    val device by vm.selectedDevice.collectAsStateWithLifecycle()
    val state by vm.connectState.collectAsStateWithLifecycle()
    val pop by vm.pop.collectAsStateWithLifecycle()
    var showAdvanced by rememberSaveable { mutableStateOf(false) }

    BackHandler(onBack = onBack)

    // No device, e.g. the process was recreated. Nothing to connect to, so leave.
    LaunchedEffect(device) { if (device == null) onBack() }

    // Start connecting on first entry.
    LaunchedEffect(Unit) { if (state is ConnectUiState.Idle) vm.connect() }

    LaunchedEffect(state) {
        when (val s = state) {
            ConnectUiState.Connected -> onConnected()
            // Wrong PoP is the most likely cause, so show the PoP field.
            is ConnectUiState.Failed -> if (s.error == ConnectError.SESSION_FAILED) showAdvanced = true
            else -> Unit
        }
    }

    Scaffold(topBar = { WizardTopBar(title = stringResource(R.string.connect_title), onBack = onBack) }) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            when (val s = state) {
                is ConnectUiState.Failed -> {
                    Icon(
                        Icons.Filled.ErrorOutline,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.error,
                        modifier = Modifier.size(64.dp),
                    )
                    Text(
                        text = stringResource(connectErrorText(s.error)),
                        textAlign = TextAlign.Center,
                        color = MaterialTheme.colorScheme.error,
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        OutlinedButton(onClick = onBack) { Text(stringResource(R.string.action_cancel)) }
                        Button(onClick = vm::connect) { Text(stringResource(R.string.action_retry)) }
                    }
                }

                else -> { // Idle (about to start), Connecting, or Connected (about to navigate)
                    CircularProgressIndicator(Modifier.size(64.dp))
                    Text(
                        text = stringResource(R.string.connect_in_progress, device?.name.orEmpty()),
                        textAlign = TextAlign.Center,
                    )
                }
            }

            // --- Advanced: Proof-of-Possession ---
            TextButton(onClick = { showAdvanced = !showAdvanced }) {
                Text(stringResource(R.string.connect_advanced))
            }
            if (showAdvanced) {
                OutlinedTextField(
                    value = pop,
                    onValueChange = vm::setPop,
                    label = { Text(stringResource(R.string.connect_pop_label)) },
                    supportingText = {
                        Text(stringResource(R.string.connect_pop_help, ProvisioningConfig.DEFAULT_POP))
                    },
                    singleLine = true,
                    // Changing the PoP mid-connection would have no effect, so lock the field.
                    enabled = state !is ConnectUiState.Connecting,
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }
    }
}

/** String resource explaining a [ConnectError]. */
private fun connectErrorText(error: ConnectError): Int = when (error) {
    ConnectError.BLUETOOTH_OFF -> R.string.connect_error_bluetooth_off
    ConnectError.CONNECTION_FAILED -> R.string.connect_error_failed
    ConnectError.TIMEOUT -> R.string.connect_error_timeout
    ConnectError.SESSION_FAILED -> R.string.connect_error_session
}
