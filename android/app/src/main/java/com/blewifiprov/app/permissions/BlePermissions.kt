/*
 * BlePermissions.kt: which runtime permissions and system toggles BLE provisioning needs.
 *
 * Architecture: platform helper used by the Start and Scan screens (docs/DESIGN.md §9, Permissions).
 *
 * The rules depend on the Android version:
 *  - Android 12+ (API 31+): BLUETOOTH_SCAN + BLUETOOTH_CONNECT runtime permissions.
 *    BLUETOOTH_SCAN is declared with `neverForLocation` in the manifest, so no
 *    location permission is needed and Location services can stay off.
 *  - Android 8–11 (API 26–30): ACCESS_FINE_LOCATION runtime permission, AND
 *    Location services must be switched on, or BLE scans return no results.
 *    BLUETOOTH / BLUETOOTH_ADMIN are install-time permissions (manifest only).
 * On every version, Bluetooth itself must be on.
 */
package com.blewifiprov.app.permissions

import android.Manifest
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.pm.PackageManager
import android.location.LocationManager
import android.os.Build
import androidx.core.content.ContextCompat
import androidx.core.location.LocationManagerCompat

/** Stateless helpers for BLE permission and adapter checks. */
object BlePermissions {

    /** Runtime permissions to request on this device's Android version. */
    val required: Array<String>
        get() = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    /** true if every permission in [required] has been granted. */
    fun allGranted(context: Context): Boolean = required.all {
        ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
    }

    /**
     * true if the Bluetooth adapter exists and is on.
     * Reading `isEnabled` needs no runtime permission on any API level.
     */
    fun isBluetoothEnabled(context: Context): Boolean =
        context.getSystemService(BluetoothManager::class.java)?.adapter?.isEnabled == true

    /** true on Android versions where BLE scanning only works with Location services on. */
    val isLocationServiceRequired: Boolean
        get() = Build.VERSION.SDK_INT < Build.VERSION_CODES.S

    /** true if Location services are on, or not needed on this Android version. */
    fun isLocationReady(context: Context): Boolean {
        if (!isLocationServiceRequired) return true
        val lm = context.getSystemService(LocationManager::class.java) ?: return false
        return LocationManagerCompat.isLocationEnabled(lm)
    }
}
