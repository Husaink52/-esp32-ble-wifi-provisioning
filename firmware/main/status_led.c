/**
 * @file status_led.c
 * @brief Implementation of the on-board RGB status LED state machine.
 *
 * Architecture: see status_led.h and docs/DESIGN.md §6 (LED table).
 *
 * How it works:
 *  - One small FreeRTOS task ("status_led") owns the LED hardware. Nobody else
 *    touches the led_strip handle, so no mutex is needed around the driver.
 *  - Other tasks only write the requested state into a variable and wake the
 *    task with a task notification. The task then redraws immediately instead
 *    of waiting for its current blink delay to run out.
 *  - A pending failure animation is a separate flag, so it can be layered on
 *    top of whatever persistent state is active.
 *
 * Dependencies: espressif/led_strip (RMT backend), FreeRTOS.
 */
#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

/** Log tag for this module; filter with `idf.py monitor` or esp_log_level_set(). */
static const char *TAG = "status_led";

/* ---- Tunables ----------------------------------------------------------- */

/**
 * WS2812 LEDs are very bright at full power and painful to look at on a desk,
 * so all colours are scaled to this level (0-255).
 */
#define LED_BRIGHTNESS          32

/** Half-period of the "slow blink" (blue, provisioning): 500 ms on, 500 ms off. */
#define SLOW_BLINK_MS           500
/** Half-period of the "fast blink" (yellow, connecting): 100 ms on, 100 ms off. */
#define FAST_BLINK_MS           100
/** Half-period of each red flash in the failure animation. */
#define FAILURE_BLINK_MS        150
/** Number of red flashes in the failure animation (docs/DESIGN.md §6). */
#define FAILURE_BLINK_COUNT     3

/** Stack size for the LED task; it only calls the RMT driver, so this is plenty. */
#define LED_TASK_STACK_SIZE     3072
/** Low priority: the LED is cosmetic and must never starve Wi-Fi/BLE tasks. */
#define LED_TASK_PRIORITY       2

/* ---- Module state -------------------------------------------------------- */

/** Handle to the led_strip driver; owned exclusively by led_task(). */
static led_strip_handle_t s_strip = NULL;

/** Handle of the LED task, used to send it wake-up notifications. */
static TaskHandle_t s_task = NULL;

/**
 * Requested persistent state. Written by any task, read by led_task().
 * A single enum-sized write is atomic on this 32-bit RISC-V MCU, so `volatile`
 * is enough and no lock is required.
 */
static volatile status_led_state_t s_state = STATUS_LED_OFF;

/** Set by status_led_signal_failure(); cleared by led_task() once it's played. */
static volatile bool s_failure_pending = false;

/**
 * Colour shown in STATUS_LED_CUSTOM, packed as 0x00RRGGBB.
 * Written by status_led_set_custom() from the HTTP server task, read by
 * led_task(); a single 32-bit write is atomic on this MCU, so no lock is needed.
 */
static volatile uint32_t s_custom_rgb = 0;

/* ---- Helpers ------------------------------------------------------------- */

/**
 * @brief Set the single LED to an RGB colour (0-255 per channel, before
 *        brightness scaling) and push it to the hardware.
 *
 * Only called from led_task().
 */
static void led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    // Scale each channel to LED_BRIGHTNESS so "255" means "as bright as we allow".
    led_strip_set_pixel(s_strip, 0,
                        (r * LED_BRIGHTNESS) / 255,
                        (g * LED_BRIGHTNESS) / 255,
                        (b * LED_BRIGHTNESS) / 255);
    led_strip_refresh(s_strip);
}

/** @brief Turn the LED off. Only called from led_task(). */
static void led_off(void)
{
    led_strip_clear(s_strip);
}

/**
 * @brief Sleep for up to @p ms, waking early if another task changed the state.
 *
 * @return true if woken by a notification (the state changed), false on timeout.
 */
static bool wait_or_wake(uint32_t ms)
{
    // ulTaskNotifyTake(pdTRUE, ...) clears the notification count, so several
    // state changes in a row cause only one wake-up.
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms)) > 0;
}

