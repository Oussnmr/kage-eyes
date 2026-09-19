param(
    [Parameter(Mandatory = $true)]
    [string]$Port,

    [Parameter(Mandatory = $true)]
    [string]$Ssid
)

$ErrorActionPreference = "Stop"

$secure = Read-Host "Mot de passe Wi-Fi pour '$Ssid'" -AsSecureString
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)

try {
    $password = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)

    $ssidBytes = [System.Text.Encoding]::UTF8.GetBytes($Ssid)
    $passBytes = [System.Text.Encoding]::UTF8.GetBytes($password)

    if ($ssidBytes.Length -lt 1 -or $ssidBytes.Length -gt 32) {
        throw "Le SSID doit contenir entre 1 et 32 octets UTF-8."
    }
    if ($passBytes.Length -gt 64) {
        throw "Le mot de passe Wi-Fi dépasse 64 octets UTF-8."
    }

    # Improv RPC payload:
    # command=0x01, payload_len, ssid_len, ssid, pass_len, password
    $rpc = [System.Collections.Generic.List[byte]]::new()
    $rpc.Add([byte]0x01)
    $rpc.Add([byte](2 + $ssidBytes.Length + $passBytes.Length))
    $rpc.Add([byte]$ssidBytes.Length)
    $rpc.AddRange([byte[]]$ssidBytes)
    $rpc.Add([byte]$passBytes.Length)
    $rpc.AddRange([byte[]]$passBytes)

    # Improv Serial frame:
    # "IMPROV", version=1, type=RPC(3), data_len, data..., checksum
    $packet = [System.Collections.Generic.List[byte]]::new()
    $packet.AddRange([byte[]][System.Text.Encoding]::ASCII.GetBytes("IMPROV"))
    $packet.Add([byte]0x01)
    $packet.Add([byte]0x03)
    $packet.Add([byte]$rpc.Count)
    $packet.AddRange([byte[]]$rpc.ToArray())

    $sum = 0
    foreach ($value in $packet) {
        $sum = ($sum + [int]$value) -band 0xFF
    }
    $packet.Add([byte]$sum)

    $serial = [System.IO.Ports.SerialPort]::new(
        $Port,
        115200,
        [System.IO.Ports.Parity]::None,
        8,
        [System.IO.Ports.StopBits]::One
    )
    $serial.ReadTimeout = 250
    $serial.WriteTimeout = 2000

    try {
        $serial.Open()
        Start-Sleep -Milliseconds 300
        $serial.DiscardInBuffer()
        $serial.Write($packet.ToArray(), 0, $packet.Count)

        $received = [System.Collections.Generic.List[byte]]::new()
        $deadline = (Get-Date).AddSeconds(5)

        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 100
            $available = $serial.BytesToRead
            if ($available -gt 0) {
                $buffer = New-Object byte[] $available
                $count = $serial.Read($buffer, 0, $buffer.Length)
                if ($count -gt 0) {
                    $received.AddRange([byte[]]$buffer[0..($count - 1)])
                }
            }
        }

        # Look for valid Improv packets and confirm either PROVISIONED state (0x04)
        # or a successful empty Wi-Fi-settings RPC response.
        $bytes = $received.ToArray()
        $provisioned = $false
        $rpcAck = $false
        $errorCode = $null

        for ($i = 0; $i -le $bytes.Length - 10; $i++) {
            if ($bytes[$i] -ne 0x49 -or $bytes[$i+1] -ne 0x4D -or
                $bytes[$i+2] -ne 0x50 -or $bytes[$i+3] -ne 0x52 -or
                $bytes[$i+4] -ne 0x4F -or $bytes[$i+5] -ne 0x56) {
                continue
            }

            $type = $bytes[$i+7]
            $len = [int]$bytes[$i+8]
            $end = $i + 9 + $len
            if ($end -ge $bytes.Length) { continue }

            $check = 0
            for ($j = $i; $j -lt $end; $j++) {
                $check = ($check + [int]$bytes[$j]) -band 0xFF
            }
            if ([byte]$check -ne $bytes[$end]) { continue }

            if ($type -eq 0x01 -and $len -ge 1 -and $bytes[$i+9] -eq 0x04) {
                $provisioned = $true
            }
            elseif ($type -eq 0x02 -and $len -ge 1) {
                $errorCode = [int]$bytes[$i+9]
            }
            elseif ($type -eq 0x04 -and $len -ge 2 -and
                    $bytes[$i+9] -eq 0x01 -and $bytes[$i+10] -eq 0x00) {
                $rpcAck = $true
            }
        }

        if ($errorCode -ne $null -and $errorCode -ne 0) {
            throw "Kage a renvoyé l'erreur Improv 0x$('{0:X2}' -f $errorCode)."
        }

        if ($provisioned -or $rpcAck) {
            Write-Host "OK: profil Wi-Fi '$Ssid' enregistré dans Kage."
        }
        else {
            Write-Host "La commande a été envoyée, mais aucune confirmation Improv n'a été reçue."
            Write-Host "Ne coupe pas encore le Wi-Fi maison; vérifie les logs Wi-Fi de Kage."
        }
    }
    finally {
        if ($serial -and $serial.IsOpen) { $serial.Close() }
        if ($serial) { $serial.Dispose() }
    }
}
finally {
    if ($bstr -ne [IntPtr]::Zero) {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    }
    Remove-Variable password -ErrorAction SilentlyContinue
}
