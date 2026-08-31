# 多子项目统一 CLI 入口（无 GUI，全部命令行，便于 AI/脚本驱动）
# 用法:   .\idf.ps1 <子项目> [idf.py 参数...]
# 示例:   .\idf.ps1 list
#         .\idf.ps1 wifi_sniffer build
#         .\idf.ps1 wifi_sniffer -p COM20 -b 460800 flash monitor
#         .\idf.ps1 flash_dump -p COM20 flash
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$projectsDir = Join-Path $root "projects"

if ($args.Count -eq 0 -or $args[0] -in @("list", "ls", "help", "-h", "--help")) {
    Write-Host "用法: .\idf.ps1 <子项目> [idf.py 参数...]"
    Write-Host "可用子项目:"
    Get-ChildItem $projectsDir -Directory | ForEach-Object {
        $hasProj = Test-Path (Join-Path $_.FullName "CMakeLists.txt")
        Write-Host ("  {0}{1}" -f $_.Name, $(if ($hasProj) { "" } else { "  (缺少 CMakeLists.txt，不可构建)" }))
    }
    exit 0
}

$proj = $args[0]
$projDir = Join-Path $projectsDir $proj
if (-not (Test-Path (Join-Path $projDir "CMakeLists.txt"))) {
    Write-Error "子项目 '$proj' 不存在或缺少 CMakeLists.txt: $projDir（用 .\idf.ps1 list 查看）"
    exit 1
}

# 幂等激活 ESP-IDF 环境（env 变量为进程级，& 调用即可生效）
if (-not $env:IDF_PATH) {
    & "E:\Espressif\esp-idf\export.ps1" *> $null
    if ($LASTEXITCODE -ne 0 -and -not $env:IDF_PATH) {
        Write-Error "ESP-IDF 环境激活失败（E:\Espressif\esp-idf\export.ps1）"
        exit 1
    }
}

$rest = @($args | Select-Object -Skip 1)
idf.py -B (Join-Path $projDir "build") -C $projDir @rest
exit $LASTEXITCODE
