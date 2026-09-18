/**
 * @file cloud_update.c
 * @brief Implementation of automatic internet firmware updates (v1.2, 2026-09-18).
 *
 * Architecture: see cloud_update.h and docs/CLOUD_UPDATES.md.
 *
 * One background task owns everything here, because HTTPS downloads block for
 * seconds at a time and must never run on the event loop or the HTTP server
 * task. The task wakes on three things:
 *   - the first IP address after boot (initial check + status report),
 *   - a periodic timer (CONFIG_APP_CLOUD_POLL_MINUTES),
 *   - a manual request (cloud_update_check_now / the /check-update endpoint).
 *
 * Update decision, in order:
 *   1. Download the manifest (a few hundred bytes of JSON).
 *   2. If manifest.version == running version → done, nothing downloaded.
 *   3. If manifest.version == the version remembered as BAD → skip it, so a
 *      broken release cannot cause an endless download → rollback → download loop.
 *   4. Otherwise download and install it, then reboot.
 *
 * Rollback interaction (ota.c owns the "mark valid" step): a freshly installed
 * image is provisional until it reaches Wi-Fi. If it never does, the bootloader
 * reverts to the previous slot. On the next boot we notice that the version we
 * tried to install is not the one running, conclude it failed, and blacklist it.
 *
 * Dependencies: esp_https_ota, esp_http_client, esp_netif_sntp, nvs_flash,
 *               json (cJSON), esp_app_desc, status_led, ota.c's HTTP server.
 */
#include "cloud_update.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "status_led.h"

/** Log tag for this module. */
static const char *TAG = "cloud_upd";

/* ---- Tunables ------------------------------------------------------------ */

/** Largest manifest we accept, to bound the receive buffer. A manifest is ~200 bytes. */
#define MANIFEST_MAX_BYTES      1024

/** HTTP timeout for the manifest and report requests (the OTA download has its own). */
#define HTTP_TIMEOUT_MS         10000

/** Delay after getting an IP before the first check, so Wi-Fi and SNTP settle first. */
#define FIRST_CHECK_DELAY_MS    15000

/** Stack for the updater task: TLS plus the OTA state machine need a generous frame. */
#define TASK_STACK_SIZE         8192

/** Below the Wi-Fi/BLE tasks: an update must never starve the network stack. */
#define TASK_PRIORITY           4

/** NVS namespace and keys for the "this version failed" memory. */
#define NVS_NAMESPACE           "cloud_upd"
#define NVS_KEY_PENDING         "pending"   /* version we are installing right now */
#define NVS_KEY_BAD             "bad"       /* version that failed to come up      */

/* ---- Module state -------------------------------------------------------- */

/** Updater task handle; also the target for wake-up notifications. */
static TaskHandle_t s_task = NULL;

/** Periodic poll timer. */
static esp_timer_handle_t s_poll_timer = NULL;

/** Set once the device has an IP address; checks only run after that. */
static volatile bool s_online = false;

/* ---- Small helpers ------------------------------------------------------- */

/**
 * @brief Read a string from our NVS namespace into @p out. Empty string if unset.
 */
static void nvs_get_str_or_empty(const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t len = out_len;
    if (nvs_get_str(handle, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
    nvs_close(handle);
}

/**
 * @brief Write (or clear, when @p value is NULL/empty) a string in our NVS namespace.
 */
static void nvs_set_str_or_erase(const char *key, const char *value)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (value && value[0]) {
        nvs_set_str(handle, key, value);
    } else {
        nvs_erase_key(handle, key);
    }
    nvs_commit(handle);
    nvs_close(handle);
}

/** @brief Device identifier used in reports: the MAC suffix, e.g. "e01c64". */
static void device_id(char *out, size_t out_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_len, "%02x%02x%02x", mac[3], mac[4], mac[5]);
}

/** @brief Version string of the firmware currently running. */
static const char *running_version(void)
{
    return esp_app_get_description()->version;
}

