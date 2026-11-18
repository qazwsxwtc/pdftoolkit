param(
    [string]$Root = 'D:\media\pdfword',
    [string]$Version = $null,
    [switch]$SkipPortable
)

# 一条命令出两个东西：
#   1) dist\Pdf-Toolkit-<ver>-win64-portable.zip   便携版（解压即用）
#   2) dist\Pdf-Toolkit-<ver>-win64-setup.exe      安装包（Inno Setup 编译 packaging\pdftoolkit.iss）
#
# 依赖：publish\ 已由 windeployqt 部署过；本机装有 Inno Setup 6（ISCC.exe）。

$ErrorActionPreference = 'Stop'
$src = Join-Path $Root 'publish'
$exe = Join-Path $src 'pdftoolkit.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    throw "找不到 $exe —— 先构建 pdfword_ui，再 windeployqt 部署到 publish\"
}

# 版本号默认取 CMakeLists 的 project(VERSION)，保证安装包与「关于」窗口一致
if (-not $Version) {
    $cmake = Get-Content -LiteralPath (Join-Path $Root 'CMakeLists.txt') -Raw
    if ($cmake -match 'project\s*\(\s*Pdf-Toolkit\s+VERSION\s+([0-9.]+)') { $Version = $Matches[1] }
    else { $Version = '0.0.1' }
}
Write-Host "版本: $Version" -ForegroundColor Cyan

$dist = Join-Path $Root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null

# ---------- 0) 干净的运行目录：主程序 + 运行库，不含 CLI / DLL / 测试残留 ----------
$appStage = Join-Path $Root 'packaging\_app'
if (Test-Path -LiteralPath $appStage) { Remove-Item -LiteralPath $appStage -Recurse -Force }
Write-Host "收集运行文件 ..."
$exclude = @('pdfword_cli.exe', 'pdfword.dll', 'pw_ctx_check.txt')
Get-ChildItem -LiteralPath $src -Recurse -File | Where-Object { $exclude -notcontains $_.Name } | ForEach-Object {
    $rel = $_.FullName.Substring($src.Length).TrimStart('\')
    $dst = Join-Path $appStage $rel
    $dir = Split-Path -Parent $dst
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Copy-Item -LiteralPath $_.FullName -Destination $dst -Force
}
$appMb = [math]::Round(((Get-ChildItem -LiteralPath $appStage -Recurse -File | Measure-Object Length -Sum).Sum) / 1MB, 1)
Write-Host "  $appStage  ($appMb MB)"

# ---------- 1) 便携 zip ----------
$zip = Join-Path $dist "Pdf-Toolkit-$Version-win64-portable.zip"
if (-not $SkipPortable) {
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Write-Host "打包便携版 ..."
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $appStage, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
    Write-Host ("  {0}  ({1} MB)" -f $zip, [math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 1))
}

# ---------- 2) Inno Setup 编译安装包 ----------
$isccCandidates = @(
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    'C:\Program Files\Inno Setup 6\ISCC.exe',
    'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
)
$iscc = $isccCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $iscc) { throw "没找到 ISCC.exe —— 先安装 Inno Setup 6" }

$iss = Join-Path $Root 'packaging\pdftoolkit.iss'
$setup = Join-Path $dist "Pdf-Toolkit-$Version-win64-setup.exe"
if (Test-Path -LiteralPath $setup) { Remove-Item -LiteralPath $setup -Force }

Write-Host "用 Inno Setup 编译安装包（ISCC: $iscc）..."
& $iscc "/DMyAppVersion=$Version" "$iss"
if ($LASTEXITCODE -ne 0) { throw "ISCC 返回 $LASTEXITCODE，见上面的编译日志" }

if (-not (Test-Path -LiteralPath $setup)) { throw "安装包没生成：$setup" }
Write-Host ""
Write-Host "完成：" -ForegroundColor Green
if (-not $SkipPortable) {
    Write-Host ("  便携版  {0}  ({1} MB)" -f $zip, [math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 1))
}
Write-Host ("  安装包  {0}  ({1} MB)" -f $setup, [math]::Round((Get-Item -LiteralPath $setup).Length / 1MB, 1))
