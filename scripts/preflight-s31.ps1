param(
    [string]$IdfVersion = "master-s31",
    [string]$Port = "",
    [int]$Baud = 460800
)

$ErrorActionPreference = "Stop"
$env:PYTHONUTF8 = "1"

function Write-Section {
    param([string]$Title)
    Write-Host ""
    Write-Host "== $Title =="
}

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-S31PortCandidates {
    $ports = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue
    if (-not $ports) {
        return @()
    }

    $ports | Sort-Object @{
        Expression = {
            if ($_.PNPDeviceID -match "VID_303A|VID_10C4|VID_1A86|VID_0403" -or $_.Name -match "Espressif|USB|UART|CP210|CH34|FTDI") {
                0
            } else {
                1
            }
        }
    }, DeviceID
}

function Get-PresentUsbHints {
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Class -eq "Ports" -or
            $_.FriendlyName -match "Espressif|JTAG|USB Serial|UART|CP210|CH34|FTDI|\(COM\d+\)" -or
            $_.InstanceId -match "VID_303A|VID_10C4|VID_1A86|VID_0403"
        } |
        Sort-Object Class, FriendlyName |
        Select-Object Class, FriendlyName, Status, InstanceId
}

$ScriptRootPath = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectDir = Resolve-Path -LiteralPath (Join-Path $ScriptRootPath "..\idf")
$BuildDir = Join-Path $ProjectDir "build"

Write-Section "Host"
Write-Host ("PowerShell: {0}" -f $PSVersionTable.PSVersion)
Write-Host ("Admin: {0}" -f (Test-Admin))
Write-Host ("Project: {0}" -f $ProjectDir)

Write-Section "EIM / IDF"
try {
    eim list
} catch {
    Write-Warning "eim list failed: $($_.Exception.Message)"
}

try {
    Push-Location $ProjectDir
    eim run "idf.py --version" $IdfVersion
} catch {
    Write-Warning "idf.py --version failed for '$IdfVersion': $($_.Exception.Message)"
} finally {
    Pop-Location
}

Write-Section "Build Artifacts"
$artifacts = @(
    "build\bootloader\bootloader.bin",
    "build\partition_table\partition-table.bin",
    "build\ouo_s31_bringup.bin",
    "build\flasher_args.json",
    "build\flash_args"
)

foreach ($relative in $artifacts) {
    $path = Join-Path $ProjectDir $relative
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path
        Write-Host ("OK      {0} ({1} bytes)" -f $relative, $item.Length)
    } else {
        Write-Host ("MISSING {0}" -f $relative)
    }
}

Write-Section "Partition CSV"
$partitionCsv = Join-Path $ProjectDir "partitions.csv"
if (Test-Path -LiteralPath $partitionCsv) {
    Get-Content -LiteralPath $partitionCsv | Where-Object {
        $_.Trim() -ne "" -and -not $_.TrimStart().StartsWith("#")
    } | ForEach-Object {
        Write-Host $_
    }
} else {
    Write-Warning "No custom partitions.csv found."
}

Write-Section "Serial Ports"
$candidates = @(Get-S31PortCandidates)
if ($candidates.Count -eq 0) {
    Write-Warning "No COM serial ports found. Connect the ESP32-S31 board or install USB serial/JTAG drivers."
} else {
    $candidates | Format-Table DeviceID, Name, Description, PNPDeviceID -AutoSize
    if ([string]::IsNullOrWhiteSpace($Port)) {
        $Port = $candidates[0].DeviceID
    }
}

Write-Section "USB Hints"
$usbHints = @(Get-PresentUsbHints)
if ($usbHints.Count -eq 0) {
    Write-Host "No Espressif/USB-serial-looking present devices found."
} else {
    $usbHints | Format-Table Class, FriendlyName, Status, InstanceId -AutoSize
    $driverProblems = $usbHints | Where-Object {
        $_.InstanceId -match "VID_10C4|VID_303A|VID_1A86|VID_0403" -and
        $_.Status -ne "OK"
    }
    if ($driverProblems) {
        Write-Warning "USB serial device is present but not usable as a COM port. Run an Administrator PowerShell and execute: .\scripts\install-drivers-admin.ps1 -Rescan"
    }
}

Write-Section "Flash Command"
if ([string]::IsNullOrWhiteSpace($Port)) {
    Write-Host ".\scripts\flash-monitor-idf.ps1 -Port COMx"
} else {
    Write-Host (".\scripts\flash-monitor-idf.ps1 -Port {0} -Baud {1}" -f $Port, $Baud)
}

if (-not (Test-Path -LiteralPath (Join-Path $BuildDir "ouo_s31_bringup.bin"))) {
    Write-Host "Build first with: .\scripts\build-idf.ps1"
}
