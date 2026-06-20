param(
    [string]$IdfVersion = "master-s31"
)

$ErrorActionPreference = "Stop"
$env:PYTHONUTF8 = "1"

$ProjectDir = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\idf")
Push-Location $ProjectDir
try {
    eim run "idf.py --preview set-target esp32s31 build" $IdfVersion
} finally {
    Pop-Location
}
