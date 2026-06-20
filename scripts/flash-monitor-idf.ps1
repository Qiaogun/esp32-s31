param(
    [string]$Port = "",
    [string]$IdfVersion = "master-s31",
    [int]$Baud = 460800
)

$ErrorActionPreference = "Stop"
$env:PYTHONUTF8 = "1"

function Get-S31Port {
    $ports = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue
    if (-not $ports) {
        return $null
    }

    $preferred = $ports | Where-Object {
        $_.PNPDeviceID -match "VID_303A|VID_10C4|VID_1A86|VID_0403" -or
        $_.Name -match "Espressif|USB|UART|CP210|CH34|FTDI"
    } | Select-Object -First 1

    if ($preferred) {
        return $preferred.DeviceID
    }

    return ($ports | Select-Object -First 1).DeviceID
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    $Port = Get-S31Port
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    throw "No serial port found. Connect the ESP32-S31 board, install USB serial/JTAG drivers if needed, then rerun this script."
}

$ProjectDir = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\idf")
Push-Location $ProjectDir
try {
    eim run "idf.py --preview -p $Port -b $Baud flash monitor" $IdfVersion
} finally {
    Pop-Location
}
