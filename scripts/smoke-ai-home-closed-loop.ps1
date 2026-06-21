param(
    [string]$Port = "",
    [int]$Baud = 115200,
    [string]$Ssid = "",
    [string]$Password = "",
    [string]$BaseUrl = "",
    [int]$ServerPort = 8787,
    [string]$Version = "0.2.0-ai-home-link",
    [string]$Channel = "dev",
    [string]$DialogText = "你好",
    [int]$TimeoutSec = 45,
    [switch]$SkipDeviceSmoke,
    [switch]$SkipPublish,
    [switch]$SkipCameraSnapshot,
    [switch]$SkipOtaCheck,
    [switch]$KeepStartedServer,
    [string]$LogDir = ""
)

$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    return (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
}

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

function Test-BackendHealth {
    param([string]$Url)

    try {
        $health = Invoke-RestMethod -Method Get -Uri (Join-Url $Url "/health") -TimeoutSec 2
        return ($health.ok -eq $true)
    } catch {
        return $false
    }
}

function Test-BackendCommandApi {
    param([string]$Url)

    try {
        $command = Invoke-RestMethod `
            -Method Post `
            -Uri (Join-Url $Url "/api/v1/device/command") `
            -ContentType "application/json" `
            -Body (@{ device_id = "ouo-closed-loop-probe" } | ConvertTo-Json -Compress) `
            -TimeoutSec 2
        return ($null -ne $command.actions)
    } catch {
        return $false
    }
}

function Wait-BackendHealth {
    param(
        [string]$Url,
        [int]$TimeoutSec
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-BackendHealth -Url $Url) {
            return
        }
        Start-Sleep -Milliseconds 500
    }
    throw "Backend did not become healthy at $Url within $TimeoutSec seconds."
}

function Wait-BackendReady {
    param(
        [string]$Url,
        [int]$TimeoutSec
    )

    Wait-BackendHealth -Url $Url -TimeoutSec $TimeoutSec
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-BackendCommandApi -Url $Url) {
            return
        }
        Start-Sleep -Milliseconds 500
    }
    throw "Backend at $Url is healthy but does not support /api/v1/device/command. Stop the old server process and rerun this script."
}

function ConvertTo-SingleQuotedPowerShell {
    param([string]$Value)
    return "'" + ($Value -replace "'", "''") + "'"
}

$root = Get-RepoRoot
if ([string]::IsNullOrWhiteSpace($LogDir)) {
    $LogDir = Join-Path $root "logs"
}
if (-not (Test-Path -LiteralPath $LogDir)) {
    New-Item -ItemType Directory -Path $LogDir | Out-Null
}

if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
    $BaseUrl = Get-LanBaseUrl -Port $ServerPort
}
$BaseUrl = $BaseUrl.TrimEnd("/")

if ([string]::IsNullOrWhiteSpace($Port)) {
    $Port = $env:OUO_SERIAL_PORT
}
if ([string]::IsNullOrWhiteSpace($Ssid)) {
    $Ssid = $env:OUO_WIFI_SSID
}
if ([string]::IsNullOrWhiteSpace($Password)) {
    $Password = $env:OUO_WIFI_PASSWORD
}

if ($BaseUrl -match "://(127\.0\.0\.1|localhost)(:|/|$)") {
    throw "BaseUrl is $BaseUrl, but the ESP needs the PC's LAN IP. Pass -BaseUrl http://<pc-ip>:$ServerPort."
}

Write-Host "AI Home closed-loop smoke"
Write-Host "base_url=$BaseUrl version=$Version channel=$Channel skip_device_smoke=$SkipDeviceSmoke"

$startedServer = $null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
if (-not (Test-BackendHealth -Url $BaseUrl)) {
    $stdout = Join-Path $LogDir "ai-home-server-$stamp.stdout.log"
    $stderr = Join-Path $LogDir "ai-home-server-$stamp.stderr.log"
    $manifestPath = Join-Path $root "server\Cargo.toml"
    $targetDir = Join-Path $env:TEMP "ouo-server-target-closed-loop"
    $quotedManifest = ConvertTo-SingleQuotedPowerShell -Value $manifestPath
    $quotedTargetDir = ConvertTo-SingleQuotedPowerShell -Value $targetDir
    $serverAddr = "0.0.0.0:$ServerPort"
    $command = "`$env:OUO_SERVER_ADDR='$serverAddr'; cargo run --manifest-path $quotedManifest --target-dir $quotedTargetDir"

    Write-Host "starting_backend=1 addr=$serverAddr stdout=$stdout stderr=$stderr"
    $startedServer = Start-Process `
        -FilePath "powershell" `
        -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", $command) `
        -WorkingDirectory $root `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -PassThru
    Wait-BackendReady -Url $BaseUrl -TimeoutSec 45
} else {
    if (-not (Test-BackendCommandApi -Url $BaseUrl)) {
        throw "Existing backend at $BaseUrl is healthy but does not support /api/v1/device/command. Stop the old server process and rerun this script."
    }
    Write-Host "starting_backend=0 existing backend is healthy"
}

try {
    $prepareArgs = @{
        BaseUrl = $BaseUrl
        Version = $Version
        Channel = $Channel
    }
    if ($SkipPublish) {
        $prepareArgs.SkipPublish = $true
    }
    & (Join-Path $PSScriptRoot "prepare-ai-home-lan.ps1") @prepareArgs

    if (-not $SkipDeviceSmoke) {
        if ([string]::IsNullOrWhiteSpace($Ssid) -or [string]::IsNullOrWhiteSpace($Password)) {
            throw "Use -Ssid and -Password, set OUO_WIFI_SSID/OUO_WIFI_PASSWORD, or pass -SkipDeviceSmoke for backend/LAN-only validation."
        }

        $deviceArgs = @{
            Baud = $Baud
            Ssid = $Ssid
            Password = $Password
            ServerUrl = $BaseUrl
            DialogText = $DialogText
            TimeoutSec = $TimeoutSec
        }
        if (-not [string]::IsNullOrWhiteSpace($Port)) {
            $deviceArgs.Port = $Port
        }
        if ($SkipCameraSnapshot) {
            $deviceArgs.SkipCameraSnapshot = $true
        }
        if ($SkipOtaCheck) {
            $deviceArgs.SkipOtaCheck = $true
        }

        & (Join-Path $PSScriptRoot "smoke-ai-home-device.ps1") @deviceArgs
    } else {
        Write-Host "device_smoke=skipped"
    }

    Write-Host "AI Home closed-loop smoke passed"
} finally {
    if ($startedServer -and -not $KeepStartedServer) {
        Write-Host "stopping_started_backend pid=$($startedServer.Id)"
        Stop-Process -Id $startedServer.Id -ErrorAction SilentlyContinue
    } elseif ($startedServer) {
        Write-Host "started_backend_left_running pid=$($startedServer.Id)"
    }
}
