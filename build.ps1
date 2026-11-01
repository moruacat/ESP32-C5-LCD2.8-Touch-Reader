# ============================================================================
# ESP32-C5 伪装时钟阅读器 —— 一键编译 / 烧录
#
# 用法（在项目根目录）：
#   .\build.ps1                编译 + 完整烧录（bootloader / 分区表 / app / 词库 / 字体）
#   .\build.ps1 -NoFlash       只编译，不烧
#   .\build.ps1 -AppOnly       只烧 app（改代码后最快，不动分区与数据）
#   .\build.ps1 -Monitor       烧完进串口监控（Ctrl+] 退出）
#   .\build.ps1 -FullClean     先 fullclean 再编译（改 sdkconfig 后建议用）
#   .\build.ps1 -Port COM20    指定串口（默认 COM20）
#
# 说明：
#   - idf.py 必须带 --no-ccache（沙箱/杀软下 ccache 会卡死）
#   - 数据分区偏移从 build/partition_table/partition-table.bin 动态解析，
#     分区表改了也不用改脚本
# ============================================================================

param(
    [string]$Port = "COM20",
    [switch]$NoFlash,
    [switch]$AppOnly,
    [switch]$Monitor,
    [switch]$FullClean
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }

# ---------- 1. 导入 ESP-IDF 环境 ----------
$exports = @(
    "C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1",
    "D:\Dev-Kit\ESP-IDF\v6.1\esp-idf\export.ps1",
    "C:\Espressif\frameworks\esp-idf-v6.1\export.ps1"
)
$export = $exports | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $export) {
    Write-Host "找不到 ESP-IDF 的导出脚本，请改脚本顶部的 `$exports 列表。" -ForegroundColor Red
    exit 1
}
Write-Host "IDF 环境: $export" -ForegroundColor DarkGray
. $export

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    Write-Host "导出环境后仍找不到 idf.py，检查 ESP-IDF 安装。" -ForegroundColor Red
    exit 1
}

Push-Location $root
try {
    # ---------- 2. 编译 ----------
    if ($FullClean) {
        Write-Host "fullclean ..." -ForegroundColor Yellow
        idf.py --no-ccache fullclean
    }

    Write-Host "编译中（idf.py --no-ccache build）..." -ForegroundColor Cyan
    idf.py --no-ccache build
    if ($LASTEXITCODE -ne 0) {
        Write-Host "编译失败（退出码 $LASTEXITCODE）" -ForegroundColor Red
        exit $LASTEXITCODE
    }

    $appBin = Join-Path $root "build\clock_reader.bin"
    if (-not (Test-Path $appBin)) {
        Write-Host "没有生成 build\clock_reader.bin，编译可能没真正完成。" -ForegroundColor Red
        exit 1
    }
    $appSize = (Get-Item $appBin).Length
    Write-Host ("编译完成：{0:N0} 字节（{1}）" -f $appSize, (Get-Item $appBin).LastWriteTime) -ForegroundColor Green

    if ($NoFlash) {
        Write-Host "-NoFlash：跳过烧录。" -ForegroundColor Yellow
        return
    }

    # ---------- 3. 烧录 ----------
    if ($AppOnly) {
        # 只烧 app（分区表不变时用）
        Write-Host "只烧 app 到 0x20000 ..." -ForegroundColor Cyan
        python -m esptool --chip esp32c5 -p $Port -b 460800 `
            --before default-reset --after hard-reset `
            write-flash --flash-mode dio --flash-size 32MB --flash-freq 80m `
            0x20000 $appBin
        if ($LASTEXITCODE -ne 0) {
            Write-Host "app 烧录失败" -ForegroundColor Red
            exit $LASTEXITCODE
        }
    } else {
        # 完整烧录：bootloader + 分区表 + app
        Write-Host "完整烧录（bootloader / 分区表 / app）到 $Port ..." -ForegroundColor Cyan
        idf.py -p $Port flash
        if ($LASTEXITCODE -ne 0) {
            Write-Host "烧录失败" -ForegroundColor Red
            exit $LASTEXITCODE
        }

        # 中文输入法已移除，flash 里不再有 dict/font 数据分区；idf.py flash
        # 已经烧完 bootloader / 分区表 / app，这里不需要额外烧任何分区。
    }

    Write-Host "全部完成。" -ForegroundColor Green

    if ($Monitor) {
        Write-Host "进入串口监控，Ctrl+] 退出 ..." -ForegroundColor Cyan
        idf.py -p $Port monitor
    }
}
finally {
    Pop-Location
}
