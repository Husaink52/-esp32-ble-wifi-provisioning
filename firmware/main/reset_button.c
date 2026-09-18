/**
 * @file reset_button.c
 * @brief Watches the BOOT button; a long press erases Wi-Fi credentials and restarts.
 *
 * Architecture: see reset_button.h and docs/DESIGN.md §6.
 *
 * Why polling instead of a GPIO interrupt:
 *  - We need to measure how long the button is held, not just detect a press.
 *  - A 50 ms poll also debounces the switch for free.
 *  - The cost is negligible: the task sleeps between samples.
 *
 * Hardware: the ESP32-C6-DevKitC-1 BOOT button connects GPIO9 to GND when
 * pressed (active low). The board has an external pull-up, and we also turn
 * on the internal one to be safe.
 *
 * Dependencies: driver/gpio, esp_wifi, esp_system, status_led.
 */
#include "reset_button.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "status_led.h"

/** Log tag for this module. */
static const char *TAG = "reset_btn";

/** How often the button is sampled; also acts as the debounce interval. */
#define POLL_INTERVAL_MS        50

/** Stack size for the button task (logging + a few esp_* calls). */
#define BUTTON_TASK_STACK_SIZE  3072

/** Low priority: a human holding a button for seconds doesn't need real-time response. */
#define BUTTON_TASK_PRIORITY    2

/**
 * @brief Erase saved Wi-Fi credentials and restart. Never returns.
 *
 * esp_wifi_restore() resets all Wi-Fi settings stored in NVS to defaults,
 * including the SSID and password the provisioning manager saved. On the
 * next boot, network_prov_mgr_is_wifi_provisioned() returns false and BLE
 * provisioning starts again.
 */
static void erase_credentials_and_restart(void)
{
    ESP_LOGW(TAG, "Erasing Wi-Fi credentials and restarting into provisioning mode");
    status_led_set(STATUS_LED_OFF);   // visual confirmation that the reset was accepted

    esp_err_t err = esp_wifi_restore();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_restore failed: %s", esp_err_to_name(err));
    }

    // Give the log output time to reach the UART before rebooting.
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/**
 * @brief Button task: measure how long the button is held and trigger the reset.
 *
 * @param arg Unused.
 */
static void button_task(void *arg)
{
    (void)arg;
    uint32_t held_ms = 0;

    for (;;) {
        // Active low: level 0 means "pressed".
        bool pressed = gpio_get_level(CONFIG_APP_RESET_BUTTON_GPIO) == 0;

        if (pressed) {
            held_ms += POLL_INTERVAL_MS;
            if (held_ms == POLL_INTERVAL_MS) {
                ESP_LOGI(TAG, "Button pressed, hold %d ms to erase Wi-Fi credentials",
                         CONFIG_APP_RESET_HOLD_TIME_MS);
            }
            if (held_ms >= (uint32_t)CONFIG_APP_RESET_HOLD_TIME_MS) {
                erase_credentials_and_restart();   // does not return
            }
        } else {
            if (held_ms > 0) {
                ESP_LOGI(TAG, "Button released early (%lu ms), no reset", (unsigned long)held_ms);
            }
            held_ms = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

/* ---- Public API (documented in reset_button.h) -------------------------- */

esp_err_t reset_button_init(void)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_APP_RESET_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,     // keep the line high when not pressed
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,       // polled, see file header
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(button_task, "reset_button", BUTTON_TASK_STACK_SIZE, NULL,
                    BUTTON_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Hold the button on GPIO%d for %d ms to erase Wi-Fi credentials",
             CONFIG_APP_RESET_BUTTON_GPIO, CONFIG_APP_RESET_HOLD_TIME_MS);
    return ESP_OK;
}
