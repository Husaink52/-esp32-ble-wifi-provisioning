/**
 * @file reset_button.h
 * @brief Public API for the BOOT-button "erase credentials" feature.
 *
 * Architecture: docs/DESIGN.md §6. This is a developer and user convenience:
 * holding the button for CONFIG_APP_RESET_HOLD_TIME_MS (default 3 s) erases
 * the saved Wi-Fi credentials and restarts the device. With no credentials,
 * it boots into BLE provisioning mode, so provisioning can be tested again
 * without reflashing.
 *
 * Dependencies: driver/gpio, esp_wifi, status_led.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the button GPIO and start the task that watches it.
 *
 * Requirements: wifi_init() must already have run, because the erase uses
 * esp_wifi_restore(), which needs the Wi-Fi driver to be initialised.
 *
 * @return ESP_OK on success, or the GPIO / task-creation error.
 */
esp_err_t reset_button_init(void);

#ifdef __cplusplus
}
#endif
