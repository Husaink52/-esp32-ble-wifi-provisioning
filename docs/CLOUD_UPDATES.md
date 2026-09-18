# Internet firmware updates (v1.2)

> **Built 2026-09-18** after the question: *"how can I push updates if the chip and the laptop are on different Wi-Fi networks?"*
> Design decision and rejected alternatives: [DESIGN.md §13](DESIGN.md#13-backlog-post-v1).
>
> **In one line:** publish a firmware version once, and every board in the world downloads it on its own within the hour.

---

## 1. Why the direction is reversed
The v1.1 push (`ota_push.ps1`, [OTA_UPDATES.md](OTA_UPDATES.md)) sends firmware **from** the laptop **to** the board. That only works on one network: home routers block connections coming from the internet, so a laptop elsewhere simply cannot reach the board.

So the board **pulls** instead:

```
You:    build → publish to GitHub Releases → update the manifest
Board:  every hour → read manifest → different version? → download → install → reboot → report
```

Nothing connects *into* the board, so it works behind any router, anywhere, for any number of boards.

## 2. The pieces

| Piece | Where | Job |
|---|---|---|
| `firmware/main/cloud_update.c` | on the board | hourly check, download, install, report |
| `firmware/version.txt` | repo | the version baked into each build |
| `cloud/manifest.json` | repo (served raw over HTTPS) | says which version is current and where to get it |
| GitHub Releases | github.com | hosts the firmware binary |
| `firmware/publish_release.ps1` | laptop | builds, creates the release, updates the manifest |
| `cloud/worker.js` | Cloudflare (free) | collects "I'm running version X" reports |

## 3. Publishing an update
```powershell
$env:GITHUB_TOKEN = "ghp_..."        # once per terminal; scope: repo
cd "C:\Users\husai\Desktop\ble app\firmware"
.\publish_release.ps1 -Version 1.2.1 -Notes "Fix LED timing"
```
That single command stamps the version, builds, creates release `v1.2.1`, uploads `ble_wifi_prov.bin`, updates `cloud/manifest.json` and pushes it.

Boards pick it up at their next check. To make one check immediately (only possible on a board you can reach):
```powershell
curl.exe -H "X-Update-Token: update-me" http://192.168.1.13/check-update
```

## 4. What the board does, step by step
1. **Waits for Wi-Fi**, then waits 15 s so the network settles.
2. **Sets its clock** from NTP. Without this, HTTPS fails: certificate checks need a real date and the chip boots believing it is 1970.
3. **Judges the previous attempt** (§6).
4. **Reports its version** to the cloud endpoint.
5. **Checks the manifest** (~200 bytes), then every `APP_CLOUD_POLL_MINUTES` (default 60) plus a random offset of up to 10%, so a fleet doesn't hit the server in lockstep.
6. **Compares versions.** Same → done. Different → download.
7. **Downloads over HTTPS** into the spare slot, verifies the image, switches the boot slot, reboots. The LED blinks purple during the download.
8. **Confirms itself** once it reaches Wi-Fi again, which cancels the automatic rollback.

## 5. Settings (`idf.py menuconfig` → *BLE Provisioning App Configuration*)

| Option | Default | Meaning |
|---|---|---|
| `APP_CLOUD_MANIFEST_URL` | *(empty)* | Manifest address. **Empty = internet updates off.** |
| `APP_CLOUD_REPORT_URL` | *(empty)* | Where to POST version reports. Empty = no reporting; updates still work. |
| `APP_CLOUD_POLL_MINUTES` | 60 | Minutes between checks. Use 2–5 while developing. |
| `APP_CLOUD_NTP_SERVER` | `pool.ntp.org` | Time source for certificate validation. |

With GitHub, the manifest address looks like:
```
https://raw.githubusercontent.com/<owner>/<repo>/main/cloud/manifest.json
```

## 6. What stops a bad release bricking everything
Three layers, and they matter most precisely when you can't reach the device:

1. **Image verification.** A truncated or corrupted download is rejected before the boot slot changes, so the running firmware survives.
2. **Automatic rollback (from v1.1).** New firmware is provisional until it reaches Wi-Fi. If it crashes or can't connect, the bootloader returns to the previous slot.
3. **Bad-version memory (new).** Rollback alone isn't enough: the old firmware would download the same broken version again, forever. So before installing, the board records the version it's attempting. On the next boot, if the running version isn't the attempted one, it concludes that version failed, **blacklists** it, and reports `rolled_back`. Publishing any other version clears the block.

Together: a broken release costs one download and one reboot per device, then everything settles back on the last good firmware.

## 7. Verification: which board is running what
Boards can't be reached from outside, so they report **outward** after every boot and check:
```json
{"device":"e01c64","version":"1.2.0","slot":"ota_1","status":"running","uptime_s":42}
```

The service in `cloud/` stores the last report per device and shows them.

**Deploying it (once, free, no card):**
```powershell
cd "C:\Users\husai\Desktop\ble app\cloud"
npx wrangler kv namespace create DEVICES     # prints an id
# paste that id into wrangler.toml, then:
npx wrangler deploy
```
The first command opens a browser to log in to Cloudflare. Deployment prints an address like `https://esp32-device-status.<you>.workers.dev`. Open it for a table of devices, versions and how recently each was heard from; `/devices` returns the same as JSON for scripts.

Then set `APP_CLOUD_REPORT_URL` to `https://.../report` in `menuconfig`, rebuild and publish.

## 8. Costs and limits
- **GitHub Releases:** free, no meaningful limits at this scale.
- **Cloudflare Workers free tier:** 100,000 requests/day. Two boards reporting hourly use about 50/day.
- **Device traffic:** a check is ~200 bytes plus the encrypted handshake; a full update is ~1.4 MB. Hourly checks are roughly 1 MB/month.
- **Repo must be public**, because the board downloads without credentials. Keep secrets out of it: the Wi-Fi password never leaves the device, but don't commit tokens.
- **Same firmware for every board.** Staged rollouts (test one board first) would need a group field in the manifest — a small addition, not a redesign.
- **Update latency** is up to one poll interval. Instant delivery would need MQTT ([DESIGN.md §13](DESIGN.md#13-backlog-post-v1)).

## 9. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Log: `No manifest URL configured` | `APP_CLOUD_MANIFEST_URL` is empty. Set it in `menuconfig`, rebuild, flash once. |
| Log: `Manifest returned HTTP 404` | Wrong URL, or the repo is private. It must be publicly readable. |
| TLS or certificate errors | Clock not set. Check the `Clock set:` line; a network that blocks NTP (port 123) breaks HTTPS. |
| Log: `Skipping version X: it failed before` | That version was blacklisted after a rollback. Publish a different version number. |
| Update never happens | Version in the manifest equals the device's own. Check `GET /info` and bump `version.txt`. |
| Device isn't in the status table | `APP_CLOUD_REPORT_URL` unset, the Worker isn't deployed, or the device is offline. |
