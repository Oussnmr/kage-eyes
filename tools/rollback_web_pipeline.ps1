param(
    [Parameter(Mandatory = $true)]
    [string]$BackupDirectory
)

$ErrorActionPreference = 'Stop'
$rollbackRoot = [IO.Path]::GetFullPath('C:\Kage\kage_codex_sandbox\rollback')
$resolvedBackup = [IO.Path]::GetFullPath($BackupDirectory)
if (-not $resolvedBackup.StartsWith($rollbackRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "Backup must be inside $rollbackRoot"
}
$manifestPath = Join-Path $resolvedBackup 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "Missing rollback manifest: $manifestPath" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.version -ne 'web-native-v2' -or $manifest.live_root -ne 'C:\Kage') {
    throw 'Rollback manifest does not describe the expected web-native-v2 deployment.'
}

foreach ($record in $manifest.files) {
    $backupFile = Join-Path $resolvedBackup $record.name
    if (-not (Test-Path -LiteralPath $backupFile)) { throw "Missing backup file: $backupFile" }
    $hash = (Get-FileHash -LiteralPath $backupFile -Algorithm SHA256).Hash
    if ($hash -ne $record.original_sha256) { throw "Backup hash mismatch for $($record.name)" }
}

foreach ($record in $manifest.files) {
    Copy-Item -LiteralPath (Join-Path $resolvedBackup $record.name) `
        -Destination (Join-Path $manifest.live_root $record.name) -Force
    $actual = (Get-FileHash -LiteralPath (Join-Path $manifest.live_root $record.name) -Algorithm SHA256).Hash
    if ($actual -ne $record.original_sha256) { throw "Restored hash mismatch for $($record.name)" }
}

$listener = Get-NetTCPConnection -State Listen -LocalPort 8000 -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($listener) {
    $serverPid = [int]$listener.OwningProcess
    $serverProcess = Get-CimInstance Win32_Process -Filter "ProcessId=$serverPid"
    if ($serverProcess.ExecutablePath -notlike '*Python312*python.exe' -or
        $serverProcess.CommandLine -notmatch '(?i)Kage[\\/]run_backend\.py') {
        throw "Port 8000 owner is not the expected Kage backend; restored files are present but restart was refused. PID=$serverPid"
    }
    Stop-Process -Id $serverPid -Force
    Start-Sleep -Milliseconds 500
}

$site = 'C:\Kage\venv\Lib\site-packages'
$env:PYTHONPATH = "$site;$site\win32;$site\win32\lib;$site\pywin32_system32;C:\Kage"
$env:PATH = "$site\pywin32_system32;$env:PATH"
Start-Process -FilePath 'C:\Users\Oussama\AppData\Local\Programs\Python\Python312\python.exe' `
    -ArgumentList 'C:\Kage\run_backend.py' -WorkingDirectory 'C:\Kage' -WindowStyle Hidden
Write-Output "Restored live files from $resolvedBackup; Kage backend restart requested."
