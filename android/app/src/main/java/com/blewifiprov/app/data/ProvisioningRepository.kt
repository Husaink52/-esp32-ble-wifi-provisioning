/*
 * ProvisioningRepository.kt: the ONLY code in the app that talks to Espressif's provisioning library.
 *
 * Architecture: data layer (docs/DESIGN.md §3 and §9). Layers: UI → ViewModel → Repository → library → BLE → ESP32-C6.
 *
 * Responsibilities:
 *  - Turn the library's callback and EventBus APIs into Kotlin coroutines and Flows:
 *      scanDevices()  → Flow<ScannedDevice>     (BLE scan, ~6 s, library timeout)
 *      connect()      → suspend                  (BLE connect + proto-ver + Security 1 session)
 *      scanWifi()     → suspend List<WifiNetwork> (prov-scan endpoint)
 *      provision()    → Flow<ProvisionUpdate>    (prov-config set/apply/poll status)
 *      disconnect()
 *  - Report unexpected BLE disconnections through [disconnections].
 *
 * Threading: the library creates Android Handlers without a Looper argument
 * (e.g. in BleScanner), so its entry points must run on the MAIN thread. Every
 * call into the library goes through Dispatchers.Main. Library callbacks can
 * arrive on binder or background threads; coroutine continuations and
 * callbackFlow channels are thread-safe, so that's fine.
 *
 * Permissions: every BLE call needs runtime permissions (see
 * permissions/BlePermissions.kt). The UI only reaches screens that use this
 * repository after they're granted, hence @SuppressLint("MissingPermission").
 *
 * Library reference: github.com/espressif/esp-idf-provisioning-android (tag lib-2.4.4).
 */
package com.blewifiprov.app.data

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.le.ScanResult
import android.content.Context
import android.util.Log
import com.espressif.provisioning.ESPConstants
import com.espressif.provisioning.ESPDevice
import com.espressif.provisioning.ESPProvisionManager
import com.espressif.provisioning.WiFiAccessPoint
import com.espressif.provisioning.listeners.BleScanListener
import com.espressif.provisioning.listeners.ProvisionListener
import com.espressif.provisioning.listeners.ResponseListener
import com.espressif.provisioning.listeners.WiFiScanListener
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.greenrobot.eventbus.EventBus
import kotlin.coroutines.resume

/**
 * Coroutine-friendly wrapper around `ESPProvisionManager` and `ESPDevice`.
 *
 * Create one per ViewModel and call [close] when it's no longer needed, which
 * unregisters the EventBus subscriber.
 *
 * @param context Any context; only the application context is kept, so an Activity isn't leaked.
 */
@SuppressLint("MissingPermission") // Permissions are checked by the UI before any call; see file header.
class ProvisioningRepository(context: Context) {

    private val appContext = context.applicationContext

    /** Library singleton: BLE scanning and ESPDevice creation. */
    private val manager: ESPProvisionManager = ESPProvisionManager.getInstance(appContext)

    /** The device currently connected (or being connected), or null. */
    private var device: ESPDevice? = null

    /**
     * Every connection event from the library, as raw `ESPConstants.EVENT_*` values.
     * A buffer instead of replay: events are only meaningful to whoever is listening at that moment.
     */
    private val connectionEvents = MutableSharedFlow<Short>(extraBufferCapacity = 16)

    /** Forwards library EventBus events into [connectionEvents]. */
    private val eventSubscriber = DeviceConnectionEventSubscriber { type ->
        Log.d(TAG, "DeviceConnectionEvent type=$type")
        connectionEvents.tryEmit(type)
    }

    init {
        EventBus.getDefault().register(eventSubscriber)
    }

    /**
     * Emits whenever the BLE link to the device drops without us asking.
     * A disconnect we start ([disconnect]) closes GATT first, so the library doesn't report it.
     */
    val disconnections: Flow<Unit> = connectionEvents
        .filter { it == ESPConstants.EVENT_DEVICE_DISCONNECTED }
        .map { }

