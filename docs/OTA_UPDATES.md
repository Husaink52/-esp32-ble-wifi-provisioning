# Wireless firmware updates (OTA)

> **Added to v1** on 2026-09-18 as **v1.1**. Design rationale and the flash layout are in [DESIGN.md §8](DESIGN.md#8-part-a-firmware-esp-idf-esp32-c6).
>
> **In one line:** after one USB flash, new firmware is sent to the board over Wi-Fi with `.\ota_push.ps1 -Mac <mac suffix>`.

---

## 1. What it does
- The board runs a small HTTP server once it's on Wi-Fi.
- You push a firmware image to it from the laptop. It writes it to a **spare firmware slot**, verifies it, and reboots into it.
- If the new firmware can't rejoin Wi-Fi, the bootloader **rolls back** to the previous one, so a bad update can't leave the board unreachable.
- The board is reachable by name: **`prov-<mac suffix>.local`** (e.g. `prov-e01c64.local`), so its IP address can change without breaking anything.

## 2. One-time setup per board (USB)
The update system needs two firmware slots, which is a change to the chip's flash layout, and that can only be installed over USB.

```powershell
cd "C:\Users\husai\Desktop\ble app\firmware"
.\build_and_flash.ps1 -Port COM3 -Clean
```

- **Wi-Fi credentials survive** this flash: the `nvs` partition keeps the same offset and size as the old layout.
- After the reboot, the board rejoins Wi-Fi (LED green) and the update server starts.

## 2b. How the script finds the board
Windows can't resolve `.local` names by itself, so `ota_push.ps1` tries three things in order, and caches whatever worked in `firmware/.ota_last_ip.txt`:
1. **mDNS query** sent directly to the network (works where multicast isn't blocked).
2. **Last known address** from the cache file, re-checked before use.
3. **Probing neighbours** from the PC's ARP table, asking each for `/info` until one identifies itself as the device.

If all three fail (e.g. the board is on another subnet), pass the address: `-Device 192.168.1.13`.

## 3. Everyday workflow (wireless)
```powershell
cd "C:\Users\husai\Desktop\ble app\firmware"

# 1. Compile your changes (no board connection needed)
.\build_and_flash.ps1 -BuildOnly

# 2. Send them to the board over Wi-Fi
.\ota_push.ps1 -Mac e01c64
```

The script prints what the board is running, uploads the image (the LED blinks **purple**), waits for the reboot, and prints the new version so you can confirm the update took effect.

**Useful variations:**
```powershell
.\ota_push.ps1 -Mac e01c64 -InfoOnly            # just show what's running
.\ota_push.ps1 -Device 192.168.31.50            # use an IP if .local doesn't resolve
.\ota_push.ps1 -Mac e01c64 -Token "my-secret"   # non-default update token
```

The MAC suffix is the same one in the board's Bluetooth name: `PROV_E01C64` → `-Mac e01c64`.

## 4. Endpoints
Both require the header `X-Update-Token: <CONFIG_APP_OTA_TOKEN>`, so other devices on the Wi-Fi can't push firmware or read device details.

| Endpoint | Purpose |
|---|---|
| `GET /info` | JSON: project, version, compile time, **build fingerprint** (`elf_sha`), ESP-IDF version, running slot, uptime |
| `POST /update` | Body is the raw `build/ble_wifi_prov.bin`; installs it and reboots |

Manual example, without the script:
```powershell
curl.exe -H "X-Update-Token: update-me" http://prov-e01c64.local/info
curl.exe -X POST -H "X-Update-Token: update-me" --data-binary "@build/ble_wifi_prov.bin" http://prov-e01c64.local/update
```

## 5. Settings (`idf.py menuconfig` → *BLE Provisioning App Configuration*)

| Option | Default | Meaning |
|---|---|---|
| `APP_OTA_TOKEN` | `update-me` | Shared secret for the update endpoints. **Change it** for anything beyond a home network. |
| `APP_OTA_PORT` | 80 | Port the update server listens on |
| `APP_MDNS_PREFIX` | `prov-` | Network-name prefix: `prov-e01c64.local` |

## 6. Flash layout (4 MB)

| Offset | Partition | Size | Purpose |
|---|---|---|---|
| 0x009000 | `nvs` | 24 KB | Wi-Fi credentials. Same place as the old layout, so they survive the upgrade. |
| 0x00F000 | `otadata` | 8 KB | Records which slot to boot |
| 0x011000 | `phy_init` | 4 KB | Radio calibration |
| 0x020000 | `ota_0` | 1.94 MB | Firmware slot A |
| 0x210000 | `ota_1` | 1.94 MB | Firmware slot B |

The firmware is about 1.4 MB, so each slot has plenty of room. The full file is [`firmware/partitions.csv`](../firmware/partitions.csv).

## 7. How the safety net works
1. A new image is installed and booted in **"pending verify"** state.
2. `ota.c` marks it **valid** only after the device gets an IP address.
3. If it crashes on boot or never reaches Wi-Fi, the bootloader boots the **previous slot** on the next restart.
4. So the worst case for a bad update is a board that returns to the old firmware, not a dead board.

To recover manually at any time, flash over USB again.

> **Confirming an update landed:** compare `elf_sha` (the first 4 bytes of the ELF SHA-256) before and after, not `compiled`. ESP-IDF keeps the old app_desc timestamp on incremental builds, so `compiled` can lie; the hash can't. The same fingerprint is printed in the boot log.

## 8. Troubleshooting

| Problem | Cause and fix |
|---|---|
| `Could not reach .../info` | Board offline (LED not green), or the laptop is on a different network (guest Wi-Fi, VPN, 5 GHz vs 2.4 GHz is fine but network isolation isn't). |
| `.local` name doesn't resolve | Some networks block mDNS. Use the IP address: `-Device 192.168.x.y`. It's in the router's client list and in the board's serial log. |
| `401 Unauthorized` | Token doesn't match `CONFIG_APP_OTA_TOKEN`. Pass `-Token`. |
| `Invalid firmware image` | The file isn't a valid app image. Send `build/ble_wifi_prov.bin`, not the merged or bootloader binary. |
| Board doesn't come back after an update | Give it a minute; then check with `-InfoOnly`. If the new firmware can't reach Wi-Fi, the rollback returns it to the previous version. |
| Update needed but the board was never USB-flashed with v1.1 | Do the one-time USB step in section 2. |
| `Object reference not set to an instance of an object` | Old copy of the script: every HTTP call needs `-UseBasicParsing` on Windows PowerShell 5.1. Note the push itself usually succeeded anyway. |
| Script can't find the device | See section 2b; pass `-Device <ip>` as the reliable fallback. |

## 9. Limits and next steps
- **Same network only.** The laptop pushes directly to the board, so both must be on the same Wi-Fi. Updating a board that is somewhere else needs the device to **pull** from an HTTPS URL instead — that is the top backlog item (DESIGN.md §13) and would sit alongside, not replace, this push path.
- **Plain HTTP with a shared token.** Fine on a trusted home network; anyone sniffing the LAN could see the token and the firmware. HTTPS with a device certificate would be the upgrade.
- **The phone app can't update devices yet.** Pushing is laptop-only for now. An "Update firmware" button in the app is a possible later feature, and it would reuse the same mDNS discovery as the planned "My devices" screen.
- **No update history** on the device beyond the current and previous slots.
