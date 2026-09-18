/**
 * @file color_api.h
 * @brief Public API for the number → colour → reply feature (added 2026-09-18).
 *
 * What it does (docs/COLOR_API.md): you send a number 1-5 over Wi-Fi, the LED
 * shows the matching colour, and the device replies with a RANDOM number 6-9.
 * Colours and the reply rule were changed on 2026-09-18 19:45 at the user's
 * request (they were red/blue/green/orange/cyan with the fixed reply 6 - n):
 *
 *   | n | colour       | reply          |
 *   |---|--------------|----------------|
 *   | 1 | yellow       | random 6-9     |
 *   | 2 | magenta      | random 6-9     |
 *   | 3 | white        | random 6-9     |
 *   | 4 | spring green | random 6-9     |
 *   | 5 | indigo       | random 6-9     |
 *
 * Architecture: an add-on to the HTTP server that ota.c already runs, so there
 * is one server on port 80 rather than two. Endpoints:
 *
 *   GET /          → a small HTML page with buttons 1-5 (handy from a phone)
 *   GET /color?n=3 → sets the colour, replies with JSON {"input":3,"reply":3,...}
 *
 * Unlike the update endpoints these need no token: the worst a stranger on your
 * Wi-Fi can do is change the colour of an LED, and requiring a header would
 * make the page unusable from a browser. See docs/COLOR_API.md §Security.
 *
 * Dependencies: esp_http_server, status_led.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the colour endpoints on an already-running HTTP server.
 *
 * Called by ota.c right after it starts the server, so both features share one
 * server instance and one port.
 *
 * @param server Handle of the running server; ignored if NULL.
 * @return ESP_OK, or the first registration error.
 */
esp_err_t color_api_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
