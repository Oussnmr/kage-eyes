param([switch]$VerifyOnly)

$ErrorActionPreference = "Stop"
$liveRoot = "C:\Kage"
$diagDir = Join-Path $liveRoot "stt_diagnostics"
$manifestPointer = Join-Path $diagDir "rollback_manifest.json"
$statePath = Join-Path $diagDir "experiment_state.json"
$liveVoice = Join-Path $liveRoot "voice.py"
$liveStt = Join-Path $liveRoot "stt_engine.py"
$python = Join-Path $liveRoot "venv\Scripts\python.exe"

if (-not (Test-Path -LiteralPath $manifestPointer)) {
    throw "Rollback manifest not found: $manifestPointer"
}
$pointer = Get-Content -LiteralPath $manifestPointer -Raw | ConvertFrom-Json
$manifestPath = Join-Path $pointer.backup_dir "rollback_manifest.json"
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "Backed-up rollback manifest not found: $manifestPath"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$voiceBackup = Join-Path $manifest.backup_dir "voice.py"
$sttBackup = Join-Path $manifest.backup_dir "stt_engine.py"
if (-not (Test-Path -LiteralPath $voiceBackup)) { throw "Voice backup missing: $voiceBackup" }
$backupHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $voiceBackup).Hash
if ($backupHash -ne $manifest.live_voice_sha256) {
    throw "Voice backup hash mismatch: $backupHash"
}
if ($manifest.live_stt_existed -and -not (Test-Path -LiteralPath $sttBackup)) {
    throw "STT backup missing: $sttBackup"
}

if ($VerifyOnly) {
    Write-Host "Rollback verified: backup files, hash, manifest and environment snapshot are present."
    return
}

# Stop only the experiment voice PID and the exact NeMo test PID, if recorded.
if (Test-Path -LiteralPath $statePath) {
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    if ($state.voice_pid) {
        Stop-Process -Id ([int]$state.voice_pid) -Force -ErrorAction SilentlyContinue
    }
}
$nemoStatePath = Join-Path $diagDir "nemotron_server.json"
if (Test-Path -LiteralPath $nemoStatePath) {
    $nemoState = Get-Content -LiteralPath $nemoStatePath -Raw | ConvertFrom-Json
    if ($nemoState.pid) {
        Stop-Process -Id ([int]$nemoState.pid) -Force -ErrorAction SilentlyContinue
    }
}

Copy-Item -LiteralPath $voiceBackup -Destination $liveVoice -Force
if ($manifest.live_stt_existed) {
    Copy-Item -LiteralPath $sttBackup -Destination $liveStt -Force
} elseif (Test-Path -LiteralPath $liveStt) {
    Remove-Item -LiteralPath $liveStt -Force
}

foreach ($property in $manifest.user_environment.PSObject.Properties) {
    [Environment]::SetEnvironmentVariable($property.Name, $property.Value, "User")
}

$restoredHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $liveVoice).Hash
if ($restoredHash -ne $manifest.live_voice_sha256) {
    throw "Rollback copied voice.py but its hash is wrong: $restoredHash"
}

if ($manifest.voice_was_running) {
    $out = Join-Path $diagDir "voice_rollback.out.log"
    $err = Join-Path $diagDir "voice_rollback.err.log"
    $process = Start-Process -FilePath $python -ArgumentList @($liveVoice) `
        -RedirectStandardOutput $out -RedirectStandardError $err `
        -WindowStyle Hidden -PassThru
    Write-Host "Previous Kage Voice restored and restarted (PID $($process.Id))."
} else {
    Write-Host "Previous Kage Voice restored and left stopped, matching the pre-experiment state."
}
Write-Host "Nemotron experiment server stopped; main was never modified."
