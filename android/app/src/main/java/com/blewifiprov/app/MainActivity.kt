/*
 * MainActivity.kt: the app's single Activity and entry point.
 *
 * Architecture: docs/DESIGN.md §9. The Activity only hosts Compose. All
 * screens are navigation destinations in ui/navigation/AppNavHost.kt, and all
 * state lives in ProvisioningViewModel.
 */
package com.blewifiprov.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.blewifiprov.app.ui.navigation.AppNavHost
import com.blewifiprov.app.ui.theme.BleProvTheme

/** Launcher activity that shows the provisioning wizard. */
class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Draw behind the status and navigation bars. Each screen's Scaffold
        // applies the system-bar insets as padding.
        enableEdgeToEdge()
        setContent {
            BleProvTheme {
                AppNavHost()
            }
        }
    }
}
