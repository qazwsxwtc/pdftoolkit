param(
    [ValidateSet("Release", "Debug")][string]$Configuration = "Release",
    [switch]$WithMuPdf,
    [string]$QtRoot = "D:/Qt/6.12.0/msvc2022_64",
    [string]$MuPdfRoot = "",
    [string]$VcpkgToolchain = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cmake = "C:\Program Files\CMake\bin\cmake.exe"
if (-not (Test-Path -LiteralPath $cmake)) { $cmake = "cmake" }

$buildDir = if ($WithMuPdf) { Join-Path $root "build/mupdf" } else { Join-Path $root "build/msvc-x64" }

$cmakeArgs = @(
    "-S", $root,
    "-B", $buildDir,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DCMAKE_PREFIX_PATH=$QtRoot",
    "-DPDFWORD_WITH_MUPDF=$(if ($WithMuPdf) { 'ON' } else { 'OFF' })"
)
if ($MuPdfRoot) { $cmakeArgs += "-DPDFWORD_MUPDF_ROOT=$MuPdfRoot" }
if ($VcpkgToolchain) { $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain" }

Write-Host "== configure ==" -ForegroundColor Cyan
& $cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "cmake 配置失败" }

Write-Host "== build ($Configuration) ==" -ForegroundColor Cyan
& $cmake --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "编译失败" }

Write-Host "== test ==" -ForegroundColor Cyan
& "C:\Program Files\CMake\bin\ctest.exe" --test-dir $buildDir -C $Configuration --output-on-failure

$bin = Join-Path $buildDir "bin"
Write-Host "`n产物目录: $bin" -ForegroundColor Green
Get-ChildItem -LiteralPath $bin -File | Select-Object Name, Length | Format-Table -AutoSize

Write-Host "下一步：" -ForegroundColor Yellow
Write-Host "  1) 把 $bin\pdfword.ini.example 复制成 pdfword.ini，填 libre_office 路径（见 docs/DEPENDENCIES.md）"
Write-Host "  2) $bin\pdfword_cli.exe doctor"
Write-Host "  3) windeployqt 部署 Qt 运行库后再双击 $bin\pdfword_ui.exe"
