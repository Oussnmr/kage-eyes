param(
    [switch]$SkipInstall,
    [switch]$SkipPull,
    [int]$Port = 8080
)

$ErrorActionPreference = "Stop"
$diagDir = "C:\Kage\stt_diagnostics"
New-Item -ItemType Directory -Force -Path $diagDir | Out-Null

$cpu = Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed
$ram = Get-CimInstance Win32_ComputerSystem | Select-Object TotalPhysicalMemory
$hardware = [ordered]@{
    timestamp = (Get-Date).ToString("o")
    cpu = $cpu
    total_ram_gb = [math]::Round($ram.TotalPhysicalMemory / 1GB, 2)
    os = (Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber)
}
$hardware | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "$diagDir\hardware.json"

if (-not $SkipInstall) {
    $installer = Join-Path $env:TEMP "install-nemo-speech.ps1"
    Invoke-WebRequest -Uri "https://raw.githubusercontent.com/NVIDIA/NeMo-Speech.cpp/main/scripts/install.ps1" -OutFile $installer
    powershell -ExecutionPolicy Bypass -File $installer
}

$defaultInstall = Join-Path $env:LOCALAPPDATA "Programs\NeMoSpeech\bin"
if (Test-Path $defaultInstall) {
    $env:Path = "$defaultInstall;$env:Path"
}

$nemo = Get-Command nemo-speech -ErrorAction Stop
& $nemo.Source --version | Tee-Object -FilePath "$diagDir\nemo_version.txt"
& $nemo.Source doctor --json | Tee-Object -FilePath "$diagDir\nemo_doctor.json"

if (-not $SkipPull) {
    & $nemo.Source pull nemotron-3.5
}

Get-CimInstance Win32_Process |
    Where-Object {
        $_.Name -match "nemo-speech" -and
        $_.CommandLine -match "serve" -and
        $_.CommandLine -match "nemotron-3.5"
    } |
    ForEach-Object {
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }

$stdout = "$diagDir\nemotron_server.out.log"
$stderr = "$diagDir\nemotron_server.err.log"
$arguments = @(
    "serve",
    "--asr-model", "nemotron-3.5",
    "--device", "cpu",
    "--host", "127.0.0.1",
    "--port", "$Port"
)

$process = Start-Process -FilePath $nemo.Source -ArgumentList $arguments -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru

$readyUrl = "http://127.0.0.1:$Port/ready"
$deadline = (Get-Date).AddMinutes(5)
$ready = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    try {
        $status = Invoke-RestMethod -Uri $readyUrl -TimeoutSec 2
        if ($status.ready -eq $true -or $status.status -eq "ok") {
            $ready = $true
            break
        }
    } catch {
        if ($process.HasExited) {
            throw "Nemotron server exited early. See $stderr"
        }
    }
}

if (-not $ready) {
    throw "Nemotron did not become ready. See $stderr"
}

[ordered]@{
    pid = $process.Id
    url = "http://127.0.0.1:$Port"
    model = "nemotron-3.5"
    device = "cpu"
    started = (Get-Date).ToString("o")
} | ConvertTo-Json | Set-Content -Encoding UTF8 "$diagDir\nemotron_server.json"

Write-Host "Nemotron is ready on http://127.0.0.1:$Port"
Write-Host "Diagnostics: $diagDir"
Write-Host "Rollback: KAGE_STT_ENGINE=whisper; KAGE_STT_LANGUAGE=en; KAGE_SILENCE_AFTER_SPEECH=0.7"
