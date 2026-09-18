/**
 * @file ota.c
 * @brief HTTP server for wireless firmware updates, plus mDNS so the board is easy to find.
 *
 * Architecture: see ota.h and docs/DESIGN.md §8 (OTA updates).
 *
 * Flow of an update:
 *   1. Laptop POSTs the new .bin to http://prov-xxxxxx.local/update
 *      with the header X-Update-Token.
 *   2. We stream the body straight into the spare app slot (esp_ota_write),
 *      never buffering the whole image, which wouldn't fit in RAM.
 *   3. esp_ota_end() checks the image, esp_ota_set_boot_partition() points the
 *      bootloader at it, and we reboot 1 s later (after the HTTP reply is sent).
 *   4. The new firmware boots in "pending verify" state. Once it reaches Wi-Fi,
 *      ota_init's IP handler marks it valid. If it never gets that far (or
 *      crashes on boot), the bootloader rolls back to the previous slot.
 *
 * Threading: handlers run on the HTTP server task. Writing flash from there is
 * fine; the Wi-Fi stack keeps running on its own tasks.
 *
 * Dependencies: esp_http_server, esp_ota_ops, app_update, espressif/mdns,
 *               esp_event, esp_timer, status_led.
 */
#include "ota.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "cloud_update.h" // shares this server for the manual "check now" endpoint
#include "color_api.h"   // shares this module's HTTP server for the colour endpoints
#include "status_led.h"

/** Log tag for this module. */
static const char *TAG = "ota";

/** Chunk size for streaming the upload into flash. Small enough to stay off the stack budget. */
#define OTA_RECV_CHUNK      1024

/** Delay before rebooting, so the HTTP response reaches the laptop first. */
#define REBOOT_DELAY_MS     1000

/** Handle of the running HTTP server, or NULL if it hasn't started yet. */
static httpd_handle_t s_server = NULL;

/** One-shot timer used to reboot shortly after a successful update. */
static esp_timer_handle_t s_reboot_timer = NULL;

/* ---- Helpers ------------------------------------------------------------- */

/**
 * @brief esp_timer callback: restart the chip after a finished update.
 */
static void reboot_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Rebooting into the new firmware");
    esp_restart();
}

/**
 * @brief Check the X-Update-Token header against CONFIG_APP_OTA_TOKEN.
 *
 * Note: this is a plain shared secret over plain HTTP, which is fine on a
 * trusted home network. HTTPS with a device certificate would be the next step
 * for anything more exposed.
 *
 * @return true if the token matches. On failure it has already answered 401.
 */
static bool token_ok(httpd_req_t *req)
{
    char token[64] = {0};
    size_t len = httpd_req_get_hdr_value_len(req, "X-Update-Token");

    if (len > 0 && len < sizeof(token) &&
        httpd_req_get_hdr_value_str(req, "X-Update-Token", token, sizeof(token)) == ESP_OK &&
        strcmp(token, CONFIG_APP_OTA_TOKEN) == 0) {
        return true;
    }

    ESP_LOGW(TAG, "Rejected request with a missing or wrong update token");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "Missing or wrong X-Update-Token header\n");
    return false;
}

/* ---- HTTP handlers ------------------------------------------------------- */

/**
 * @brief GET /info: report what's running, as JSON.
 *
 * Useful to confirm an update actually took effect: the compile time and the
 * running slot (ota_0 / ota_1) change after a successful update.
 */
static esp_err_t info_get_handler(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return ESP_OK;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *desc = esp_app_get_description();

    // First 4 bytes of the ELF SHA-256 as hex: a build fingerprint that changes
    // whenever the code does. Added 2026-09-18 because `compiled` (the app_desc
    // timestamp) is NOT regenerated on incremental builds, so it can report an
    // old time for new firmware and make a successful update look like a no-op.
    char elf_sha[9] = {0};
    for (int i = 0; i < 4; i++) {
        snprintf(elf_sha + i * 2, 3, "%02x", desc->app_elf_sha256[i]);
    }

    char body[380];
    snprintf(body, sizeof(body),
             "{\"project\":\"%s\",\"version\":\"%s\",\"compiled\":\"%s %s\","
             "\"elf_sha\":\"%s\",\"idf\":\"%s\",\"slot\":\"%s\",\"uptime_s\":%lld}\n",
             desc->project_name, desc->version, desc->date, desc->time,
             elf_sha, desc->idf_ver, running ? running->label : "?",
             esp_timer_get_time() / 1000000);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/**
 * @brief POST /update: receive a firmware image and install it.
 *
 * Body: the raw contents of build/ble_wifi_prov.bin.
 * Replies 200 and reboots, or an error status without touching the boot slot.
 */
static esp_err_t update_post_handler(httpd_req_t *req)
{
    if (!token_ok(req)) {
        return ESP_OK;
    }

    // The slot that is NOT running: safe to overwrite.
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "No OTA partition available\n");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Update started: %d bytes into partition '%s'", req->content_len, target->label);
    status_led_set(STATUS_LED_UPDATING);

    esp_ota_handle_t handle = 0;
    // OTA_WITH_SEQUENTIAL_WRITES erases as it goes, so a short image doesn't
    // pay for erasing the whole 1.94 MB slot up front.
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        status_led_set(STATUS_LED_WIFI_CONNECTED);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Could not start the update\n");
        return ESP_FAIL;
    }

    char buf[OTA_RECV_CHUNK];
    int remaining = req->content_len;
    int received_total = 0;
    int next_log_percent = 10;

    while (remaining > 0) {
        int chunk = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, buf, chunk);

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue; // slow network: wait for more data instead of failing
        }
        if (received <= 0) {
            ESP_LOGE(TAG, "Upload aborted after %d of %d bytes", received_total, req->content_len);
            esp_ota_abort(handle);
            status_led_set(STATUS_LED_WIFI_CONNECTED);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "Upload interrupted\n");
            return ESP_FAIL;
        }

        err = esp_ota_write(handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            status_led_set(STATUS_LED_WIFI_CONNECTED);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "Flash write failed\n");
            return ESP_FAIL;
        }

        remaining -= received;
        received_total += received;

        // Progress every 10%, so the serial log shows movement without flooding.
        int percent = (received_total * 100) / req->content_len;
        if (percent >= next_log_percent) {
            ESP_LOGI(TAG, "Update progress: %d%%", percent);
            next_log_percent += 10;
        }
    }

    // Verifies the image header and checksum before we commit to it.
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Image rejected: %s", esp_err_to_name(err));
        status_led_set(STATUS_LED_WIFI_CONNECTED);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req,
                           (err == ESP_ERR_OTA_VALIDATE_FAILED)
                               ? "Invalid firmware image\n"
                               : "Update could not be finished\n");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        status_led_set(STATUS_LED_WIFI_CONNECTED);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Could not switch to the new firmware\n");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Update installed to '%s', rebooting in %d ms", target->label, REBOOT_DELAY_MS);
    httpd_resp_sendstr(req, "OK: update installed, rebooting\n");

    // Reboot from a timer, so this response is fully sent first.
    esp_timer_start_once(s_reboot_timer, (uint64_t)REBOOT_DELAY_MS * 1000ULL);
    return ESP_OK;
}