/**
 * @brief Play one half-on / half-off blink cycle in the given colour.
 *
 * Stops early if the state changes, so the LED reacts quickly.
 */
static void blink_once(uint8_t r, uint8_t g, uint8_t b, uint32_t half_period_ms)
{
    led_rgb(r, g, b);
    if (wait_or_wake(half_period_ms)) {
        return;
    }
    led_off();
    wait_or_wake(half_period_ms);
}

/**
 * @brief The LED animation task: loops forever rendering s_state.
 *
 * @param arg Unused.
 */
static void led_task(void *arg)
{
    (void)arg;

    for (;;) {
        // The failure animation takes priority over the persistent state.
        // After it plays, the loop falls through and redraws s_state.
        if (s_failure_pending) {
            s_failure_pending = false;
            for (int i = 0; i < FAILURE_BLINK_COUNT; i++) {
                led_rgb(255, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(FAILURE_BLINK_MS));
                led_off();
                vTaskDelay(pdMS_TO_TICKS(FAILURE_BLINK_MS));
            }
            // Drop any notification that arrived during the animation; the
            // loop below reads the latest s_state anyway.
            ulTaskNotifyTake(pdTRUE, 0);
        }

        switch (s_state) {
        case STATUS_LED_PROVISIONING:
            blink_once(0, 0, 255, SLOW_BLINK_MS);           // blue, slow
            break;
        case STATUS_LED_BLE_CONNECTED:
            led_rgb(0, 0, 255);                             // blue, solid
            wait_or_wake(portMAX_DELAY);                    // nothing to animate
            break;
        case STATUS_LED_WIFI_CONNECTING:
            blink_once(255, 180, 0, FAST_BLINK_MS);         // yellow/amber, fast
            break;
        case STATUS_LED_WIFI_CONNECTED:
            led_rgb(0, 255, 0);                             // green, solid
            wait_or_wake(portMAX_DELAY);
            break;
        case STATUS_LED_UPDATING:
            blink_once(180, 0, 255, FAST_BLINK_MS);         // purple, fast: OTA in progress
            break;
        case STATUS_LED_CUSTOM:
            // Solid colour requested through the colour API (docs/COLOR_API.md).
            led_rgb((s_custom_rgb >> 16) & 0xFF, (s_custom_rgb >> 8) & 0xFF, s_custom_rgb & 0xFF);
            wait_or_wake(portMAX_DELAY);
            break;
        case STATUS_LED_OFF:
        default:
            led_off();
            wait_or_wake(portMAX_DELAY);
            break;
        }
    }
}

/* ---- Public API (documented in status_led.h) ---------------------------- */

esp_err_t status_led_init(void)
{
    // General LED strip settings: one WS2812 on the configured GPIO.
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_APP_STATUS_LED_GPIO,
        .max_leds = 1,                        // the DevKitC-1 has exactly one RGB LED
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB, // WS2812 wire order is G-R-B
        .flags.invert_out = false,            // no inverter/level shifter on the board
    };

    // RMT backend settings. 10 MHz resolution gives WS2812 timing enough
    // precision; DMA isn't needed for a single pixel.
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED driver init failed on GPIO%d: %s",
                 CONFIG_APP_STATUS_LED_GPIO, esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_strip);

    if (xTaskCreate(led_task, "status_led", LED_TASK_STACK_SIZE, NULL,
                    LED_TASK_PRIORITY, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LED task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Status LED ready on GPIO%d", CONFIG_APP_STATUS_LED_GPIO);
    return ESP_OK;
}

void status_led_set(status_led_state_t state)
{
    s_state = state;
    // If init failed (no task), setting the variable is harmless and we skip the wake-up.
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void status_led_set_custom(uint8_t r, uint8_t g, uint8_t b)
{
    // Pack first, then switch state, so led_task() can never read a half-written colour.
    s_custom_rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    status_led_set(STATUS_LED_CUSTOM);
}

void status_led_signal_failure(void)
{
    s_failure_pending = true;
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}
