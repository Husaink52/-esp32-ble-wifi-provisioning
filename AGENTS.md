# Instructions for AI agents (and humans)

<!--
  This file is read automatically by AI coding agents (Claude Code reads CLAUDE.md,
  which points here; other tools read AGENTS.md directly). Keep it short: detailed
  design lives in docs/DESIGN.md.
-->

## 1. Read first
**[`docs/DESIGN.md`](docs/DESIGN.md)** is the single source of truth. Task guides: [`docs/FIRMWARE_SETUP.md`](docs/FIRMWARE_SETUP.md) (first flash) and [`docs/OTA_UPDATES.md`](docs/OTA_UPDATES.md) (wireless updates). It covers the architecture, the decisions and the reasons for them, the BLE protocol, LED states, milestones, the test plan, and the backlog. Read it before changing anything.

## 2. Project in one paragraph
This project provisions Wi-Fi credentials over BLE:
- **ESP32-C6 firmware** (`firmware/`, ESP-IDF, `espressif/network_provisioning`, NimBLE, Security 1 with a shared PoP)
- **native Android app** (`android/`, Kotlin + Compose, Espressif's `esp-idf-provisioning-android`)
- **iOS app** (`ios/`, Swift), later

**v1 is deliberately basic.** Don't add backlog features (QR code / Security 2, button confirmation, custom config endpoints, factory reset from the app) unless the user asks.

## 3. Mandatory rules
1. **Comment everything.** Every file you create or edit needs:
   - a **header comment**: what the file does, where it fits in the architecture (reference the DESIGN.md section), and its key dependencies
   - **doc comments** on every function, class, type, and public constant (Doxygen `/** */` in C, KDoc in Kotlin, `///` in Swift), covering purpose, parameters, return value, side effects, and which task or thread calls it
   - **inline "why" comments** for anything not obvious: event ordering, timings, GPIO numbers, security choices, workarounds
   - comments in **config and build files** for every non-default setting and permission
2. **Keep the docs current.** If behavior, configuration, or a decision changes, update `docs/DESIGN.md` and add a line to its **Changelog**.
3. **Keep firmware and app in sync.** The default PoP (`abcd1234`) and the BLE name prefix (`PROV_`) must match on both sides.
4. **Follow the existing layering.** Firmware modules each own one job (`prov`, `wifi`, `status_led`, `reset_button`). On Android, UI → ViewModel → Repository; only the Repository touches the Espressif library.

## 4. Build commands
- **Firmware** (in an ESP-IDF terminal): `cd firmware && idf.py set-target esp32c6 && idf.py build`, then `idf.py -p COMx flash monitor`
- **Firmware helper:** `firmware/build_and_flash.ps1` (see `docs/FIRMWARE_SETUP.md`)
- **Wireless firmware update:** `firmware/ota_push.ps1 -Mac <mac suffix>` (see `docs/OTA_UPDATES.md`); needs the laptop and board on the same Wi-Fi
- **Colour demo endpoints:** `GET /` and `GET /color?n=1..5` on the device (see `docs/COLOR_API.md`)
- **Publish an internet update:** `firmware/publish_release.ps1 -Version x.y.z` (see `docs/CLOUD_UPDATES.md`); bump `firmware/version.txt` every release
- **Android:** `cd android && ./gradlew assembleDebug`. Needs JDK 17+, e.g. `JAVA_HOME="C:/Program Files/Android/Android Studio/jbr"`. Test on a physical phone, because emulators have no BLE.
