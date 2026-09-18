<#
.SYNOPSIS
    Build and flash the ESP32-C6 BLE provisioning firmware, then open the serial monitor.

.DESCRIPTION
    Automates the steps in docs/FIRMWARE_SETUP.md. Run it after ESP-IDF is installed.

    What it does, in order:
      1. Makes sure ESP-IDF's `idf.py` is available. If it isn't, it activates
         ESP-IDF itself, using the Windows installer's config (C:\Espressif\esp_idf.json)
         to pick the right Python, then <esp-idf>\export.ps1.
      2. Sets the build target to esp32c6 if it isn't set yet (first run only).
      3. Builds the firmware. The first build also downloads the managed
         components network_provisioning and led_strip.
      4. Finds the board's COM port (or uses -Port).
      5. Flashes the board and opens the serial monitor. Press Ctrl+] to exit.

    Where this fits: docs/DESIGN.md §8 (Firmware) and §12 (Verification).

.PARAMETER Port
    Serial port of the board, e.g. COM5. If omitted and exactly one COM port
    exists, that port is used. If several exist, the script lists them and stops.

.PARAMETER IdfPath
    Folder of the ESP-IDF framework (the one that contains export.ps1), e.g.
    C:\esp\v5.5.5\esp-idf. Only needed if the script can't find ESP-IDF itself.

.PARAMETER BuildOnly
    Build only; don't flash or open the monitor. Useful before the board is plugged in.

.PARAMETER Clean
    Run `idf.py fullclean` before building. Use it after changing sdkconfig.defaults
    (and delete firmware/sdkconfig too) or if the build gets into a strange state.

.PARAMETER NoMonitor
    Flash, but don't open the serial monitor afterwards.

.EXAMPLE
    .\build_and_flash.ps1
    Build, auto-detect the port, flash and monitor.

.EXAMPLE
    .\build_and_flash.ps1 -Port COM7

.EXAMPLE
    .\build_and_flash.ps1 -BuildOnly

.EXAMPLE
    .\build_and_flash.ps1 -IdfPath C:\esp\v5.5.5\esp-idf -Port COM7
#>
[CmdletBinding()]
param(
    [string]$Port,
    [string]$IdfPath,
    [switch]$BuildOnly,
    [switch]$Clean,
    [switch]$NoMonitor
)

# Stop at the first failing PowerShell cmdlet. Native tools like idf.py
# report failure through $LASTEXITCODE, which Invoke-Idf checks.
$ErrorActionPreference = 'Stop'

# The firmware project folder is the folder this script lives in, so the
# script works no matter which directory it's started from.
$FirmwareDir = $PSScriptRoot

# ESP-IDF chip target for the ESP32-C6-DevKitC-1 (docs/DESIGN.md §1).
$Target = 'esp32c6'

# ---------------------------------------------------------------------------
# Helper functions
# ---------------------------------------------------------------------------

<#
.SYNOPSIS
    Print a highlighted step header so the output is easy to follow.
#>
function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

<#
.SYNOPSIS
    Run idf.py with the given arguments and stop the script if it fails.
#>
function Invoke-Idf([string[]]$IdfArgs) {
    Write-Host "idf.py $($IdfArgs -join ' ')" -ForegroundColor DarkGray
    & idf.py @IdfArgs
    if ($LASTEXITCODE -ne 0) {
        throw "idf.py $($IdfArgs -join ' ') failed with exit code $LASTEXITCODE."
    }
}

<#
.SYNOPSIS
    Work out how to activate ESP-IDF in this terminal. Returns @{ Script = <path to dot-source> }.

.DESCRIPTION
    Only called when idf.py isn't available yet. The "ESP-IDF 5.5 PowerShell"
    Start-menu shortcut has already activated it, so there nothing happens.

    Two install layouts are supported:

    1. ESP-IDF Windows installer (default C:\Espressif). Tools and the Python
       virtual env live under C:\Espressif, NOT the default ~\.espressif, and the
       env uses the installer's bundled Python (e.g. 3.11). Running export.ps1
       as-is would use the system Python (e.g. 3.13) and look in ~\.espressif,
       and fails with "Python virtual environment ... not found".
       The installer's own launcher (Initialize-Idf.ps1) isn't reliable from a
       script either: it looks up the Python path by matching IDF_PATH text
       against its config, and a slash or trailing-separator difference makes
       the lookup return nothing.
       So we read the installer's config file (C:\Espressif\esp_idf.json)
       ourselves and prepare the environment the same way the launcher does:
       set IDF_TOOLS_PATH, put the configured Python and Git first on PATH, and
       clear PYTHONPATH/PYTHONHOME. After that, export.ps1 works.

    2. Manual git clone / EIM install: dot-source <esp-idf>\export.ps1 directly.

    Environment variables set here persist after the function returns, but the
    export script must be dot-sourced by the caller at script scope, because
    it defines `idf.py` as a PowerShell function, which would be lost when this
    function returns (see the Main section).
