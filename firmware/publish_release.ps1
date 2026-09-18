<#
.SYNOPSIS
    Publish a firmware version to GitHub Releases so devices update themselves over the internet.

.DESCRIPTION
    Added 2026-09-18 for v1.2 (docs/CLOUD_UPDATES.md). One command takes a
    version number and does everything:

      1. Writes firmware/version.txt (the version embedded in the image).
      2. Builds the firmware.
      3. Creates a GitHub release tagged v<version> and uploads ble_wifi_prov.bin.
      4. Updates cloud/manifest.json (version + download URL) and pushes it.

    Devices fetch that manifest on their own schedule (hourly by default),
    compare the version with their own, and install when it differs. Nothing is
    pushed to devices from here — see docs/CLOUD_UPDATES.md for why pulling is
    the only thing that works through home routers.

    Requirements:
      * A PUBLIC GitHub repo set as the "origin" remote (devices download
        without credentials, so the files must be publicly readable).
      * A GitHub token with 'repo' scope, either in $env:GITHUB_TOKEN or passed
        via -Token. Create one at https://github.com/settings/tokens
      * ESP-IDF available (the script calls build_and_flash.ps1 -BuildOnly).

.PARAMETER Version
    Version to publish, e.g. 1.2.1. Must differ from the version currently in
    the manifest, or devices won't see a change. Use semantic-ish numbering:
    bump the last part for small fixes.

.PARAMETER Token
    GitHub personal access token. Defaults to $env:GITHUB_TOKEN.

.PARAMETER Notes
    Optional release notes, shown on the GitHub release page.

.PARAMETER SkipBuild
    Publish the existing build/ble_wifi_prov.bin without rebuilding. Only safe
    when it was built from the current sources with the same version number.

.EXAMPLE
    $env:GITHUB_TOKEN = "ghp_..."
    .\publish_release.ps1 -Version 1.2.1 -Notes "Fix LED timing"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version,

    [string]$Token = $env:GITHUB_TOKEN,
    [string]$Notes = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$FirmwareDir = $PSScriptRoot
$RepoRoot = Split-Path $FirmwareDir -Parent
$ManifestPath = Join-Path $RepoRoot 'cloud\manifest.json'
$BinPath = Join-Path $FirmwareDir 'build\ble_wifi_prov.bin'
$VersionFile = Join-Path $FirmwareDir 'version.txt'

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

if (-not $Token) {
    throw "No GitHub token. Set `$env:GITHUB_TOKEN or pass -Token. Create one at https://github.com/settings/tokens (scope: repo)."
}

# --- Work out owner/repo from the git remote ---------------------------------
# Supports both HTTPS (https://github.com/owner/repo.git) and SSH
# (git@github.com:owner/repo.git) remote formats.
Push-Location $RepoRoot
try {
    $remote = (git remote get-url origin 2>$null)
    if (-not $remote) {
        throw "No 'origin' remote. Create a public GitHub repo and run: git remote add origin <url>"
    }
    if ($remote -notmatch 'github\.com[:/](?<owner>[^/]+)/(?<repo>[^/.]+)') {
        throw "Could not read owner/repo from the origin remote: $remote"
    }
    $owner = $Matches['owner']
    $repo = $Matches['repo']
    Write-Host "Repository: $owner/$repo"
}
finally {
    Pop-Location
}

# --- 1. Stamp the version ------------------------------------------------------
Write-Step "Setting firmware version to $Version"
# No trailing newline issues: ESP-IDF trims whitespace when it reads this file.
Set-Content -Path $VersionFile -Value $Version -Encoding ascii -NoNewline

# --- 2. Build --------------------------------------------------------------------
if ($SkipBuild) {
    Write-Step "Skipping build (-SkipBuild)"
} else {
    Write-Step "Building firmware"
    & (Join-Path $FirmwareDir 'build_and_flash.ps1') -BuildOnly
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed; nothing was published."
    }
}
if (-not (Test-Path $BinPath)) {
    throw "Firmware image not found: $BinPath"
}

# --- 3. Create the GitHub release and upload the image ---------------------------
$headers = @{
    Authorization          = "Bearer $Token"
    Accept                 = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
}
$tag = "v$Version"

Write-Step "Creating GitHub release $tag"
$releaseBody = @{
    tag_name = $tag
    name     = $tag
    body     = if ($Notes) { $Notes } else { "Firmware $Version" }
} | ConvertTo-Json

try {
    $release = Invoke-RestMethod -Method Post -UseBasicParsing `
        -Uri "https://api.github.com/repos/$owner/$repo/releases" `
        -Headers $headers -ContentType 'application/json' -Body $releaseBody
} catch {
    # 422 means the tag already exists: reuse that release so re-publishing the
    # same version replaces the asset instead of failing outright.
    if ($_.Exception.Response.StatusCode.value__ -eq 422) {
        Write-Host "Release $tag already exists; reusing it." -ForegroundColor Yellow
        $release = Invoke-RestMethod -UseBasicParsing `
            -Uri "https://api.github.com/repos/$owner/$repo/releases/tags/$tag" -Headers $headers
        # Remove an old asset with the same name, or the upload is rejected.
        $existing = $release.assets | Where-Object { $_.name -eq 'ble_wifi_prov.bin' }
        if ($existing) {
            Invoke-RestMethod -Method Delete -UseBasicParsing `
                -Uri "https://api.github.com/repos/$owner/$repo/releases/assets/$($existing.id)" -Headers $headers | Out-Null
        }
    } else {
        throw
    }
}

Write-Step "Uploading ble_wifi_prov.bin"
$uploadUrl = ($release.upload_url -replace '\{\?name,label\}', '') + '?name=ble_wifi_prov.bin'
$asset = Invoke-RestMethod -Method Post -UseBasicParsing -Uri $uploadUrl `
    -Headers $headers -ContentType 'application/octet-stream' -InFile $BinPath
Write-Host "Uploaded: $($asset.browser_download_url)"

# --- 4. Update the manifest devices poll ------------------------------------------
Write-Step "Updating cloud/manifest.json"
$manifest = [ordered]@{
    version   = $Version
    # Stable "latest release" URL: GitHub redirects it to the newest asset, and
    # the device follows redirects, so older devices still resolve it correctly.
    url       = "https://github.com/$owner/$repo/releases/download/$tag/ble_wifi_prov.bin"
    notes     = $Notes
    published = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
}
New-Item -ItemType Directory -Force -Path (Split-Path $ManifestPath) | Out-Null
$manifest | ConvertTo-Json | Set-Content -Path $ManifestPath -Encoding ascii

Push-Location $RepoRoot
try {
    git add cloud/manifest.json firmware/version.txt
    git commit -q -m "Release firmware $Version"
    git push -q origin HEAD
    Write-Host "Manifest pushed."
}
finally {
    Pop-Location
}

Write-Step "Published $Version"
Write-Host "Devices will pick it up at their next check (hourly by default)."
Write-Host "To trigger one immediately on a device you can reach:"
Write-Host "  curl.exe -H `"X-Update-Token: update-me`" http://<device-ip>/check-update" -ForegroundColor DarkGray
Write-Host "Manifest URL to configure in the firmware:" -ForegroundColor DarkGray
Write-Host "  https://raw.githubusercontent.com/$owner/$repo/main/cloud/manifest.json" -ForegroundColor DarkGray