/* ---- Time sync ----------------------------------------------------------- */

/**
 * @brief Set the clock from an NTP server, and wait (briefly) for it to take effect.
 *
 * Why it matters: HTTPS verifies the server certificate's validity dates. The
 * chip boots believing it is 1 Jan 1970, so every certificate looks
 * "not yet valid" and downloads fail with a confusing TLS error. One sync per
 * boot is enough.
 */
static void sync_clock(void)
{
    struct tm timeinfo = {0};
    time_t now = 0;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year > (2020 - 1900)) {
        return; // already set (e.g. after a soft restart)
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_APP_CLOUD_NTP_SERVER);
    if (esp_netif_sntp_init(&config) != ESP_OK) {
        ESP_LOGW(TAG, "Could not start time sync; HTTPS may fail");
        return;
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
        ESP_LOGW(TAG, "Time sync timed out; HTTPS may fail until it completes");
        return;
    }
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_LOGI(TAG, "Clock set: %04d-%02d-%02d %02d:%02d UTC",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min);
}

/* ---- Manifest ------------------------------------------------------------ */

/**
 * @brief Download the manifest and extract `version` and `url`.
 *
 * Manifest format (published next to the firmware; see docs/CLOUD_UPDATES.md):
 *   {"version":"1.2.0","url":"https://.../ble_wifi_prov.bin","notes":"optional"}
 *
 * @param[out] version  Buffer for the version string.
 * @param      ver_len  Size of @p version.
 * @param[out] url      Buffer for the firmware URL.
 * @param      url_len  Size of @p url.
 * @return ESP_OK on success, or an error if the download or parsing failed.
 */
static esp_err_t fetch_manifest(char *version, size_t ver_len, char *url, size_t url_len)
{
    esp_http_client_config_t config = {
        .url = CONFIG_APP_CLOUD_MANIFEST_URL,
        .timeout_ms = HTTP_TIMEOUT_MS,
        // Validate the server against the certificate bundle built into the
        // firmware, so a spoofed server can't feed us a fake manifest.
        .crt_bundle_attach = esp_crt_bundle_attach,
        // GitHub (and most hosts) redirect downloads to a CDN.
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Manifest request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "Manifest returned HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    char body[MANIFEST_MAX_BYTES] = {0};
    int read_len = esp_http_client_read(client, body, sizeof(body) - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        ESP_LOGW(TAG, "Empty manifest");
        return ESP_FAIL;
    }
    body[read_len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGW(TAG, "Manifest is not valid JSON");
        return ESP_FAIL;
    }

    const cJSON *j_version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *j_url = cJSON_GetObjectItemCaseSensitive(root, "url");
    err = ESP_FAIL;
    if (cJSON_IsString(j_version) && cJSON_IsString(j_url)) {
        strlcpy(version, j_version->valuestring, ver_len);
        strlcpy(url, j_url->valuestring, url_len);
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "Manifest is missing \"version\" or \"url\"");
    }

    cJSON_Delete(root);
    return err;
}

/* ---- Status reporting ---------------------------------------------------- */

/**
 * @brief Tell the cloud endpoint what this board is running.
 *
 * Sent once after each boot, and again after every check, so a remote operator
 * can confirm an update landed without being on the device's network.
 * Silently skipped when CONFIG_APP_CLOUD_REPORT_URL is empty.
 *
 * @param status Short state word: "running" or "rolled_back".
 */
