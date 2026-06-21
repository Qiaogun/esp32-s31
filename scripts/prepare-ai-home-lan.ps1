param(
    [string]$BaseUrl = "",
    [int]$Port = 8787,
    [string]$Version = "0.2.0-ai-home-link",
    [string]$Channel = "dev",
    [switch]$SkipPublish
)

$ErrorActionPreference = "Stop"

function Get-LanBaseUrl {
    param([int]$Port)

    $configs = Get-NetIPConfiguration -ErrorAction SilentlyContinue | Where-Object {
        $_.IPv4DefaultGateway -and $_.IPv4Address
    }
    foreach ($config in $configs) {
        foreach ($addr in $config.IPv4Address) {
            if ($addr.IPAddress -and $addr.IPAddress -notmatch "^(127\.|169\.254\.)") {
                return "http://$($addr.IPAddress):$Port"
            }
        }
    }

    throw "No LAN IPv4 address with a default gateway was found. Pass -BaseUrl http://<pc-ip>:$Port."
}

function Join-Url {
    param(
        [string]$Base,
        [string]$Path
    )
    return "$($Base.TrimEnd('/'))/$($Path.TrimStart('/'))"
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

if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
    $BaseUrl = Get-LanBaseUrl -Port $Port
}
$BaseUrl = $BaseUrl.TrimEnd("/")

if ($BaseUrl -match "://(127\.0\.0\.1|localhost)(:|/|$)") {
    throw "BaseUrl is $BaseUrl, but device LAN smoke needs the PC's LAN IP."
}

Write-Host "AI Home LAN prepare"
Write-Host "base_url=$BaseUrl version=$Version channel=$Channel skip_publish=$SkipPublish"

if (-not $SkipPublish) {
    & (Join-Path $PSScriptRoot "publish-ota.ps1") -Version $Version -Channel $Channel -BaseUrl $BaseUrl
}

try {
    $health = Invoke-RestMethod -Method Get -Uri (Join-Url $BaseUrl "/health") -TimeoutSec 10
} catch {
    throw "Cannot reach backend at $BaseUrl. Start it with: cd server; cargo run"
}

Assert-True -Condition ($health.ok -eq $true) -Message "health.ok was not true at $BaseUrl"

& (Join-Path $PSScriptRoot "smoke-ai-home-backend.ps1") -BaseUrl $BaseUrl

$manifest = Invoke-RestMethod -Method Get -Uri (Join-Url $BaseUrl "/api/v1/ota/manifest") -TimeoutSec 10
Assert-True -Condition ([string]$manifest.firmware_url -like "$BaseUrl/*") -Message "OTA firmware_url does not use the LAN base URL: $($manifest.firmware_url)"

Write-Host "AI Home LAN prepare passed"
Write-Host "device_smoke=.\scripts\smoke-ai-home-device.ps1 -Port <port> -Ssid <ssid> -Password <password> -ServerUrl $BaseUrl"
