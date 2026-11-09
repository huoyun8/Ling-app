#!/usr/bin/env bash
# 一套源码，两个产物 —— Linux / macOS 交叉编译版（对应 Windows 的 build.ps1）
#
#   ./build.sh            两个版本都出
#   ./build.sh core       只出 SnapCore（不带 OCR 引擎）
#   ./build.sh ocr        只出 SnapOCR（带 OCR 引擎）
#   ./build.sh clean      先清掉 target / target-ocr / dist
#
# 依赖：
#   rustup target add x86_64-pc-windows-gnu
#   Debian/Ubuntu: sudo apt install gcc-mingw-w64-x86-64
#   macOS:         brew install mingw-w64
#
# 产物：dist/SnapCore.exe、dist/SnapOCR.exe（Windows x64，GNU 工具链）
set -euo pipefail

cd "$(dirname "$0")"
target=x86_64-pc-windows-gnu
build_dir="target/$target/release"
ocr_build_dir="target-ocr/$target/release"

case "${1:-all}" in
    clean)
        rm -rf target target-ocr dist
        exit 0
        ;;
    core) build_core=1; build_ocr=0 ;;
    ocr)  build_core=0; build_ocr=1 ;;
    all|"") build_core=1; build_ocr=1 ;;
    *) echo "用法: $0 [all|core|ocr|clean]" >&2; exit 2 ;;
esac

command -v cargo >/dev/null || { echo "未找到 cargo" >&2; exit 1; }
command -v x86_64-w64-mingw32-gcc >/dev/null || {
    echo "未找到 x86_64-w64-mingw32-gcc，请先安装 MinGW 工具链（见脚本头部注释）" >&2
    exit 1
}
rustup target list --installed 2>/dev/null | grep -qx "$target" || {
    echo "缺少 Rust 目标标准库，请先执行: rustup target add $target" >&2
    exit 1
}

mkdir -p dist

if [[ $build_ocr == 1 ]]; then
    echo "[1/2] cargo build --release --features ocr --target $target -> SnapOCR"
    cargo build --release --features ocr --target "$target" --target-dir target-ocr
    cp "$ocr_build_dir/snapcore.exe" dist/SnapOCR.exe
fi

if [[ $build_core == 1 ]]; then
    echo "[2/2] cargo build --release --target $target            -> SnapCore"
    cargo build --release --target "$target"
    cp "$build_dir/snapcore.exe" dist/SnapCore.exe
fi

ls -l dist
