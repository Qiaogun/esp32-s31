param(
    [string]$BaseUrl = "http://127.0.0.1:8787",
    [string]$DeviceId = "ouo-s31-smoke",
    [string]$FirmwareVersion = "0.2.0-ai-home-link",
    [int]$TimeoutSec = 10
)

$ErrorActionPreference = "Stop"

function Join-Url {
    param(
        [string]$Base,
        [string]$Path
    )
    return "$($Base.TrimEnd('/'))/$($Path.TrimStart('/'))"
}

function Invoke-JsonPost {
    param(
        [string]$Path,
        [object]$Body
    )
    $json = $Body | ConvertTo-Json -Depth 8 -Compress
    Invoke-RestMethod `
        -Method Post `
        -Uri (Join-Url $BaseUrl $Path) `
        -Body $json `
        -ContentType "application/json" `
        -TimeoutSec $TimeoutSec
}

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

function Assert-NotBlank {
    param(
        [string]$Value,
        [string]$Message
    )
    Assert-True -Condition (-not [string]::IsNullOrWhiteSpace($Value)) -Message $Message
}

$validMoods = @("smile", "grump", "surprise", "squint", "sad", "blank", "upset", "blink", "cheeky", "frown")

Write-Host "AI Home backend smoke"
Write-Host "base_url=$BaseUrl device_id=$DeviceId firmware=$FirmwareVersion"

$health = Invoke-RestMethod -Method Get -Uri (Join-Url $BaseUrl "/health") -TimeoutSec $TimeoutSec
Assert-True -Condition ($health.ok -eq $true) -Message "health.ok was not true"
Assert-True -Condition ($health.service -eq "ouo-ai-home-server") -Message "unexpected health.service: $($health.service)"
Write-Host "ok health service=$($health.service) version=$($health.version)"

$heartbeat = Invoke-JsonPost "/api/v1/device/heartbeat" @{
    device_id = $DeviceId
    firmware_version = $FirmwareVersion
    ip = "127.0.0.1"
    battery_percent = $null
    current_mood = "smile"
}
Assert-True -Condition ($heartbeat.device_id -eq $DeviceId) -Message "heartbeat did not preserve device_id"
Assert-True -Condition ($heartbeat.online -eq $true) -Message "heartbeat did not mark device online"
Write-Host "ok heartbeat online=$($heartbeat.online) mood=$($heartbeat.assistant.last_emotion)"

$queuedCommand = Invoke-JsonPost "/api/v1/device/command" @{
    device_id = $DeviceId
    kind = "set_mood"
    value = "sad"
}
Assert-True -Condition ($queuedCommand.queued -ge 1) -Message "device command did not queue"
$polledCommand = Invoke-JsonPost "/api/v1/device/command" @{
    device_id = $DeviceId
}
Assert-True -Condition (@($polledCommand.actions).Count -eq 1) -Message "device command poll did not return one action"
Assert-True -Condition ($polledCommand.actions[0].kind -eq "set_mood") -Message "device command kind mismatch"
Assert-True -Condition ($polledCommand.actions[0].value -eq "sad") -Message "device command value mismatch"
$emptyCommand = Invoke-JsonPost "/api/v1/device/command" @{
    device_id = $DeviceId
}
Assert-True -Condition (@($emptyCommand.actions).Count -eq 0) -Message "device command queue was not drained"
Write-Host "ok command action=$($polledCommand.actions[0].kind):$($polledCommand.actions[0].value)"

$queuedCameraCommand = Invoke-JsonPost "/api/v1/device/command" @{
    device_id = $DeviceId
    kind = "capture_camera"
    value = "snapshot"
}
Assert-True -Condition ($queuedCameraCommand.queued -ge 1) -Message "camera command did not queue"
$polledCameraCommand = Invoke-JsonPost "/api/v1/device/command" @{
    device_id = $DeviceId
}
Assert-True -Condition (@($polledCameraCommand.actions).Count -eq 1) -Message "camera command poll did not return one action"
Assert-True -Condition ($polledCameraCommand.actions[0].kind -eq "capture_camera") -Message "camera command kind mismatch"
Assert-True -Condition ($polledCameraCommand.actions[0].value -eq "snapshot") -Message "camera command value mismatch"
Write-Host "ok command action=$($polledCameraCommand.actions[0].kind):$($polledCameraCommand.actions[0].value)"

$wake = Invoke-JsonPost "/api/v1/wake" @{
    device_id = $DeviceId
    phrase = "ouo"
    confidence = 0.91
}
Assert-True -Condition ($wake.assistant.last_wake_phrase -eq "ouo") -Message "wake phrase was not recorded"
Assert-True -Condition ($wake.assistant.last_emotion -eq "blink") -Message "wake did not map high confidence to blink"
Write-Host "ok wake emotion=$($wake.assistant.last_emotion)"
$wakeCommand = Invoke-JsonPost "/api/v1/device/command" @{
    device_id = $DeviceId
}
Assert-True -Condition (@($wakeCommand.actions).Count -eq 1) -Message "wake did not queue one device action"
Assert-True -Condition ($wakeCommand.actions[0].kind -eq "set_mood") -Message "wake action kind mismatch"
Assert-True -Condition ($wakeCommand.actions[0].value -eq "blink") -Message "wake action value mismatch"
Write-Host "ok wake action=$($wakeCommand.actions[0].kind):$($wakeCommand.actions[0].value)"

$emotion = Invoke-JsonPost "/api/v1/emotion/map" @{
    text = "你好，我今天很开心"
    wake_confidence = 0.80
    vision_hint = $null
}
Assert-True -Condition ($validMoods -contains $emotion.device_mood) -Message "emotion returned invalid device_mood: $($emotion.device_mood)"
Write-Host "ok emotion device_mood=$($emotion.device_mood) confidence=$($emotion.confidence)"

$dialog = Invoke-JsonPost "/api/v1/dialog" @{
    device_id = $DeviceId
    text = "你好"
    locale = "zh-CN"
    context = "smoke test"
}
Assert-NotBlank -Value $dialog.text -Message "dialog.text was blank"
Assert-True -Condition ($validMoods -contains $dialog.device_mood) -Message "dialog returned invalid device_mood: $($dialog.device_mood)"
Assert-True -Condition ($dialog.actions.Count -ge 1) -Message "dialog.actions was empty"
Assert-True -Condition ($dialog.actions[0].kind -eq "set_mood") -Message "dialog first action was not set_mood"
Assert-NotBlank -Value $dialog.actions[0].value -Message "dialog first action value was blank"
Write-Host "ok dialog mood=$($dialog.device_mood) action=$($dialog.actions[0].kind):$($dialog.actions[0].value)"

$bmp1x1 = "Qk06AAAAAAAAADYAAAAoAAAAAQAAAP////8BABgAAAAAAAQAAAATCwAAEwsAAAAAAAAAAAAA////AA=="
$camera = Invoke-JsonPost "/api/v1/camera/frame" @{
    device_id = $DeviceId
    mime = "image/bmp"
    width = 1
    height = 1
    image_base64 = $bmp1x1
}
Assert-True -Condition ($camera.device_id -eq $DeviceId) -Message "camera meta did not preserve device_id"
Assert-True -Condition ($camera.mime -eq "image/bmp") -Message "camera meta mime mismatch"
Assert-True -Condition ($camera.bytes -gt 0) -Message "camera meta byte count was zero"
$latestCamera = Invoke-WebRequest -Method Get -Uri (Join-Url $BaseUrl "/api/v1/camera/latest") -TimeoutSec $TimeoutSec
Assert-True -Condition ($latestCamera.StatusCode -eq 200) -Message "camera latest did not return HTTP 200"
$latestCameraContentType = [string]::Join("; ", @($latestCamera.Headers["Content-Type"]))
Assert-True -Condition ($latestCameraContentType -match "image/bmp") -Message "camera latest content type was not image/bmp"
Write-Host "ok camera bytes=$($camera.bytes) latest_content_type=$latestCameraContentType"

$manifest = Invoke-RestMethod -Method Get -Uri (Join-Url $BaseUrl "/api/v1/ota/manifest") -TimeoutSec $TimeoutSec
Assert-NotBlank -Value $manifest.version -Message "ota manifest version was blank"
Assert-NotBlank -Value $manifest.firmware_url -Message "ota manifest firmware_url was blank"
Assert-NotBlank -Value $manifest.sha256 -Message "ota manifest sha256 was blank"
Assert-True -Condition ($manifest.sha256 -match "^[0-9a-fA-F]{64}$") -Message "ota manifest sha256 was not 64 hex chars"
Assert-True -Condition ($manifest.size -gt 0) -Message "ota manifest size was not positive"
$firmwareHead = Invoke-WebRequest -Method Head -Uri $manifest.firmware_url -TimeoutSec $TimeoutSec
Assert-True -Condition ($firmwareHead.StatusCode -eq 200) -Message "firmware HEAD did not return HTTP 200"
$firmwareContentLength = [int64]([string]::Join("", @($firmwareHead.Headers["Content-Length"])))
Assert-True -Condition ($firmwareContentLength -eq [int64]$manifest.size) -Message "firmware Content-Length did not match manifest size"
$firmwareTemp = Join-Path ([System.IO.Path]::GetTempPath()) ("ouo-ota-smoke-{0}.bin" -f ([guid]::NewGuid().ToString("N")))
try {
    Invoke-WebRequest -Method Get -Uri $manifest.firmware_url -OutFile $firmwareTemp -TimeoutSec $TimeoutSec
    $firmwareFile = Get-Item -LiteralPath $firmwareTemp
    Assert-True -Condition ($firmwareFile.Length -eq [int64]$manifest.size) -Message "downloaded firmware size did not match manifest size"
    $firmwareSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $firmwareTemp).Hash.ToLowerInvariant()
    Assert-True -Condition ($firmwareSha256 -eq ([string]$manifest.sha256).ToLowerInvariant()) -Message "downloaded firmware sha256 did not match manifest sha256"
} finally {
    if (Test-Path -LiteralPath $firmwareTemp) {
        Remove-Item -LiteralPath $firmwareTemp -Force
    }
}
Write-Host "ok ota manifest version=$($manifest.version) size=$($manifest.size) sha256=$($manifest.sha256)"

$ota = Invoke-JsonPost "/api/v1/ota/report" @{
    device_id = $DeviceId
    current_version = $FirmwareVersion
    target_version = $manifest.version
    status = "smoke_ok"
    detail = "backend smoke completed"
}
Assert-True -Condition ($ota.last_status -eq "smoke_ok") -Message "ota report did not preserve status"
Write-Host "ok ota report status=$($ota.last_status) target=$($ota.target_version)"

Write-Host "AI Home backend smoke passed"
