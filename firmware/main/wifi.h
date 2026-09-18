/**
 * @file wifi.h
 * @brief Public API for the Wi-Fi station (STA) module.
 *
 * Architecture: docs/DESIGN.md §3 and §5. This module owns the esp_wifi /
 * esp_netif setup and the WIFI_EVENT / IP_EVENT handlers.
 *
 * It has two modes, chosen by the "auto-reconnect" flag:
 *  - Provisioning (auto-reconnect OFF): the provisioning manager (prov.c)
 *    drives the connection attempt with the credentials from the phone. We
 *    must NOT retry, so a wrong password is reported to the app as a failure.
 *  - Normal operation (auto-reconnect ON): the device is provisioned and
 *    should stay connected, retrying with back-off whenever the link drops.
 *
 * Dependencies: esp_wifi, esp_netif, esp_event, esp_timer, status_led.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the TCP/IP stack, the default STA netif and the Wi-Fi
 *        driver, and register the Wi-Fi/IP event handlers.
 *
 * Requirements: nvs_flash_init() and esp_event_loop_create_default() must
 * already have run (done in app_main). Does NOT start Wi-Fi; call
 * wifi_start_sta(), or let the provisioning manager start it.
 *
 * @return ESP_OK on success, otherwise the first failing esp_* error.
 */
esp_err_t wifi_init(void);

/**
 * @brief Start Wi-Fi in station mode using the credentials saved in NVS.
 *
 * Used on boot when the device is already provisioned. Also turns on
 * auto-reconnect. The actual connect happens in the WIFI_EVENT_STA_START
 * handler.
 *
 * @return ESP_OK on success, or the esp_wifi error.
 */
esp_err_t wifi_start_sta(void);

/**
 * @brief Enable or disable automatic reconnection when the link drops.
 *
 * prov.c turns this on once the phone-supplied credentials connect
 * successfully (NETWORK_PROV_WIFI_CRED_SUCCESS).
 *
 * @param enable true = retry on disconnect (normal operation),
 *               false = let the provisioning manager report the failure.
 */
void wifi_set_auto_reconnect(bool enable);

#ifdef __cplusplus
}
#endif
