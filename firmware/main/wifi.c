/**
 * @file wifi.c
 * @brief Wi-Fi station setup, connection/reconnection logic and IP events.
 *
 * Architecture: see wifi.h and docs/DESIGN.md §5 (flow).
 *
 * Event flow handled here (on the default event-loop task):
 *   WIFI_EVENT_STA_START        → connect, but only in normal operation
 *   WIFI_EVENT_STA_DISCONNECTED → reconnect with back-off, only in normal operation
 *   IP_EVENT_STA_GOT_IP         → log the IP, LED solid green
 *
 * Retry policy (normal operation only): CONFIG_APP_WIFI_FAST_RETRIES immediate
 * attempts, then one attempt every CONFIG_APP_WIFI_RETRY_BACKOFF_MS, forever.
 * The delay uses an esp_timer instead of vTaskDelay, because blocking inside an
 * event handler would stall every other event on the default loop.
 *
 * Dependencies: esp_wifi, esp_netif, esp_event, esp_timer, status_led.
 */
#include "wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "status_led.h"

/** Log tag for this module. */
static const char *TAG = "wifi";

/**
 * true once the device is provisioned (see wifi.h). Only touched from the
 * event-loop task and app_main before the loop sees any Wi-Fi events, so no
 * lock is needed.
 */
static bool s_auto_reconnect = false;

/** Consecutive failed attempts since the last successful connection. */
static int s_retry_count = 0;

/** One-shot timer used to delay reconnects once fast retries are used up. */
static esp_timer_handle_t s_retry_timer = NULL;

/**
 * @brief esp_timer callback: try to connect again after the back-off delay.
 *
 * Runs in the esp_timer task, where calling esp_wifi_connect() is fine.
 */
static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (s_auto_reconnect) {
        ESP_LOGI(TAG, "Back-off elapsed, reconnecting...");
        esp_wifi_connect();
    }
}

/**
 * @brief Handler for WIFI_EVENT and IP_EVENT (registered in wifi_init()).
 *
 * Runs on the default event-loop task. Must not block.
 */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // During provisioning the manager calls esp_wifi_connect() itself
        // after it receives credentials, so only connect here in normal mode.
        if (s_auto_reconnect) {
            ESP_LOGI(TAG, "STA started, connecting to saved network...");
            status_led_set(STATUS_LED_WIFI_CONNECTING);
            esp_wifi_connect();
        }

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "Disconnected (reason %d)", evt->reason);

        // While provisioning, do NOT retry: the provisioning manager watches
        // this same event and reports FAILED(reason) to the phone app.
        if (!s_auto_reconnect) {
            return;
        }

        status_led_set(STATUS_LED_WIFI_CONNECTING);
        s_retry_count++;
        if (s_retry_count <= CONFIG_APP_WIFI_FAST_RETRIES) {
            ESP_LOGI(TAG, "Reconnect attempt %d/%d", s_retry_count, CONFIG_APP_WIFI_FAST_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "Retrying in %d ms", CONFIG_APP_WIFI_RETRY_BACKOFF_MS);
            // esp_timer_start_once fails if the timer is already running;
            // that's harmless because a retry is already scheduled.
            esp_timer_start_once(s_retry_timer,
                                 (uint64_t)CONFIG_APP_WIFI_RETRY_BACKOFF_MS * 1000ULL);
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected, got IP " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry_count = 0;
        status_led_set(STATUS_LED_WIFI_CONNECTED);
    }
}

/* ---- Public API (documented in wifi.h) ---------------------------------- */

esp_err_t wifi_init(void)
{
    esp_err_t err;

    // TCP/IP stack (lwIP) and the default station network interface.
    err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    esp_netif_create_default_wifi_sta();

    // Wi-Fi driver with default buffer and task settings. Credentials are
    // stored in flash (the default), which is how they survive a reboot.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Register handlers on the default event loop created in app_main.
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));

    // Back-off timer for reconnects (see the file header).
    const esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    return esp_timer_create(&timer_args, &s_retry_timer);
}

esp_err_t wifi_start_sta(void)
{
    // Already provisioned: stay connected from now on.
    wifi_set_auto_reconnect(true);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    // esp_wifi_start() fires WIFI_EVENT_STA_START, whose handler calls
    // esp_wifi_connect() with the config saved in NVS.
    return esp_wifi_start();
}

void wifi_set_auto_reconnect(bool enable)
{
    s_auto_reconnect = enable;
    s_retry_count = 0;
}
