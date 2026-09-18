/*
 * AppNavHost.kt: navigation graph of the provisioning wizard.
 *
 * Architecture: UI layer (docs/DESIGN.md §9, Screens). Flow:
 *
 *   start ──▶ scan ──▶ connect ──▶ wifi ──▶ password ──▶ provision
 *               ▲                   │                        │
 *               └──── back / cancel / done / device lost ────┘
 *
 * Back-stack rules:
 *  - start is removed once permissions are OK, so Back on scan exits the app.
 *  - connect is replaced by wifi once connected, so Back on wifi disconnects
 *    and returns to scan instead of reconnecting.
 *  - provision → "Try again" pops back to password on the same connection.
 */
package com.blewifiprov.app.ui.navigation

import androidx.compose.runtime.Composable
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import com.blewifiprov.app.ui.ProvisioningViewModel
import com.blewifiprov.app.ui.screens.ConnectScreen
import com.blewifiprov.app.ui.screens.DeviceScanScreen
import com.blewifiprov.app.ui.screens.PasswordScreen
import com.blewifiprov.app.ui.screens.ProvisionScreen
import com.blewifiprov.app.ui.screens.StartScreen
import com.blewifiprov.app.ui.screens.WifiListScreen

/** Route names for every destination. */
object Routes {
    const val START = "start"
    const val SCAN = "scan"
    const val CONNECT = "connect"
    const val WIFI = "wifi"
    const val PASSWORD = "password"
    const val PROVISION = "provision"
}

/**
 * Hosts all screens.
 *
 * @param navController Injectable for tests; normally created here.
 */
@Composable
fun AppNavHost(navController: NavHostController = rememberNavController()) {
    // Created OUTSIDE the NavHost, so it's scoped to the Activity and shared by
    // every screen (see ProvisioningViewModel's file header).
    val vm: ProvisioningViewModel = viewModel()

    /** Leave the wizard and go back to a fresh device scan. */
    fun backToScan() {
        vm.finish()
        navController.navigate(Routes.SCAN) {
            popUpTo(Routes.SCAN) { inclusive = true }
        }
    }

    NavHost(navController = navController, startDestination = Routes.START) {

        composable(Routes.START) {
            StartScreen(
                onReady = {
                    navController.navigate(Routes.SCAN) {
                        popUpTo(Routes.START) { inclusive = true }
                    }
                }
            )
        }

        composable(Routes.SCAN) {
            DeviceScanScreen(
                vm = vm,
                onDeviceSelected = { device ->
                    vm.selectDevice(device)
                    navController.navigate(Routes.CONNECT)
                }
            )
        }

        composable(Routes.CONNECT) {
            ConnectScreen(
                vm = vm,
                onConnected = {
                    navController.navigate(Routes.WIFI) {
                        popUpTo(Routes.CONNECT) { inclusive = true }
                    }
                },
                onBack = {
                    vm.disconnect()
                    navController.popBackStack()
                }
            )
        }

        composable(Routes.WIFI) {
            WifiListScreen(
                vm = vm,
                onNetworkChosen = { navController.navigate(Routes.PASSWORD) },
                onExit = ::backToScan,
            )
        }

        composable(Routes.PASSWORD) {
            PasswordScreen(
                vm = vm,
                onSubmit = { navController.navigate(Routes.PROVISION) },
                onBack = { navController.popBackStack() },
                onExit = ::backToScan,
            )
        }

        composable(Routes.PROVISION) {
            ProvisionScreen(
                vm = vm,
                onRetry = { navController.popBackStack() }, // back to password, same connection
                onFinished = ::backToScan,
            )
        }
    }
}
