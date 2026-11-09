# 一套源码，两个产物
#   .\build.ps1            两个版本都出
#   .\build.ps1 -Core      只出 SnapCore（不带 OCR 引擎）
#   .\build.ps1 -Ocr       只出 SnapOCR（带 OCR 引擎）
#   .\build.ps1 -Clean     先清掉 target / target-ocr / dist
#
# 不带 ocr 特性  -> target\release\snapcore.exe      -> dist\SnapCore.exe（约 1.3MB）
# 带 ocr 特性    -> target-ocr\release\snapcore.exe  -> dist\SnapOCR.exe（约 33MB）
# 品牌名由 build.rs 按 feature 注入（窗口类名、互斥体、注册表项、配置目录都随之变化），
# 两个 exe 可以同机同时运行、互不覆盖配置。
param(
    [switch]$Core,
    [switch]$Ocr,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root
try {
    if ($Clean) {
        Remove-Item target, target-ocr, dist -Recurse -Force -ErrorAction SilentlyContinue
    }
    $all = -not ($Core -or $Ocr)
    New-Item -ItemType Directory -Force dist | Out-Null
    if ($all -or $Ocr) {
        Write-Host "[1/2] cargo build --release --features ocr -> SnapOCR"
        cargo build --release --features ocr --target-dir target-ocr
        if ($LASTEXITCODE -ne 0) { throw "SnapOCR 构建失败" }
        Copy-Item target-ocr\release\snapcore.exe dist\SnapOCR.exe -Force
    }
    if ($all -or $Core) {
        Write-Host "[2/2] cargo build --release            -> SnapCore"
        cargo build --release
        if ($LASTEXITCODE -ne 0) { throw "SnapCore 构建失败" }
        Copy-Item target\release\snapcore.exe dist\SnapCore.exe -Force
    }
    Get-ChildItem dist | Select-Object Name, Length
}
finally {
    Pop-Location
}
