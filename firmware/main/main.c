/**
 * @file main.c
 * @brief Firmware entry point: runs the boot sequence and wires the modules together.
 *
 * Architecture: docs/DESIGN.md §3 (modules) and §5 (flow).
 *
 * Boot sequence (order matters; each step depends on the ones before it):
 *   1. NVS flash      needed by the Wi-Fi driver, which stores credentials there
 *   2. Event loop     every module talks through esp_event
 *   3. Status LED     up early so the user sees feedback from the start
 *   4. Wi-Fi driver   the provisioning manager and reset button both need it
 *   5. Reset button   BOOT long-press works in every mode
 *   6. OTA service    arms the wireless-update server (starts once on Wi-Fi)
 *   7. Cloud updates  arms the hourly internet update check (v1.2)
 *   8. Provisioning   connect with saved creds, or start BLE provisioning
 *
 * After step 8, app_main() returns. That's normal in ESP-IDF: the FreeRTOS
 * tasks and event handlers created above keep running.
 *
 * Dependencies: nvs_flash, esp_event, and the local modules status_led,
 *               wifi, reset_button, ota, cloud_update, prov.
 */
#include "esp_app_desc.h"   // esp_app_get_description: build stamp logged at boot
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"    // esp_ota_get_running_partition: which slot we booted from
#include "nvs_flash.h"

#include "ota.h"
#include "cloud_update.h"
#include "prov.h"
#include "reset_button.h"
#include "status_led.h"
#include "wifi.h"

/** Log tag for this module. */
static const char *TAG = "app";

/**
 * @brief Initialise NVS, erasing and retrying if the partition can't be used.
 *
 * ESP_ERR_NVS_NO_FREE_PAGES and ESP_ERR_NVS_NEW_VERSION_FOUND mean the NVS
 * partition is full, or was written by an incompatible IDF version. The
 * standard fix is to erase it and init again. Any saved credentials are lost,
 * so the device simply goes back to provisioning mode.
 */
static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition unusable (%s), erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/**
 * @brief ESP-IDF application entry point, called by the startup code on the main task.
 *
 * Fatal setup errors (NVS, event loop, Wi-Fi, provisioning) abort via
 * ESP_ERROR_CHECK, and the chip reboots. That's the right response for a
 * device that can't do its only job. The LED and button are optional, so
 * their failures are only logged.
 */
void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-C6 BLE Wi-Fi provisioning firmware starting");

    // Added 2026-09-18 18:10 while testing wireless updates: after an OTA push
    // the only way to tell old firmware from new was to query GET /info over
    // the network. Printing the build stamp and the active slot (ota_0/ota_1)
    // at boot makes that visible in the serial log too, which is the quickest
    // way to confirm an update actually took effect.
    const esp_app_desc_t *desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    // The build timestamp can be stale after an incremental build, so also print
    // the first bytes of the ELF SHA-256, which always change when the code does
    // (added 2026-09-18 while verifying that an OTA push really landed).
    ESP_LOGI(TAG, "Firmware built %s %s (build %02x%02x%02x%02x), running from '%s'",
             desc->date, desc->time,
             desc->app_elf_sha256[0], desc->app_elf_sha256[1],
             desc->app_elf_sha256[2], desc->app_elf_sha256[3],
             running ? running->label : "unknown");

    init_nvs();                                          // 1
    ESP_ERROR_CHECK(esp_event_loop_create_default());    // 2

    if (status_led_init() != ESP_OK) {                   // 3 (optional)
        ESP_LOGW(TAG, "Continuing without status LED");
    }

    ESP_ERROR_CHECK(wifi_init());                        // 4

    if (reset_button_init() != ESP_OK) {                 // 5 (optional)
        ESP_LOGW(TAG, "Continuing without reset button");
    }

    if (ota_init() != ESP_OK) {                          // 6 (optional)
        ESP_LOGW(TAG, "Continuing without wireless firmware updates");
    }

    if (cloud_update_init() != ESP_OK) {                 // 7 (optional)
        ESP_LOGW(TAG, "Continuing without internet firmware updates");
    }

    ESP_ERROR_CHECK(prov_start());                       // 8
}
