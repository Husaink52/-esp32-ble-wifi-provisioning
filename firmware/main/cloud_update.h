/**
 * @file cloud_update.h
 * @brief Automatic firmware updates from the internet (added 2026-09-18, v1.2).
 *
 * Why this exists: the v1.1 push (ota.c) only works when the laptop and the
 * board are on the SAME Wi-Fi, because home routers block inbound connections.
 * Here the direction is reversed — the board **pulls** from a public HTTPS URL,
 * so it can update from anywhere, behind any router, with no port forwarding.
 * Design discussion and alternatives: docs/DESIGN.md §13 and docs/CLOUD_UPDATES.md.
 *
 * How it works:
 *   1. On boot (after a short delay) and then every CONFIG_APP_CLOUD_POLL_MINUTES,
 *      the board downloads a small JSON manifest:
 *         {"version":"1.2.0","url":"https://.../ble_wifi_prov.bin","notes":"..."}
 *   2. It compares `version` with its own (esp_app_get_description()->version).
 *      Equal → nothing happens; the check cost ~200 bytes.
 *   3. Different → it streams the image straight into the spare slot over
 *      HTTPS (esp_https_ota), verifies it, and reboots into it. Rollback from
 *      v1.1 still applies: firmware that can't reach Wi-Fi is undone.
 *   4. After every boot it reports what it is running to
 *      CONFIG_APP_CLOUD_REPORT_URL, so you can confirm remotely that the
 *      update landed (docs/CLOUD_UPDATES.md §Verification).
 *
 * Safety features (see cloud_update.c for the details):
 *   - **Bad-version memory:** a version that fails to confirm itself is
 *     remembered in NVS and skipped, so one broken release can't trap the
 *     board in a download-rollback loop.
 *   - **Time sync first:** HTTPS certificate checks need a real clock; the chip
 *     boots believing it is 1970, so SNTP runs before the first check.
 *   - **Poll jitter:** a random offset spreads many boards out instead of
 *     having them all hit the server on the same second.
 *
 * Dependencies: esp_https_ota, esp_http_client, esp-tls + certificate bundle,
 *               esp_netif_sntp, nvs_flash, json (cJSON), status_led.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the background updater: time sync, boot check, hourly timer, status report.
 *
 * Call once from app_main after wifi_init(). Nothing happens until the device
 * actually has an IP address; the module waits for that event itself.
 *
 * Does nothing (logs a warning) when CONFIG_APP_CLOUD_MANIFEST_URL is empty,
 * so a board can run without cloud updates configured.
 *
 * @return ESP_OK, or an error from task creation / event registration.
 */
esp_err_t cloud_update_init(void);

/**
 * @brief Ask for an update check right now, without waiting for the next poll.
 *
 * Returns immediately; the check runs on the updater task. Used by the
 * "/check-update" endpoint and handy while developing, when waiting an hour
 * for the next scheduled poll is impractical.
 */
void cloud_update_check_now(void);

/**
 * @brief Register the "/check-update" endpoint on the HTTP server ota.c runs.
 *
 * Requires the same X-Update-Token header as the other update endpoints:
 * triggering a download is a privileged action, unlike changing an LED colour.
 *
 * @param server Handle of the running server; ignored if NULL.
 * @return ESP_OK or the registration error.
 */
esp_err_t cloud_update_register_endpoint(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