static void report_status(const char *status)
{
    if (strlen(CONFIG_APP_CLOUD_REPORT_URL) == 0) {
        return;
    }

    char id[8];
    device_id(id, sizeof(id));
    const esp_partition_t *running = esp_ota_get_running_partition();

    char body[256];
    int len = snprintf(body, sizeof(body),
                       "{\"device\":\"%s\",\"version\":\"%s\",\"slot\":\"%s\","
                       "\"status\":\"%s\",\"uptime_s\":%lld}",
                       id, running_version(), running ? running->label : "?",
                       status, esp_timer_get_time() / 1000000);

    esp_http_client_config_t config = {
        .url = CONFIG_APP_CLOUD_REPORT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, len);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Reported version %s (%s) to the cloud", running_version(), status);
    } else {
        // Not fatal: reporting is for our convenience, updates work without it.
        ESP_LOGW(TAG, "Status report failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

/* ---- Install ------------------------------------------------------------- */

/**
 * @brief Download and install the firmware at @p url, then reboot.
 *
 * Streams straight into the spare slot; the image is verified before the boot
 * slot is switched, so a failed download leaves the running firmware untouched.
 * Returns only on failure — on success the device restarts.
 *
 * @param url     HTTPS URL of the .bin.
 * @param version Version being installed; remembered so a failure can be detected.
 */
static void install_update(const char *url, const char *version)
{
    ESP_LOGI(TAG, "Installing version %s from %s", version, url);
    status_led_set(STATUS_LED_UPDATING);

    // Remember what we are installing BEFORE rebooting into it. If the new
    // image never confirms itself and the bootloader rolls back, the next boot
    // sees this value next to a different running version and blacklists it.
    nvs_set_str_or_erase(NVS_KEY_PENDING, version);

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 30000,          // large images over slow links need patience
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,   // GitHub redirects release assets to a CDN
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_err_t err = esp_https_ota(&ota_config);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Version %s installed, rebooting", version);
        vTaskDelay(pdMS_TO_TICKS(500));   // let the log reach the serial port
        esp_restart();
    }

    // Failed before anything was switched: clear the pending marker so the next
    // boot doesn't mistake this for a rollback, and go back to the normal LED.
    ESP_LOGE(TAG, "Update to %s failed: %s", version, esp_err_to_name(err));
    nvs_set_str_or_erase(NVS_KEY_PENDING, NULL);
    status_led_set(STATUS_LED_WIFI_CONNECTED);
}

/* ---- Check --------------------------------------------------------------- */

/**
 * @brief One full check: fetch the manifest, decide, and install if needed.
 */
static void run_check(void)
{
    if (strlen(CONFIG_APP_CLOUD_MANIFEST_URL) == 0) {
        return;
    }

    char version[32] = {0};
    char url[256] = {0};
    if (fetch_manifest(version, sizeof(version), url, sizeof(url)) != ESP_OK) {
        return; // already logged; try again at the next poll
    }

    if (strcmp(version, running_version()) == 0) {
        ESP_LOGI(TAG, "Up to date (version %s)", version);
        return;
    }

    char bad[32] = {0};
    nvs_get_str_or_empty(NVS_KEY_BAD, bad, sizeof(bad));
    if (bad[0] && strcmp(version, bad) == 0) {
        // Prevents the download → rollback → download loop described in the
        // file header. Publishing any other version clears the block.
        ESP_LOGW(TAG, "Skipping version %s: it failed on this device before", version);
        return;
    }

    ESP_LOGI(TAG, "Update available: %s (running %s)", version, running_version());
    install_update(url, version);
}

/**
 * @brief Work out whether the previous install attempt succeeded, and record it.
 *
 * Runs once per boot, before any check:
 *   - pending == running version  → the update worked; clear the marker.
 *   - pending != running version  → the image was rolled back; remember the
 *     version as bad so it is not downloaded again, and report it.
 */
static void evaluate_previous_attempt(void)
{
    char pending[32] = {0};
    nvs_get_str_or_empty(NVS_KEY_PENDING, pending, sizeof(pending));
    if (pending[0] == '\0') {
        return; // no install was in progress
    }

    if (strcmp(pending, running_version()) == 0) {
        ESP_LOGI(TAG, "Update to %s confirmed", pending);
        nvs_set_str_or_erase(NVS_KEY_PENDING, NULL);
        nvs_set_str_or_erase(NVS_KEY_BAD, NULL);   // a good install clears old blocks
    } else {
        ESP_LOGE(TAG, "Version %s did not come up; rolled back to %s. Blacklisting it.",
                 pending, running_version());
        nvs_set_str_or_erase(NVS_KEY_BAD, pending);
        nvs_set_str_or_erase(NVS_KEY_PENDING, NULL);
        report_status("rolled_back");
    }
}

/* ---- Task, timer and events ---------------------------------------------- */

/**
 * @brief Updater task: waits for wake-ups, then performs one check each time.
 */
static void updater_task(void *arg)
{
    (void)arg;

    // Wait for the first IP address; the event handler notifies us.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(FIRST_CHECK_DELAY_MS));

    sync_clock();                 // HTTPS needs a real date before anything else
    evaluate_previous_attempt();  // did the last install actually come up?
    report_status("running");
    run_check();

    for (;;) {
        // Woken by the poll timer or by cloud_update_check_now().
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_online) {
            run_check();
        }
    }
}

