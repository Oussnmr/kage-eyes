$ErrorActionPreference = "Stop"

$liveRoot = "C:\Kage"
$diagDir = Join-Path $liveRoot "stt_diagnostics"
$manifestPath = Join-Path $diagDir "rollback_manifest.json"
$liveVoice = Join-Path $liveRoot "voice.py"
$liveStt = Join-Path $liveRoot "stt_engine.py"
$python = Join-Path $liveRoot "venv\Scripts\python.exe"

Get-CimInstance Win32_Process |
    Where-Object {
        ($_.Name -match "^python" -and $_.CommandLine -match [regex]::Escape($liveVoice)) -or
        ($_.Name -match "nemo-speech" -and $_.CommandLine -match "serve")
    } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

if (Test-Path $manifestPath) {
    $manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
    $backupDir = $manifest.backup_dir
    $voiceBackup = Join-Path $backupDir "voice.py"
    $sttBackup = Join-Path $backupDir "stt_engine.py"

    if (Test-Path $voiceBackup) {
        Copy-Item $voiceBackup $liveVoice -Force
    }
    if ($manifest.live_stt_existed -and (Test-Path $sttBackup)) {
        Copy-Item $sttBackup $liveStt -Force
    } elseif (Test-Path $liveStt) {
        Remove-Item $liveStt -Force
    }
}

[Environment]::SetEnvironmentVariable("KAGE_STT_ENGINE", "whisper", "User")
[Environment]::SetEnvironmentVariable("KAGE_STT_LANGUAGE", "en", "User")
[Environment]::SetEnvironmentVariable("KAGE_SILENCE_AFTER_SPEECH", "0.7", "User")
$env:KAGE_STT_ENGINE = "whisper"
$env:KAGE_STT_LANGUAGE = "en"
$env:KAGE_SILENCE_AFTER_SPEECH = "0.7"

if ((Test-Path $python) -and (Test-Path $liveVoice)) {
    $out = Join-Path $diagDir "voice_rollback.out.log"
    $err = Join-Path $diagDir "voice_rollback.err.log"
    $process = Start-Process -FilePath $python -ArgumentList @($liveVoice) -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
    Write-Host "Previous Kage Voice restored and restarted (PID $($process.Id))."
} else {
    Write-Host "Rollback files/settings restored. Start Kage Voice manually."
}
Write-Host "Nemotron test server stopped. Git main was never modified."
