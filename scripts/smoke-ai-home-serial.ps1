param(
    [string]$Port = "",
    [int]$Baud = 115200,
    [string]$ServerUrl = "http://127.0.0.1:8787",
    [string]$OtaManifestUrl = "",
    [int]$TimeoutSec = 12,
    [switch]$SkipCameraSnapshot,
    [switch]$NoRestore,
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

function Assert-Matches {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Message
    )
    if ($Text -notmatch $Pattern) {
        throw "$Message. Pattern '$Pattern' did not match. Check the log: $LogPath"
    }
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    $Port = $env:OUO_SERIAL_PORT
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    $Port = Get-S31Port
}

if ([string]::IsNullOrWhiteSpace($Port)) {
    throw "No serial port found. Flash the ESP32-S31 first, connect its COM port, then rerun this script."
}

if ([string]::IsNullOrWhiteSpace($OtaManifestUrl)) {
    $OtaManifestUrl = Join-Url $ServerUrl "/api/v1/ota/manifest"
}

if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $LogPath = Join-Path (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")) "logs\ai-home-serial-smoke-$stamp.log"
}

$logDir = Split-Path -Parent $LogPath
if (-not (Test-Path -LiteralPath $logDir)) {
    New-Item -ItemType Directory -Path $logDir | Out-Null
}

