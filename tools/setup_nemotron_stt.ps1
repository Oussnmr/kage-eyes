param(
    [switch]$SkipInstall,
    [switch]$SkipPull,
    [int]$Port = 8080,
    [string]$Version = "0.1.0"
)

$ErrorActionPreference = "Stop"
$diagDir = "C:\Kage\stt_diagnostics"
$installRoot = Join-Path $env:LOCALAPPDATA "Programs\NeMoSpeech"
$nemo = Join-Path $installRoot "bin\nemo-speech.exe"
New-Item -ItemType Directory -Force -Path $diagDir | Out-Null

$cpu = Get-CimInstance Win32_Processor |
    Select-Object Name, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed
$ram = Get-CimInstance Win32_ComputerSystem | Select-Object TotalPhysicalMemory
[ordered]@{
    timestamp = (Get-Date).ToString("o")
    cpu = $cpu
    total_ram_gb = [math]::Round($ram.TotalPhysicalMemory / 1GB, 2)
    os = (Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber)
} | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "$diagDir\hardware.json"

if (-not $SkipInstall) {
    # Pin the reviewed installer to the requested release. BinaryOnly fails
    # safely instead of launching a long, resource-heavy source build.
    $installer = Join-Path $diagDir "install-nemo-speech-$Version.ps1"
    $installerUri = "https://raw.githubusercontent.com/NVIDIA/NeMo-Speech.cpp/v$Version/scripts/install.ps1"
    Invoke-WebRequest -UseBasicParsing -Uri $installerUri -OutFile $installer
    Get-FileHash -Algorithm SHA256 -LiteralPath $installer |
        Format-List | Out-String | Set-Content -Encoding UTF8 "$diagDir\nemo_installer_sha256.txt"
    powershell -ExecutionPolicy Bypass -File $installer `
        -Version $Version -Backend cpu -Profile server -BinaryOnly -NoModifyPath
}

if (-not (Test-Path -LiteralPath $nemo)) {
    throw "NeMo-Speech.cpp binary not found at $nemo"
}

& $nemo --version | Tee-Object -FilePath "$diagDir\nemo_version.txt"
if ($LASTEXITCODE -ne 0) { throw "nemo-speech --version failed ($LASTEXITCODE)" }
& $nemo --json doctor | Tee-Object -FilePath "$diagDir\nemo_doctor.json"
if ($LASTEXITCODE -ne 0) { throw "nemo-speech doctor failed ($LASTEXITCODE)" }
& $nemo --json model list | Tee-Object -FilePath "$diagDir\nemo_models.json"
if ($LASTEXITCODE -ne 0) { throw "nemo-speech model list failed ($LASTEXITCODE)" }

if (-not $SkipPull) {
    & $nemo pull nemotron-3.5
    if ($LASTEXITCODE -ne 0) { throw "Nemotron model pull failed ($LASTEXITCODE)" }
}

# Replace only a previous experiment server using this exact executable/port.
Get-CimInstance Win32_Process |
    Where-Object {
        $_.ExecutablePath -eq $nemo -and
        $_.CommandLine -match "(?i)\bserve\b" -and
        $_.CommandLine -match "(?i)--port\s+$Port(?:\s|$)"
    } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction Stop }

$stdout = "$diagDir\nemotron_server.out.log"
$stderr = "$diagDir\nemotron_server.err.log"
$arguments = @(
    "--json", "serve",
    "--asr-model", "nemotron-3.5",
    "--host", "127.0.0.1",
    "--port", "$Port",
    "--threads", "2",
    "--no-ui",
    "--access-log",
    "--log-format", "json"
)

$process = Start-Process -FilePath $nemo -ArgumentList $arguments `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr `
    -WindowStyle Hidden -PassThru

$readyUrl = "http://127.0.0.1:$Port/ready"
$deadline = (Get-Date).AddMinutes(5)
$ready = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    try {
        $status = Invoke-RestMethod -Uri $readyUrl -TimeoutSec 2
        if ($status.ready -eq $true) {
            $ready = $true
            break
        }
    } catch {
        if ($process.HasExited) { throw "Nemotron server exited early. See $stderr" }
    }
}
if (-not $ready) { throw "Nemotron did not become ready. See $stderr" }

$models = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/v1/models" -TimeoutSec 5
$models | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 "$diagDir\nemotron_loaded_models.json"

[ordered]@{
    pid = $process.Id
    executable = $nemo
    version = $Version
    url = "http://127.0.0.1:$Port"
    model = "nemotron-3.5"
    backend = "cpu"
    http_threads = 2
    started = (Get-Date).ToString("o")
} | ConvertTo-Json | Set-Content -Encoding UTF8 "$diagDir\nemotron_server.json"

Write-Host "Nemotron is ready on http://127.0.0.1:$Port (PID $($process.Id))."
Write-Host "Diagnostics: $diagDir"
