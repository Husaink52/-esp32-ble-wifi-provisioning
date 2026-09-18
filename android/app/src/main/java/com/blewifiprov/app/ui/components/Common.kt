/*
 * Common.kt: small composables shared by several screens.
 *
 * Architecture: UI layer (docs/DESIGN.md §9). Keeps the screen files focused on their own layout.
 */
package com.blewifiprov.app.ui.components

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.SignalWifi0Bar
import androidx.compose.material.icons.filled.SignalWifi4Bar
import androidx.compose.material.icons.filled.NetworkWifi1Bar
import androidx.compose.material.icons.filled.NetworkWifi2Bar
import androidx.compose.material.icons.filled.NetworkWifi3Bar
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.blewifiprov.app.R

/**
 * Standard top app bar with an optional back arrow.
 *
 * @param title Screen title.
 * @param onBack If non-null, a back arrow is shown that calls it.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun WizardTopBar(title: String, onBack: (() -> Unit)? = null) {
    TopAppBar(
        title = { Text(title) },
        navigationIcon = {
            if (onBack != null) {
                IconButton(onClick = onBack) {
                    Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.action_back))
                }
            }
        },
    )
}

/**
 * Centered column for empty, loading and message states.
 *
 * @param modifier Usually the Scaffold's inner padding.
 * @param content Items stacked vertically with 16dp spacing.
 */
@Composable
fun CenteredMessage(modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    Column(
        modifier = modifier
            .fillMaxSize()
            .padding(24.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp, Alignment.CenterVertically),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        content()
    }
}

/** Body text centered, for use inside [CenteredMessage]. */
@Composable
fun CenteredText(text: String) {
    Text(text = text, textAlign = TextAlign.Center)
}

/**
 * Map an RSSI (dBm) to a Wi-Fi strength icon with 0–4 bars.
 *
 * The thresholds are common rules of thumb: -55 dBm or better is excellent,
 * -85 dBm or worse is barely usable. Also used for BLE RSSI, where the same
 * ranges roughly mean "near" and "far".
 */
fun signalIcon(rssi: Int): ImageVector = when {
    rssi >= -55 -> Icons.Filled.SignalWifi4Bar
    rssi >= -65 -> Icons.Filled.NetworkWifi3Bar
    rssi >= -75 -> Icons.Filled.NetworkWifi2Bar
    rssi >= -85 -> Icons.Filled.NetworkWifi1Bar
    else -> Icons.Filled.SignalWifi0Bar
}

/**
 * Modal dialog shown when the BLE link to the device drops mid-wizard.
 *
 * @param onDismiss Called for OK or outside tap; should return to the scan screen.
 */
@Composable
fun DeviceLostDialog(onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.device_lost_title)) },
        text = { Text(stringResource(R.string.device_lost_body)) },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.action_ok)) }
        },
    )
}