$serial = [System.IO.Ports.SerialPort]::new($Port, $Baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.ReadTimeout = 500
$serial.WriteTimeout = 1000
$serial.NewLine = "`n"
$output = ""
$initialServerUrl = $null
$initialOtaManifestUrl = $null
$initialAutostart = $null

try {
    $serial.Open()
    $serial.DiscardInBuffer()
    $serial.DiscardOutBuffer()

    $output = "OuO AI Home serial smoke`r`nport=$Port baud=$Baud timeout_sec=$TimeoutSec server_url=$ServerUrl ota_manifest_url=$OtaManifestUrl skip_camera_snapshot=$SkipCameraSnapshot restore=$(-not $NoRestore)`r`n"
    $output += Read-SerialFor -Serial $serial -Milliseconds 1500

    $initialStatus = Send-DeviceCommand -Serial $serial -Command "ai_home_status" -Label "ai_home_status initial" -WaitMs 1200
    $output += $initialStatus
    if ($initialStatus -match 'server_url="([^"]+)"') {
        $initialServerUrl = $Matches[1]
    }
    if ($initialStatus -match 'ota_manifest="([^"]+)"') {
        $initialOtaManifestUrl = $Matches[1]
    }
    if ($initialStatus -match 'ai_home autostart=([01])') {
        $initialAutostart = $Matches[1]
    }

    $output += Send-DeviceCommand -Serial $serial -Command "ai_home_autostart off" -Label "ai_home_autostart off" -WaitMs 1000
    $output += Send-DeviceCommand -Serial $serial -Command "ai_home_server $ServerUrl" -Label "ai_home_server $ServerUrl" -WaitMs 1000
    $output += Send-DeviceCommand -Serial $serial -Command "ota_manifest_url $OtaManifestUrl" -Label "ota_manifest_url $OtaManifestUrl" -WaitMs 1000
    $output += Send-DeviceCommand -Serial $serial -Command "ai_home_status" -Label "ai_home_status configured" -WaitMs 1200
    $output += Send-DeviceCommand -Serial $serial -Command "wifi_status" -Label "wifi_status" -WaitMs 2000
    $output += Send-DeviceCommand -Serial $serial -Command "wake ouo 0.91" -Label "wake ouo 0.91" -WaitMs 2000
    $output += Send-DeviceCommand -Serial $serial -Command "emotion_map joke" -Label "emotion_map joke" -WaitMs 1200
    $output += Send-DeviceCommand -Serial $serial -Command "ai_home_ping" -Label "ai_home_ping" -WaitMs ([Math]::Max(7000, $TimeoutSec * 1000))
    $output += Send-DeviceCommand -Serial $serial -Command "ai_home_poll" -Label "ai_home_poll" -WaitMs ([Math]::Max(7000, $TimeoutSec * 1000))
    $output += Send-DeviceCommand -Serial $serial -Command "ai_home_dialog offline test" -Label "ai_home_dialog offline test" -WaitMs ([Math]::Max(7000, $TimeoutSec * 1000))
    $output += Send-DeviceCommand -Serial $serial -Command "ota_status" -Label "ota_status" -WaitMs 1200
    $output += Send-DeviceCommand -Serial $serial -Command "ota_check" -Label "ota_check" -WaitMs ([Math]::Max(7000, $TimeoutSec * 1000))

    if (-not $SkipCameraSnapshot) {
        $output += Send-DeviceCommand -Serial $serial -Command "ai_home_camera_snapshot" -Label "ai_home_camera_snapshot" -WaitMs 30000
    }

    $output += Send-DeviceCommand -Serial $serial -Command "status" -Label "status" -WaitMs 2000

    if (-not $NoRestore) {
        if (-not [string]::IsNullOrWhiteSpace($initialServerUrl)) {
            $output += Send-DeviceCommand -Serial $serial -Command "ai_home_server $initialServerUrl" -Label "restore ai_home_server" -WaitMs 1000
        }
        if (-not [string]::IsNullOrWhiteSpace($initialOtaManifestUrl)) {
            $output += Send-DeviceCommand -Serial $serial -Command "ota_manifest_url $initialOtaManifestUrl" -Label "restore ota_manifest_url" -WaitMs 1000
        }
        if ($null -ne $initialAutostart) {
            $restoreAutostart = if ($initialAutostart -eq "1") { "on" } else { "off" }
            $output += Send-DeviceCommand -Serial $serial -Command "ai_home_autostart $restoreAutostart" -Label "restore ai_home_autostart $restoreAutostart" -WaitMs 1000
        }
    }

    Set-Content -LiteralPath $LogPath -Value $output -Encoding utf8
    Write-Host $output
    Write-Host ("Saved AI Home serial smoke log: {0}" -f $LogPath)

    Assert-Contains -Text $output -Needle "ai_home firmware=" -Message "AI Home status did not run"
    Assert-Contains -Text $output -Needle "ok ai_home_autostart=0" -Message "AI Home autostart did not accept off"
    Assert-Contains -Text $output -Needle "ok ai_home_server=" -Message "AI Home server URL was not persisted"
    Assert-Contains -Text $output -Needle "ok ota_manifest_url=" -Message "OTA manifest URL was not persisted"
    Assert-Contains -Text $output -Needle 'wake ok phrase="ouo" confidence=0.91 mood=blink' -Message "Wake mapping did not apply blink mood"
    Assert-Contains -Text $output -Needle "emotion_map mood=cheeky" -Message "Emotion mapping did not apply cheeky mood"
    Assert-Matches -Text $output -Pattern "ai_home_ping (ok http=2[0-9][0-9]|err=)" -Message "Heartbeat command did not return a controlled result"
    Assert-Matches -Text $output -Pattern "ai_home_poll (ok http=2[0-9][0-9]|err=)" -Message "Command poll did not return a controlled result"
    Assert-Matches -Text $output -Pattern "ai_home_dialog (ok http=2[0-9][0-9]|err=)" -Message "Dialog command did not return a controlled result"
    Assert-Contains -Text $output -Needle "ota ready=1" -Message "OTA partition readiness was not reported"
    Assert-Matches -Text $output -Pattern "ota_check (ok http=2[0-9][0-9]|err=)" -Message "OTA check did not return a controlled result"
    if (-not $SkipCameraSnapshot) {
        Assert-Contains -Text $output -Needle "ai_home_camera_snapshot" -Message "Camera snapshot command did not run"
        Assert-Matches -Text $output -Pattern "ai_home_camera_snapshot (ok http=2[0-9][0-9]|err=)" -Message "Camera snapshot did not return a controlled result"
    }
    Assert-Contains -Text $output -Needle "mood=cheeky" -Message "Final status did not retain mapped mood"
} finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
}
