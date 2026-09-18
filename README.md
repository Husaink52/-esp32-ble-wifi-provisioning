# ESP32-C6 BLE Wi-Fi Provisioning

A phone app sends Wi-Fi credentials to an **ESP32-C6** over **Bluetooth Low Energy**, so a device without a screen can join your network.

| Part | Folder | Tech | Status |
|---|---|---|---|
| Firmware | [`firmware/`](firmware/) | ESP-IDF v5.x, `network_provisioning`, NimBLE | ✅ v1.1 (provisioning + wireless updates) |
| Android app | [`android/`](android/) | Kotlin, Jetpack Compose, `esp-idf-provisioning-android` | 🚧 v1 in progress |
| iOS app | `ios/` | Swift, SwiftUI, `ESPProvision` | ⏳ Later |

📐 **Full design, decisions, and roadmap: [`docs/DESIGN.md`](docs/DESIGN.md)**. Read it first.

---

## How it works (v1)
1. On first boot, the ESP32-C6 advertises over BLE as `PROV_XXXXXX`, and its LED blinks blue.
2. The app finds the device, connects, and sets up an encrypted session (Security 1 with the Proof-of-Possession `abcd1234` by default).
3. The device scans for Wi-Fi networks. You pick one in the app and enter the password.
4. The device joins the network, saves the credentials, and turns its LED green. The app shows success.
5. On later boots, the device reconnects automatically. **Hold BOOT for 3 s** to erase the credentials and provision again.

## Firmware: build & flash
> 👉 **Step-by-step guide: [`docs/FIRMWARE_SETUP.md`](docs/FIRMWARE_SETUP.md)**. Or run `firmware\build_and_flash.ps1` from an ESP-IDF PowerShell.

Prerequisites: [ESP-IDF v5.3+](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/index.html), installed with the Windows installer or the VS Code ESP-IDF extension.

Run these in an **ESP-IDF terminal**:
```sh
cd firmware
idf.py set-target esp32c6          # only needed once
idf.py menuconfig                  # optional: "BLE Provisioning App Configuration" → change PoP
idf.py build
idf.py -p COMx flash monitor       # replace COMx with your board's port; Ctrl+] exits the monitor
```
The IDF Component Manager downloads the managed components (`network_provisioning`, `led_strip`) on the first build.

**Test without our app:** install Espressif's **"ESP BLE Provisioning"** app from the Play Store or App Store. Choose *Provision New Device* → *I don't have a QR code*, select `PROV_XXXXXX`, and enter PoP `abcd1234`.

## Colour demo (v1.1)
With the device on Wi-Fi, open `http://<device-ip>/` and tap 1-5: the LED changes colour and the device replies with `6 - n` (1→red→5, 2→blue→4, 3→green→3, 4→orange→2, 5→cyan→1). Details: [`docs/COLOR_API.md`](docs/COLOR_API.md).

## Wireless firmware updates (v1.1)
After one USB flash, new firmware goes to the board over Wi-Fi:
```powershell
cd firmware
.uild_and_flash.ps1 -BuildOnly     # compile your changes
.\ota_push.ps1 -Mac e01c64           # send them over Wi-Fi (same network required)
```
Details, endpoints and troubleshooting: [`docs/OTA_UPDATES.md`](docs/OTA_UPDATES.md).

## Android app
Prerequisites: Android Studio (it provides JDK 17 and the Android SDK) and a **physical Android 8.0+ phone** with USB debugging turned on. Emulators have no Bluetooth LE.

```sh
cd android
./gradlew installDebug        # or open android/ in Android Studio and press Run
```
If `./gradlew` can't find Java, point `JAVA_HOME` at Android Studio's JDK, e.g. `C:\Program Files\Android\Android Studio\jbr`.

**App flow:** allow Bluetooth permission → pick `PROV_XXXXXX` → the app connects (PoP `abcd1234`, editable under *Advanced*) → pick a Wi-Fi network → enter the password → the device connects and the app shows *All set!*

Architecture and implementation notes: [`docs/DESIGN.md` §9](docs/DESIGN.md#9-part-b-android-app-kotlin--jetpack-compose).

## Contributing (humans & AI agents)
- Keep [`docs/DESIGN.md`](docs/DESIGN.md) up to date, including its changelog.
- **Comment every file**: a header comment, doc comments on every function, and inline comments that explain *why*. See the rules in [`docs/DESIGN.md` §14](docs/DESIGN.md#14-documentation--commenting-rules).
