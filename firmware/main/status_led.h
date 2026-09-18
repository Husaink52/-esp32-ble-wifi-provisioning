/**
 * @file status_led.h
 * @brief Public API for the on-board RGB status LED.
 *
 * Architecture: the firmware's "user feedback" module (docs/DESIGN.md §6).
 * The other modules (prov.c, wifi.c, reset_button.c) only say WHAT state the
 * device is in; this module decides HOW that looks (colour, blink rate).
 *
 * Hardware: one WS2812 addressable LED on CONFIG_APP_STATUS_LED_GPIO
 * (GPIO8 on the ESP32-C6-DevKitC-1), driven via the espressif/led_strip
 * component.
 *
 * Thread safety: every function here may be called from any task, including
 * the default event-loop task that runs our Wi-Fi/provisioning handlers.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Persistent LED states, one per row of the LED table in docs/DESIGN.md §6.
 *
 * A persistent state stays until another status_led_set() call.
 * The temporary "failure" animation is separate: see status_led_signal_failure().
 */
typedef enum {
    STATUS_LED_OFF = 0,          /**< LED dark (e.g. just before a reset/restart). */
    STATUS_LED_PROVISIONING,     /**< Blue slow blink: BLE provisioning waiting for a phone. */
    STATUS_LED_BLE_CONNECTED,    /**< Blue solid: a phone is connected over BLE. */
    STATUS_LED_WIFI_CONNECTING,  /**< Yellow fast blink: trying to join the Wi-Fi network. */
    STATUS_LED_WIFI_CONNECTED,   /**< Green solid: associated and got an IP address. */
    STATUS_LED_UPDATING,         /**< Purple fast blink: receiving a firmware update over Wi-Fi. */
    STATUS_LED_CUSTOM,           /**< Solid colour chosen by status_led_set_custom() (colour API). */
} status_led_state_t;

/**
 * @brief Initialise the LED driver and start the LED animation task.
 *
 * Must be called once, early in app_main(), before any other status_led_*()
 * call. The LED starts in STATUS_LED_OFF.
 *
 * @return ESP_OK on success, or the error from the led_strip driver / task
 *         creation. The caller may choose to continue without an LED.
 */
esp_err_t status_led_init(void);

/**
 * @brief Switch the LED to a new persistent state.
 *
 * Returns immediately; the animation task picks up the change within a few
 * milliseconds (it is woken with a task notification). If a failure animation
 * is running, it finishes first and then shows this new state.
 *
 * @param state New state to display.
 */
void status_led_set(status_led_state_t state);

/**
 * @brief Show a solid colour of your choice and hold it until the next state change.
 *
 * Added 2026-09-18 for the colour API (docs/COLOR_API.md): a request like
 * GET /color?n=1 turns the LED red. Sets the state to STATUS_LED_CUSTOM.
 *
 * Note: a later Wi-Fi event (disconnect/reconnect) still overrides the colour,
 * because connection status matters more than a demo colour.
 *
 * @param r Red 0-255.
 * @param g Green 0-255.
 * @param b Blue 0-255.
 */
void status_led_set_custom(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Play the "failed" animation: red blinks 3 times, then go back to the
 *        current persistent state.
 *
 * Used when Wi-Fi credentials from the app did not work (wrong password or
 * network not found). Returns immediately; the animation runs in the LED task.
 */
void status_led_signal_failure(void);

#ifdef __cplusplus
}
#endif
