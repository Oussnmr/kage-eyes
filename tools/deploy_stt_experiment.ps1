$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$liveRoot = "C:\Kage"
$diagDir = Join-Path $liveRoot "stt_diagnostics"
$rollbackRoot = Join-Path $liveRoot "stt_rollback"
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$backupDir = Join-Path $rollbackRoot $stamp

New-Item -ItemType Directory -Force -Path $diagDir | Out-Null
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null

$liveVoice = Join-Path $liveRoot "voice.py"
$liveStt = Join-Path $liveRoot "stt_engine.py"
$repoVoice = Join-Path $repoRoot "backend\voice.py"
$repoStt = Join-Path $repoRoot "backend\stt_engine.py"
$python = Join-Path $liveRoot "venv\Scripts\python.exe"

if (-not (Test-Path $repoVoice)) { throw "Missing $repoVoice" }
if (-not (Test-Path $repoStt)) { throw "Missing $repoStt" }
if (-not (Test-Path $python)) { throw "Missing Kage venv Python: $python" }

if (Test-Path $liveVoice) {
    Copy-Item $liveVoice (Join-Path $backupDir "voice.py") -Force
}
if (Test-Path $liveStt) {
    Copy-Item $liveStt (Join-Path $backupDir "stt_engine.py") -Force
}

$manifest = [ordered]@{
    created = (Get-Date).ToString("o")
    backup_dir = $backupDir
    live_voice_existed = (Test-Path $liveVoice)
    live_stt_existed = (Test-Path $liveStt)
    repo_root = $repoRoot
}
$manifest | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $diagDir "rollback_manifest.json")

# Prepare the new runtime first. Until this succeeds, the current live voice
# client is left untouched and keeps running.
& $python -m pip install --disable-pip-version-check requests
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "setup_nemotron_stt.ps1")

# Only after Nemotron is installed and /ready succeeded do we switch the live
# client. The FastAPI backend is left untouched.
Get-CimInstance Win32_Process |
    Where-Object {
        $_.Name -match "^python" -and
        $_.CommandLine -match [regex]::Escape($liveVoice)
    } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

Copy-Item $repoVoice $liveVoice -Force
Copy-Item $repoStt $liveStt -Force

[Environment]::SetEnvironmentVariable("KAGE_STT_ENGINE", "auto", "User")
[Environment]::SetEnvironmentVariable("KAGE_STT_LANGUAGE", "fr", "User")
[Environment]::SetEnvironmentVariable("KAGE_SILENCE_AFTER_SPEECH", "0.55", "User")
$env:KAGE_STT_ENGINE = "auto"
$env:KAGE_STT_LANGUAGE = "fr"
$env:KAGE_SILENCE_AFTER_SPEECH = "0.55"

$voiceOut = Join-Path $diagDir "voice_experiment.out.log"
$voiceErr = Join-Path $diagDir "voice_experiment.err.log"
$voiceProcess = Start-Process -FilePath $python -ArgumentList @($liveVoice) -RedirectStandardOutput $voiceOut -RedirectStandardError $voiceErr -PassThru

[ordered]@{
    started = (Get-Date).ToString("o")
    voice_pid = $voiceProcess.Id
    stt_engine = "auto"
    language = "fr"
    silence_after_speech = 0.55
    rollback_manifest = (Join-Path $diagDir "rollback_manifest.json")
} | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $diagDir "experiment_state.json")

Write-Host "French STT experiment deployed."
Write-Host "Live voice PID: $($voiceProcess.Id)"
Write-Host "Rollback: powershell -ExecutionPolicy Bypass -File .\tools\rollback_stt.ps1"