/** @brief Poll timer callback: nudge the task (never do network work in a timer). */
static void poll_timer_cb(void *arg)
{
    (void)arg;
    cloud_update_check_now();
}

/**
 * @brief IP event handler: start polling once the device is online.
 */
static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    bool first_time = !s_online;
    s_online = true;
    if (!first_time) {
        return; // a reconnect: the timer is already running
    }

    if (s_task) {
        xTaskNotifyGive(s_task);
    }

    // Start the periodic poll. The interval gets up to 10% of random jitter so
    // that a fleet of boards doesn't hit the server in lockstep.
    uint64_t period_us = (uint64_t)CONFIG_APP_CLOUD_POLL_MINUTES * 60ULL * 1000000ULL;
    uint64_t jitter_us = (uint64_t)(esp_random() % (uint32_t)(period_us / 10ULL));
    esp_timer_start_periodic(s_poll_timer, period_us + jitter_us);
    ESP_LOGI(TAG, "Update checks every %d minutes (+%llu s jitter)",
             CONFIG_APP_CLOUD_POLL_MINUTES, jitter_us / 1000000ULL);
}

/* ---- HTTP endpoint ------------------------------------------------------- */

/**
 * @brief GET /check-update: run a check now instead of waiting for the next poll.
 *
 * Token-protected like the other update endpoints, because it can cause a
 * firmware download. Replies immediately; the check runs in the background.
 */
static esp_err_t check_update_handler(httpd_req_t *req)
{
    char token[64] = {0};
    size_t len = httpd_req_get_hdr_value_len(req, "X-Update-Token");
    if (len == 0 || len >= sizeof(token) ||
        httpd_req_get_hdr_value_str(req, "X-Update-Token", token, sizeof(token)) != ESP_OK ||
        strcmp(token, CONFIG_APP_OTA_TOKEN) != 0) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "Missing or wrong X-Update-Token header\n");
        return ESP_OK;
    }

    cloud_update_check_now();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"checking\"}\n");
}

/* ---- Public API (documented in cloud_update.h) --------------------------- */

esp_err_t cloud_update_init(void)
{
    if (strlen(CONFIG_APP_CLOUD_MANIFEST_URL) == 0) {
        ESP_LOGW(TAG, "No manifest URL configured: internet updates are off");
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = poll_timer_cb,
        .name = "cloud_poll",
    };
    esp_err_t err = esp_timer_create(&timer_args, &s_poll_timer);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(updater_task, "cloud_upd", TASK_STACK_SIZE, NULL,
                    TASK_PRIORITY, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL);
}

void cloud_update_check_now(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

esp_err_t cloud_update_register_endpoint(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    static const httpd_uri_t check_uri = {
        .uri = "/check-update",
        .method = HTTP_GET,
        .handler = check_update_handler,
    };
    return httpd_register_uri_handler(server, &check_uri);
}
