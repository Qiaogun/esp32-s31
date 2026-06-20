param(
    [string]$Port = "",
    [int]$Baud = 115200,
    [int]$TimeoutSec = 12,
    [ValidateSet("generic", "function-coreboard-1", "korvo-1")]
    [string]$Board = "generic",
    [switch]$WifiScan,
    [switch]$FunctionLed,
    [switch]$KorvoLed,
    [int]$KorvoLedGpio = 37,
    [string]$LogPath = ""
)

$ErrorActionPreference = "Stop"

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

function Read-SerialFor {
    param(
        [System.IO.Ports.SerialPort]$Serial,
        [int]$Milliseconds
    )

    $deadline = [DateTime]::UtcNow.AddMilliseconds($Milliseconds)
    $text = ""
    while ([DateTime]::UtcNow -lt $deadline) {
        $chunk = $Serial.ReadExisting()
        if ($chunk.Length -gt 0) {
            $text += $chunk
        }
        Start-Sleep -Milliseconds 100
    }
    return $text
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    $Port = Get-S31Port
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    throw "No serial port found. Flash the ESP32-S31 first, connect its COM port, then rerun this script."
}

if ($Board -eq "function-coreboard-1") {
    $FunctionLed = $true
}
if ($Board -eq "korvo-1") {
    $KorvoLed = $true
}

if ($FunctionLed -and $Board -ne "function-coreboard-1") {
    throw "function_led_test is only enabled for -Board function-coreboard-1. Do not run it on Korvo or an unknown board."
}
if ($KorvoLed -and $Board -ne "korvo-1") {
    throw "korvo_led_test is only enabled for -Board korvo-1."
}

if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $LogPath = Join-Path (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")) "logs\s31-smoke-$stamp.log"
}

$logDir = Split-Path -Parent $LogPath
if (-not (Test-Path -LiteralPath $logDir)) {
    New-Item -ItemType Directory -Path $logDir | Out-Null
}

$commands = @("help", "status", "mac", "partitions", "board_info", "storage_test")
if ($WifiScan) {
    $commands += "wifi_scan"
    if ($TimeoutSec -lt 20) {
        $TimeoutSec = 20
    }
}
if ($FunctionLed) {
    $commands += "function_led_test"
}
if ($KorvoLed) {
    $commands += "lcd_test"
    $commands += "renderer_test"
    $commands += ("korvo_led_test {0}" -f $KorvoLedGpio)
}

$serial = [System.IO.Ports.SerialPort]::new($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.ReadTimeout = 500
$serial.WriteTimeout = 1000
$serial.NewLine = "`n"

try {
    $serial.Open()
    $serial.DiscardInBuffer()
    $serial.DiscardOutBuffer()

    $output = "OuO ESP32-S31 serial smoke`r`nport=$Port baud=$Baud timeout_sec=$TimeoutSec board=$Board wifi_scan=$WifiScan function_led=$FunctionLed korvo_led=$KorvoLed korvo_led_gpio=$KorvoLedGpio`r`n"
    $output += Read-SerialFor -Serial $serial -Milliseconds 1500

    foreach ($command in $commands) {
        $output += "`r`n>>> $command`r`n"
        $serial.Write("$command`n")
        $commandName = ($command -split "\s+", 2)[0]
        $waitMs = switch ($commandName) {
            "wifi_scan" { $TimeoutSec * 1000; break }
            "storage_test" { 15000; break }
            "function_led_test" { 5000; break }
            "lcd_test" { 5000; break }
            "renderer_test" { 5000; break }
            "korvo_led_test" { 5000; break }
            default { 1500; break }
        }
        $output += Read-SerialFor -Serial $serial -Milliseconds $waitMs
    }

    Set-Content -LiteralPath $LogPath -Value $output -Encoding utf8
    Write-Host $output
    Write-Host ("Saved smoke log: {0}" -f $LogPath)

    $required = @("commands:", "board=", "flash=", "mac_wifi_sta=", "running_partition", "partition label=factory", "active_profile=", "storage_test", "match=1")
    foreach ($needle in $required) {
        if ($output -notmatch [regex]::Escape($needle)) {
            throw "Smoke test did not find expected output '$needle'. Check the log: $LogPath"
        }
    }

    if ($WifiScan -and $output -notmatch "wifi_scan (total|err=|init_err=)") {
        throw "Smoke test did not find a wifi_scan result. Check the log: $LogPath"
    }

    if ($FunctionLed -and $output -notmatch "function_led_test ok") {
        throw "Smoke test did not find a successful function_led_test result. Check the log: $LogPath"
    }

    if ($KorvoLed -and $output -notmatch "korvo_led_test ok") {
        throw "Smoke test did not find a successful korvo_led_test result. Check the log: $LogPath"
    }

    if ($Board -eq "korvo-1" -and $output -notmatch "psram=[1-9][0-9]*") {
        throw "Korvo smoke test expected enabled PSRAM, but status did not report a non-zero psram value. Check the log: $LogPath"
    }

    if ($Board -eq "korvo-1" -and $output -notmatch "lcd_test ok") {
        throw "Korvo smoke test did not find a successful lcd_test result. Check the log: $LogPath"
    }

    if ($Board -eq "korvo-1" -and $output -notmatch "renderer_test ok") {
        throw "Korvo smoke test did not find a successful renderer_test result. Check the log: $LogPath"
    }
} finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
}
