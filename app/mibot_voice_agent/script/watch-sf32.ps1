$port = [System.IO.Ports.SerialPort]::new(
  "COM5",
  1000000,
  [System.IO.Ports.Parity]::None,
  8,
  [System.IO.Ports.StopBits]::One
)

$port.Handshake = [System.IO.Ports.Handshake]::None
$port.DtrEnable = $false
$port.RtsEnable = $false

try {
  $port.Open()
  Write-Host "COM5 opened. Press board Reset, then type NSH commands."
  Write-Host "Press Ctrl+C to exit."

  while ($true) {
    if ($port.BytesToRead -gt 0) {
      $count = $port.BytesToRead
      $bytes = New-Object byte[] $count
      $read = $port.Read($bytes, 0, $count)
      [Console]::Write(
        [System.Text.Encoding]::ASCII.GetString($bytes, 0, $read)
      )
    }

    if ([Console]::KeyAvailable) {
      $key = [Console]::ReadKey($true)

      if ($key.Key -eq [ConsoleKey]::Enter) {
        $port.Write("`r`n")
      }
      elseif ($key.Key -eq [ConsoleKey]::Backspace) {
        $port.Write(([char]8).ToString())
      }
      else {
        $port.Write($key.KeyChar.ToString())
      }
    }

    Start-Sleep -Milliseconds 20
  }
}
finally {
  if ($port.IsOpen) {
    $port.RtsEnable = $false
    $port.DtrEnable = $false
    $port.Close()
  }
  $port.Dispose()
}