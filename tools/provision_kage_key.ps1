param(
    [string]$Key = $env:KAGE_API_KEY,
    [string]$Port
)

$ErrorActionPreference = "Stop"

if ($Key -notmatch '^[0-9a-fA-F]{64}$') {
    throw "KAGE_API_KEY doit contenir exactement 64 caractères hexadécimaux."
}

if (-not $Port) {
    $devices = Get-CimInstance Win32_PnPEntity |
        Where-Object {
            $_.Name -match '\\(COM\\d+\\)' -and
            $_.Name -match 'JTAG|Espressif|USB Serial|serial debug'
        }

    $ports = @(
        $devices | ForEach-Object {
            if ($_.Name -match '\\((COM\\d+)\\)') { $Matches[1] }
        } | Select-Object -Unique
    )

    if ($ports.Count -eq 0) {
        throw "Port USB de Kage introuvable. Passe -Port COMx explicitement."
    }
    if ($ports.Count -gt 1) {
        throw "Plusieurs ports possibles: $($ports -join ', '). Relance avec -Port COMx."
    }
    $Port = $ports[0]
}

Write-Host "Kage: envoi de la clé sur $Port (la clé ne sera pas affichée)."

$serial = [System.IO.Ports.SerialPort]::new(
    $Port,
    115200,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)
$serial.ReadTimeout = 500
$serial.WriteTimeout = 2000

try {
    $serial.Open()
    Start-Sleep -Milliseconds 250
    $serial.DiscardInBuffer()
    $serial.Write("KAGEKEY:$Key" + [Environment]::NewLine)

    $response = ""
    $deadline = (Get-Date).AddSeconds(4)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $response += $serial.ReadExisting()
        if ($response -match 'KAGEKEY:OK') {
            Write-Host "OK: clé enregistrée localement dans Kage."
            exit 0
        }
        if ($response -match 'KAGEKEY:ERROR') {
            throw "Kage a refusé la clé."
        }
    }

    throw "Pas de confirmation de Kage. Vérifie le port COM et ferme tout moniteur série."
}
finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}
