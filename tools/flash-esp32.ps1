# 烧录 ESP32-S3 固件（bootloader + 分区表 + 应用）。
#
# 只需要 esptool，不需要装完整 ESP-IDF:
#   pip install esptool
#
# 用法:
#   .\flash-esp32.ps1 -Port COM7

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    [string]$FirmwareDir = (Join-Path $PSScriptRoot "..\firmware\esp32"),
    [int]$Baud = 460800
)

$ErrorActionPreference = "Stop"

$dir = (Resolve-Path -LiteralPath $FirmwareDir).Path
$images = [ordered]@{
    "0x0"     = Join-Path $dir "bootloader.bin"
    "0x8000"  = Join-Path $dir "partition-table.bin"
    "0x10000" = Join-Path $dir "mibot_esp32s3.bin"
}
foreach ($entry in $images.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value)) { throw "固件缺失: $($entry.Value)" }
}

# 找一个带 esptool 的解释器。优先当前 PATH 上的 python；找不到就试 ESP-IDF
# 自带的虚拟环境（装了 IDF 的话 esptool 一定在里面）。
function Test-Esptool([string]$Exe) {
    if (-not $Exe) { return $false }
    if (($Exe -ne "python") -and (-not (Test-Path -LiteralPath $Exe))) { return $false }
    # 探测失败是正常分支。脚本顶部设了 ErrorActionPreference=Stop，那会把原生命令
    # 的 stderr 变成终止错误，所以这里临时降级，否则"没装 esptool"会直接炸掉脚本。
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    try {
        & $Exe -m esptool version 2>&1 | Out-Null
        return ($LASTEXITCODE -eq 0)
    } catch {
        return $false
    } finally {
        $ErrorActionPreference = $previous
    }
}

$candidates = @("python")
if ($env:IDF_PYTHON_ENV_PATH) {
    $candidates += (Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe")
}
$candidates += Get-ChildItem "C:\Espressif\tools\python_env\*\Scripts\python.exe" -ErrorAction SilentlyContinue |
    ForEach-Object { $_.FullName }

$python = $null
foreach ($candidate in $candidates) {
    if (Test-Esptool $candidate) { $python = $candidate; break }
}
if (-not $python) {
    Write-Host "找不到 esptool。任选一种装法:" -ForegroundColor Yellow
    Write-Host "  python -m pip install esptool        # 只烧录，不需要完整 ESP-IDF"
    Write-Host "  或在 ESP-IDF 环境里运行本脚本"
    exit 1
}
Write-Host ("esptool 解释器: {0}" -f $python)

Write-Host ("端口: {0} @ {1}" -f $Port, $Baud)
foreach ($entry in $images.GetEnumerator()) {
    $item = Get-Item -LiteralPath $entry.Value
    Write-Host ("  {0,-9} {1,-22} {2,9:N0} 字节" -f $entry.Key, $item.Name, $item.Length)
}
Write-Host ""

# 显式指定端口，避免 esptool 自动探测选错板子。
$esptoolArgs = @(
    "-m", "esptool", "--chip", "esp32s3", "-p", $Port, "-b", "$Baud",
    "--before", "default-reset", "--after", "hard-reset",
    "write-flash", "--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "80m"
)
foreach ($entry in $images.GetEnumerator()) {
    $esptoolArgs += $entry.Key
    $esptoolArgs += $entry.Value
}

# esptool 把进度和错误都写 stderr。ErrorActionPreference=Stop 会把它变成终止错误，
# 那样下面的排障提示就永远打不出来，所以这一段降级为 Continue，靠退出码判断成败。
$previous = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    # stderr 行会以 ErrorRecord 形式过来，直接 Write-Host 会打成类型名，取 ToString()。
    & $python @esptoolArgs 2>&1 | ForEach-Object { Write-Host $_.ToString() }
    $code = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previous
}

Write-Host ""
if ($code -ne 0) {
    Write-Warning "esptool 返回码 $code。"
    Write-Warning "连不上就按住 BOOT 键再上电进下载模式；或确认 $Port 没被串口终端占着。"
    exit $code
}
Write-Host "烧录完成。"
