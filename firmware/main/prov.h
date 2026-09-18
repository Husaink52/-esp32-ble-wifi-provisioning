/**
 * @file prov.h
 * @brief Public API for the BLE Wi-Fi provisioning module.
 *
 * Architecture: docs/DESIGN.md §4 (BLE endpoints) and §5 (flow). This module
 * wraps Espressif's network_provisioning manager:
 *  - decides at boot whether provisioning is needed (are credentials saved?)
 *  - starts BLE provisioning with Security 1 + PoP under the name PROV_XXXXXX
 *  - handles provisioning/BLE/security events: logs them, updates the LED,
 *    and allows a retry after wrong credentials
 *  - frees the manager (and BLE memory) when provisioning ends
 *
 * Dependencies: espressif/network_provisioning, protocomm, wifi, status_led.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the device: connect with saved credentials, or start BLE
 *        provisioning if there are none.
 *
 * Requirements: wifi_init() and status_led_init() must already have run.
 *
 * Behaviour:
 *  - Already provisioned → deinit the manager (freeing BLE memory), then
 *    wifi_start_sta(). BLE is never advertised.
 *  - Not provisioned → advertise over BLE as CONFIG_APP_PROV_NAME_PREFIX plus
 *    the last 3 MAC bytes, using Security 1 with CONFIG_APP_PROV_POP.
 *
 * Returns straight away; everything after that is event-driven.
 *
 * @return ESP_OK on success, or the network_prov_mgr / esp_wifi error.
 */
esp_err_t prov_start(void);

#ifdef __cplusplus
}
#endif
