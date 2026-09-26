$ErrorActionPreference = "Stop"

[Environment]::SetEnvironmentVariable("KAGE_STT_ENGINE", "whisper", "User")
[Environment]::SetEnvironmentVariable("KAGE_STT_LANGUAGE", "en", "User")
[Environment]::SetEnvironmentVariable("KAGE_SILENCE_AFTER_SPEECH", "0.7", "User")

Get-CimInstance Win32_Process |
    Where-Object {
        $_.Name -match "nemo-speech" -and
        $_.CommandLine -match "serve" -and
        $_.CommandLine -match "nemotron-3.5"
    } |
    ForEach-Object {
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }

Write-Host "STT rollback configured."
Write-Host "Restart Kage Voice to restore Whisper small / CPU INT8 / English / 0.7 s endpoint silence."
Write-Host "Git main was never modified by the experiment."
