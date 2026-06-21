param(
    [string]$Port = "",
    [int]$Baud = 115200,
    [string]$Ssid = "",
    [string]$Password = "",
    [string]$ServerUrl = "",
    [string]$DialogText = "你好",
    [int]$TimeoutSec = 45,
    [switch]$SkipWifiConnect,
    [switch]$SkipOtaCheck,
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

function Get-DefaultServerUrl {
    $configs = Get-NetIPConfiguration -ErrorAction SilentlyContinue | Where-Object {
        $_.IPv4DefaultGateway -and $_.IPv4Address
    }
    foreach ($config in $configs) {
        foreach ($addr in $config.IPv4Address) {
            if ($addr.IPAddress -and $addr.IPAddress -notmatch "^(127\.|169\.254\.)") {
                return "http://$($addr.IPAddress):8787"
            }
        }
    }
    return "http://127.0.0.1:8787"
}

function Join-Url {
    param(
        [string]$Base,
        [string]$Path
    )
    return "$($Base.TrimEnd('/'))/$($Path.TrimStart('/'))"
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

function Assert-Contains {
    param(
        [string]$Text,
        [string]$Needle,
        [string]$Message
    )
    if ($Text -notmatch [regex]::Escape($Needle)) {
        throw "$Message. Missing '$Needle'. Check the log: $LogPath"
    }
}

function Send-DeviceCommand {
    param(
        [System.IO.Ports.SerialPort]$Serial,
        [string]$Command,
        [string]$Label,
        [int]$WaitMs
    )

    $text = "`r`n>>> $Label`r`n"
    $Serial.Write("$Command`n")
    $text += Read-SerialFor -Serial $Serial -Milliseconds $WaitMs
    return $text
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    $Port = Get-S31Port
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    throw "No serial port found. Flash the ESP32-S31 first, connect its COM port, then rerun this script."
}

if (-not $SkipWifiConnect) {
    if ([string]::IsNullOrWhiteSpace($Ssid) -or [string]::IsNullOrWhiteSpace($Password)) {
        throw "Use -Ssid and -Password, or pass -SkipWifiConnect if the device is already connected."
    }
    if ($Ssid -match "\s" -or $Password -match "\s") {
        throw "Current firmware command parsing does not support spaces in SSID or password."
    }
}

if ([string]::IsNullOrWhiteSpace($ServerUrl)) {
    $ServerUrl = Get-DefaultServerUrl
}

if ($ServerUrl -match "://(127\.0\.0\.1|localhost)(:|/|$)" -and -not $SkipWifiConnect) {
    throw "ServerUrl is $ServerUrl, which the ESP cannot reach over Wi-Fi. Pass -ServerUrl http://<pc-ip>:8787."
}

if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $LogPath = Join-Path (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")) "logs\ai-home-device-smoke-$stamp.log"
}

$logDir = Split-Path -Parent $LogPath
if (-not (Test-Path -LiteralPath $logDir)) {
    New-Item -ItemType Directory -Path $logDir | Out-Null
}

$serial = [System.IO.Ports.SerialPort]::new($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.ReadTimeout = 500
$serial.WriteTimeout = 1000
$serial.NewLine = "`n"

try {
    $serial.Open()
    $serial.DiscardInBuffer()
    $serial.DiscardOutBuffer()

    $output = "OuO AI Home device smoke`r`nport=$Port baud=$Baud timeout_sec=$TimeoutSec server_url=$ServerUrl skip_wifi_connect=$SkipWifiConnect skip_ota_check=$SkipOtaCheck`r`n"
    $output += Read-SerialFor -Serial $serial -Milliseconds 1500

    $output += Send-DeviceCommand -Serial $serial -Command "ai_home_autostart off" -Label "ai_home_autostart off" -WaitMs 1000

    if (-not $SkipWifiConnect) {
        $wifiCommand = "wifi_connect $Ssid $Password"
        $output += Send-DeviceCommand -Serial $serial -Command $wifiCommand -Label "wifi_connect $Ssid ********" -WaitMs ($TimeoutSec * 1000)
    }

    $output += Send-DeviceCommand -Serial $serial -Command "wifi_status" -Label "wifi_status" -WaitMs 2000
    $output += Send-DeviceCommand -Serial $serial -Command "ai_home_server $ServerUrl" -Label "ai_home_server $ServerUrl" -WaitMs 1000
    $output += Send-DeviceCommand -Serial $serial -Command ("ota_manifest_url {0}" -f (Join-Url $ServerUrl "/api/v1/ota/manifest")) -Label "ota_manifest_url <server>/api/v1/ota/manifest" -WaitMs 1000
    $output += Send-DeviceCommand -Serial $serial -Command "ai_home_ping" -Label "ai_home_ping" -WaitMs 7000
    $output += Send-DeviceCommand -Serial $serial -Command "wake ouo 0.91" -Label "wake ouo 0.91" -WaitMs 3000
    $output += Send-DeviceCommand -Serial $serial -Command "ai_home_dialog $DialogText" -Label "ai_home_dialog <text>" -WaitMs 15000

    if (-not $SkipOtaCheck) {
        $output += Send-DeviceCommand -Serial $serial -Command "ota_check" -Label "ota_check" -WaitMs 10000
    }

    $output += Send-DeviceCommand -Serial $serial -Command "status" -Label "status" -WaitMs 2000

    Set-Content -LiteralPath $LogPath -Value $output -Encoding utf8
    Write-Host $output
    Write-Host ("Saved AI Home device smoke log: {0}" -f $LogPath)

    Assert-Contains -Text $output -Needle "wifi=connected" -Message "Device did not report Wi-Fi connected"
    Assert-Contains -Text $output -Needle "ok ai_home_server=" -Message "Device did not persist ai_home_server"
    Assert-Contains -Text $output -Needle "ai_home_ping ok http=200" -Message "Device heartbeat did not reach backend"
    Assert-Contains -Text $output -Needle "wake ok phrase=" -Message "Wake diagnostic did not run"
    Assert-Contains -Text $output -Needle "ai_home_dialog ok http=200" -Message "Dialog did not reach backend"
    Assert-Contains -Text $output -Needle "mood=" -Message "Status did not include mood"
    if (-not $SkipOtaCheck) {
        Assert-Contains -Text $output -Needle "ota_check ok http=200" -Message "OTA manifest check did not reach backend"
    }
} finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
}
