param(
    [ValidateSet("whisper", "auto", "nemotron")]
    [string]$Engine = "whisper",
    [string]$Language = "fr",
    [ValidateSet("0.45", "0.5", "0.55", "0.7")]
    [string]$SilenceAfterSpeech = "0.55",
    [string]$ExpectedLiveVoiceSha256 = "EEB03619E1EE4C91FDE19B9B35579A901ACFDCB6D8436036403FDE4A07FD90CA"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$liveRoot = "C:\Kage"
$diagDir = Join-Path $liveRoot "stt_diagnostics"
$rollbackRoot = Join-Path $liveRoot "stt_rollback"
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$backupDir = Join-Path $rollbackRoot $stamp
$liveVoice = Join-Path $liveRoot "voice.py"
$liveStt = Join-Path $liveRoot "stt_engine.py"
$repoVoice = Join-Path $repoRoot "backend\voice.py"
$repoStt = Join-Path $repoRoot "backend\stt_engine.py"
$python = Join-Path $liveRoot "venv\Scripts\python.exe"

foreach ($required in @($liveVoice, $repoVoice, $repoStt, $python)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Missing required file: $required" }
}

$actualLiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $liveVoice).Hash
if ($actualLiveHash -ne $ExpectedLiveVoiceSha256) {
    throw "Live voice.py changed since review ($actualLiveHash). Refusing to overwrite it."
}

# Validate the staged Python before stopping or replacing anything live.
& $python -m py_compile $repoVoice $repoStt
if ($LASTEXITCODE -ne 0) { throw "Staged Python validation failed ($LASTEXITCODE)" }

if ($Engine -in @("auto", "nemotron")) {
    try {
        $ready = Invoke-RestMethod -Uri "http://127.0.0.1:8080/ready" -TimeoutSec 2
    } catch {
        throw "Nemotron was requested but its local server is not ready."
    }
    if ($ready.ready -ne $true) { throw "Nemotron was requested but its local server is not ready." }
}

New-Item -ItemType Directory -Force -Path $diagDir, $backupDir | Out-Null
Copy-Item -LiteralPath $liveVoice -Destination (Join-Path $backupDir "voice.py") -Force
$liveSttExisted = Test-Path -LiteralPath $liveStt
if ($liveSttExisted) {
    Copy-Item -LiteralPath $liveStt -Destination (Join-Path $backupDir "stt_engine.py") -Force
}

$voiceProcesses = @(Get-CimInstance Win32_Process | Where-Object {
    $_.Name -match "^python(w)?\.exe$" -and
    $_.CommandLine -match '(?i)[\\/]Kage[\\/]voice\.py(?:"|\s|$)'
})

$trackedEnv = @(
    "KAGE_STT_ENGINE", "KAGE_STT_LANGUAGE", "KAGE_SILENCE_AFTER_SPEECH",
    "KAGE_WHISPER_MODEL", "KAGE_WHISPER_COMPUTE", "KAGE_WHISPER_BEAM",
    "KAGE_WHISPER_CPU_THREADS", "KAGE_WHISPER_NUM_WORKERS", "KAGE_WHISPER_PROMPT",
    "KAGE_STT_FALLBACK", "KAGE_STT_CONTEXT", "KAGE_NEMO_URL",
    "KAGE_NEMO_CONFIDENCE_USABLE", "KAGE_STT_MIN_CONFIDENCE"
)
$envBackup = [ordered]@{}
foreach ($name in $trackedEnv) {
    $envBackup[$name] = [Environment]::GetEnvironmentVariable($name, "User")
}

$manifest = [ordered]@{
    created = (Get-Date).ToString("o")
    backup_dir = $backupDir
    live_voice_sha256 = $actualLiveHash
    live_stt_existed = $liveSttExisted
    voice_was_running = ($voiceProcesses.Count -gt 0)
    voice_processes = @($voiceProcesses | Select-Object ProcessId, ParentProcessId, ExecutablePath, CommandLine)
    user_environment = $envBackup
    repo_root = $repoRoot
}
$manifestPath = Join-Path $backupDir "rollback_manifest.json"
$manifest | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 $manifestPath
Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $diagDir "rollback_manifest.json") -Force

# The backup and manifest now exist; only this exact voice client may be stopped.
foreach ($process in $voiceProcesses) {
    Stop-Process -Id $process.ProcessId -Force -ErrorAction Stop
}

Copy-Item -LiteralPath $repoVoice -Destination $liveVoice -Force
Copy-Item -LiteralPath $repoStt -Destination $liveStt -Force

[Environment]::SetEnvironmentVariable("KAGE_STT_ENGINE", $Engine, "User")
[Environment]::SetEnvironmentVariable("KAGE_STT_LANGUAGE", $Language, "User")
[Environment]::SetEnvironmentVariable("KAGE_SILENCE_AFTER_SPEECH", $SilenceAfterSpeech, "User")
$env:KAGE_STT_ENGINE = $Engine
$env:KAGE_STT_LANGUAGE = $Language
$env:KAGE_SILENCE_AFTER_SPEECH = $SilenceAfterSpeech

$voiceOut = Join-Path $diagDir "voice_experiment.out.log"
$voiceErr = Join-Path $diagDir "voice_experiment.err.log"
$voiceProcess = Start-Process -FilePath $python -ArgumentList @($liveVoice) `
    -RedirectStandardOutput $voiceOut -RedirectStandardError $voiceErr `
    -WindowStyle Hidden -PassThru

Start-Sleep -Seconds 3
if ($voiceProcess.HasExited) {
    throw "Experimental voice client exited during startup. Run rollback_stt.ps1; see $voiceErr"
}

[ordered]@{
    started = (Get-Date).ToString("o")
    voice_pid = $voiceProcess.Id
    stt_engine = $Engine
    language = $Language
    silence_after_speech = [double]$SilenceAfterSpeech
    rollback_manifest = $manifestPath
} | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $diagDir "experiment_state.json")

Write-Host "French STT experiment deployed (PID $($voiceProcess.Id))."
Write-Host "Rollback: powershell -ExecutionPolicy Bypass -File $PSScriptRoot\rollback_stt.ps1"
