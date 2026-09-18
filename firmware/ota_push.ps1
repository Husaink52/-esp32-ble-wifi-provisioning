<#
.SYNOPSIS
    Push a firmware update to an ESP32-C6 over Wi-Fi (no USB cable needed).

.DESCRIPTION
    Sends build\ble_wifi_prov.bin to the device's update server (see
    firmware/main/ota.c). The device writes it to its spare app slot, verifies
    it, and reboots into it. If the new firmware can't reach Wi-Fi, the
    bootloader rolls back to the previous one, so a bad update is recoverable.

    Prerequisites:
      * The device has been provisioned and is on the SAME Wi-Fi network as this PC.
      * It was flashed ONCE over USB with this OTA-capable firmware
        (the two-slot partition layout can only be installed over USB).
      * You built the new firmware first: .\build_and_flash.ps1 -BuildOnly

    Progress is visible on the device: the LED blinks purple while the image
    is being received, then it reboots (yellow → green).

.PARAMETER Device
    Device address: an mDNS name (e.g. prov-e01c64.local, the default form) or
    a plain IP address (e.g. 192.168.31.50). Defaults to the mDNS name derived
    from -Mac if given.

.PARAMETER Mac
    Last 3 bytes of the device's MAC in hex, e.g. e01c64 (the same suffix as its
    BLE name PROV_E01C64). Used to build the mDNS name when -Device is omitted.

.PARAMETER Token
    Update token; must match CONFIG_APP_OTA_TOKEN in the firmware.
    Default: the firmware default "update-me".

.PARAMETER Firmware
    Path to the .bin to send. Defaults to build\ble_wifi_prov.bin.

.PARAMETER Port
    Update server port; must match CONFIG_APP_OTA_PORT (default 80).

.PARAMETER InfoOnly
    Only query GET /info and print what the device is running. No update is sent.
    Use it before and after an update to confirm the change.

.EXAMPLE
    .\ota_push.ps1 -Mac e01c64
    Build first, then push to prov-e01c64.local.

.EXAMPLE
    .\ota_push.ps1 -Device 192.168.31.50 -InfoOnly
    Show what that device is currently running.
#>
[CmdletBinding()]
param(
    [string]$Device,
    [string]$Mac,
    [string]$Token = 'update-me',
    [string]$Firmware,
    [int]$Port = 80,
    [switch]$InfoOnly
)

$ErrorActionPreference = 'Stop'

# Default paths are relative to this script, so it works from any directory.
if (-not $Firmware) {
    $Firmware = Join-Path $PSScriptRoot 'build\ble_wifi_prov.bin'
}

# Work out the address: -Device wins, otherwise build the mDNS name from -Mac.
if (-not $Device) {
    if (-not $Mac) {
        throw "Specify the device: -Device <name-or-ip> or -Mac <last 3 MAC bytes, e.g. e01c64>."
    }
    $Device = "prov-$($Mac.ToLower()).local"
}

<#
.SYNOPSIS
    Resolve a <name>.local address by asking the network directly (mDNS), returning an IP string.

.DESCRIPTION
    Windows has no usable mDNS resolver for scripts: .NET's DNS client and
    ping both fail on ".local" names, even though the device answers mDNS
    correctly (macOS and Linux resolve it out of the box). So we send the
    query ourselves: a standard DNS question for an A record, sent to the mDNS
    multicast group 224.0.0.251:5353, and read the first answer.

    Returns $null if nothing answers in time, so the caller can fall back to
    the name as typed.
