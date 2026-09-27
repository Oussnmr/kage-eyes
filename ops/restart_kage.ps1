$ErrorActionPreference = 'SilentlyContinue'

# Stop only Kage voice processes, never every Python process on the PC.
Get-CimInstance Win32_Process |
    Where-Object { $_.CommandLine -and $_.CommandLine -match '(?i)[\\/]Kage[\\/]voice\.py' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force }

Start-Sleep -Milliseconds 500
$host.UI.RawUI.WindowTitle = 'Kage Voice'
$env:KAGE_TTS_VOICE = 'ff_siwis'
$env:KAGE_NEMO_URL = 'http://127.0.0.1:18080'
$env:KAGE_SILENCE_AFTER_SPEECH = '1.0'
$kageSitePackages = 'C:\Kage\venv\Lib\site-packages'
$env:PYTHONPATH = "$kageSitePackages;$kageSitePackages\win32;$kageSitePackages\win32\lib;$kageSitePackages\pywin32_system32;C:\Kage"
$env:PATH = "$kageSitePackages\pywin32_system32;$env:PATH"
$kagePython = 'C:\Users\Oussama\AppData\Local\Programs\Python\Python312\python.exe'

# The venv launcher currently points at an unusable base executable. Reuse
# its installed packages with the working system interpreter instead of
# reinstalling or replacing the environment.
if (-not (Get-NetTCPConnection -State Listen -LocalPort 8000 -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath $kagePython -ArgumentList 'C:\Kage\run_backend.py' `
        -WorkingDirectory 'C:\Kage' -WindowStyle Hidden
}

# NeMo-Speech is optional: start it when the dedicated port is free, then
# let the client fall back to Whisper if model warmup or the service fails.
$nemoExe = 'C:\Users\Oussama\AppData\Local\Programs\NeMoSpeech\bin\nemo-speech.exe'
$nemoReady = Get-NetTCPConnection -State Listen -LocalPort 18080 -ErrorAction SilentlyContinue
if (-not $nemoReady -and (Test-Path $nemoExe)) {
    Start-Process -FilePath $nemoExe -ArgumentList @(
        'serve', '--asr-model', 'nemotron-3.5', '--host', '127.0.0.1',
        '--port', '18080', '--threads', '2', '--backend', 'cpu',
        '--access-log', '--log-format', 'json', '--no-ui'
    ) -WorkingDirectory 'C:\Kage' -WindowStyle Hidden `
        -RedirectStandardOutput 'C:\Kage\stt_diagnostics\nemotron_18080.out.log' `
        -RedirectStandardError 'C:\Kage\stt_diagnostics\nemotron_18080.err.log'
}
& $kagePython 'C:\Kage\voice.py'
