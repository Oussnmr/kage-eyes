param(
    [string]$LiveRoot = 'C:\Kage',
    [string]$RollbackRoot = 'C:\Kage\kage_codex_sandbox\rollback'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$version = 'web-progressive-v3.6'
$files = @('app.py', 'codex_bridge.py', 'voice.py', 'web_search.py', 'stt_engine.py', 'kage_sounds.py')
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$backupDir = Join-Path $RollbackRoot "${version}_$stamp"
New-Item -ItemType Directory -Path $backupDir -Force | Out-Null

$records = @()
foreach ($name in $files) {
    $liveFile = Join-Path $LiveRoot $name
    $sourceFile = Join-Path (Join-Path $repoRoot 'backend') $name
    if (-not (Test-Path -LiteralPath $liveFile)) { throw "Missing live file: $liveFile" }
    if (-not (Test-Path -LiteralPath $sourceFile)) { throw "Missing versioned file: $sourceFile" }
    $before = (Get-FileHash -LiteralPath $liveFile -Algorithm SHA256).Hash
    $sourceHash = (Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $liveFile -Destination (Join-Path $backupDir $name)
    $backupHash = (Get-FileHash -LiteralPath (Join-Path $backupDir $name) -Algorithm SHA256).Hash
    if ($backupHash -ne $before) { throw "Backup verification failed for $name" }
    $records += [pscustomobject]@{
        name = $name
        original_sha256 = $before
        deployed_sha256 = $sourceHash
    }
}

$manifest = [pscustomobject]@{
    version = $version
    created_at = (Get-Date).ToString('o')
    live_root = $LiveRoot
    rollback_directory = $backupDir
    files = $records
}
$manifestPath = Join-Path $backupDir 'manifest.json'
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

try {
    foreach ($record in $records) {
        $sourceFile = Join-Path (Join-Path $repoRoot 'backend') $record.name
        Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $LiveRoot $record.name) -Force
        $actual = (Get-FileHash -LiteralPath (Join-Path $LiveRoot $record.name) -Algorithm SHA256).Hash
        if ($actual -ne $record.deployed_sha256) { throw "Deployment hash mismatch for $($record.name)" }
    }

    $listener = Get-NetTCPConnection -State Listen -LocalPort 8000 -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($listener) {
        $serverPid = [int]$listener.OwningProcess
        $serverProcess = Get-CimInstance Win32_Process -Filter "ProcessId=$serverPid"
        if ($serverProcess.ExecutablePath -notlike '*Python312*python.exe' -or
            $serverProcess.CommandLine -notmatch '(?i)Kage[\\/]run_backend\.py') {
            throw "Port 8000 owner is not the expected Kage backend; files are deployed but restart was refused. PID=$serverPid"
        }
        Stop-Process -Id $serverPid -Force
        $deadline = (Get-Date).AddSeconds(10)
        do {
            Start-Sleep -Milliseconds 250
            $listener = Get-NetTCPConnection -State Listen -LocalPort 8000 -ErrorAction SilentlyContinue |
                Select-Object -First 1
        } while ($listener -and (Get-Date) -lt $deadline)
        if ($listener) { throw 'Kage backend did not release port 8000' }
    }

    $site = 'C:\Kage\venv\Lib\site-packages'
    $env:PYTHONPATH = "$site;$site\win32;$site\win32\lib;$site\pywin32_system32;C:\Kage"
    $env:PATH = "$site\pywin32_system32;$env:PATH"
    Start-Process -FilePath 'C:\Users\Oussama\AppData\Local\Programs\Python\Python312\python.exe' `
        -ArgumentList 'C:\Kage\run_backend.py' -WorkingDirectory 'C:\Kage' -WindowStyle Hidden

    $ready = $false
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline -and -not $ready) {
        Start-Sleep -Milliseconds 500
        try {
            $null = Invoke-WebRequest -Uri 'http://127.0.0.1:8000/status' -TimeoutSec 2 -ErrorAction Stop
        } catch {
            $status = $_.Exception.Response.StatusCode.value__
            if ($status -in @(401, 403)) { $ready = $true }
        }
    }
    if (-not $ready) { throw 'Kage backend did not become ready on port 8000' }

    # voice.py is part of this deployment. Use the existing guarded launcher:
    # it terminates only Kagé voice.py instances and leaves NeMo/backend alone
    # when their ports are already available.
    Start-Process -FilePath 'powershell.exe' -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', 'C:\Kage\restart_kage.ps1'
    ) -WorkingDirectory 'C:\Kage'
    Write-Output "Deployed $version and restarted Kage Voice; rollback snapshot: $backupDir"
} catch {
    Write-Error "Deployment failed: $_"
    Write-Error "Restore with tools/rollback_web_pipeline.ps1 -BackupDirectory '$backupDir'"
    throw
}
