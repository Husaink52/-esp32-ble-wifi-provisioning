/*
 * DeviceConnectionEventSubscriber.kt: receives BLE connection events from the Espressif library.
 *
 * Architecture: data layer, used only by ProvisioningRepository (docs/DESIGN.md §9).
 *
 * Why this class exists: the Espressif library doesn't report connect or
 * disconnect through a listener. It posts `DeviceConnectionEvent`s on the
 * global greenrobot EventBus. EventBus finds `@Subscribe` methods by
 * reflection and can only call them on a *public* class, so this small public
 * class forwards each event to a plain Kotlin callback.
 */
package com.blewifiprov.app.data

import com.espressif.provisioning.DeviceConnectionEvent
import org.greenrobot.eventbus.Subscribe
import org.greenrobot.eventbus.ThreadMode

/**
 * Forwards EventBus `DeviceConnectionEvent`s to [onEvent].
 *
 * @param onEvent Called on the main thread with the event type, one of
 *                `ESPConstants.EVENT_DEVICE_CONNECTED`,
 *                `EVENT_DEVICE_CONNECTION_FAILED` or `EVENT_DEVICE_DISCONNECTED`.
 */
class DeviceConnectionEventSubscriber(private val onEvent: (Short) -> Unit) {

    /**
     * EventBus entry point. MAIN thread mode, so [onEvent] never races with
     * UI state updates.
     */
    @Subscribe(threadMode = ThreadMode.MAIN)
    fun onDeviceConnectionEvent(event: DeviceConnectionEvent) {
        onEvent(event.eventType)
    }
}
