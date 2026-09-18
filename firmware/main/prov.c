/**
 * @file prov.c
 * @brief BLE Wi-Fi provisioning: manager setup, start decision and events.
 *
 * Architecture: see prov.h, docs/DESIGN.md §4 and §5.
 *
 * Events handled (all on the default event-loop task):
 *
 *  PROTOCOMM_TRANSPORT_BLE_EVENT
 *    CONNECTED / DISCONNECTED        → LED blue solid / blue blink
 *  PROTOCOMM_SECURITY_SESSION_EVENT
 *    SETUP_OK                        → log: PoP accepted, session encrypted
 *    CREDENTIALS_MISMATCH / INVALID  → log: wrong PoP (the app shows the error)
 *  NETWORK_PROV_EVENT
 *    START                           → LED blue blink
 *    WIFI_CRED_RECV                  → log SSID (never the password), LED yellow
 *    WIFI_CRED_FAIL                  → LED red ×3, reset state after a delay so
 *                                      the app can resend credentials
 *    WIFI_CRED_SUCCESS               → switch wifi.c to auto-reconnect mode
 *    END                             → network_prov_mgr_deinit() (frees BLE)
 *
 * Dependencies: espressif/network_provisioning, protocomm, esp_timer,
 *               wifi (wifi.h), status_led (status_led.h).
 */
#include "prov.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include <network_provisioning/manager.h>
#include <network_provisioning/scheme_ble.h>
#include "protocomm_ble.h"        /* PROTOCOMM_TRANSPORT_BLE_EVENT */
#include "protocomm_security.h"   /* PROTOCOMM_SECURITY_SESSION_EVENT */

#include "status_led.h"
#include "wifi.h"

/** Log tag for this module. */
static const char *TAG = "prov";

/**
 * Delay before resetting the provisioning state machine after bad credentials.
 *
 * Why wait: after sending credentials the phone app polls `get_status`.
 * Espressif's Android library (esp-idf-provisioning-android lib-2.4.4, in
 * ESPDevice.pollForWifiConnectionStatus) polls every 5 s. If we reset before
 * its next poll, it sees "not configured" instead of "FAILED(reason)", and
 * the user gets a vague error. 10 s is longer than one poll interval plus
 * BLE latency, so the app always reads the real failure reason first.
 * After the reset, the app can send new credentials over the same BLE session.
 * Side effect: a retry sent within 10 s of a failure is rejected by the device.
 * The app handles that by asking the user to wait a moment.
 */
#define PROV_FAIL_RESET_DELAY_MS    10000

/**
 * 128-bit BLE service UUID the provisioning service advertises
 * (little-endian byte order, as network_provisioning expects).
 * It's the value from Espressif's official provisioning examples, which
 * keeps us compatible with Espressif's phone apps and libraries.
 * Readable form: 021a9004-0382-4aea-bff4-6b3f1c5adfb4.
 */
static uint8_t s_service_uuid[] = {
    0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
    0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
};

/** true while a phone is connected over BLE; picks the LED state after a failure. */
static bool s_ble_connected = false;

/**
 * true from WIFI_CRED_RECV until WIFI_CRED_FAIL, i.e. while the device is
 * trying (or has managed) to join Wi-Fi with the phone's credentials. Used so
 * a BLE disconnect doesn't switch the LED back to "waiting" mid-attempt.
 */
static bool s_wifi_attempt_active = false;

/** One-shot timer that resets the provisioning state after WIFI_CRED_FAIL. */
static esp_timer_handle_t s_fail_reset_timer = NULL;

/* ---- Helpers ------------------------------------------------------------- */

/**
 * @brief Build the BLE advertised name: prefix + last 3 bytes of the STA MAC.
 *
 * Example: "PROV_A1B2C3". The MAC suffix makes names unique, so several
 * boards can be told apart in the app. The STA MAC is also what the Wi-Fi
 * router shows, so the name can be matched to the router's client list.
 *
 * @param[out] buf  Destination buffer.
 * @param      len  Size of @p buf in bytes.
 */
static void get_device_service_name(char *buf, size_t len)
{
    uint8_t mac[6] = {0};
    // esp_read_mac reads from eFuse, so it works before Wi-Fi is started.
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "%s%02X%02X%02X",
             CONFIG_APP_PROV_NAME_PREFIX, mac[3], mac[4], mac[5]);
}

/** @brief Show the right LED state for "waiting in provisioning mode". */
static void led_show_waiting(void)
{
    status_led_set(s_ble_connected ? STATUS_LED_BLE_CONNECTED : STATUS_LED_PROVISIONING);
}

/**
 * @brief esp_timer callback: re-arm the provisioning manager after a failure.
 *
 * Runs in the esp_timer task, PROV_FAIL_RESET_DELAY_MS after WIFI_CRED_FAIL.
 */
static void fail_reset_timer_cb(void *arg)
{
    (void)arg;
    // Clears the rejected credentials and puts the manager back in "waiting
    // for credentials" so the app can call set_config/apply_config again.
    // Returns ESP_ERR_INVALID_STATE if provisioning already ended, which is
    // harmless here.
    esp_err_t err = network_prov_mgr_reset_wifi_sm_state_on_failure();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Provisioning state reset: ready for new credentials");
    } else {
        ESP_LOGW(TAG, "State reset skipped: %s", esp_err_to_name(err));
    }
}

/* ---- Event handler ------------------------------------------------------- */

/**
 * @brief Single handler for provisioning, BLE-transport and security events.
 *
 * Registered in prov_start(). Runs on the default event-loop task and must
 * not block.
 */
