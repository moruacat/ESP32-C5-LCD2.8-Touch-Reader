# ============================================================================
# 下载 GitHub Actions 里最新一次成功构建的固件，可顺带烧录
#
# 用法（项目根目录）：
#   $env:GITHUB_TOKEN = "ghp_xxx"          # 只需一次，当前终端有效
#   .\tools\fetch_ci_firmware.ps1          下载到 build\ci\build\...
#   .\tools\fetch_ci_firmware.ps1 -Flash   下载后直接烧录（默认 COM20）
#   .\tools\fetch_ci_firmware.ps1 -Flash -Port COM7
#   .\tools\fetch_ci_firmware.ps1 -RunId 123456789  指定某次 run
#
# Token 说明：
#   GitHub 的 artifact 下载接口即使公开仓库也要认证。
#   生成：GitHub → Settings → Developer settings → Personal access tokens，
#   勾 public_repo（或细粒度 token 的 Actions: Read）。
#
# 为什么快：
#   artifact 只能整包下载，GitHub 单连接会限速。这里按 HTTP Range
#   把 zip 切成 8 段并行拉取，再按顺序拼回去。
# ============================================================================

param(
    [string]$Token  = $env:GITHUB_TOKEN,
    [string]$Repo   = "moruacat/ESP32-C5-LCD2.8-Touch-Reader",
    [string]$Branch = "main",
    [long]  $RunId  = 0,
    [string]$OutDir = "",
    [int]   $Threads = 8,
    [switch]$Flash,
    [string]$Port   = "COM20"
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

if (-not $OutDir) {
    $OutDir = Join-Path $PSScriptRoot "..\build\ci"
}
$OutDir = [IO.Path]::GetFullPath($OutDir)

if (-not $Token) {
    Write-Host "缺少 GitHub token。" -ForegroundColor Red
    Write-Host '先执行：$env:GITHUB_TOKEN = "ghp_xxx"' -ForegroundColor Yellow
    exit 1
}

$headers = @{
    Authorization = "Bearer $Token"
    Accept        = "application/vnd.github+json"
}

# ---------- 1. 选 run ----------
if ($RunId -eq 0) {
    Write-Host "查询 $Repo 最近的成功构建 ..." -ForegroundColor Cyan
    $runs = Invoke-RestMethod -Headers $headers -Uri `
        "https://api.github.com/repos/$Repo/actions/runs?branch=$Branch&per_page=20"
    $run = $runs.workflow_runs | Where-Object { $_.conclusion -eq "success" } | Select-Object -First 1
    if (-not $run) {
        Write-Host "没找到成功的构建，去 Actions 页面看看是不是还在跑或者失败了。" -ForegroundColor Red
        exit 1
    }
} else {
    $run = Invoke-RestMethod -Headers $headers -Uri "https://api.github.com/repos/$Repo/actions/runs/$RunId"
}

Write-Host ("选中 run {0}  {1}  {2}" -f $run.id, $run.head_sha.Substring(0, 7), $run.created_at)

# ---------- 2. 找 firmware artifact ----------
$arts = Invoke-RestMethod -Headers $headers -Uri `
    "https://api.github.com/repos/$Repo/actions/runs/$($run.id)/artifacts"
$art = $arts.artifacts | Where-Object { $_.name -eq "firmware" } | Select-Object -First 1
if (-not $art) {
    Write-Host "这次 run 没有 firmware 产物（可能在编译阶段就失败了）。" -ForegroundColor Red
    exit 1
}
if ($art.expired) {
    Write-Host "产物已过期（GitHub 默认只保留 90 天）。" -ForegroundColor Red
    exit 1
}

# ---------- 3. 拿到最终下载地址 ----------
Add-Type -AssemblyName System.Net.Http

$handler = New-Object System.Net.Http.HttpClientHandler
$handler.AllowAutoRedirect = $true
$client = New-Object System.Net.Http.HttpClient($handler)
$client.Timeout = [TimeSpan]::FromMinutes(15)
$client.DefaultRequestHeaders.Authorization =
    (New-Object System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", $Token))
$client.DefaultRequestHeaders.UserAgent.ParseAdd("fetch-ci-firmware")

$zipApi = "https://api.github.com/repos/$Repo/actions/artifacts/$($art.id)/zip"
Write-Host "解析下载地址 ..." -ForegroundColor Cyan
$resp = $client.GetAsync($zipApi, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
$resp.EnsureSuccessStatusCode()

$finalUrl = $resp.RequestMessage.RequestUri.AbsoluteUri
$total    = $resp.Content.Headers.ContentLength
$resp.Dispose()
if (-not $total -or $total -le 0) {
    Write-Host "拿不到 artifact 大小，改走单连接下载。" -ForegroundColor Yellow
}

# ---------- 4. 分段并行下载 ----------
$tmp = Join-Path $env:TEMP ("ci_fw_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp | Out-Null
$zipPath = Join-Path $tmp "firmware.zip"

try {
    if (-not $total -or $total -le 0) {
        # 兜底：整包下载
        $bytes = $client.GetByteArrayAsync($finalUrl).GetAwaiter().GetResult()
        [IO.File]::WriteAllBytes($zipPath, $bytes)
    } else {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        Write-Host ("开始下载 {0:N0} 字节，{1} 线程 ..." -f $total, $Threads) -ForegroundColor Cyan

        $chunk = [long][Math]::Ceiling($total / $Threads)
        $pool  = [runspacefactory]::CreateRunspacePool(1, $Threads)
        $pool.Open()
        $jobs = @()

        for ($i = 0; $i -lt $Threads; $i++) {
            $start = [long]$i * $chunk
            if ($start -ge $total) { break }
            $end  = [Math]::Min($start + $chunk - 1, $total - 1)
            $part = Join-Path $tmp ("part_{0:D2}" -f $i)

            $ps = [powershell]::Create()
            $ps.RunspacePool = $pool
            $null = $ps.AddScript({
                param($url, $start, $end, $path)
                $c = New-Object System.Net.Http.HttpClient
                $c.Timeout = [TimeSpan]::FromMinutes(15)
                $req = New-Object System.Net.Http.HttpRequestMessage("Get", $url)
                $req.Headers.Range =
                    (New-Object System.Net.Http.Headers.RangeHeaderValue($start, $end))
                $r = $c.SendAsync($req).GetAwaiter().GetResult()
                if ([int]$r.StatusCode -ne 206) {
                    throw ("服务器不支持分段（HTTP {0}）" -f [int]$r.StatusCode)
                }
                $s = $r.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
                $fs = [IO.File]::Create($path)
                try { $s.CopyTo($fs) } finally { $fs.Dispose(); $s.Dispose(); $c.Dispose() }
            }).AddArgument($finalUrl).AddArgument($start).AddArgument($end).AddArgument($part)

            $jobs += [pscustomobject]@{ Ps = $ps; Handle = $ps.BeginInvoke(); Part = $part }
        }

        foreach ($j in $jobs) {
            $j.Ps.EndInvoke($j.Handle)
            $j.Ps.Dispose()
        }
        $pool.Close()
        $pool.Dispose()

        # 按顺序拼接
        $out = [IO.File]::Create($zipPath)
        try {
            foreach ($j in ($jobs | Sort-Object { $_.Part })) {
                $fs = [IO.File]::OpenRead($j.Part)
                try { $fs.CopyTo($out) } finally { $fs.Dispose() }
            }
        } finally {
            $out.Dispose()
        }

        $sw.Stop()
        $speed = $total / 1MB / $sw.Elapsed.TotalSeconds
        Write-Host ("下载完成，{0:N1} 秒，{1:N1} MB/s" -f $sw.Elapsed.TotalSeconds, $speed) -ForegroundColor Green
    }

    # ---------- 5. 解压 ----------
    if (Test-Path $OutDir) {
        Remove-Item $OutDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $OutDir | Out-Null
    Expand-Archive -Path $zipPath -DestinationPath $OutDir -Force

    Write-Host "固件已解压到 $OutDir" -ForegroundColor Green
    Get-ChildItem $OutDir -Recurse -Filter *.bin |
        ForEach-Object { "  {0,-42} {1,10:N0} 字节" -f $_.FullName.Substring($OutDir.Length + 1), $_.Length }
}
finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    $client.Dispose()
}

# ---------- 6. 可选：直接烧录 ----------
if ($Flash) {
    $build = Join-Path $OutDir "build"
    $app   = Join-Path $build "clock_reader.bin"
    $boot  = Join-Path $build "bootloader\bootloader.bin"
    $part  = Join-Path $build "partition_table\partition-table.bin"
    $ota   = Join-Path $build "ota_data_initial.bin"

    if (-not (Test-Path $app)) {
        Write-Host "解压后没找到 clock_reader.bin，检查 artifact 内容。" -ForegroundColor Red
        exit 1
    }

    $flashArgs = @(
        "--chip", "esp32c5", "-p", $Port, "-b", "460800",
        "--before", "default-reset", "--after", "hard-reset",
        "write-flash", "--flash-mode", "dio", "--flash-size", "32MB", "--flash-freq", "80m"
    )
    if (Test-Path $boot) { $flashArgs += @("0x2000", $boot) }
    if (Test-Path $part) { $flashArgs += @("0x8000", $part) }
    if (Test-Path $ota)  { $flashArgs += @("0xf000", $ota) }
    $flashArgs += @("0x20000", $app)

    Write-Host "烧录到 $Port ..." -ForegroundColor Cyan
    python -m esptool @flashArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "烧录失败" -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "烧录完成。" -ForegroundColor Green
}
