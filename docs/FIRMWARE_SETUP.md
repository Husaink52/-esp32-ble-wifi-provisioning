# Firmware setup: first build, flash & test

> Run these steps **after ESP-IDF v5.5.5 is installed**. Part of milestones A1 and A6 in [DESIGN.md](DESIGN.md#11-execution-order--milestones).
>
> **Quick path:** open the **ESP-IDF 5.5 PowerShell** from the Start menu, then run:
> ```powershell
> cd "C:\Users\husai\Desktop\ble app\firmware"
> .\build_and_flash.ps1
> ```
> The script ([`firmware/build_and_flash.ps1`](../firmware/build_and_flash.ps1)) does steps 2–5 below for you. Run `Get-Help .\build_and_flash.ps1 -Full` for its options.

---

## 1. Open an ESP-IDF terminal
- Use the **"ESP-IDF 5.5 PowerShell"** (or CMD) shortcut the installer adds to the Start menu. It puts `idf.py` and the toolchain on your `PATH`.
- A normal terminal won't find `idf.py`. The script tries to activate ESP-IDF automatically; if that fails, pass `-IdfPath C:\path\to\esp-idf`.
- If PowerShell blocks the script ("running scripts is disabled"), run this once for the current terminal: `Set-ExecutionPolicy -Scope Process Bypass`

Check that it works:
```powershell
idf.py --version     # should print v5.5.5
```

## 2. Connect the board
1. Plug the ESP32-C6-DevKitC-1 in using the USB port labelled **UART**.
2. Open **Device Manager → Ports (COM & LPT)** and note the COM number (e.g. `COM5`).
3. If no port appears, try a different cable (some are charge-only) and install the board's USB-to-UART driver.

## 3. Set the target (first time only)
```powershell
cd "C:\Users\husai\Desktop\ble app\firmware"
idf.py set-target esp32c6
```
This creates `firmware/sdkconfig` from [`sdkconfig.defaults`](../firmware/sdkconfig.defaults).

## 4. (Optional) Change settings
```powershell
idf.py menuconfig
```
Go to **BLE Provisioning App Configuration** to change the PoP (default `abcd1234`), the name prefix, GPIOs or timings. If you change the PoP, use the same value in the app.

## 5. Build, flash & monitor
```powershell
idf.py build                        # first build downloads network_provisioning + led_strip
idf.py -p COM5 flash monitor        # replace COM5 with your port; Ctrl+] exits the monitor
```

**Expected output on a fresh board:**
```
I (...) app: ESP32-C6 BLE Wi-Fi provisioning firmware starting
I (...) status_led: Status LED ready on GPIO8
I (...) reset_btn: Hold the button on GPIO9 for 3000 ms to erase Wi-Fi credentials
I (...) prov: Starting BLE provisioning as "PROV_XXXXXX" (Security 1)
I (...) prov: Provisioning started, waiting for the phone app
```
The LED should **blink blue**.

## 6. Test with Espressif's official app (milestone A6)
1. Install **"ESP BLE Provisioning"** from the Google Play Store.
2. Tap **Provision New Device**, then **I don't have a QR code**.
3. Select `PROV_XXXXXX` and enter the PoP **`abcd1234`**.
4. Pick your 2.4 GHz Wi-Fi network and enter its password.
5. Check the following:

| Check | Expected result |
|---|---|
| Phone connects over BLE | Log `Phone connected over BLE`, LED **solid blue** |
| Correct PoP | Log `Secure session established (PoP accepted)` |
| Credentials sent | Log `Received Wi-Fi credentials for SSID "..."`, LED **yellow fast blink** |
| Success | Log `Connected, got IP x.x.x.x`, LED **solid green**, app shows success |
| Wrong password | Log `Connection failed: authentication error`, LED **red ×3**, then `Provisioning state reset` about 10 s later. A retry then works without rebooting. |
| Reboot (press RESET) | Log `Already provisioned, connecting with saved credentials`, no BLE, LED green |
| Hold BOOT for 3 s | Log `Erasing Wi-Fi credentials...`, reboot, back to blue blinking |

## 7. If something goes wrong
- **Build errors:** copy the **first** error from the output and share it (with an AI agent or a teammate). Managed component versions resolve on the first build and can need small API adjustments.
- **`Failed to connect to ESP32-C6`:** hold **BOOT**, tap **RESET**, release BOOT, then flash again. Also close any other program that has the COM port open.
- **Device not visible in the app:** check the log shows `Starting BLE provisioning`. If it says `Already provisioned`, hold BOOT for 3 s.
- **Wi-Fi never connects:** the ESP32-C6 only supports **2.4 GHz**, so a 5 GHz-only network won't work.
- **Weird configuration after changing `sdkconfig.defaults`:** delete `firmware/sdkconfig` and run `.\build_and_flash.ps1 -Clean`.
