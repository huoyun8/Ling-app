#!/usr/bin/env bash
# 排版解析离线自测的编译脚本。和 build.sh 共用同一套工具链，但绕开预编译头
# （/Y-），这样单文件就能编，不必等整包构建。
#
#   ./tools/layout_test.sh
set -euo pipefail
export MSYS2_ARG_CONV_EXCL='*'

VC_ROOT="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"
SDK_INC="/c/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0"
SDK_LIB="/c/Program Files (x86)/Windows Kits/10/Lib/10.0.22621.0"
CL="$VC_ROOT/bin/Hostx64/x64/cl.exe"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/build/lt"

w() { cygpath -w "$1"; }

"$CL" /nologo /std:c++20 /EHsc /MT /utf-8 /O2 /W3 /Y- \
    /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS \
    "/I$(w "$SDK_INC/cppwinrt")" "/I$(w "$SDK_INC/winrt")" "/I$(w "$SDK_INC/ucrt")" \
    "/I$(w "$SDK_INC/um")" "/I$(w "$SDK_INC/shared")" "/I$(w "$VC_ROOT/include")" "/I$(w "$ROOT/Src")" \
    "/Fo$(w "$ROOT/build/lt/" )" "/Fe$(w "$ROOT/build/layout_test.exe")" \
    "$(w "$ROOT/tools/layout_test.cpp")" "$(w "$ROOT/Src/Ocr/OcrLayout.cpp")" \
    /link "/LIBPATH:$(w "$VC_ROOT/lib/x64")" "/LIBPATH:$(w "$SDK_LIB/ucrt/x64")" "/LIBPATH:$(w "$SDK_LIB/um/x64")" \
    > "$ROOT/build/lt/build.log" 2>&1 || { tail -20 "$ROOT/build/lt/build.log"; exit 1; }

"$ROOT/build/layout_test.exe"
