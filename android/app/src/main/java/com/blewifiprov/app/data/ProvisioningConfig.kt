/*
 * ProvisioningConfig.kt: app-wide constants that must match the firmware.
 *
 * Architecture: data layer (docs/DESIGN.md §9). Anything here that also exists
 * in firmware/main/Kconfig.projbuild MUST stay in sync with it, or the app
 * won't find or talk to the device (docs/DESIGN.md §2, "Default configuration values").
 */
package com.blewifiprov.app.data

/**
 * Constants shared between the app and the ESP32-C6 firmware, plus a few timeouts.
 */
object ProvisioningConfig {

    /**
     * BLE name prefix the device advertises (`PROV_` + last 3 MAC bytes).
     * Firmware: `CONFIG_APP_PROV_NAME_PREFIX`. Scan results are filtered on it.
     */
    const val DEVICE_NAME_PREFIX = "PROV_"

    /**
     * Default Proof-of-Possession for Security 1. Firmware: `CONFIG_APP_PROV_POP`.
     * Users can edit it on the Connect screen under "Advanced". v1 uses one
     * shared PoP for all devices (decision D3).
     */
    const val DEFAULT_POP = "abcd1234"

    /**
     * BLE service UUID of the provisioning service. The firmware sets it in
     * prov.c (`s_service_uuid`). It's used as a fallback when a scan result
     * doesn't include the service UUID, which can happen when the scan
     * response isn't merged into the advertisement.
     */
    const val DEFAULT_SERVICE_UUID = "021a9004-0382-4aea-bff4-6b3f1c5adfb4"

    /**
     * Maximum time to wait for BLE connect + GATT service discovery + the
     * `proto-ver` handshake before giving up. The first connection to a
     * device can take several seconds on some phones.
     */
    const val CONNECT_TIMEOUT_MS = 20_000L

    /**
     * After a failed attempt the firmware rejects new credentials for this long
     * (PROV_FAIL_RESET_DELAY_MS in firmware/main/prov.c). Shown to the user
     * so they know how long to wait before retrying.
     */
    const val DEVICE_RETRY_COOLDOWN_SECONDS = 10

    /** Minimum WPA/WPA2/WPA3 passphrase length (IEEE 802.11i). */
    const val MIN_WPA_PASSWORD_LENGTH = 8
}