    /**
     * Scan for devices in provisioning mode (name starts with [ProvisioningConfig.DEVICE_NAME_PREFIX]).
     *
     * The same device may be emitted several times as new advertisements arrive
     * (with updated RSSI), so callers should de-duplicate by [ScannedDevice.address].
     * The flow completes when the library's scan window (about 6 s) ends.
     * Cancelling collection stops the scan early.
     *
     * @throws BluetoothOffException (as a flow error) if Bluetooth is off.
     */
    fun scanDevices(): Flow<ScannedDevice> = callbackFlow {
        val listener = object : BleScanListener {
            override fun scanStartFailed() {
                // The library calls this only when the adapter is disabled.
                close(BluetoothOffException())
            }

            override fun onPeripheralFound(btDevice: BluetoothDevice, scanResult: ScanResult) {
                val record = scanResult.scanRecord ?: return
                val name = record.deviceName ?: return
                trySend(
                    ScannedDevice(
                        name = name,
                        address = btDevice.address,
                        rssi = scanResult.rssi,
                        serviceUuid = record.serviceUuids?.firstOrNull()?.toString(),
                        bluetoothDevice = btDevice,
                    )
                )
            }

            override fun scanCompleted() {
                close() // normal end of the scan window
            }

            override fun onFailure(e: Exception) {
                close(e)
            }
        }

        manager.searchBleEspDevices(ProvisioningConfig.DEVICE_NAME_PREFIX, listener)

        // Runs on cancellation or after close(). Stopping an already-stopped
        // scan is harmless in the library.
        awaitClose { runCatching { manager.stopBleScan() } }
    }.flowOn(Dispatchers.Main)

    /**
     * Connect to [target] over BLE and set up the encrypted Security 1 session.
     *
     * The session is set up here rather than lazily on the first request, so a
     * wrong PoP shows up on the Connect screen instead of later on the Wi-Fi screen.
     *
     * Steps:
     *  1. Create an ESPDevice (BLE transport, Security 1) and set the PoP.
     *  2. connectBLEDevice → GATT connect, MTU, service discovery, `proto-ver`.
     *     The library then posts EVENT_DEVICE_CONNECTED or EVENT_DEVICE_CONNECTION_FAILED.
     *  3. initSession → Security 1 handshake on `prov-session`.
     *
     * @param target Device picked from [scanDevices].
     * @param pop Proof-of-Possession; must equal the firmware's CONFIG_APP_PROV_POP.
     * @throws ConnectException with the specific [ConnectError].
     */
    suspend fun connect(target: ScannedDevice, pop: String): Unit = withContext(Dispatchers.Main) {
        disconnect() // drop any earlier connection so only one device is ever active

        val espDevice = manager.createESPDevice(
            ESPConstants.TransportType.TRANSPORT_BLE,
            ESPConstants.SecurityType.SECURITY_1,
        )
        espDevice.proofOfPossession = pop
        device = espDevice

        val serviceUuid = target.serviceUuid ?: ProvisioningConfig.DEFAULT_SERVICE_UUID

        coroutineScope {
            // Start listening BEFORE connecting (UNDISPATCHED subscribes right away),
            // so a fast CONNECTED event can't be missed.
            val outcome = async(start = CoroutineStart.UNDISPATCHED) {
                connectionEvents.first {
                    it == ESPConstants.EVENT_DEVICE_CONNECTED ||
                        it == ESPConstants.EVENT_DEVICE_CONNECTION_FAILED
                }
            }

            espDevice.connectBLEDevice(target.bluetoothDevice, serviceUuid)

            val event = withTimeoutOrNull(ProvisioningConfig.CONNECT_TIMEOUT_MS) { outcome.await() }
            if (event == null) {
                outcome.cancel()
                disconnect()
                throw ConnectException(ConnectError.TIMEOUT)
            }
            if (event != ESPConstants.EVENT_DEVICE_CONNECTED) {
                disconnect()
                throw ConnectException(ConnectError.CONNECTION_FAILED)
            }
        }

        // Security 1 handshake. The library reads sec_ver from proto-ver and
        // uses the PoP set above.
        val sessionError: Exception? = suspendCancellableCoroutine { cont ->
            espDevice.initSession(object : ResponseListener {
                override fun onSuccess(returnData: ByteArray?) {
                    if (cont.isActive) cont.resume(null)
                }

                override fun onFailure(e: Exception) {
                    if (cont.isActive) cont.resume(e)
                }
            })
        }
        if (sessionError != null) {
            Log.w(TAG, "Session setup failed", sessionError)
            disconnect()
            throw ConnectException(ConnectError.SESSION_FAILED, sessionError)
        }
    }

