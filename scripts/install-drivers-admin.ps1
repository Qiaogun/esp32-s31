param(
    [switch]$Rescan
)

$ErrorActionPreference = "Stop"

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    throw "Run this script from an Administrator PowerShell window."
}

eim install-drivers --do-not-track true

if ($Rescan) {
    pnputil /scan-devices
}

$ports = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
    Select-Object DeviceID, Name, Description, PNPDeviceID

if ($ports) {
    $ports | Format-Table -AutoSize
} else {
    Write-Warning "No COM ports visible yet. Unplug/replug the board, then run .\scripts\preflight-s31.ps1."
}
