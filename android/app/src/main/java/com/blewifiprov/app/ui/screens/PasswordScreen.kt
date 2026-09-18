/*
 * PasswordScreen.kt: Screen 5, confirm the SSID and enter the Wi-Fi password.
 *
 * Architecture: UI layer (docs/DESIGN.md §9, Screens table, row 5).
 * State: ProvisioningViewModel.selectedNetwork (set on the Wi-Fi list screen).
 *
 * Validation (802.11 limits):
 *  - SSID: 1–32 characters. Editable only for "Other network".
 *  - Password: known secured network → 8–64 characters (64 = raw hex PSK);
 *              known open network → field hidden;
 *              manual network → empty (open) or 8–64 characters.
 * The password is never logged or stored. It's only passed to the ViewModel.
 */
package com.blewifiprov.app.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.blewifiprov.app.R
import com.blewifiprov.app.data.ProvisioningConfig
import com.blewifiprov.app.ui.ProvisioningViewModel
import com.blewifiprov.app.ui.components.DeviceLostDialog
import com.blewifiprov.app.ui.components.WizardTopBar

/** Maximum SSID length in bytes (802.11). Approximated as characters for ASCII SSIDs. */
private const val MAX_SSID_LENGTH = 32

/** Maximum WPA passphrase length: 63 ASCII characters, or a 64-character hex PSK. */
private const val MAX_PASSWORD_LENGTH = 64

/**
 * Credentials entry form.
 *
 * @param vm Shared wizard ViewModel.
 * @param onSubmit Called after vm.provision() starts; navigate to the provisioning screen.
 * @param onBack Return to the network list.
 * @param onExit Leave the wizard (device lost).
 */
@Composable
fun PasswordScreen(
    vm: ProvisioningViewModel,
    onSubmit: () -> Unit,
    onBack: () -> Unit,
    onExit: () -> Unit,
) {
    val network by vm.selectedNetwork.collectAsStateWithLifecycle()
    val deviceLost by vm.deviceLost.collectAsStateWithLifecycle()

    // Nothing selected, e.g. after the process was recreated, so go back to the list.
    LaunchedEffect(network) { if (network == null) onBack() }
    val selected = network ?: return

    // rememberSaveable keeps typed text across rotation. Keyed on the selection,
    // so choosing a different network starts with fresh fields.
    var ssid by rememberSaveable(selected) { mutableStateOf(selected.ssid) }
    var password by rememberSaveable(selected) { mutableStateOf("") }
    var passwordVisible by rememberSaveable { mutableStateOf(false) }

    val passwordNeeded = !selected.isOpen
    val passwordValid = when {
        !passwordNeeded -> true
        selected.isManual && password.isEmpty() -> true // manual entry may be an open hidden network
        else -> password.length >= ProvisioningConfig.MIN_WPA_PASSWORD_LENGTH
    }
    val canSubmit = ssid.isNotBlank() && passwordValid

    if (deviceLost) {
        DeviceLostDialog(onDismiss = { vm.acknowledgeDeviceLost(); onExit() })
    }

    Scaffold(topBar = { WizardTopBar(title = stringResource(R.string.password_title), onBack = onBack) }) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            OutlinedTextField(
                value = ssid,
                onValueChange = { ssid = it.take(MAX_SSID_LENGTH) },
                label = { Text(stringResource(R.string.password_ssid_label)) },
                singleLine = true,
                // SSIDs picked from the scan must be used exactly as reported, so they're read-only.
                readOnly = !selected.isManual,
                modifier = Modifier.fillMaxWidth(),
            )

            if (passwordNeeded) {
                OutlinedTextField(
                    value = password,
                    onValueChange = { password = it.take(MAX_PASSWORD_LENGTH) },
                    label = { Text(stringResource(R.string.password_label)) },
                    singleLine = true,
                    visualTransformation = if (passwordVisible) VisualTransformation.None else PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, imeAction = ImeAction.Done),
                    trailingIcon = {
                        IconButton(onClick = { passwordVisible = !passwordVisible }) {
                            Icon(
                                imageVector = if (passwordVisible) Icons.Filled.VisibilityOff else Icons.Filled.Visibility,
                                contentDescription = stringResource(
                                    if (passwordVisible) R.string.password_hide else R.string.password_show
                                ),
                            )
                        }
                    },
                    // Only show the error after the user starts typing, not on an empty field.
                    isError = password.isNotEmpty() && !passwordValid,
                    supportingText = {
                        if (password.isNotEmpty() && !passwordValid) {
                            Text(stringResource(R.string.password_too_short, ProvisioningConfig.MIN_WPA_PASSWORD_LENGTH))
                        }
                    },
                    modifier = Modifier.fillMaxWidth(),
                )
            } else {
                Text(stringResource(R.string.password_open_network))
            }

            Button(
                onClick = {
                    vm.provision(ssid = ssid, password = if (passwordNeeded) password else "")
                    onSubmit()
                },
                enabled = canSubmit,
                modifier = Modifier.fillMaxWidth(),
            ) { Text(stringResource(R.string.password_connect)) }
        }
    }
}
