# ESP32-C6 BLE Wi-Fi Provisioning: System Design (v1)

> **Status:** v1.2 (v1 + local wireless updates + internet updates). **Last updated:** 2026-09-18
>
> This document is the **single source of truth** for the project. Any AI agent or human working on the code should read it first and keep it up to date. When a decision changes, edit the relevant section **and** add an entry to the [Changelog](#16-changelog).

---

## Table of contents
1. [Goal & context](#1-goal--context)
2. [Key decisions (and why)](#2-key-decisions-and-why)
3. [System architecture](#3-system-architecture)
4. [BLE protocol & endpoints](#4-ble-protocol--endpoints)
5. [End-to-end provisioning flow](#5-end-to-end-provisioning-flow)
6. [Device status LED & button](#6-device-status-led--button)
7. [Repository layout](#7-repository-layout)
8. [Part A: Firmware](#8-part-a-firmware-esp-idf-esp32-c6)
9. [Part B: Android app](#9-part-b-android-app-kotlin--jetpack-compose)
10. [Part C: iOS app (later)](#10-part-c-ios-app-later)
11. [Execution order & milestones](#11-execution-order--milestones)
12. [Verification / test plan](#12-verification--test-plan)
13. [Backlog (post-v1)](#13-backlog-post-v1)
14. [Documentation & commenting rules](#14-documentation--commenting-rules)
15. [Glossary](#15-glossary)
16. [Changelog](#16-changelog)

---

## 1. Goal & context
We have an **ESP32-C6 development kit** (ESP32-C6-DevKitC-1). The device has no screen or keyboard, so it can't be told which Wi-Fi network to join. We are building a **provisioning system**:

- **Firmware** on the ESP32-C6 exposes a BLE service that accepts Wi-Fi credentials.
- A **mobile app** finds the device over BLE, lets the user pick a Wi-Fi network, enters the password, sends it securely, and shows whether the device connected.

The mobile app is built for **Android first**, then **iOS**.

**v1 scope is intentionally basic:**
- discover the device over BLE
- connect securely
- pick a network
- send credentials
- report success or failure

All other features are listed in the [Backlog](#13-backlog-post-v1).

## 2. Key decisions (and why)

| # | Decision | Why |
|---|---|---|
| D1 | Use **Espressif's standard provisioning protocol** (`espressif/network_provisioning`: protocomm over BLE, with protobuf messages) instead of a custom GATT protocol | It is battle-tested and handles framing, encryption, Wi-Fi scanning, and status reporting. Espressif ships official Android and iOS libraries that speak it, which saves weeks of work on both sides. |
| D2 | **Native apps**: Kotlin + Jetpack Compose (Android), then Swift + SwiftUI (iOS) | Gives the best BLE reliability, and each platform can use Espressif's native provisioning library directly. The cost of two codebases is acceptable because the app is small. |
| D3 | **Security 1 with one shared Proof-of-Possession (PoP) string** for v1 | Simple to use on a dev kit. The session is still encrypted (Curve25519 key exchange + AES-CTR), so the Wi-Fi password never crosses BLE in plain text. The limitation: anyone who knows the shared PoP and is in BLE range can provision the device. |
| D4 | Defer the **QR code (Security 2 / SRP6a, per-device secret)** to a later version | Keeps v1 basic. The QR code would be printed on a sticker (or shown in the serial monitor during development). It holds the device name and a per-device secret. Switching to it later changes the security parameters, not the architecture. |
| D5 | Use the **on-board RGB LED** for status and the **BOOT button** for a developer reset | This hardware is already on the dev kit. The LED shows the user what the device is doing. The button makes repeated testing easy. |
| D6 | Use the **NimBLE** Bluetooth host stack | It uses less RAM and flash than Bluedroid, and Espressif recommends it for BLE-only use. |
| D7 | Free BLE memory after provisioning finishes | BLE is only needed during provisioning, so its RAM goes back to the application. |
| D8 | Test the firmware with **Espressif's official "ESP BLE Provisioning" app** before building our own | It proves the device side works on its own, so any later bug can be isolated to the app. |

**Default configuration values (v1):**
- **Device BLE name:** `PROV_` + the last 3 bytes of the Wi-Fi STA MAC address in hex, e.g. `PROV_A1B2C3`.
- **PoP:** `abcd1234`. Set in firmware via `menuconfig`, under *BLE Provisioning App Configuration*. The Android app pre-fills the same default and lets the user edit it.

> ⚠️ **Change the PoP for any real deployment.** The app and firmware defaults must match.

## 3. System architecture

```
┌──────────────────────── Android App (Kotlin/Compose) ────────────────────────┐
│ UI (Compose screens) → ViewModels → ProvisioningRepository                   │
│                                         │                                    │
│                         esp-idf-provisioning-android library                 │
│                         (BLE transport + Security1 + protobuf)               │
└─────────────────────────────────────────┬────────────────────────────────────┘
                                          │  BLE GATT (encrypted session)
┌─────────────────────────────────────────┴──── ESP32-C6 Firmware (ESP-IDF) ───┐
│ app_main → provisioning manager (network_provisioning, BLE scheme, NimBLE)   │
│          → Wi-Fi station (esp_wifi)                                          │
│          → NVS (stores credentials; "already provisioned?" check on boot)   │
│          → Status LED (on-board RGB LED, GPIO8, led_strip component)         │
│          → BOOT button (GPIO9) long-press = erase credentials (dev reset)    │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Firmware modules:** see [section 8](#8-part-a-firmware-esp-idf-esp32-c6).
- `main.c`: boot sequence
- `prov.c`: provisioning manager and its events
- `wifi.c`: Wi-Fi station
- `status_led.c`: LED patterns
- `reset_button.c`: BOOT long-press

**Android layers:** see [section 9](#9-part-b-android-app-kotlin--jetpack-compose).
- **UI**: Compose screens only; they render state and send user actions.
- **ViewModel**: holds screen state as `StateFlow`.
- **Repository**: the only code that talks to the Espressif library. It turns callbacks into coroutines and Flows.

## 4. BLE protocol & endpoints
All endpoints are provided by `network_provisioning`, so we write no custom GATT code in v1.

- Each endpoint is a GATT characteristic under one provisioning service.
- Each characteristic has a user-description descriptor carrying the endpoint name, which is how the phone library finds it.
- Every payload after `proto-ver` is protobuf, encrypted with the session key.

| Endpoint | Direction | Purpose |
|---|---|---|
| `proto-ver` | app ↔ device | Version and capabilities JSON (e.g. `wifi_scan`), not encrypted |
| `prov-session` | app ↔ device | Security 1 handshake: Curve25519 key exchange mixed with the PoP. A wrong PoP makes the handshake fail. |
| `prov-scan` | app → device → app | Starts a Wi-Fi scan on the device and returns SSIDs with RSSI, channel, and auth mode |
| `prov-config` | app → device → app | `set_config` (SSID/password) → `apply_config` → `get_status`, which the app polls until `CONNECTED` or `FAILED(reason)` |

**Failure reasons reported to the app:** `AuthError` (wrong password), `NetworkNotFound`.

**BLE advertising:** the device name is `PROV_XXXXXX`. The app filters scan results on the `PROV_` prefix.

## 5. End-to-end provisioning flow

```
 Phone app                                   ESP32-C6
    │                                           │  boot → NVS has creds?
    │                                           │   ├─ yes → connect Wi-Fi (LED green when up)
    │                                           │   └─ no  → start BLE provisioning (LED blue blink)
    │── BLE scan (filter "PROV_") ─────────────▶│
    │── connect GATT ──────────────────────────▶│  (LED solid blue)
    │── proto-ver ─────────────────────────────▶│
    │── prov-session (Security 1 + PoP) ───────▶│
    │── prov-scan ─────────────────────────────▶│  Wi-Fi scan
    │◀──────────────────────── network list ────│
    │   user picks SSID, types password         │
    │── prov-config: set_config + apply ───────▶│  (LED yellow fast blink) try to connect
    │── prov-config: get_status (poll) ────────▶│
    │◀──────────── CONNECTED / FAILED(reason) ──│
    │                                           │  success → creds already in NVS, stop BLE,
    │                                           │            free BLE memory, LED solid green
    │                                           │  failure → LED red ×3, state reset so the
    │                                           │            app can retry without reboot
```

1. **Boot.** The firmware checks NVS for saved Wi-Fi credentials.
   - If they exist, it connects directly. BLE never starts.
   - If not, it starts BLE provisioning.
2. **Find the device.** The app scans and lists `PROV_*` devices. The user taps one.
3. **Connect.** The app connects and completes the Security 1 session with the PoP.
4. **Pick a network.** The app asks the device to scan Wi-Fi, and the user picks a network (or types a hidden SSID) and enters the password.
5. **Send credentials.** The app sends them. The device tries to join the network.
6. **Report the result.** The device reports `CONNECTED` or `FAILED` (wrong password or network not found). The app shows success, or the error with a Retry button.
7. **Finish.** On success, provisioning stops and BLE memory is freed. On failure, the provisioning state is reset so the app can send new credentials over the same connection.
8. **Dev reset.** Holding BOOT for 3 s erases the credentials and reboots into provisioning mode.

## 6. Device status LED & button
**Hardware (ESP32-C6-DevKitC-1):**
- **RGB LED:** a single WS2812 addressable LED on **GPIO8**, driven by the `espressif/led_strip` component.
- **BOOT button:** on **GPIO9**, active low with a pull-up. GPIO9 is a strapping pin, but it is safe to read as an input once the app is running.

| State (`status_led_state_t`) | LED pattern | Triggered by |
|---|---|---|
| `STATUS_LED_PROVISIONING` | Blue, slow blink (500 ms on/off) | Provisioning started, waiting for the app |
| `STATUS_LED_BLE_CONNECTED` | Blue, solid | Phone connected over BLE |
| `STATUS_LED_WIFI_CONNECTING` | Yellow, fast blink (100 ms) | Credentials received, or connecting from saved creds |
| `STATUS_LED_WIFI_CONNECTED` | Green, solid | Got an IP address |
| `STATUS_LED_UPDATING` | Purple, fast blink | Receiving a firmware update over Wi-Fi (v1.1) |
| `STATUS_LED_CUSTOM` | Solid colour, held | Colour requested through the colour API (v1.1, see [COLOR_API.md](COLOR_API.md)) |
| `STATUS_LED_FAILED` | Red, 3 blinks, then back to the previous waiting state | Credentials failed |
| `STATUS_LED_OFF` | Off | Reset in progress |

**BOOT button:** held for **3 seconds** → the firmware erases the Wi-Fi credentials (`network_prov_mgr_reset_wifi_provisioning` / `esp_wifi_restore`), and `esp_restart()` reboots into provisioning mode.

## 7. Repository layout
```
ble app/
├── README.md                  # Quick start: build/flash firmware, run the app
├── .gitignore                 # Build outputs, local sdkconfig, managed_components
├── CLAUDE.md / AGENTS.md      # Instructions for AI agents (read DESIGN.md first)
├── docs/
│   ├── DESIGN.md              # ← this file (single source of truth)
│   ├── FIRMWARE_SETUP.md      # Step-by-step first build / flash / test guide
│   ├── OTA_UPDATES.md         # Wireless firmware updates: setup + daily workflow
│   ├── COLOR_API.md           # Number 1-5 -> LED colour + random reply (v1.1)
│   └── CLOUD_UPDATES.md       # Internet (pull) updates + version reporting (v1.2)
├── firmware/                  # ESP-IDF project (target esp32c6)
│   ├── build_and_flash.ps1    # One-command build + flash + monitor (Windows)
│   ├── ota_push.ps1           # Push firmware over Wi-Fi, same network (v1.1)
│   ├── publish_release.ps1    # Build + publish to GitHub Releases (v1.2)
│   ├── version.txt            # Firmware version embedded in the image (v1.2)
│   ├── partitions.csv         # Two app slots (ota_0/ota_1) for wireless updates
│   ├── CMakeLists.txt         # Top-level ESP-IDF project file
│   ├── sdkconfig.defaults     # Target, NimBLE, partition table, etc.
│   └── main/
│       ├── CMakeLists.txt     # Registers main component sources
│       ├── idf_component.yml  # Managed deps: network_provisioning, led_strip
│       ├── Kconfig.projbuild  # PoP, name prefix, LED/button GPIOs, timings
│       ├── main.c             # app_main: boot sequence
│       ├── prov.c / prov.h    # Provisioning manager start/stop + events
│       ├── wifi.c / wifi.h    # Wi-Fi STA init, connect, retry, IP events
│       ├── status_led.c / .h  # LED state machine task
│       ├── ota.c / ota.h      # Update server (HTTP) + mDNS name (v1.1)
│       ├── color_api.c / .h   # /color endpoints + demo page (v1.1)
│       ├── cloud_update.c / .h# Hourly internet update check + reporting (v1.2)
│       └── reset_button.c / .h# BOOT long-press → erase creds + restart
├── cloud/                     # Internet-update support (v1.2)
│   ├── manifest.json          # Current version + download URL (devices poll this)
│   ├── worker.js              # Cloudflare Worker collecting version reports
│   └── wrangler.toml          # Worker deployment settings
├── android/                   # Android Studio project (Part B)
│   ├── settings.gradle.kts    # Repos (google, mavenCentral, JitPack) + :app module
│   ├── build.gradle.kts       # Root: plugin declarations only
│   ├── gradle/libs.versions.toml  # Version catalog: every dependency version
│   ├── gradlew(.bat) + gradle/wrapper/  # Gradle 8.10.2 wrapper
│   └── app/
│       ├── build.gradle.kts   # compileSdk/targetSdk 35, minSdk 26, dependencies
│       └── src/main/
│           ├── AndroidManifest.xml     # BLE + Wi-Fi-state permissions, single activity
│           ├── res/                    # strings (all UI text), theme, launcher icon
│           └── java/com/blewifiprov/app/
│               ├── MainActivity.kt             # Hosts Compose
│               ├── data/
│               │   ├── ProvisioningConfig.kt   # Constants shared with firmware (PoP, prefix, UUID)
│               │   ├── Models.kt               # ScannedDevice, WifiNetwork, errors, ProvisionUpdate
│               │   ├── DeviceConnectionEventSubscriber.kt # EventBus -> callback bridge
│               │   └── ProvisioningRepository.kt # ONLY class touching the Espressif library
│               ├── permissions/BlePermissions.kt # Per-API-level permission rules
│               └── ui/
│                   ├── ProvisioningViewModel.kt  # Wizard state (activity-scoped, shared)
│                   ├── navigation/AppNavHost.kt  # Routes + back-stack rules
│                   ├── components/Common.kt      # Top bar, signal icon, dialogs
│                   ├── theme/Theme.kt
│                   └── screens/                  # Start, DeviceScan, Connect, WifiList, Password, Provision
└── ios/                       # Xcode project (Part C, later)
```

## 8. Part A: Firmware (ESP-IDF, ESP32-C6)
**Toolchain:**
- ESP-IDF **v5.x** (v5.3 or later), installed with the Windows ESP-IDF Installer or the VS Code ESP-IDF extension.
- Managed components are downloaded automatically by the IDF Component Manager on the first build.

**Components:**
- `espressif/network_provisioning`: provisioning manager, BLE scheme, Security 1
- `espressif/led_strip`: WS2812 driver using RMT
- Built into ESP-IDF: `esp_wifi`, `esp_netif`, `esp_event`, `nvs_flash`, NimBLE (`bt`)

**Milestones:**

| ID | Task | Done when |
|---|---|---|
| A1 | Install ESP-IDF, build and flash `hello_world` | Output appears in the serial monitor |
| A2 | Provisioning core: BLE scheme, NimBLE, Security 1, PoP, `PROV_` name, event handling, reset state on failure | Official app can provision |
| A3 | Saved credentials: skip BLE when already provisioned, reconnect on disconnect | Reboot reconnects without BLE |
| A4 | Status LED state machine | LED matches the table in §6 |
| A5 | BOOT long-press reset | 3 s hold → back to provisioning mode |
| A6 | Full test with Espressif's official "ESP BLE Provisioning" Android app | All firmware checks in §12 pass |

**Menuconfig options** (`main/Kconfig.projbuild`, menu *BLE Provisioning App Configuration*):

| Option | Default | Meaning |
|---|---|---|
| `APP_PROV_POP` | `abcd1234` | Security 1 Proof-of-Possession (must match the app) |
| `APP_PROV_NAME_PREFIX` | `PROV_` | BLE name prefix (must match the app's scan filter) |
| `APP_STATUS_LED_GPIO` | 8 | WS2812 RGB LED |
| `APP_RESET_BUTTON_GPIO` | 9 | BOOT button |
| `APP_RESET_HOLD_TIME_MS` | 3000 | Hold time to erase credentials |
| `APP_WIFI_FAST_RETRIES` | 5 | Immediate reconnects after a drop (provisioned mode only) |
| `APP_WIFI_RETRY_BACKOFF_MS` | 10000 | Delay between reconnects after the fast retries are used up |

**Retry behaviour:**
- **During provisioning**, `wifi.c` never retries by itself. A failed attempt is reported to the app as `FAILED(reason)`.
- **10 s after a failure**, `prov.c` calls `network_prov_mgr_reset_wifi_sm_state_on_failure()`, so the app can resend credentials over the same BLE session. The delay is longer than the Espressif Android library's 5 s status-poll interval, so the app always reads the real failure reason first. A retry sent within those 10 s is rejected, and the app asks the user to wait a moment.
- **Once provisioned**, `wifi.c` reconnects automatically: immediate retries first, then retries with back-off.
- **BLE service UUID:** `021a9004-0382-4aea-bff4-6b3f1c5adfb4`, the same one Espressif's examples use, for compatibility with the official apps.

**Wireless firmware updates (v1.1, added 2026-09-18).** Full guide: [OTA_UPDATES.md](OTA_UPDATES.md).
- **Why:** re-flashing over USB is impractical once a device is installed somewhere.
- **How:** `ota.c` starts an HTTP server when the device gets an IP. `POST /update` streams a new image into the spare app slot (`esp_ota_write`), verifies it, and reboots into it; `GET /info` reports the running version. Both need the header `X-Update-Token` (`CONFIG_APP_OTA_TOKEN`).
- **Discovery:** the `espressif/mdns` component publishes `prov-<mac>.local`, so the push script doesn't need the IP. The same service advertisement will serve the planned "My devices" screen.
- **Safety:** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` boots a new image in *pending verify*; `ota.c` marks it valid only after Wi-Fi comes up, so an update that breaks Wi-Fi is rolled back automatically.
- **Flash layout:** custom `partitions.csv` with `otadata` + two 1.94 MB app slots on 4 MB flash. `nvs` keeps its old offset/size, so provisioned devices keep their credentials when upgrading from v1.
- **Limits:** plain HTTP with a shared token (fine on a trusted LAN, not on a hostile one); pushing is laptop-only, the phone app can't update devices yet.
- **One-time USB flash per board:** the partition change can't be installed over the air.

**Key APIs** (from `network_provisioning/manager.h` and `network_provisioning/scheme_ble.h`):
- `network_prov_mgr_init(config)`: set up the manager with `network_prov_scheme_ble` and `NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM`
- `network_prov_mgr_is_wifi_provisioned(&bool)`: check for saved credentials
- `network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, pop, service_name, NULL)`: start BLE provisioning
- `network_prov_mgr_reset_wifi_sm_state_on_failure()`: let the app retry after a failure
- `network_prov_mgr_deinit()`: release the manager's resources
- Events on `NETWORK_PROV_EVENT`: `START`, `WIFI_CRED_RECV`, `WIFI_CRED_FAIL`, `WIFI_CRED_SUCCESS`, `END`
- Events on `PROTOCOMM_TRANSPORT_BLE_EVENT`: `CONNECTED`, `DISCONNECTED`

## 9. Part B: Android app (Kotlin + Jetpack Compose)
**Stack:**
- Kotlin, Jetpack Compose (Material 3), single activity, Navigation Compose
- MVVM with ViewModel + `StateFlow`, coroutines
- **minSdk 26**, **compileSdk/targetSdk 35**
- Espressif **`esp-idf-provisioning-android`** library, **lib-2.4.4**, via JitPack. It requires compileSdk 35.
- Build tooling: AGP 8.7.3, Gradle 8.10.2 (wrapper), Kotlin 2.0.21, JDK 17 (Android Studio's bundled JBR)

**Permissions:**

| Android version | Permissions |
|---|---|
| 12+ (API 31+) | `BLUETOOTH_SCAN` (declared `neverForLocation`, so no location permission is needed), `BLUETOOTH_CONNECT` |
| ≤ 11 (API ≤ 30) | `BLUETOOTH`, `BLUETOOTH_ADMIN` (install-time), `ACCESS_FINE_LOCATION` (runtime), and Location services must be on |
| All | `ACCESS_WIFI_STATE`, `CHANGE_WIFI_STATE`, `ACCESS_NETWORK_STATE` (normal permissions; the Espressif library calls Wi-Fi APIs internally) |

On every version, the Start screen asks the user to turn on Bluetooth (and Location on Android 11 and lower) if they are off.

**Screens (v1):**

| # | Screen | Purpose |
|---|---|---|
| 1 | Start / Permissions | Explains the app, requests permissions, checks Bluetooth is on |
| 2 | Device scan | Lists `PROV_*` devices, nearest first, each with an **estimated distance** ("≈ 0.5 m") and a signal icon, plus a Rescan button. The distance comes from smoothed RSSI using a log-distance model (`data/DistanceEstimator.kt`: RSSI at 1 m = -50 dBm, path-loss exponent 2.5). It's approximate, often off by 2× or more indoors. |
| 3 | Connecting | BLE connect and Security 1 session. PoP is pre-filled and editable under "Advanced". Shows errors. |
| 4 | Wi-Fi networks | Networks scanned by the device (RSSI, lock icon for secured networks), plus manual SSID entry |
| 5 | Password | Password field with a show/hide toggle |
| 6 | Provisioning progress | Sending credentials → Applying → Connecting. Back is blocked while in progress. |
| 7 | Result | Success, or failure with the reason, plus Retry and Done buttons. Screens 6 and 7 share one destination (`ProvisionScreen`). |

**Implementation notes (things that aren't obvious from the library docs):**
- **Main thread.** The library creates `Handler`s without an explicit Looper, so every call into it runs on `Dispatchers.Main` (done in `ProvisioningRepository`).
- **Connection events.** Connect and disconnect are reported through **greenrobot EventBus** (`DeviceConnectionEvent`), not listeners. The subscriber must be a public class.
- **Session setup.** The Security 1 session is set up right after connecting (`ESPDevice.initSession`), so a wrong PoP shows up on the Connect screen.
- **Status polling.** The library polls `get_status` every **5 s**. That's why the firmware waits 10 s before resetting after a failure.
- **Retry cooldown.** A retry within that cooldown gets `wifiConfigFailed`, which the app shows as "device still recovering, wait ~10 s" (`ProvisionError.CONFIG_REJECTED`).
- **Expected disconnect.** After success the firmware turns BLE off, so the ViewModel ignores that disconnect.
- **Shared ViewModel.** One activity-scoped `ProvisioningViewModel` holds the whole wizard, because each step depends on the previous one.

**Build & run:**
```sh
cd android
./gradlew assembleDebug      # APK: app/build/outputs/apk/debug/app-debug.apk
./gradlew installDebug       # install on a USB-connected phone (USB debugging on)
```
Or open `android/` in Android Studio and press Run. Use a **physical phone**, because emulators have no BLE.

**Milestones:**
- **B1:** project setup, permissions, BLE scan
- **B2:** connect with Security 1 and the PoP, fetch the Wi-Fi list
- **B3:** send credentials, poll status, result screens
- **B4:** error and edge cases: Bluetooth turned off mid-flow, device lost, timeouts, wrong PoP, rotation, process death

## 10. Part C: iOS app (later)
- SwiftUI app using Espressif's **`ESPProvision`** library (Swift Package Manager or CocoaPods).
- Same screens and Repository → ViewModel → View layering as Android.
- Needs a Mac with Xcode and a **physical iPhone**, because the simulator has no Bluetooth.
- Needs `NSBluetoothAlwaysUsageDescription` in `Info.plist`.
- **The firmware needs no changes.**

## 11. Execution order & milestones
0. **Documentation first:** this file, `README.md`, and `CLAUDE.md`/`AGENTS.md`.
1. **Firmware A1–A6**, tested with Espressif's official app. This locks down the device side.
2. **Android B1–B4**, against the tested firmware.
3. **End-to-end hardening:** repeated provisioning cycles, wrong passwords, out-of-range devices.
4. **v1 done.** Then iOS (Part C).

## 12. Verification / test plan
**Firmware:**
- [ ] `idf.py set-target esp32c6 && idf.py build flash monitor` builds and flashes with no errors.
- [ ] On a fresh device, the logs show provisioning started with name `PROV_XXXXXX`, and the LED blinks blue.
- [ ] Espressif's official app finds and provisions the device. The logs show `got ip`, and the LED is solid green.
- [ ] Wrong password: the app shows an auth error, the LED blinks red 3 times, and retrying with the correct password works **without rebooting**.
- [ ] Reboot after provisioning: the device connects straight to Wi-Fi and BLE does not start.
- [ ] Hold BOOT for 3 s: the credentials are erased, the device reboots, and it is back in provisioning mode.

**Android:**
- [ ] Run on a **physical phone** (emulators have no BLE).
- [ ] Full flow succeeds, and the device appears on the router's client list or can be pinged.
- [ ] Wrong PoP shows a clear error.
- [ ] Wrong Wi-Fi password shows a clear error, and Retry works.
- [ ] Hidden SSID entered manually works.
- [ ] Turning Bluetooth off mid-flow is handled without a crash.
- [ ] Denied permissions are explained, with a path to Settings.

## 13. Backlog (post-v1)
- **⭐ TOP PRIORITY — Internet (pull-based) firmware updates.** Added 2026-09-18 20:10. Today's OTA push (v1.1) only works when the laptop and the board sit on the **same** Wi-Fi: a home router blocks inbound connections, so a laptop elsewhere can't reach the device. Fix by inverting the direction — the device **pulls** instead of us pushing.
  - **How:** publish the firmware plus a small version manifest (version + download URL + checksum) at a fixed HTTPS address (GitHub Releases is the easiest free host; an S3 bucket or any web server works). The device checks that manifest on boot and on a timer, compares it with its own running version, and downloads only when they differ — ESP-IDF's `esp_https_ota` does the transfer, reusing the two-slot layout and rollback we already have.
  - **Why this over the alternatives:** works through any router with no port forwarding, from anywhere, and scales to many devices at once. It's also the standard approach for shipped IoT products.
  - **Security:** HTTPS with the server certificate pinned in firmware, so nobody can serve the device fake firmware. No inbound port is ever opened.
  - **Trade-off:** updates arrive on the device's polling schedule (e.g. hourly) rather than instantly. Add an MQTT "check now" nudge later if instant delivery matters.
  - **Effort:** roughly a day — a version-check module, HTTPS OTA wiring, a certificate bundle, and a small publish script on our side. `ota_push.ps1` stays for same-network development.
  - **Alternatives considered and rejected:** a VPN such as Tailscale (no firmware work, but needs setup at every site); port forwarding with dynamic DNS (exposes the update endpoint to the internet); a full MQTT/IoT platform (overkill until there are many devices).
- **"My devices" list (planned as the first v2 feature, after the v1 end-to-end test passes).** v1 forgets a device after "All set!", and the device turns BLE off once it's on Wi-Fi, so the app can't show what's been added. Recommended design: combine a local history with Wi-Fi discovery.
  - **A. Local history (app only):** after a successful provision, save the device name, BLE MAC, SSID and date on the phone (e.g. Room or DataStore). A new "My devices" home screen lists them, with a button to add a device (the current wizard).
  - **B. Wi-Fi discovery via mDNS:** once the firmware gets an IP, it announces itself with the `mdns` component: hostname `prov-xxxxxx.local` and a service such as `_blewifiprov._tcp` whose TXT record holds the device name. The app browses for that service with Android's `NsdManager` (iOS later: `NWBrowser`) while the list is open.
  - **Result:** each saved device shows **Online / Offline** and its **IP address**, matched by device name. No cloud or extra hardware needed.
  - **Caveats:** the phone must be on the same Wi-Fi network. Some routers (guest or client-isolation modes) block mDNS, in which case devices show "Offline" but still work. On Android, `NsdManager` needs the `NEARBY_WIFI_DEVICES`/multicast setup to be checked on Android 13+.
  - This covers the earlier "Device IP shown in the app" item.
- **QR code provisioning:** Security 2 (SRP6a) with a per-device username and password, shown in the serial monitor during development and printed on a sticker in production. The app scans the QR code, which carries the device name and secret.
- **BOOT-button confirmation:** the device accepts credentials only after the user presses a physical button, as proof of physical presence.
- **Extra settings:** a custom endpoint for device name, cloud/MQTT URL, or auth token.
- **Factory reset from the app.**
- **Device IP shown in the app** after connecting. Covered by the "My devices" list above.
- **iOS app** (Part C).

## 14. Documentation & commenting rules
These rules apply to **every file** in the repo, whether written by a person or an AI agent.
1. **File header comment:** what the file does, where it fits in the architecture (link to the section of this doc), and its key dependencies.
2. **Doc comment on every function, class, type, and public constant:**
   - Doxygen `/** ... */` in C
   - KDoc in Kotlin
   - `///` in Swift
   - Cover purpose, parameters, return value, side effects, and the threading context (e.g. "called from the event loop task").
3. **Inline comments explain the *why***: event ordering, timing values, GPIO numbers, security choices, workarounds.
4. **Config and build files** (`Kconfig.projbuild`, `sdkconfig.defaults`, `idf_component.yml`, `CMakeLists.txt`, Gradle files, `AndroidManifest.xml`) get a comment for every non-default setting and every permission.
5. **Update this document** whenever behavior or a decision changes, and add a Changelog entry.

## 15. Glossary
- **Provisioning:** giving a device the configuration it needs (here, Wi-Fi credentials) after manufacture.
- **BLE / GATT:** Bluetooth Low Energy / Generic Attribute Profile. Data is exchanged through *characteristics* grouped into *services*.
- **protocomm:** Espressif's transport-independent secure request/response layer that provisioning runs on.
- **PoP (Proof of Possession):** a shared secret mixed into the Security 1 key exchange. A client without it can't set up a session.
- **Security 1 / Security 2:** Espressif's provisioning security schemes. Sec1: X25519 + AES-CTR + PoP. Sec2: SRP6a + AES-GCM with a username and password.
- **NVS:** Non-Volatile Storage, a key-value store in flash. Wi-Fi credentials are saved here.
- **NimBLE:** a lightweight BLE host stack used by ESP-IDF.
- **Strapping pin:** a GPIO sampled at reset to select boot mode (GPIO9 on the C6). It's safe to use as an input after boot.

## 16. Changelog
> Convention (set 2026-09-18): every entry records **when** the change was made and **why**, not just what changed, so anyone reading later understands the motivation without digging through the code.

| Date & time | Change | Why |
|---|---|---|
| 2026-09-18 22:55 | **v1.2 built: internet firmware updates.** New `cloud_update.c/.h` (NTP clock sync, hourly manifest poll with jitter, HTTPS download via `esp_https_ota`, bad-version blacklist in NVS, status reporting, token-protected `/check-update`), `firmware/version.txt` for real version numbers, `firmware/publish_release.ps1` (build → GitHub release → manifest), `cloud/worker.js` + `wrangler.toml` (free Cloudflare service collecting version reports), guide in [CLOUD_UPDATES.md](CLOUD_UPDATES.md). Project also put under git with an initial commit. | The user needs to update boards that are on a different Wi-Fi from the laptop, plus automatic delivery and a way to confirm remotely that a version landed. Chosen settings: hourly polling, GitHub Releases hosting, cloud status endpoint. |
| 2026-09-18 20:10 | Backlog: added **internet (pull-based) firmware updates** as the top item, ahead of the "My devices" list. | The user asked how to update a board that's on a different Wi-Fi from the laptop. v1.1 pushes only work on one network, so remote updates need the device to pull from an HTTPS URL instead. |
| 2026-09-18 19:45 | Colour API changed: new colours (yellow, magenta, white, spring green, indigo) and the reply is now a **random 6-9** from the hardware RNG instead of the fixed `6 - n`. Pushed over Wi-Fi (build `97ed11db`, slot `ota_1`) and verified, including that repeated identical inputs give varying replies. | User request. New colours avoid the status colours (blue/green/purple/red); solid yellow is safe because the status yellow only blinks. |
| 2026-09-18 19:10 | **Colour API added** (`color_api.c/.h`, new `STATUS_LED_CUSTOM` state): `GET /color?n=1..5` sets the LED colour (red, blue, green, orange, cyan) and replies with `6 - n`; `GET /` serves a button page. Registered on the HTTP server `ota.c` already runs. Delivered to the board **over Wi-Fi** (build `f9618173`) and all five inputs verified. Guide: [COLOR_API.md](COLOR_API.md). | User request: enter a number, see a colour, get a different number back. Reusing the existing server keeps one port and one code path; the endpoints skip the token because they only change an LED and must work from a browser. |
| 2026-09-18 18:35 | OTA verified end to end: firmware pushed over Wi-Fi to `PROV_E01C64`, which rebooted into slot `ota_1` running build `52a7c124`, no cable involved. | Proves the v1.1 update path works on real hardware. |
| 2026-09-18 18:25 | `/info` and the boot log now report the first 4 bytes of the **ELF SHA-256** as a build fingerprint (`ota.c`, `main.c`). | ESP-IDF doesn't regenerate the app_desc *compile timestamp* on incremental builds, so it still showed 17:44 for an 18:10 build, making a successful update look like nothing had happened. The hash always changes with the code. |
| 2026-09-18 18:20 | `ota_push.ps1`: added `-UseBasicParsing` to every HTTP call. | Windows PowerShell 5.1 otherwise parses responses with the legacy Internet Explorer engine and throws "Object reference not set to an instance of an object". The pushes had actually succeeded; only the client-side reply handling failed. |
| 2026-09-18 18:15 | `ota_push.ps1`: upload switched from an in-memory byte array to `-InFile` streaming. | A 1.4 MB byte[] body crashes Invoke-WebRequest on PowerShell 5.1; streaming also keeps the image out of memory. |
| 2026-09-18 18:05 | `ota_push.ps1`: device discovery now falls back from mDNS to a cached last-known IP, then to probing neighbours from the ARP table (`.ota_last_ip.txt`). | Windows can't resolve `.local` names at all (its DNS client ignores mDNS, and ping and .NET both fail), and the hand-rolled mDNS query got no reply on this network. Without a fallback, every push would need the IP typed by hand. |
| 2026-09-18 18:10 | Boot log now prints the firmware build stamp and active slot (`main.c`). | While testing updates, the only way to tell old firmware from new was over the network; the serial log now answers it directly. |
| 2026-09-17 | Second ESP32-C6 board flashed with the same firmware, no code changes (COM4, advertises as `PROV_F8ADB4`). Confirms per-board unique names from the MAC suffix. |
| 2026-09-17 | Android scan list now shows estimated distance instead of raw dBm (smoothed RSSI, log-distance model). |
| 2026-09-18 | **v1.1: wireless firmware updates (OTA).** New `firmware/main/ota.c` (HTTP update server + mDNS), `partitions.csv` (two 1.94 MB app slots on 4 MB flash), rollback enabled, purple "updating" LED, new `firmware/ota_push.ps1`, guide in `docs/OTA_UPDATES.md`. Board `PROV_E01C64` flashed with it over USB, kept its Wi-Fi credentials, reconnected as 192.168.1.13 and serves `prov-e01c64.local`. First push not yet verified: the laptop was on a different network at the time. |
| 2026-09-17 | Backlog: added the "My devices" list (local history + mDNS online status) as the first v2 feature, to start after the v1 end-to-end test. |
| 2026-09-17 | **Firmware built & flashed** (A1 done). ESP-IDF v5.5.5 resolved `network_provisioning` 1.2.4 and `led_strip` 3.0.3 (pinned in `firmware/dependencies.lock`). App image 0x151d80 bytes, 10% of the 1.5 MB partition free. The board (ESP32-C6FH4, 4 MB flash, CP210x on COM3) boots and advertises as `PROV_E01C64`. `build_and_flash.ps1` now reads `C:\Espressif\esp_idf.json` to pick the installer's Python venv, because plain `export.ps1` and `Initialize-Idf.ps1` both fail when run from a script. |
| 2026-09-17 | Android app installed and running on a OnePlus Nord 3 (Android 16); the scan screen works. |
| 2026-09-17 | Android app B1–B3 written (all 7 screens, repository, ViewModel). Not yet tested against hardware. |
| 2026-09-17 | Firmware: failure-reset delay raised from 5 s to 10 s (longer than the Android library's 5 s status poll). Added `firmware/build_and_flash.ps1` and `docs/FIRMWARE_SETUP.md`. |
| 2026-09-17 | Firmware A2–A5 code written (`firmware/`): prov, wifi, status_led, reset_button modules. Not yet built or flashed on hardware. |
| 2026-09-17 | Initial v1 design: Espressif protocol, Security 1 with shared PoP, NimBLE, LED + BOOT button, native Android first. QR code / Security 2 deferred. |
