# 打包 usb-uart-bridge MCP 分发 zip（Windows PowerShell）
# 用法: .\make-package.ps1 [-Version 1.1.0]
param([string]$Version = "1.1.0")

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$proj = Split-Path $here -Parent
$src  = Join-Path $proj "mcp-package"
$zip  = Join-Path $proj "usb-uart-bridge-mcp-$Version.zip"

# 同步仓库内工具到包（单一真源：包内 server.py）
$tool = Join-Path $proj "tools\mcp_server.py"
if (Test-Path $tool) {
    $content = Get-Content $tool -Raw
    if ($content -notmatch "usb_uart_bridge_mcp\.server") {
        Write-Warning "$tool 不是薄封装，跳过同步"
    }
}

# 清理构建残留
Get-ChildItem $src -Recurse -Directory -Filter "__pycache__" -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force
foreach ($d in @("build", "dist", "usb_uart_bridge_mcp.egg-info", "*.egg-info")) {
    Get-ChildItem $src -Path $d -ErrorAction SilentlyContinue |
        Remove-Item -Recurse -Force
}

# 校验
& python -m py_compile (Join-Path $src "usb_uart_bridge_mcp\server.py")
if ($LASTEXITCODE -ne 0) { throw "server.py 语法检查失败" }

# 清理校验产生的 __pycache__
Get-ChildItem $src -Recurse -Directory -Filter "__pycache__" -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force

# 重新打包
if (Test-Path $zip) {
    [System.GC]::Collect(); Start-Sleep -Milliseconds 300
    Remove-Item $zip -Force
}
Compress-Archive -Path (Join-Path $src "*") -DestinationPath $zip -Force

$item = Get-Item $zip
Write-Host ("已生成: {0} ({1:N0} bytes)" -f $item.FullName, $item.Length) -ForegroundColor Green
Write-Host "分发方式：把该 zip 拷到目标机器解压，运行 .\setup.ps1 -Port COMxx"
