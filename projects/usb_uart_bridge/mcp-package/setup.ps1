# 一键安装 usb-uart-bridge MCP（Windows PowerShell）
# 用法: .\setup.ps1 [-Port COM23] [-Install]
#   -Port    指定默认串口（写入生成的 mcp-fragment.json）
#   -Install 同时 pip install .（提供 usb-uart-bridge-mcp 命令）
param(
    [string]$Port = "",
    [switch]$Install
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$pkg = Join-Path $here "usb_uart_bridge_mcp\server.py"

Write-Host "== usb-uart-bridge-mcp 安装检查 ==" -ForegroundColor Cyan

# 1. Python
$py = (Get-Command python -ErrorAction SilentlyContinue)
if (-not $py) { throw "未找到 python，请先安装 Python 3.8+ 并加入 PATH" }
Write-Host "[1/3] Python: $((& python --version) -join ' ')"

# 2. pyserial
& python -c "import serial" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "[2/3] 安装 pyserial ..." -ForegroundColor Yellow
    & python -m pip install pyserial
    if ($LASTEXITCODE -ne 0) { throw "pyserial 安装失败" }
} else {
    Write-Host "[2/3] pyserial 已安装"
}

# 3. 自检（可选，无设备时跳过）
Write-Host "[3/3] 设备自检 ..."
$args = @($pkg, "--selftest")
if ($Port) { $args += @("--port", $Port) }
& python @args
if ($LASTEXITCODE -ne 0) {
    Write-Warning "自检未通过（可能设备未连接）。可稍后用: python `"$pkg`" --port COMxx --selftest"
}

# 生成 mcp 配置片段
$frag = Join-Path $here "mcp-fragment.json"
$entryArgs = @($pkg)
if ($Port) { $entryArgs += @("--port", $Port) }
$json = [ordered]@{
    mcpServers = [ordered]@{
        "usb-uart-bridge" = [ordered]@{
            command = "python"
            args    = $entryArgs
        }
    }
}
$json | ConvertTo-Json -Depth 10 |
    Set-Content -Path $frag -Encoding UTF8
Write-Host "已生成配置片段: $frag" -ForegroundColor Green
Write-Host "把其中内容合并到你的 MCP 客户端配置（如 ~/.cursor/mcp.json）即可。"

if ($Install) {
    Write-Host "pip install . ..." -ForegroundColor Yellow
    Push-Location $here
    try { & python -m pip install . } finally { Pop-Location }
    Write-Host "之后可用命令: usb-uart-bridge-mcp --port COMxx" -ForegroundColor Green
}

Write-Host "完成。" -ForegroundColor Green