    /**
     * Ask the connected device to scan for Wi-Fi networks (`prov-scan` endpoint).
     *
     * The ESP32-C6 does the scan itself, so the list shows what the DEVICE can
     * see (2.4 GHz only), which is what matters for provisioning.
     *
     * @return Networks with a non-empty SSID, one entry per SSID (strongest
     *         signal wins), sorted strongest first.
     * @throws IllegalStateException if no device is connected.
     * @throws Exception from the library if the scan request fails.
     */
    suspend fun scanWifi(): List<WifiNetwork> = withContext(Dispatchers.Main) {
        val espDevice = checkNotNull(device) { "No device connected" }

        val accessPoints: List<WiFiAccessPoint> = suspendCancellableCoroutine { cont ->
            espDevice.scanNetworks(object : WiFiScanListener {
                override fun onWifiListReceived(wifiList: ArrayList<WiFiAccessPoint>?) {
                    if (cont.isActive) cont.resume(wifiList.orEmpty())
                }

                override fun onWiFiScanFailed(e: Exception) {
                    if (cont.isActive) cont.resumeWith(Result.failure(e))
                }
            })
        }

        accessPoints
            .filter { !it.wifiName.isNullOrBlank() }          // hidden networks show up with no name
            .groupBy { it.wifiName }
            .map { (_, sameSsid) -> sameSsid.maxBy { it.rssi } } // several APs/mesh nodes share one SSID
            .map { WifiNetwork(ssid = it.wifiName, rssi = it.rssi, authMode = it.security) }
            .sortedByDescending { it.rssi }
    }

    /**
     * Send Wi-Fi credentials to the device and follow the result.
     *
     * The library runs `set_config` → `apply_config`, then polls `get_status`
     * every 5 s until the device reports connected or failed. Each step is
     * emitted as a [ProvisionUpdate]. The flow completes after [ProvisionUpdate.Success]
     * or [ProvisionUpdate.Failed].
     *
     * After success the firmware ends provisioning and turns BLE off, so expect
     * the link to drop. Callers should ignore [disconnections] after success.
     *
     * @param ssid Network name.
     * @param password Passphrase; empty for open networks. Never logged.
     */
    fun provision(ssid: String, password: String): Flow<ProvisionUpdate> = callbackFlow {
        val espDevice = device
        if (espDevice == null) {
            send(ProvisionUpdate.Failed(ProvisionError.DEVICE_DISCONNECTED))
            close()
            return@callbackFlow
        }

        /** Emit a terminal failure and finish the flow. */
        fun fail(error: ProvisionError, e: Exception? = null) {
            Log.w(TAG, "Provisioning failed: $error", e)
            trySend(ProvisionUpdate.Failed(error))
            close()
        }

        espDevice.provision(ssid, password, object : ProvisionListener {
            override fun createSessionFailed(e: Exception) =
                fail(ProvisionError.DEVICE_DISCONNECTED, e)

            override fun wifiConfigSent() {
                trySend(ProvisionUpdate.CredentialsSent)
            }

            // The device refuses set_config while it's still in the failure
            // state from a previous attempt (see the firmware's
            // PROV_FAIL_RESET_DELAY_MS), so this usually means "wait and retry".
            override fun wifiConfigFailed(e: Exception) =
                fail(ProvisionError.CONFIG_REJECTED, e)

            override fun wifiConfigApplied() {
                trySend(ProvisionUpdate.CredentialsApplied)
            }

            override fun wifiConfigApplyFailed(e: Exception) =
                fail(ProvisionError.CONFIG_REJECTED, e)

            override fun provisioningFailedFromDevice(
                failureReason: ESPConstants.ProvisionFailureReason,
            ) = fail(
                when (failureReason) {
                    ESPConstants.ProvisionFailureReason.AUTH_FAILED -> ProvisionError.WRONG_PASSWORD
                    ESPConstants.ProvisionFailureReason.NETWORK_NOT_FOUND -> ProvisionError.NETWORK_NOT_FOUND
                    ESPConstants.ProvisionFailureReason.DEVICE_DISCONNECTED -> ProvisionError.DEVICE_DISCONNECTED
                    else -> ProvisionError.UNKNOWN
                }
            )

            override fun deviceProvisioningSuccess() {
                trySend(ProvisionUpdate.Success)
                close()
            }

            override fun onProvisioningFailed(e: Exception) =
                fail(ProvisionError.UNKNOWN, e)
        })

        // The library can't cancel a provisioning run in progress. Leaving the
        // screen just stops listening.
        awaitClose { }
    }.flowOn(Dispatchers.Main)

    /**
     * Close the BLE connection to the current device, if any. Safe to call repeatedly.
     */
    fun disconnect() {
        device?.let { dev ->
            runCatching { dev.disconnectDevice() }
                .onFailure { Log.w(TAG, "disconnectDevice failed", it) }
        }
        device = null
    }

    /**
     * Release everything: disconnect and unregister from EventBus.
     * Call from `ViewModel.onCleared()`. Don't use the instance afterwards.
     */
    fun close() {
        disconnect()
        EventBus.getDefault().unregister(eventSubscriber)
    }

    private companion object {
        const val TAG = "ProvisioningRepo"
    }
}
