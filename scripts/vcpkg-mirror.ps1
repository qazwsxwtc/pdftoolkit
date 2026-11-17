# 让 vcpkg 在 GitHub 直连不通的网络里也能安装依赖。
#
# 背景：vcpkg 每个 port 的下载地址都写死在 portfile 里（GitHub raw / codeload 等），
#      官方没有国内镜像，PyPI 镜像（清华/中科大/阿里云）对 vcpkg 完全无效。
# 做法：vcpkg 支持 X_VCPKG_ASSET_SOURCES="x-script,<命令>"，把下载外包给任意命令，
#      这里用 curl 把 URL 套上图片/代码代理镜像（默认 ghproxy.net），失败再回落直连。
#
# 用法：
#   pwsh -File scripts\vcpkg-mirror.ps1
#   pwsh -File scripts\vcpkg-mirror.ps1 -Packages libmupdf:x64-windows
#   pwsh -File scripts\vcpkg-mirror.ps1 -Mirror https://ghproxy.net/

param(
    [string]$VcpkgRoot = "D:\vcpkg",
    [string[]]$Packages = @("libmupdf:x64-windows"),
    [string]$Mirror = "https://ghproxy.net/"
)

$vcpkgExe = Join-Path $VcpkgRoot "vcpkg.exe"
if (-not (Test-Path -LiteralPath $vcpkgExe)) {
    throw "找不到 vcpkg.exe: $vcpkgExe（用 -VcpkgRoot 指定 vcpkg 根目录）"
}

$curl = "C:\Windows\System32\curl.exe"
if (-not (Test-Path -LiteralPath $curl)) { throw "找不到 curl.exe: $curl" }

# vcpkg 会在构建环境里通过 cmd 执行这条命令，所以用 .bat 包一层最稳
$bat = Join-Path $env:TEMP "vcpkg-asset-proxy.bat"
$lines = @(
    '@echo off',
    'setlocal',
    'set "U=%~1"',
    'set "D=%~2"',
    ('"' + $curl + '" -L --fail --silent --show-error --retry 2 --retry-delay 2 --connect-timeout 20 -o "%D%" "' + $Mirror + '%U%" && exit /b 0'),
    ('"' + $curl + '" -L --fail --silent --show-error --retry 2 --retry-delay 2 --connect-timeout 20 -o "%D%" "%U%" && exit /b 0'),
    'exit /b 1'
)
[IO.File]::WriteAllText($bat, ($lines -join "`r`n") + "`r`n", (New-Object Text.UTF8Encoding $false))

$env:X_VCPKG_ASSET_SOURCES = 'x-script,' + $bat + ' "{url}" "{dst}"'
Write-Host "镜像前缀: $Mirror"
Write-Host "资产源  : $($env:X_VCPKG_ASSET_SOURCES)"

& $vcpkgExe install @Packages
exit $LASTEXITCODE
