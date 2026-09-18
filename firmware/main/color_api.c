/**
 * @file color_api.c
 * @brief Implementation of the number → colour → reply feature (added 2026-09-18).
 *
 * Architecture: see color_api.h and docs/COLOR_API.md.
 *
 * Request flow:
 *   GET /color?n=3
 *     → parse and validate n (1-5)
 *     → look the colour up in COLORS[]
 *     → status_led_set_custom() turns the LED that colour and holds it
 *     → reply with JSON: the number sent, a RANDOM reply number 6-9, and the colour name
 *
 * Changed 2026-09-18 19:45 at the user's request: the reply used to be the
 * fixed mirror 6 - n (1→5, 2→4). It is now a random number in 6..9, drawn
 * fresh on every request, so the same input can give different answers.
 *
 * Threading: handlers run on the HTTP server task started in ota.c. They only
 * write a colour and send a short response, so they never block that task.
 *
 * Dependencies: esp_http_server, status_led.
 */
#include "color_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"   // esp_random(): hardware RNG used for the reply number

#include "status_led.h"

/** Log tag for this module. */
static const char *TAG = "color_api";

/** Lowest accepted input number. */
#define COLOR_MIN_INPUT     1
/** Highest accepted input number. */
#define COLOR_MAX_INPUT     5

/** Reply range (inclusive), added 2026-09-18: every request answers with a random 6-9. */
#define COLOR_MIN_REPLY     6
#define COLOR_MAX_REPLY     9

/** One row per accepted number: the colour to show and its name for the reply. */
typedef struct {
    const char *name;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} color_entry_t;

/**
 * Colours for inputs 1..5.
 *
 * Replaced 2026-09-18 19:45 on the user's request to change every colour
 * (they were red, blue, green, orange, cyan). The new set is picked to stay
 * easy to tell apart on the small on-board LED and to avoid the colours the
 * device already uses for status: blue (waiting), green (online), yellow-amber
 * (connecting), purple (updating), red (failure). Yellow is included because
 * the status yellow only ever blinks, while these are solid.
 */
static const color_entry_t COLORS[COLOR_MAX_INPUT] = {
    /* 1 */ { "yellow",        255, 255,   0 },  /* red + green at full        */
    /* 2 */ { "magenta",       255,   0, 255 },  /* red + blue                 */
    /* 3 */ { "white",         255, 255, 255 },  /* all three channels         */
    /* 4 */ { "spring green",    0, 255, 128 },  /* green with a touch of blue */
    /* 5 */ { "indigo",         75,   0, 255 },  /* blue with a touch of red   */
};

/**
 * @brief GET /color?n=<1-5>: set the colour and reply with a random number 6-9.
 *
 * Replies 400 with a short explanation if `n` is missing or out of range, so a
 * typo is obvious in the browser rather than silently ignored.
 */
static esp_err_t color_get_handler(httpd_req_t *req)
{
    char query[32] = {0};
    char value[8] = {0};

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "n", value, sizeof(value)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Send a number: /color?n=1 (1-5)\n");
        return ESP_OK;
    }

    int n = atoi(value);
    if (n < COLOR_MIN_INPUT || n > COLOR_MAX_INPUT) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Number out of range: use 1-5\n");
        return ESP_OK;
    }

    const color_entry_t *color = &COLORS[n - 1];

    // Random reply in COLOR_MIN_REPLY..COLOR_MAX_REPLY, drawn per request.
    // esp_random() is the hardware random generator; it needs no seeding and,
    // unlike rand(), gives different values after every reboot. The modulo is
    // safe here because the range (4 values) divides evenly into 2^32.
    int reply = COLOR_MIN_REPLY + (int)(esp_random() % (COLOR_MAX_REPLY - COLOR_MIN_REPLY + 1));

    status_led_set_custom(color->r, color->g, color->b);
    ESP_LOGI(TAG, "Input %d -> LED %s, replying %d", n, color->name, reply);

    char body[96];
    snprintf(body, sizeof(body),
             "{\"input\":%d,\"reply\":%d,\"color\":\"%s\"}\n", n, reply, color->name);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

/**
 * @brief GET /: a tiny page with buttons 1-5, so the feature is usable from a phone.
 *
 * Kept as one self-contained string with no external files, images or fonts:
 * everything is served from the chip's own flash, and the page works offline.
 * The script calls /color and shows the reply without reloading the page.
 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char PAGE[] =
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>ESP32-C6 colour demo</title>"
        "<style>body{font-family:system-ui,sans-serif;margin:2rem;text-align:center}"
        "button{font-size:2rem;margin:.4rem;padding:1rem 1.6rem;border-radius:.6rem;"
        "border:1px solid #888;background:#f4f4f4;cursor:pointer}"
        "#out{font-size:1.4rem;margin-top:1.5rem;min-height:2rem}</style>"
        "<h2>Pick a number</h2>"
        "<div>"
        "<button onclick=send(1)>1</button><button onclick=send(2)>2</button>"
        "<button onclick=send(3)>3</button><button onclick=send(4)>4</button>"
        "<button onclick=send(5)>5</button>"
        "</div><div id=out></div>"
        "<script>function send(n){fetch('/color?n='+n).then(r=>r.json())"
        ".then(d=>{out.textContent='Sent '+d.input+' \\u2192 LED '+d.color+', device replied '+d.reply+' (random 6-9)';})"
        ".catch(e=>{out.textContent='Request failed';});}</script>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

/* ---- Public API (documented in color_api.h) ----------------------------- */

esp_err_t color_api_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    static const httpd_uri_t color_uri = {
        .uri = "/color",
        .method = HTTP_GET,
        .handler = color_get_handler,
    };
    static const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };

    esp_err_t err = httpd_register_uri_handler(server, &color_uri);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &root_uri);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Colour API ready: open http://<device>/ or GET /color?n=1..5");
    return ESP_OK;
}
