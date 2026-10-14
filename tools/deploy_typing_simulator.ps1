# Deployment deliberately avoids replacing the live C:\Kage\app.py.
# No firmware flash, motor command, or automatic backend restart.
[CmdletBinding()]
param(
    [string]$Repo = 'C:\Kage\kage-eyes',
    [string]$Destination = 'C:\Kage',
    [switch]$Install
)
$ErrorActionPreference = 'Stop'
$source = Join-Path $Repo 'backend'
$assetDir = Join-Path $source 'typing_simulator_dist'
$required = @('typing_simulator.html','typing_simulator_routes.py')
foreach($filename in $required) {
    if(-not (Test-Path (Join-Path $source $filename))) { throw "Missing: $filename" }
}
foreach($filename in @('kage_lvgl.js','kage_lvgl.wasm')) {
    if(-not (Test-Path (Join-Path $assetDir $filename))) {
        throw "WASM missing: $filename. Run the GitHub build first."
    }
}
$appPath = Join-Path $Destination 'app.py'
if(-not (Test-Path $appPath)) { throw "Cannot find live backend $appPath" }
$register = @'

# Kage typing simulator — isolated and read-only for hardware
from typing_simulator_routes import make_typing_router
app.include_router(make_typing_router(require_kage_key))
'@
$existing = Get-Content $appPath -Raw -Encoding UTF8
if(-not ($existing -match 'def require_kage_key\(')) {
    throw 'Live backend has no require_kage_key() function; manual review needed.'
}
$needsRoute = $existing -notmatch 'app\.include_router\(make_typing_router\(require_kage_key\)\)'
Write-Host "Source $source"
Write-Host "Target $Destination"
Write-Host "Append route to live app.py: $needsRoute"
if(-not $Install) {
    Write-Host 'Dry run. Add -Install to copy/append; no backend restart will occur.'
    return
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backupDir = Join-Path $Destination "typing-simulator-backup-$stamp"
New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
Copy-Item $appPath (Join-Path $backupDir 'app.py')
foreach($filename in $required) {
    $dest = Join-Path $Destination $filename
    if(Test-Path $dest) {Copy-Item $dest (Join-Path $backupDir $filename)}
    Copy-Item (Join-Path $source $filename) $dest -Force
}
$destDist = Join-Path $Destination 'typing_simulator_dist'
New-Item -ItemType Directory -Path $destDist -Force | Out-Null
foreach($filename in @('kage_lvgl.js','kage_lvgl.wasm')) {
    $dest = Join-Path $destDist $filename
    if(Test-Path $dest){ Copy-Item $dest (Join-Path $backupDir $filename)}
    Copy-Item (Join-Path $assetDir $filename) $dest -Force
}
if($needsRoute){Add-Content -Path $appPath -Value $register -Encoding UTF8}
Write-Host "Installed; app.py backup: $backupDir"
Write-Host 'Restart the backend only after verifying the Python syntax.'