/* ---- Server + mDNS lifecycle --------------------------------------------- */

/**
 * @brief Start the HTTP server and register the two handlers.
 */
static void start_server(void)
{
    if (s_server != NULL) {
        return; // already running (e.g. after a Wi-Fi reconnect)
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_APP_OTA_PORT;
    // Flashing from a request handler needs more stack than the default.
    config.stack_size = 8192;
    // Default is 8 handlers; we register 4 (/update, /info, /, /color) and want
    // headroom for future endpoints without a silent registration failure.
    config.max_uri_handlers = 12;
    // Wait up to 10 s for the next chunk before reporting a timeout, which the
    // upload loop treats as "keep waiting" rather than an error.
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    // Close the oldest connection when a new one arrives, so a dropped laptop
    // connection can't block the next update attempt.
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the update server: %s", esp_err_to_name(err));
        s_server = NULL;
        return;
    }

    static const httpd_uri_t update_uri = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = update_post_handler,
    };
    static const httpd_uri_t info_uri = {
        .uri = "/info",
        .method = HTTP_GET,
        .handler = info_get_handler,
    };
    httpd_register_uri_handler(s_server, &update_uri);
    httpd_register_uri_handler(s_server, &info_uri);

    // Added 2026-09-18: the colour feature hangs its own endpoints (/ and
    // /color) off this same server, so the device runs one HTTP server on one
    // port instead of two competing ones.
    color_api_register(s_server);

    // Added 2026-09-18 (v1.2): lets you trigger an internet update check
    // immediately instead of waiting for the hourly poll.
    cloud_update_register_endpoint(s_server);

    ESP_LOGI(TAG, "Update server listening on port %d (POST /update, GET /info)",
             CONFIG_APP_OTA_PORT);
}

/**
 * @brief Publish the device on the local network as <prefix><mac>.local.
 *
 * Lets the push script use a stable name instead of an IP address that the
 * router may change. Also advertises an _http._tcp service, which is how the
 * planned "My devices" screen in the app will discover devices.
 */
static void start_mdns(void)
{
    static bool started = false;
    if (started) {
        return;
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable: %s (use the IP address instead)", esp_err_to_name(err));
        return;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char hostname[32];
    // Lower case: mDNS names are case-insensitive but tools display them as sent.
    snprintf(hostname, sizeof(hostname), "%s%02x%02x%02x",
             CONFIG_APP_MDNS_PREFIX, mac[3], mac[4], mac[5]);

    mdns_hostname_set(hostname);
    mdns_instance_name_set("ESP32-C6 Wi-Fi provisioning device");
    mdns_service_add(NULL, "_http", "_tcp", CONFIG_APP_OTA_PORT, NULL, 0);

    started = true;
    ESP_LOGI(TAG, "Reachable as %s.local", hostname);
}

/**
 * @brief IP event handler: start the services once the device is online.
 *
 * Also confirms a freshly installed firmware. Reaching this point means the
 * new image can join Wi-Fi, so the pending rollback is cancelled. Without
 * this, the next reboot would return to the previous firmware.
 */
static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "New firmware reached Wi-Fi: marking it valid (no rollback)");
        esp_ota_mark_app_valid_cancel_rollback();
    }

    start_mdns();
    start_server();
}

/* ---- Public API (documented in ota.h) ----------------------------------- */

esp_err_t ota_init(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = reboot_timer_cb,
        .name = "ota_reboot",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_reboot_timer);
    if (err != ESP_OK) {
        return err;
    }

    return esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL);
}
