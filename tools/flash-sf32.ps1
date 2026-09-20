# 烧录 SF32 固件。烧录器和固件都在包内，路径自动解析，不需要改任何文件。
#
# 用法:
#   .\flash-sf32.ps1 -Port COM5
#   .\flash-sf32.ps1 -Port COM5 -Bin D:\path\to\your_nuttx.bin

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Port,
    # 默认烧包内的预编译全桩固件。
    [string]$Bin = (Join-Path $PSScriptRoot "..\firmware\sf32\voice_agent_nuttx.bin"),
    # SF32LB52X 的固件起始地址，一般不用改。
    [string]$Address = "0x12010000",
    [int]$Baud = 1000000
)

$ErrorActionPreference = "Stop"

$tool = Join-Path $PSScriptRoot "sf32-flash\ImgDownUart.exe"
if (-not (Test-Path $tool)) { throw "包内烧录器缺失: $tool" }

# ImgDownUart 的烧录清单只认绝对路径，这里按运行时实际位置生成，
# 避免包被解压到别处后清单里的路径失效。
$binPath = (Resolve-Path -LiteralPath $Bin).Path
if (-not (Test-Path -LiteralPath $binPath)) { throw "固件不存在: $binPath" }

$ini = Join-Path ([IO.Path]::GetTempPath()) ("mibot-sf32-burn-{0}.ini" -f [guid]::NewGuid().ToString("N"))
@"
[FILEINFO]
FILE0=$binPath
ADDR0=$Address
NUM=1
"@ | Set-Content -LiteralPath $ini -Encoding ascii

$info = Get-Item -LiteralPath $binPath
Write-Host ("固件: {0} ({1:N0} 字节, {2})" -f $info.Name, $info.Length, $info.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))
Write-Host ("端口: {0} @ {1}" -f $Port, $Baud)
Write-Host ("地址: {0}" -f $Address)
Write-Host ""

try {
    & $tool --port $Port --baund $Baud --device SF32LB52X --file $ini --loadram 1 --postact 1
    $code = $LASTEXITCODE
} finally {
    Remove-Item -LiteralPath $ini -Force -ErrorAction SilentlyContinue
}

Write-Host ""
if ($code -ne 0) {
    Write-Warning "烧录器返回码 $code。看上面日志里有没有 percent:100。"
    Write-Warning "报 DownLoadUart fail 通常是 $Port 被别的程序占着（串口终端、测试脚本都会占）。"
    Write-Warning "查占用: Get-Process python,putty -ErrorAction SilentlyContinue"
    exit $code
}
Write-Host "烧录完成。断电复位一次再跑测试。"