static void prov_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;

    if (base == NETWORK_PROV_EVENT) {
        switch (id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "Provisioning started, waiting for the phone app");
            led_show_waiting();
            break;

        case NETWORK_PROV_WIFI_CRED_RECV: {
            wifi_sta_config_t *cfg = (wifi_sta_config_t *)data;
            // Security: log only the SSID. Never print the password.
            ESP_LOGI(TAG, "Received Wi-Fi credentials for SSID \"%s\", connecting...",
                     (const char *)cfg->ssid);
            // If a delayed reset from an earlier failure is still pending,
            // cancel it so it doesn't wipe these new credentials.
            esp_timer_stop(s_fail_reset_timer);
            s_wifi_attempt_active = true;
            status_led_set(STATUS_LED_WIFI_CONNECTING);
            break;
        }

        case NETWORK_PROV_WIFI_CRED_FAIL: {
            network_prov_wifi_sta_fail_reason_t *reason =
                (network_prov_wifi_sta_fail_reason_t *)data;
            ESP_LOGE(TAG, "Connection failed: %s",
                     (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR)
                         ? "authentication error (wrong password?)"
                         : "access point not found");
            s_wifi_attempt_active = false;
            // Go back to the waiting LED state and play the red failure blinks on top.
            led_show_waiting();
            status_led_signal_failure();
            // Let the app read the failure first, then allow a retry (see
            // PROV_FAIL_RESET_DELAY_MS).
            esp_timer_stop(s_fail_reset_timer);   // harmless if not running
            esp_timer_start_once(s_fail_reset_timer,
                                 (uint64_t)PROV_FAIL_RESET_DELAY_MS * 1000ULL);
            break;
        }

        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful: Wi-Fi connected");
            // From now on, stay connected and retry on drops (wifi.c).
            wifi_set_auto_reconnect(true);
            // No LED change: IP_EVENT_STA_GOT_IP in wifi.c turns it green.
            break;

        case NETWORK_PROV_END:
            // The manager stops on its own shortly after success. Free its
            // resources; the FREE_BTDM scheme handler also frees BLE memory
            // (decision D7).
            ESP_LOGI(TAG, "Provisioning ended, releasing provisioning resources");
            network_prov_mgr_deinit();
            break;

        default:
            break;
        }

    } else if (base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        switch (id) {
        case PROTOCOMM_TRANSPORT_BLE_CONNECTED:
            ESP_LOGI(TAG, "Phone connected over BLE");
            s_ble_connected = true;
            status_led_set(STATUS_LED_BLE_CONNECTED);
            break;
        case PROTOCOMM_TRANSPORT_BLE_DISCONNECTED:
            ESP_LOGI(TAG, "Phone disconnected from BLE");
            s_ble_connected = false;
            // Don't override the LED if Wi-Fi is already connecting or connected.
            // Only go back to blue blinking when we're still waiting.
            if (!s_wifi_attempt_active) {
                led_show_waiting();
            }
            break;
        default:
            break;
        }

    } else if (base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        switch (id) {
        case PROTOCOMM_SECURITY_SESSION_SETUP_OK:
            ESP_LOGI(TAG, "Secure session established (PoP accepted)");
            break;
        case PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS:
            ESP_LOGE(TAG, "Session setup failed: invalid security parameters from app");
            break;
        case PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH:
            ESP_LOGE(TAG, "Session setup failed: wrong Proof-of-Possession (PoP)");
            break;
        default:
            break;
        }
    }
}

/* ---- Public API (documented in prov.h) ---------------------------------- */

esp_err_t prov_start(void)
{
    esp_err_t err;

    // Register handlers before starting anything, so no event is missed.
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                               prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
                                               prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID,
                                               prov_event_handler, NULL));

    const esp_timer_create_args_t timer_args = {
        .callback = fail_reset_timer_cb,
        .name = "prov_fail_reset",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_fail_reset_timer));

    // Manager config:
    //  - scheme = BLE: the provisioning transport is GATT over NimBLE.
    //  - FREE_BTDM: once provisioning is finished (or skipped), release all
    //    Bluetooth controller and host memory, since this app doesn't use BLE
    //    afterwards (decision D7).
    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    err = network_prov_mgr_init(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network_prov_mgr_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // "Provisioned" means Wi-Fi credentials are saved in NVS.
    bool provisioned = false;
    err = network_prov_mgr_is_wifi_provisioned(&provisioned);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not read provisioning status: %s", esp_err_to_name(err));
        return err;
    }

    if (provisioned) {
        // Normal boot: BLE isn't needed. Deinit frees the manager and BLE memory.
        ESP_LOGI(TAG, "Already provisioned, connecting with saved credentials");
        network_prov_mgr_deinit();
        return wifi_start_sta();
    }

    // First boot, or credentials were erased with the BOOT button.
    char service_name[32];
    get_device_service_name(service_name, sizeof(service_name));

    // Security 1: X25519 key exchange + AES-CTR, authenticated by the shared
    // PoP (decision D3). For Security 1 the "params" are just the PoP string.
    network_prov_security_t security = NETWORK_PROV_SECURITY_1;
    network_prov_security1_params_t *sec_params = CONFIG_APP_PROV_POP;

    // Fixed service UUID so Espressif's apps and libraries recognise the device.
    network_prov_scheme_ble_set_service_uuid(s_service_uuid);

    ESP_LOGI(TAG, "Starting BLE provisioning as \"%s\" (Security 1)", service_name);
    // service_key is only used by the SoftAP scheme, so it's NULL for BLE.
    err = network_prov_mgr_start_provisioning(security, (void *)sec_params,
                                              service_name, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start provisioning: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