#>
function Resolve-IdfActivation {
    # Framework folders (contain tools\idf.py), most specific first.
    $frameworks = @(
        $IdfPath,
        $env:IDF_PATH,
        'C:\Espressif\frameworks\esp-idf-v5.5.5',
        'C:\esp\v5.5.5\esp-idf',
        "$env:USERPROFILE\esp\v5.5.5\esp-idf",
        "$env:USERPROFILE\esp\esp-idf"
    ) | Where-Object { $_ -and (Test-Path (Join-Path $_ 'tools\idf.py')) } | Select-Object -First 1

    if (-not $frameworks) {
        throw @"
ESP-IDF isn't active in this terminal and no ESP-IDF installation was found.
Fix it one of these ways:
  * Open the "ESP-IDF 5.5 PowerShell" shortcut from the Start menu, then run this script again, or
  * Pass the framework folder:  .\build_and_flash.ps1 -IdfPath C:\path\to\esp-idf
"@
    }
    $env:IDF_PATH = (Resolve-Path $frameworks).Path

    # Layout 1: Windows installer. Its tools folder holds esp_idf.json.
    $toolsDir = @($env:IDF_TOOLS_PATH, 'C:\Espressif') |
        Where-Object { $_ -and (Test-Path (Join-Path $_ 'esp_idf.json')) } |
        Select-Object -First 1
    if ($toolsDir) {
        $config = Get-Content (Join-Path $toolsDir 'esp_idf.json') -Raw | ConvertFrom-Json

        # Normalise both sides (backslashes, no trailing separator) before
        # comparing, which is exactly the mismatch that breaks Initialize-Idf.ps1.
        $normalise = { param($p) ($p -replace '/', '\').TrimEnd('\').ToLowerInvariant() }
        $wanted = & $normalise $env:IDF_PATH

        # idfInstalled is an object keyed by install id, so iterate its properties.
        $install = $config.idfInstalled.PSObject.Properties.Value |
            Where-Object { (& $normalise $_.path) -eq $wanted } |
            Select-Object -First 1

        if ($install) {
            $env:IDF_TOOLS_PATH = $toolsDir
            $pythonDir = Split-Path ($install.python -replace '/', '\')
            $gitDir = Split-Path ($config.gitPath -replace '/', '\')
            # Put the installer's Python (inside its venv) and Git first, so
            # export.ps1's `python` call runs the right interpreter.
            $env:PATH = "$pythonDir;$gitDir;$toolsDir;$env:PATH"
            # Settings from another Python install can break the venv, so clear them.
            $env:PYTHONPATH = $null
            $env:PYTHONHOME = $null
            $env:PYTHONNOUSERSITE = 'True'
        } else {
            Write-Warning "ESP-IDF at $env:IDF_PATH isn't listed in $toolsDir\esp_idf.json; trying export.ps1 with default settings."
        }
    }

    # Both layouts end by running the framework's export.ps1.
    return @{ Script = (Join-Path $env:IDF_PATH 'export.ps1') }
}

<#
.SYNOPSIS
    Return $true if firmware/sdkconfig already targets esp32c6.

.DESCRIPTION
    `idf.py set-target` wipes the build folder and regenerates sdkconfig, so
    it should only run once, not on every build.
#>
function Test-TargetSet {
    $sdkconfig = Join-Path $FirmwareDir 'sdkconfig'
    if (-not (Test-Path $sdkconfig)) {
        return $false
    }
    return [bool](Select-String -Path $sdkconfig -Pattern "^CONFIG_IDF_TARGET=`"$Target`"" -Quiet)
}

<#
.SYNOPSIS
    Pick the serial port to flash: -Port if given, otherwise the only COM port present.
#>
function Resolve-SerialPort {
    if ($Port) {
        return $Port
    }

    $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object -Unique
    switch ($ports.Count) {
        0 {
            throw @"
No COM port found. Check that:
  * the board is plugged in using the USB port labelled "UART",
  * the USB cable carries data (some cables are charge-only),
  * the USB-to-UART driver is installed (Device Manager > Ports (COM & LPT)).
"@
        }
        1 {
            Write-Host "Using the only serial port found: $ports"
            return $ports
        }
        default {
            throw "Several COM ports found: $($ports -join ', '). Re-run with -Port <COMx>. Device Manager > Ports shows which one is the board."
        }
    }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

Push-Location $FirmwareDir
try {
    Write-Step "Checking ESP-IDF"
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
        $activation = Resolve-IdfActivation
        Write-Host "Activating ESP-IDF via $($activation.Script)"
        # Dot-source HERE, at script scope, not inside a function. The activation
        # scripts define `idf.py` as a PowerShell *function*, and a function
        # defined inside another function disappears when that function returns.
        . $activation.Script
        if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
            throw "ESP-IDF activation ran, but idf.py is still not available. Re-run the ESP-IDF installer to repair the installation."
        }
    }
    Invoke-Idf @('--version')

    if ($Clean) {
        Write-Step "Cleaning previous build"
        Invoke-Idf @('fullclean')
    }

    if (-not (Test-TargetSet)) {
        Write-Step "Setting target to $Target (first run only)"
        Invoke-Idf @('set-target', $Target)
    }

    Write-Step "Building firmware (the first build downloads components and takes a few minutes)"
    Invoke-Idf @('build')

    if ($BuildOnly) {
        Write-Step "Build finished. Skipping flash (-BuildOnly)."
        return
    }

    $serialPort = Resolve-SerialPort

    if ($NoMonitor) {
        Write-Step "Flashing via $serialPort"
        Invoke-Idf @('-p', $serialPort, 'flash')
    } else {
        Write-Step "Flashing via $serialPort and opening the monitor (press Ctrl+] to exit)"
        Write-Host 'Expected on a fresh board: "Starting BLE provisioning as \"PROV_XXXXXX\"" and a blue blinking LED.'
        Invoke-Idf @('-p', $serialPort, 'flash', 'monitor')
    }
}
finally {
    # Always return to the directory the user started in, even after an error.
    Pop-Location
}