#>
function Resolve-MdnsAddress([string]$Name, [int]$TimeoutMs = 2500) {
    $shortName = $Name -replace '\.local$', ''
    $client = $null
    try {
        $client = New-Object System.Net.Sockets.UdpClient
        $client.Client.ReceiveTimeout = $TimeoutMs
        # Let other mDNS users (e.g. a browser) keep port 5353 bound too.
        $client.Client.SetSocketOption('Socket', 'ReuseAddress', $true)
        $client.Client.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)))

        # --- Build the query packet ---
        $bytes = New-Object System.Collections.Generic.List[byte]
        $bytes.AddRange([byte[]](0x00, 0x00))   # transaction id (0 for mDNS)
        $bytes.AddRange([byte[]](0x00, 0x00))   # flags: standard query
        $bytes.AddRange([byte[]](0x00, 0x01))   # 1 question
        $bytes.AddRange([byte[]](0x00, 0x00))   # 0 answers
        $bytes.AddRange([byte[]](0x00, 0x00))   # 0 authority records
        $bytes.AddRange([byte[]](0x00, 0x00))   # 0 additional records
        # QNAME: each label prefixed with its length, terminated by a zero byte.
        foreach ($label in @($shortName, 'local')) {
            $bytes.Add([byte]$label.Length)
            $bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes($label))
        }
        $bytes.Add(0x00)
        $bytes.AddRange([byte[]](0x00, 0x01))   # QTYPE  = A (IPv4 address)
        $bytes.AddRange([byte[]](0x00, 0x01))   # QCLASS = IN

        $query = $bytes.ToArray()
        $mdnsEndpoint = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse('224.0.0.251'), 5353)
        $client.Send($query, $query.Length, $mdnsEndpoint) | Out-Null

        # --- Read answers until one carries an A record or we run out of time ---
        $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
        while ((Get-Date) -lt $deadline) {
            $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
            $response = $client.Receive([ref]$remote)

            # Find an A record: type A (00 01), class IN with or without the
            # cache-flush bit (00 01 / 80 01), a 4-byte TTL, then length 4 and
            # the address itself. Scanning for that pattern avoids having to
            # implement DNS name-compression parsing.
            for ($i = 0; $i -lt $response.Length - 13; $i++) {
                if ($response[$i] -eq 0x00 -and $response[$i + 1] -eq 0x01 -and
                    ($response[$i + 2] -eq 0x00 -or $response[$i + 2] -eq 0x80) -and $response[$i + 3] -eq 0x01 -and
                    $response[$i + 8] -eq 0x00 -and $response[$i + 9] -eq 0x04) {
                    $ip = "{0}.{1}.{2}.{3}" -f $response[$i + 10], $response[$i + 11], $response[$i + 12], $response[$i + 13]
                    if ($ip -ne '0.0.0.0') {
                        return $ip
                    }
                }
            }
        }
    } catch {
        # Timeout or a network error: treated as "not found" by the caller.
    } finally {
        if ($client) { $client.Close() }
    }
    return $null
}

<#
.SYNOPSIS
    Ask one IP address whether it's our update server. Returns the /info object, or $null.
