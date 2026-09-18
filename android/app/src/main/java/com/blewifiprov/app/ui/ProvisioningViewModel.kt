/*
 * ProvisioningViewModel.kt: holds the state of the whole provisioning wizard.
 *
 * Architecture: ViewModel layer (docs/DESIGN.md §9). UI → **ViewModel** → Repository.
 *
 * Why one shared ViewModel instead of one per screen: the wizard is a straight
 * line (scan → connect → Wi-Fi list → password → provision) and each step
 * needs the previous step's result: the chosen device, the open connection,
 * the chosen network. The ViewModel is scoped to MainActivity (see
 * AppNavHost), so every screen sees the same instance, and it survives
 * rotation and other configuration changes.
 *
 * Screens read immutable state from StateFlows and call the public functions
 * below. They never touch the repository or the Espressif library.
 */
package com.blewifiprov.app.ui

import android.app.Application
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.blewifiprov.app.data.BluetoothOffException
import com.blewifiprov.app.data.ConnectError
import com.blewifiprov.app.data.ConnectException
import com.blewifiprov.app.data.DistanceEstimator
import com.blewifiprov.app.data.ProvisionError
import com.blewifiprov.app.data.ProvisionUpdate
import com.blewifiprov.app.data.ProvisioningConfig
import com.blewifiprov.app.data.ProvisioningRepository
import com.blewifiprov.app.data.ScannedDevice
import com.blewifiprov.app.data.WifiNetwork
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.onCompletion
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/* ---------------------------------------------------------------------------
 * UI state types, one per wizard step
 * ------------------------------------------------------------------------- */

/** State of the device-scan screen. */
data class ScanUiState(
    val isScanning: Boolean = false,
    /** Found devices, unique by address, strongest signal first. */
    val devices: List<ScannedDevice> = emptyList(),
    val error: ScanError? = null,
)

/** Why a BLE scan couldn't run. */
enum class ScanError { BLUETOOTH_OFF, FAILED }

/** State of the connect screen. */
sealed interface ConnectUiState {
    data object Idle : ConnectUiState
    data object Connecting : ConnectUiState
    data object Connected : ConnectUiState
    data class Failed(val error: ConnectError) : ConnectUiState
}

/** State of the Wi-Fi network list screen. */
data class WifiListUiState(
    val isLoading: Boolean = false,
    val networks: List<WifiNetwork> = emptyList(),
    /** true if the device's Wi-Fi scan request failed. */
    val loadFailed: Boolean = false,
)

/**
 * The network the user wants to join.
 *
 * @property ssid Pre-filled SSID; empty when the user picks "Other network".
 * @property isOpen true if the device reported no security (no password needed).
 * @property isManual true if the user types the SSID (hidden network).
 */
data class SelectedNetwork(
    val ssid: String,
    val isOpen: Boolean,
    val isManual: Boolean,
)

/** Progress stages shown on the provisioning screen, in order. */
enum class ProvisionStage { SENDING, APPLYING, CONNECTING, SUCCESS, FAILED }

/** State of the provisioning progress/result screen. */
data class ProvisionUiState(
    val stage: ProvisionStage = ProvisionStage.SENDING,
    /** SSID being provisioned, shown in the result text. */
    val ssid: String = "",
    /** Set only when [stage] is [ProvisionStage.FAILED]. */
    val error: ProvisionError? = null,
)

/* ---------------------------------------------------------------------------
 * ViewModel
 * ------------------------------------------------------------------------- */

/**
 * Wizard state holder. See the file header for the scoping rationale.
 *
 * @param application Used to create the [ProvisioningRepository] (application context only).
 */
class ProvisioningViewModel(application: Application) : AndroidViewModel(application) {

    private val repository = ProvisioningRepository(application)

    // --- Scan -----------------------------------------------------------------
    private val _scanState = MutableStateFlow(ScanUiState())
    val scanState: StateFlow<ScanUiState> = _scanState.asStateFlow()
    private var scanJob: Job? = null

    // --- Connect -----------------------------------------------------------------
    private val _selectedDevice = MutableStateFlow<ScannedDevice?>(null)
    val selectedDevice: StateFlow<ScannedDevice?> = _selectedDevice.asStateFlow()

