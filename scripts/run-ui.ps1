# 启动 Pdf-Toolkit 界面（可执行文件仍名 pdfword_ui.exe）。
#
# 工程有两个构建目录，产物同名，很容易双击错的：
#   build\mupdf     PDFWORD_WITH_MUPDF=ON，PDF 后端 = mupdf（可用），exe ~36 MB
#   build\msvc-x64  最小骨架，PDF 后端 = stub（不可用），exe ~0.4 MB
#
# 用法：
#   pwsh -File scripts\run-ui.ps1              # 默认跑带 MuPDF 的界面
#   pwsh -File scripts\run-ui.ps1 -Build skeleton   # 故意跑骨架界面

param(
    [ValidateSet("mupdf", "skeleton")]
    [string]$Build = "mupdf"
)

$root = Split-Path -Parent $PSScriptRoot

$exe = if ($Build -eq "mupdf") {
    Join-Path $root "build\mupdf\bin\pdfword_ui.exe"
} else {
    Join-Path $root "build\msvc-x64\bin\pdfword_ui.exe"
}

if (-not (Test-Path -LiteralPath $exe)) {
    if ($Build -eq "mupdf") {
        Write-Host "没有找到 build\mupdf\bin\pdfword_ui.exe。" -ForegroundColor Yellow
        Write-Host "先做一次带 MuPDF 的构建（README「带 MuPDF 的完整构建」一节）：" -ForegroundColor Yellow
        Write-Host '  cmake -S . -B build/mupdf -G "Visual Studio 17 2022" -A x64 `' -ForegroundColor Yellow
        Write-Host '        -DCMAKE_PREFIX_PATH=D:/Qt/6.12.0/msvc2022_64 `' -ForegroundColor Yellow
        Write-Host '        -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake `' -ForegroundColor Yellow
        Write-Host '        -DPDFWORD_WITH_MUPDF=ON' -ForegroundColor Yellow
        Write-Host '  cmake --build build/mupdf --config Release --parallel' -ForegroundColor Yellow
    } else {
        Write-Host "没有找到 build\msvc-x64\bin\pdfword_ui.exe，先跑构建。" -ForegroundColor Yellow
    }
    exit 1
}

$sizeMB = [math]::Round((Get-Item -LiteralPath $exe).Length / 1MB, 1)
Write-Host "启动 $exe（$sizeMB MB）"
if ($sizeMB -lt 1) {
    Write-Host "注意：这是骨架构建，界面里 PDF 后端会是 stub（不可用）。要 PDF 能力请跑 scripts\run-ui.ps1。" -ForegroundColor Yellow
}

Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe)
