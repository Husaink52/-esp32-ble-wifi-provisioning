/**
 * @file ota.h
 * @brief Public API for wireless firmware updates (OTA) over the local network.
 *
 * Architecture: docs/DESIGN.md §8 (OTA updates). Once the device is on Wi-Fi it
 * runs a small HTTP server with two endpoints:
 *
 *   GET  /info    → JSON: device name, firmware version, running slot, uptime
 *   POST /update  → firmware image (raw .bin); writes it to the spare slot,
 *                   then reboots into it
 *
 * Both require the header `X-Update-Token: <CONFIG_APP_OTA_TOKEN>`, so not
 * everyone on the Wi-Fi network can read the device details or push firmware.
 *
 * The device is reachable as `<CONFIG_APP_MDNS_PREFIX><mac suffix>.local`
 * (e.g. prov-e01c64.local) through mDNS, so you don't need its IP address.
 *
 * Safety: the bootloader rolls back to the previous firmware if the new image
 * doesn't boot. A new image is marked valid only after it reaches Wi-Fi
 * (see ota_init), so an update that breaks Wi-Fi is undone automatically.
 *
 * Dependencies: esp_http_server, esp_ota_ops, app_update, espressif/mdns,
 *               esp_event, status_led.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register for IP events so the update server starts when Wi-Fi connects.
 *
 * Call once from app_main, after wifi_init(). Nothing starts until the device
 * actually gets an IP address; on every later reconnect the already-running
 * server is left alone.
 *
 * On the first IP after an update it also marks the running image as valid,
 * which cancels the pending rollback.
 *
 * @return ESP_OK, or the error from registering the event handler.
 */
esp_err_t ota_init(void);

#ifdef __cplusplus
}
#endif