    /** PoP used for the next connection; editable on the Connect screen. */
    private val _pop = MutableStateFlow(ProvisioningConfig.DEFAULT_POP)
    val pop: StateFlow<String> = _pop.asStateFlow()

    private val _connectState = MutableStateFlow<ConnectUiState>(ConnectUiState.Idle)
    val connectState: StateFlow<ConnectUiState> = _connectState.asStateFlow()
    private var connectJob: Job? = null

    /**
     * true when the device dropped the BLE link unexpectedly while connected and
     * not yet provisioned. Screens show a "device disconnected" dialog.
     */
    private val _deviceLost = MutableStateFlow(false)
    val deviceLost: StateFlow<Boolean> = _deviceLost.asStateFlow()

    // --- Wi-Fi list / password -----------------------------------------------------
    private val _wifiState = MutableStateFlow(WifiListUiState())
    val wifiState: StateFlow<WifiListUiState> = _wifiState.asStateFlow()

    private val _selectedNetwork = MutableStateFlow<SelectedNetwork?>(null)
    val selectedNetwork: StateFlow<SelectedNetwork?> = _selectedNetwork.asStateFlow()

    // --- Provision ---------------------------------------------------------------
    private val _provisionState = MutableStateFlow(ProvisionUiState())
    val provisionState: StateFlow<ProvisionUiState> = _provisionState.asStateFlow()
    private var provisionJob: Job? = null

    init {
        // Watch for unexpected BLE disconnects. After a successful provision
        // the firmware turns BLE off on purpose, so that disconnect is ignored.
        viewModelScope.launch {
            repository.disconnections.collect {
                val connected = _connectState.value is ConnectUiState.Connected
                val succeeded = _provisionState.value.stage == ProvisionStage.SUCCESS
                if (connected && !succeeded) {
                    _deviceLost.value = true
                }
            }
        }
    }

    /* ---- Scan ---------------------------------------------------------------- */

    /**
     * Start a new BLE scan for `PROV_*` devices (about 6 s). Clears previous results.
     * Does nothing if a scan is already running.
     */
    fun startScan() {
        if (scanJob?.isActive == true) return
        _scanState.value = ScanUiState(isScanning = true)

        scanJob = viewModelScope.launch {
            repository.scanDevices()
                .catch { e ->
                    _scanState.update {
                        it.copy(error = if (e is BluetoothOffException) ScanError.BLUETOOTH_OFF else ScanError.FAILED)
                    }
                }
                .onCompletion { _scanState.update { it.copy(isScanning = false) } }
                .collect { found ->
                    _scanState.update { state ->
                        // Replace an earlier entry for the same device. RSSI jumps
                        // ±5 dB between adverts, so blend it with the previous value
                        // to keep the estimated distance on screen steady.
                        val previous = state.devices.firstOrNull { it.address == found.address }
                        val smoothed = found.copy(rssi = DistanceEstimator.smoothRssi(previous?.rssi, found.rssi))
                        // Raw vs smoothed RSSI, for calibrating DistanceEstimator.RSSI_AT_1M.
                        Log.d(TAG, "${found.name}: raw=${found.rssi} dBm, smoothed=${smoothed.rssi} dBm")
                        val merged = state.devices.filterNot { it.address == found.address } + smoothed
                        state.copy(devices = merged.sortedByDescending { it.rssi }) // nearest first
                    }
                }
        }
    }

    /** Stop a running scan early (e.g. when the user picks a device). */
    fun stopScan() {
        scanJob?.cancel()
        scanJob = null
    }

    /* ---- Connect ------------------------------------------------------------- */

    /**
     * Choose the device to provision. Stops scanning and resets all later wizard steps.
     */
    fun selectDevice(device: ScannedDevice) {
        stopScan()
        repository.disconnect()
        _selectedDevice.value = device
        _connectState.value = ConnectUiState.Idle
        _deviceLost.value = false
        _wifiState.value = WifiListUiState()
        _selectedNetwork.value = null
        _provisionState.value = ProvisionUiState()
    }

