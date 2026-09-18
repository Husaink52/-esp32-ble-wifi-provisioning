/*
 * StartScreen.kt: Screen 1, the welcome screen plus permission and Bluetooth/Location checks.
 *
 * Architecture: UI layer (docs/DESIGN.md §9, Screens table, row 1).
 *
 * Gatekeeper for the rest of the wizard. [onReady] fires only when:
 *   1. every runtime permission in BlePermissions.required is granted,
 *   2. Bluetooth is on,
 *   3. Location services are on (Android 11 and lower only).
 * The checks run again every time the screen resumes (e.g. coming back from
 * system Settings), so the user continues automatically once everything is
 * fixed. If everything is already OK at launch, the screen is skipped immediately.
 */
package com.blewifiprov.app.ui.screens

import android.bluetooth.BluetoothAdapter
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import com.blewifiprov.app.R
import com.blewifiprov.app.permissions.BlePermissions
import com.blewifiprov.app.ui.components.WizardTopBar

/**
 * Welcome and permission screen.
 *
 * @param onReady Called once when all prerequisites are met; should navigate to the scan screen.
 */
@Composable
fun StartScreen(onReady: () -> Unit) {
    val context = LocalContext.current

    // What's currently blocking the user. Each flag shows a message and a fix-it button.
    var permissionsDenied by remember { mutableStateOf(false) }
    var bluetoothOff by remember { mutableStateOf(false) }
    var locationOff by remember { mutableStateOf(false) }

    // Guard so resume, permission and Bluetooth callbacks can't navigate twice.
    var navigated by remember { mutableStateOf(false) }

    /**
     * Re-check everything and continue if possible. If permissions are still
     * missing, wait for the user to tap "Get started" instead of prompting on our own.
     */
    fun evaluate() {
        if (navigated || !BlePermissions.allGranted(context)) return
        permissionsDenied = false
        bluetoothOff = !BlePermissions.isBluetoothEnabled(context)
        locationOff = !BlePermissions.isLocationReady(context)
        if (!bluetoothOff && !locationOff) {
            navigated = true
            onReady()
        }
    }

    // System dialog "Allow <app> to turn on Bluetooth?"; re-check with the result.
    val enableBluetoothLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { evaluate() }

    // Runtime permission prompt.
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { results ->
        permissionsDenied = results.values.any { granted -> !granted }
        evaluate()
    }

    // Runs on first display and whenever the user comes back from Settings.
    LifecycleResumeEffect(Unit) {
        evaluate()
        onPauseOrDispose { }
    }

    Scaffold(topBar = { WizardTopBar(title = stringResource(R.string.start_title)) }) { padding ->
        Column(
            modifier = Modifier
                .padding(padding)
                .fillMaxSize()
                .padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Icon(
                imageVector = Icons.Filled.Bluetooth,
                contentDescription = null, // decorative
                modifier = Modifier.size(96.dp),
                tint = MaterialTheme.colorScheme.primary,
            )
            Text(stringResource(R.string.start_body), style = MaterialTheme.typography.bodyLarge)

            Button(
                onClick = { permissionLauncher.launch(BlePermissions.required) },
                modifier = Modifier.fillMaxWidth(),
            ) { Text(stringResource(R.string.start_button)) }

            if (permissionsDenied) {
                Text(stringResource(R.string.start_permissions_denied), color = MaterialTheme.colorScheme.error)
                // After "Don't ask again", Android won't show the prompt, so point to the app's settings page.
                OutlinedButton(onClick = {
                    context.startActivity(
                        Intent(
                            Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                            Uri.fromParts("package", context.packageName, null),
                        )
                    )
                }) { Text(stringResource(R.string.start_open_settings)) }
            }

            if (bluetoothOff) {
                Text(stringResource(R.string.start_bluetooth_off), color = MaterialTheme.colorScheme.error)
                OutlinedButton(onClick = {
                    // Permission (BLUETOOTH_CONNECT on API 31+) is granted before this button can appear.
                    enableBluetoothLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
                }) { Text(stringResource(R.string.start_turn_on_bluetooth)) }
            }

            if (locationOff) {
                Text(stringResource(R.string.start_location_off), color = MaterialTheme.colorScheme.error)
                OutlinedButton(onClick = {
                    context.startActivity(Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS))
                }) { Text(stringResource(R.string.start_turn_on_location)) }
            }
        }
    }
}