#>
function Test-DeviceAt([string]$Ip, [int]$TimeoutSec = 2) {
    try {
        return Invoke-RestMethod -Uri "http://${Ip}:$Port/info" `
            -Headers @{ 'X-Update-Token' = $Token } -TimeoutSec $TimeoutSec -UseBasicParsing
    } catch {
        return $null
    }
}

<#
.SYNOPSIS
    Find the device by probing hosts the PC has recently talked to (the ARP table).

.DESCRIPTION
    Fallback for when mDNS doesn't work (Windows Firewall or a router that
    blocks multicast). The ARP table lists neighbours on the local subnet, which
    is a much shorter list than scanning all 254 addresses. Each candidate is
    asked for /info; the first one that answers with our project name wins.
    A device that hasn't been contacted recently may be missing from the table,
    in which case pass -Device <ip> explicitly.
#>
function Find-DeviceByProbe {
    $neighbours = @(Get-NetNeighbor -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.State -in 'Reachable', 'Stale', 'Delay', 'Probe' -and $_.IPAddress -notlike '224.*' -and $_.IPAddress -notlike '239.*' -and $_.IPAddress -ne '255.255.255.255' } |
        Select-Object -ExpandProperty IPAddress -Unique)

    if (-not $neighbours) {
        return $null
    }
    Write-Host "Probing $($neighbours.Count) hosts on the local network..." -ForegroundColor DarkGray
    foreach ($ip in $neighbours) {
        $info = Test-DeviceAt -Ip $ip -TimeoutSec 1
        if ($info -and $info.project) {
            Write-Host "Found $($info.project) at $ip" -ForegroundColor DarkGray
            return $ip
        }
    }
    return $null
}

# Remembers the address that worked last time, so repeated pushes are instant.
$cacheFile = Join-Path $PSScriptRoot '.ota_last_ip.txt'

# Windows can't resolve .local names itself (its DNS client ignores mDNS), so
# work the address out here. An IP or plain hostname is passed through untouched.
if ($Device -like '*.local') {
    $target = $null

    Write-Host "Looking up $Device on the network (mDNS)..." -ForegroundColor DarkGray
    $target = Resolve-MdnsAddress -Name $Device

    # mDNS blocked? Try the address that worked last time.
    if (-not $target -and (Test-Path $cacheFile)) {
        $cached = (Get-Content $cacheFile -Raw).Trim()
        if ($cached -and (Test-DeviceAt -Ip $cached)) {
            Write-Host "Using last known address $cached" -ForegroundColor DarkGray
            $target = $cached
        }
    }

    # Still nothing: probe the neighbours on this subnet.
    if (-not $target) {
        $target = Find-DeviceByProbe
    }

    if ($target) {
        $Device = $target
        Set-Content -Path $cacheFile -Value $target -Encoding ascii
    } else {
        Write-Warning "Couldn't find $Device automatically. Trying it as-is; if that fails, pass -Device <ip>."
    }
}

$baseUrl = "http://${Device}:$Port"
$headers = @{ 'X-Update-Token' = $Token }

<#
.SYNOPSIS
    Query GET /info and print what the device is running.
#>
function Show-DeviceInfo {
    try {
        # -UseBasicParsing: Windows PowerShell 5.1 otherwise tries to parse
        # responses with the legacy Internet Explorer engine, which throws
        # "Object reference not set to an instance of an object" when that
        # engine isn't available (hit on 2026-09-18).
        $info = Invoke-RestMethod -Uri "$baseUrl/info" -Headers $headers -TimeoutSec 10 -UseBasicParsing
        Write-Host "Device  : $Device"
        Write-Host "Firmware: $($info.project) $($info.version), compiled $($info.compiled), build $($info.elf_sha)"
        Write-Host "Slot    : $($info.slot)   Uptime: $($info.uptime_s)s   IDF: $($info.idf)"
    } catch {
        throw @"
Could not reach $baseUrl/info : $($_.Exception.Message)
Check that:
  * the device is powered and its LED is solid green (connected to Wi-Fi),
  * this PC is on the same Wi-Fi network (not a guest network),
  * the token matches CONFIG_APP_OTA_TOKEN,
  * if the .local name fails, pass the IP address instead: -Device 192.168.x.y
    (your router's client list shows it, and so does the device's serial log).
"@
    }
}

Write-Host "==> Current device state" -ForegroundColor Cyan
Show-DeviceInfo

if ($InfoOnly) {
    return
}

if (-not (Test-Path $Firmware)) {
    throw "Firmware image not found: $Firmware. Build it first: .\build_and_flash.ps1 -BuildOnly"
}
$sizeKb = [math]::Round((Get-Item $Firmware).Length / 1KB)

Write-Host ""
Write-Host "==> Uploading $Firmware ($sizeKb KB) to $baseUrl/update" -ForegroundColor Cyan
Write-Host "The device LED blinks purple while receiving, then reboots." -ForegroundColor DarkGray

# Send the raw image as the request body.
# -InFile streams the file rather than passing a byte[] through -Body: Windows
# PowerShell 5.1 throws "Object reference not set to an instance of an object"
# on byte[] bodies of this size (hit on 2026-09-18 with the 1.4 MB image), and
# streaming also keeps the whole image out of memory.
$response = Invoke-WebRequest -Uri "$baseUrl/update" `
    -Method Post `
    -Headers $headers `
    -ContentType 'application/octet-stream' `
    -InFile $Firmware `
    -TimeoutSec 180 `
    -UseBasicParsing

Write-Host "Device replied: $($response.Content.Trim())" -ForegroundColor Green

# The device needs to reboot and rejoin Wi-Fi before it answers again.
Write-Host ""
Write-Host "==> Waiting for the device to come back (up to 60 s)" -ForegroundColor Cyan
$deadline = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    try {
        $info = Invoke-RestMethod -Uri "$baseUrl/info" -Headers $headers -TimeoutSec 5 -UseBasicParsing
        Write-Host ""
        Write-Host "Update complete." -ForegroundColor Green
        Write-Host "Now running: $($info.project) $($info.version), build $($info.elf_sha), slot $($info.slot)"
        return
    } catch {
        Write-Host "." -NoNewline
    }
}

Write-Warning @"
The device didn't answer within 60 s.
It may still be reconnecting. Re-run with -InfoOnly to check.
If it never comes back, the new firmware failed to reach Wi-Fi and the
bootloader will have rolled back to the previous version on its next boot.
"@
