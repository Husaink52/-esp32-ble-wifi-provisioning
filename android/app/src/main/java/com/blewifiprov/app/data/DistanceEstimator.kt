/*
 * DistanceEstimator.kt: turns BLE signal strength (RSSI, dBm) into an approximate distance.
 *
 * Architecture: data layer helper, used by ProvisioningViewModel (smoothing) and
 * DeviceScanScreen (display). See docs/DESIGN.md §9, "Device scan".
 *
 * IMPORTANT, accuracy: BLE can't measure distance. We estimate it with the
 * standard log-distance path-loss model:
 *
 *     distance_m = 10 ^ ((RSSI_AT_1M - rssi) / (10 * PATH_LOSS_EXPONENT))
 *
 * Walls, bodies (even the hand holding the phone), board orientation and
 * reflections easily shift RSSI by 10–20 dB, which changes the estimate by
 * 2–5×. That's why the UI shows "≈", rounds coarsely, and smooths readings.
 * Treat it as "very close / same room / far", not as a measurement.
 */
package com.blewifiprov.app.data

import kotlin.math.pow
import kotlin.math.roundToInt

/** Stateless RSSI → distance helpers. */
object DistanceEstimator {

    /**
     * Expected RSSI (dBm) with the phone 1 m from the board, line of sight.
     *
     * Derived for the ESP32-C6 default BLE TX power (+9 dBm): free-space loss
     * at 2.4 GHz over 1 m is about 40 dB, minus roughly 10–20 dB of antenna and
     * body losses in a typical phone, gives about -50 dBm.
     * To calibrate: hold the phone 1 m from the board, note the dBm the scan
     * reports (see the Logcat tag "ProvisioningVM"), and put that value here.
     */
    const val RSSI_AT_1M = -50

    /**
     * How fast the signal fades with distance: 2.0 = free space; 2.5–3.0 is
     * typical indoors (walls, furniture). 2.5 is a middle-of-the-road indoor value.
     */
    const val PATH_LOSS_EXPONENT = 2.5

    /**
     * Weight of each new reading in the exponential moving average
     * (0 < alpha ≤ 1). 0.3 means each advertisement moves the value 30% of the
     * way toward the new reading. That smooths the ±5 dB jitter between
     * adverts while still following real movement within a few seconds.
     */
    const val SMOOTHING_ALPHA = 0.3

    /**
     * Blend a new RSSI reading into the previous smoothed value.
     *
     * @param previous Last smoothed RSSI for this device, or null for the first reading.
     * @param latest Newly received RSSI.
     * @return Smoothed RSSI, rounded to a whole dBm.
     */
    fun smoothRssi(previous: Int?, latest: Int): Int =
        if (previous == null) latest
        else (previous + SMOOTHING_ALPHA * (latest - previous)).roundToInt()

    /**
     * Estimated distance in metres for an RSSI value. See the file header for accuracy limits.
     *
     * @param rssi Signal strength in dBm (negative; closer to 0 = stronger).
     */
    fun estimateMeters(rssi: Int): Double =
        10.0.pow((RSSI_AT_1M - rssi) / (10.0 * PATH_LOSS_EXPONENT))
}
