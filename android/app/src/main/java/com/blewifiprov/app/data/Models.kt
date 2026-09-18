/*
 * Models.kt: plain data types passed from the repository to ViewModels and UI.
 *
 * Architecture: data layer (docs/DESIGN.md §9). These types wrap the
 * Espressif library's Java classes, so the UI never depends on the library
 * directly. That keeps the ViewModel and screens easy to test and to mirror on iOS later.
 */
package com.blewifiprov.app.data

import android.bluetooth.BluetoothDevice
import com.espressif.provisioning.ESPConstants

/**
 * A provisioning-mode device found during a BLE scan.
 *
 * @property name Advertised BLE name, e.g. `PROV_A1B2C3`.
 * @property address Bluetooth MAC address; used as the unique key in lists.
 * @property rssi Signal strength in dBm (closer to 0 = stronger).
 * @property serviceUuid Primary service UUID from the advertisement, if present.
 * @property bluetoothDevice Android handle needed to open the GATT connection.
 */
data class ScannedDevice(
    val name: String,
    val address: String,
    val rssi: Int,
    val serviceUuid: String?,
    val bluetoothDevice: BluetoothDevice,
)

/**
 * A Wi-Fi network the ESP32-C6 saw in its own scan (`prov-scan` endpoint).
 *
 * @property ssid Network name.
 * @property rssi Signal strength in dBm, measured by the device, not the phone.
 * @property authMode Security mode code from the device, one of
 *                    `ESPConstants.WIFI_OPEN`, `WIFI_WPA2_PSK`, and so on.
 */
data class WifiNetwork(
    val ssid: String,
    val rssi: Int,
    val authMode: Int,
) {
    /** true if the network needs no password. */
    val isOpen: Boolean get() = authMode == ESPConstants.WIFI_OPEN.toInt()
}

/** Why connecting to a device (BLE + secure session) failed. */
enum class ConnectError {
    /** Bluetooth was switched off. */
    BLUETOOTH_OFF,
    /** BLE connection or GATT discovery failed (out of range, device busy, ...). */
    CONNECTION_FAILED,
    /** No response within [ProvisioningConfig.CONNECT_TIMEOUT_MS]. */
    TIMEOUT,
    /** BLE worked, but the Security 1 handshake failed. Almost always a wrong PoP. */
    SESSION_FAILED,
}

/** Why sending Wi-Fi credentials failed. */
enum class ProvisionError {
    /** Device reported an authentication error: wrong Wi-Fi password. */
    WRONG_PASSWORD,
    /** Device couldn't find the SSID (out of range, 5 GHz only, typo). */
    NETWORK_NOT_FOUND,
    /** Device refused the credentials. Usually still in its retry cooldown after a failure. */
    CONFIG_REJECTED,
    /** The BLE link or secure session was lost during provisioning. */
    DEVICE_DISCONNECTED,
    /** Any other or unknown failure. */
    UNKNOWN,
}

/**
 * Progress updates emitted by [ProvisioningRepository.provision], in order:
 * [CredentialsSent] → [CredentialsApplied] → [Success] or [Failed].
 */
sealed interface ProvisionUpdate {
    /** `set_config` accepted: the device has the SSID and password. */
    data object CredentialsSent : ProvisionUpdate
    /** `apply_config` accepted: the device is now trying to join the network. */
    data object CredentialsApplied : ProvisionUpdate
    /** Device reported CONNECTED (it has an IP address). Terminal. */
    data object Success : ProvisionUpdate
    /** Provisioning failed. Terminal. */
    data class Failed(val error: ProvisionError) : ProvisionUpdate
}

/**
 * Exception used internally by the repository to carry a [ConnectError] to the ViewModel.
 */
class ConnectException(val error: ConnectError, cause: Throwable? = null) :
    Exception("Connect failed: $error", cause)

/** Thrown by the BLE scan flow when scanning can't start because Bluetooth is off. */
class BluetoothOffException : Exception("Bluetooth is turned off")
