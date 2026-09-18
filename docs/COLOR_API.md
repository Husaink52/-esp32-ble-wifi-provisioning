# Colour API: send a number, get a colour and a reply

> **Added 2026-09-18 19:10** as part of v1.1, at the user's request: "enter a number between 1 and 5, the LED shows a colour, and the chip sends back a different number".
> Pushed to board `PROV_E01C64` over Wi-Fi (build `f9618173`) and tested for all five inputs.

---

## 1. The rule
You send a number **1-5**. The LED shows its colour and the device replies with **6 − n**, mirroring the number within the range (1↔5, 2↔4, 3 stays 3).

| You send | LED colour | Device replies |
|---|---|---|
| 1 | red | 5 |
| 2 | blue | 4 |
| 3 | green | 3 |
| 4 | orange | 2 |
| 5 | cyan | 1 |

1 = red and 2 = blue came from the original request; 3-5 were chosen (with the user, 2026-09-18) to be easy to tell apart on the small on-board LED, and to avoid purple, which already means "firmware updating".

## 2. How to use it
The device must be powered, on Wi-Fi (LED green), and on the same network as you.

**From a phone or browser**, open the device's page and tap a button:
```
http://192.168.1.13/         (or http://prov-e01c64.local/ where .local names resolve)
```
The page shows five buttons and prints, for example: *Sent 1 → LED red, device replied 5*.

**From the command line:**
```powershell
Invoke-RestMethod http://192.168.1.13/color?n=1 -UseBasicParsing
# input reply color
#     1     5 red
```
```powershell
curl.exe "http://192.168.1.13/color?n=4"
# {"input":4,"reply":2,"color":"orange"}
```

The colour stays on until you send another number or the device changes state (e.g. losing Wi-Fi turns the LED back to yellow while it reconnects; connection status takes priority over a demo colour).

**Finding the address:** the board's IP is in its serial log and your router's client list. `firmware/ota_push.ps1 -Mac e01c64 -InfoOnly` also prints it (it caches the address in `firmware/.ota_last_ip.txt`).

## 3. Endpoints

| Endpoint | Reply |
|---|---|
| `GET /` | HTML page with buttons 1-5 |
| `GET /color?n=<1-5>` | `{"input":3,"reply":3,"color":"green"}` |
| `GET /color?n=9` | `400 Bad Request` — "Number out of range: use 1-5" |
| `GET /color` (no number) | `400 Bad Request` — "Send a number: /color?n=1 (1-5)" |

## 4. How it's built
- **`firmware/main/color_api.c`**: the two handlers, the colour table, and the reply rule. About 150 lines.
- **`firmware/main/status_led.c`**: new `STATUS_LED_CUSTOM` state plus `status_led_set_custom(r,g,b)`, which holds any colour you ask for.
- **`firmware/main/ota.c`**: calls `color_api_register()` right after starting its HTTP server, so the device runs **one** server on port 80 serving both the update endpoints and these. Its handler limit was raised from the default 8 to 12 to leave room for future endpoints.

Request path: `GET /color?n=3` → parse and validate → look up the colour → `status_led_set_custom()` → reply with JSON.

## 5. Security
These endpoints need **no token**, unlike `/update` and `/info`:
- the worst anyone on your Wi-Fi can do is change the colour of an LED, and
- requiring a header would make the page unusable from a browser.

If this ever controls something that matters (a relay, a motor), add the same token check `ota.c` uses, and accept it as a query parameter so the page can still work.

## 6. Ideas for later
- Remember the last colour across reboots (store it in NVS).
- Accept a colour directly (`/color?rgb=ff8800`) as well as a number.
- Add buttons to the phone app instead of the browser page.
- A `POST` variant, if this becomes an API other software drives.
