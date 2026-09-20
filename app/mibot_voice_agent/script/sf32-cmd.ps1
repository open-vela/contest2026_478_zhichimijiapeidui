param(
  [string]$Port = "COM5",
  [int]$Baud = 1000000,
  [string[]]$Commands = @(),
  [int]$ReadMs = 3000,
  [int]$PostCmdMs = 1500
)

# Non-interactive SF32 NSH helper: open the port, optionally send a series of
# commands, capture everything the board prints for a bounded window, then
# close.  Used to script the voice-agent bring-up test.

Add-Type -AssemblyName System.IO.Ports -ErrorAction SilentlyContinue

$port = [System.IO.Ports.SerialPort]::new($Port, $Baud,
  [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
Write-Host ("port type: " + $port.GetType().FullName)
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.DtrEnable = $false
$port.RtsEnable = $false
$port.ReadTimeout = 200
$port.WriteTimeout = 500

function Drain([int]$ms) {
  $deadline = (Get-Date).AddMilliseconds($ms)
  while ((Get-Date) -lt $deadline) {
    if ($port.BytesToRead -gt 0) {
      $count = $port.BytesToRead
      $bytes = New-Object byte[] $count
      $read = $port.Read($bytes, 0, $count)
      [Console]::Write([System.Text.Encoding]::ASCII.GetString($bytes, 0, $read))
    } else {
      Start-Sleep -Milliseconds 20
    }
  }
}

try {
  $port.Open()
  Write-Host "=== $Port opened @ $Baud ==="

  # First, drain any pending output (banner / prior state).
  Drain $ReadMs

  foreach ($c in $Commands) {
    Write-Host ""
    Write-Host ">>> $c"
    $port.Write("$c`r`n")
    Drain $PostCmdMs
  }

  Write-Host ""
  Write-Host "=== done ==="
}
finally {
  if ($port.IsOpen) { $port.Close() }
  $port.Dispose()
}
