/*
 * ProvisionScreen.kt: Screens 6 + 7, provisioning progress and final result.
 *
 * Architecture: UI layer (docs/DESIGN.md §9, Screens table, rows 6 and 7).
 * One destination with two looks, because the result replaces the progress in place:
 *  - In progress: three steps (sending → applying → connecting), each with a
 *    done / active / pending indicator. Back is blocked, because leaving
 *    can't cancel the operation on the device.
 *  - Result: success (Done → back to scan), or failure with a specific
 *    message and Try again (→ password screen, same BLE connection) or Cancel.
 */
package com.blewifiprov.app.ui.screens

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.ErrorOutline
import androidx.compose.material.icons.filled.RadioButtonUnchecked
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.blewifiprov.app.R
import com.blewifiprov.app.data.ProvisionError
import com.blewifiprov.app.data.ProvisioningConfig
import com.blewifiprov.app.ui.ProvisionStage
import com.blewifiprov.app.ui.ProvisionUiState
import com.blewifiprov.app.ui.ProvisioningViewModel
import com.blewifiprov.app.ui.components.WizardTopBar
import com.blewifiprov.app.ui.theme.SuccessGreen

/**
 * Progress and result screen.
 *
 * @param vm Shared wizard ViewModel.
 * @param onRetry Go back to the password screen to try again on the same connection.
 * @param onFinished Leave the wizard (success, cancel, or connection lost).
 */
@Composable
fun ProvisionScreen(vm: ProvisioningViewModel, onRetry: () -> Unit, onFinished: () -> Unit) {
    val state by vm.provisionState.collectAsStateWithLifecycle()

    // In progress: swallow Back. Finished: Back acts like the main button.
    BackHandler {
        when (state.stage) {
            ProvisionStage.SUCCESS -> onFinished()
            ProvisionStage.FAILED ->
                if (state.error == ProvisionError.DEVICE_DISCONNECTED) onFinished() else onRetry()
            else -> Unit
        }
    }

    Scaffold(topBar = { WizardTopBar(title = stringResource(R.string.provision_title)) }) { padding ->
        Box(
            modifier = Modifier.padding(padding).fillMaxSize().padding(24.dp),
            contentAlignment = Alignment.Center,
        ) {
            when (state.stage) {
                ProvisionStage.SUCCESS -> SuccessContent(ssid = state.ssid, onDone = onFinished)
                ProvisionStage.FAILED -> FailureContent(state = state, onRetry = onRetry, onCancel = onFinished)
                else -> ProgressContent(stage = state.stage)
            }
        }
    }
}

/** The three in-progress steps, with the current one showing a spinner. */
@Composable
private fun ProgressContent(stage: ProvisionStage) {
    val steps = listOf(
        ProvisionStage.SENDING to R.string.provision_step_sending,
        ProvisionStage.APPLYING to R.string.provision_step_applying,
        ProvisionStage.CONNECTING to R.string.provision_step_connecting,
    )
    Column(verticalArrangement = Arrangement.spacedBy(20.dp), modifier = Modifier.fillMaxWidth()) {
        steps.forEach { (stepStage, label) ->
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                // Stages are declared in order, so comparing ordinals tells done / active / pending.
                when {
                    stage.ordinal > stepStage.ordinal ->
                        Icon(Icons.Filled.CheckCircle, contentDescription = null, tint = SuccessGreen, modifier = Modifier.size(28.dp))
                    stage == stepStage ->
                        CircularProgressIndicator(modifier = Modifier.size(28.dp), strokeWidth = 3.dp)
                    else ->
                        Icon(Icons.Filled.RadioButtonUnchecked, contentDescription = null, modifier = Modifier.size(28.dp))
                }
                Text(stringResource(label), style = MaterialTheme.typography.bodyLarge)
            }
        }
    }
}

/** Big green check with a Done button. */
@Composable
private fun SuccessContent(ssid: String, onDone: () -> Unit) {
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Icon(Icons.Filled.CheckCircle, contentDescription = null, tint = SuccessGreen, modifier = Modifier.size(96.dp))
        Text(stringResource(R.string.provision_success_title), style = MaterialTheme.typography.headlineSmall)
        Text(stringResource(R.string.provision_success_body, ssid), textAlign = TextAlign.Center)
        Button(onClick = onDone, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.provision_done)) }
    }
}

/** Error icon, a reason-specific message, and the right recovery buttons. */
@Composable
private fun FailureContent(state: ProvisionUiState, onRetry: () -> Unit, onCancel: () -> Unit) {
    val message = when (state.error) {
        ProvisionError.WRONG_PASSWORD -> stringResource(R.string.provision_error_wrong_password, state.ssid)
        ProvisionError.NETWORK_NOT_FOUND -> stringResource(R.string.provision_error_not_found, state.ssid)
        ProvisionError.CONFIG_REJECTED ->
            stringResource(R.string.provision_error_rejected, ProvisioningConfig.DEVICE_RETRY_COOLDOWN_SECONDS)
        ProvisionError.DEVICE_DISCONNECTED -> stringResource(R.string.provision_error_disconnected)
        ProvisionError.UNKNOWN, null -> stringResource(R.string.provision_error_unknown)
    }
    // Once the BLE link is gone, retrying on "the same connection" is impossible.
    val canRetry = state.error != ProvisionError.DEVICE_DISCONNECTED

    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Icon(
            Icons.Filled.ErrorOutline,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.error,
            modifier = Modifier.size(96.dp),
        )
        Text(stringResource(R.string.provision_failed_title), style = MaterialTheme.typography.headlineSmall)
        Text(message, textAlign = TextAlign.Center)

        if (canRetry) {
            Button(onClick = onRetry, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.action_retry)) }
            OutlinedButton(onClick = onCancel, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.action_cancel)) }
        } else {
            Button(onClick = onCancel, modifier = Modifier.fillMaxWidth()) { Text(stringResource(R.string.provision_start_over)) }
        }
    }
}