    /** Update the PoP used for the next [connect] call. */
    fun setPop(value: String) {
        _pop.value = value
    }

    /**
     * Connect to the selected device with the current PoP. The result goes to [connectState].
     * Does nothing if no device is selected or a connection attempt is already running.
     */
    fun connect() {
        val target = _selectedDevice.value ?: return
        if (connectJob?.isActive == true) return

        _connectState.value = ConnectUiState.Connecting
        _deviceLost.value = false

        connectJob = viewModelScope.launch {
            _connectState.value = try {
                repository.connect(target, _pop.value)
                ConnectUiState.Connected
            } catch (e: CancellationException) {
                throw e // never swallow coroutine cancellation
            } catch (e: ConnectException) {
                ConnectUiState.Failed(e.error)
            } catch (e: Exception) {
                ConnectUiState.Failed(ConnectError.CONNECTION_FAILED)
            }
        }
    }

    /**
     * Drop the connection and forget the selected device. Used by Back/Cancel
     * and after a finished provisioning run.
     */
    fun disconnect() {
        connectJob?.cancel()
        provisionJob?.cancel()
        repository.disconnect()
        _connectState.value = ConnectUiState.Idle
        _deviceLost.value = false
    }

    /** Clear the "device disconnected" flag after the user has dismissed the dialog. */
    fun acknowledgeDeviceLost() {
        _deviceLost.value = false
        disconnect()
    }

    /* ---- Wi-Fi list ------------------------------------------------------------ */

    /** Ask the device to scan for Wi-Fi networks. The result goes to [wifiState]. */
    fun loadWifiNetworks() {
        if (_wifiState.value.isLoading) return
        _wifiState.update { it.copy(isLoading = true, loadFailed = false) }

        viewModelScope.launch {
            try {
                val networks = repository.scanWifi()
                _wifiState.value = WifiListUiState(networks = networks)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                _wifiState.update { it.copy(isLoading = false, loadFailed = true) }
            }
        }
    }

    /** User tapped a network from the device's scan list. */
    fun selectNetwork(network: WifiNetwork) {
        _selectedNetwork.value = SelectedNetwork(ssid = network.ssid, isOpen = network.isOpen, isManual = false)
    }

    /** User chose "Other network" to type a hidden or unlisted SSID. */
    fun selectManualNetwork() {
        _selectedNetwork.value = SelectedNetwork(ssid = "", isOpen = false, isManual = true)
    }

    /* ---- Provision ----------------------------------------------------------- */

    /**
     * Send credentials to the device and track progress in [provisionState].
     *
     * @param ssid Network name (trimmed by the caller).
     * @param password Passphrase; empty for open networks.
     */
    fun provision(ssid: String, password: String) {
        provisionJob?.cancel()
        _provisionState.value = ProvisionUiState(stage = ProvisionStage.SENDING, ssid = ssid)

        provisionJob = viewModelScope.launch {
            repository.provision(ssid, password).collect { update ->
                _provisionState.update { state ->
                    when (update) {
                        ProvisionUpdate.CredentialsSent -> state.copy(stage = ProvisionStage.APPLYING)
                        // After apply_config the device is joining the network,
                        // and the library polls its status until it's done.
                        ProvisionUpdate.CredentialsApplied -> state.copy(stage = ProvisionStage.CONNECTING)
                        ProvisionUpdate.Success -> state.copy(stage = ProvisionStage.SUCCESS)
                        is ProvisionUpdate.Failed -> state.copy(stage = ProvisionStage.FAILED, error = update.error)
                    }
                }
            }
        }
    }

    /**
     * End the wizard after success (or give up after failure): disconnect and
     * reset state, ready to provision another device.
     */
    fun finish() {
        disconnect()
        _selectedDevice.value = null
        _wifiState.value = WifiListUiState()
        _selectedNetwork.value = null
        _provisionState.value = ProvisionUiState()
        _scanState.value = ScanUiState()
    }

    /** Release the BLE connection and the EventBus registration. */
    override fun onCleared() {
        repository.close()
    }

    private companion object {
        /** Logcat tag; filter with `adb logcat -s ProvisioningVM`. */
        const val TAG = "ProvisioningVM"
    }
}
