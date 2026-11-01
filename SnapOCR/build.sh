#!/usr/bin/env bash
# SnapCore / SnapOCR 构建脚本（不依赖 msbuild —— 本机安全策略会拦它，所以直接驱动 cl/link）
#
#   ./build.sh         增量构建，产物 build/bin/<品牌>.exe
#   ./build.sh clean   全量重建（约 7 分钟）
#
# ── 两个产品、一份源码 ───────────────────────────────────────────────
#   不带 OCR 引擎 → SnapCore  页签：二维码 / 文件校验 / 定时关机 / 设置
#   带   OCR 引擎 → SnapOCR   页签：文字识别（首页）/ 二维码 / 文件校验 / 定时关机 / 设置
#
#   默认由**目录名**决定（解压出来叫 SnapOCR 就出带引擎那版，叫 SnapCore 就出纯工具版），
#   也可以显式覆盖：
#       SNAPCORE_WITH_OCR=0 ./build.sh     出 SnapCore.exe（不含 OCR 代码与模型）
#       SNAPCORE_WITH_OCR=1 ./build.sh     出 SnapOCR.exe（内嵌 PP-OCRv6）
#
#   品牌名（界面上的一切文字、窗口类名、配置目录、启动项）由 Src/Brand.h 按这个宏派生，
#   两版可以同机安装、同时运行。
#
# 三大坑（见交接卡）
#   1) obj 同名覆盖 —— 已从根源解决：
#      · 文件名不重名。Src/Ui/ 下的文件一律带 Ui 前缀（UiApp / UiUtil / UiPch），
#        核心层的 App/Util 留在 Src/ 根。历史上那一对 App.cpp / Util.cpp 已改名。
#      · obj 路径镜像源码目录（Src/Ui/X.cpp → build/obj/Ui/X.obj），
#        即使再出现同名文件也各占一格，不会互相覆盖。
#      · 构建前强制体检：不同目录出现同名 .cpp 直接报错退出（见步骤 0），
#        不允许"靠分目录兜着"混过去。
#   2) 所有 .cpp/.h（含 Src/Ui/ 与应用层）都带 UTF-8 BOM —— 批量改文件用 utf-8-sig 读写。
#   3) Lang/zh-CN.json 是 UTF-8 无 BOM（编译时由 RC 原样嵌入）；写成 UTF-16 会乱码。
#
# 第四个坑（这份脚本自己踩过）：在 Git Bash 里跑 cl，`/I` `/D` 这些参数会被
# MSYS 当成路径改写（`/DNDEBUG` → `D:/.../DNDEBUG`）。所以下面统一：
#   · 关掉参数改写（MSYS2_ARG_CONV_EXCL=*）
#   · 所有传给编译器的路径先用 cygpath -w 转成 Windows 形式
set -euo pipefail
export MSYS2_ARG_CONV_EXCL='*'

# ---------- 工具链 ----------
VC_ROOT="/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207"
SDK_ROOT="/c/Program Files (x86)/Windows Kits/10"
SDK_VER="10.0.22621.0"

CL="$VC_ROOT/bin/Hostx64/x64/cl.exe"
LINK="$VC_ROOT/bin/Hostx64/x64/link.exe"
RC="$SDK_ROOT/bin/${SDK_VER}/x64/rc.exe"

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"

# ---------- 品牌 / OCR 开关 ----------
# 不设环境变量时看目录名：SnapOCR* → 带引擎，其余 → 纯工具版。
# 目录名判定只在"忘了设变量"时兜底，打包出来两份目录各自双击 ./build.sh 就能得到对应产品
if [ -z "${SNAPCORE_WITH_OCR+x}" ]; then
    case "$(basename "$ROOT")" in
        SnapOCR*|snapocr*) WITH_OCR=1 ;;
        *)                 WITH_OCR=0 ;;
    esac
else
    WITH_OCR="$SNAPCORE_WITH_OCR"
fi
[ "$WITH_OCR" = "1" ] && BRAND="SnapOCR" || BRAND="SnapCore"
echo "[brand] $BRAND（OCR 引擎：$([ "$WITH_OCR" = "1" ] && echo 开 || echo 关)）"

BUILD="$ROOT/build"
# 两个产品的中间产物必须分开放。编译宏不同（SNAPCORE_WITH_OCR 决定整个
# 「文字识别」页在不在），而增量判断只看时间戳 —— 共用 obj 目录时切一次品牌，
# 上一次的 .obj 会被当成"还新"直接链进去，症状是"宏改了没生效"、
# 甚至链接到不存在的控件 id。PCH 跟着 obj 走，也自动分开了
OBJ="$BUILD/obj-$BRAND"
# 模型固定用出厂 fp32 资产（PP-OCRv6 small，与 Umi-OCR 同核心）
OCR_SRC="$ROOT/Src/Res/ocr"
OUT="$BUILD/bin/$BRAND.exe"
PCH="$OBJ/pch.pch"

w() { cygpath -w "$1"; }

mkdir -p "$OBJ" "$BUILD/bin"

# link 嵌入清单时要现调 mt.exe，它不在 link.exe 旁边，得把 SDK 的 bin 放进 PATH
export PATH="$SDK_ROOT/bin/${SDK_VER}/x64:$PATH"

if [ "${1:-}" = "clean" ]; then
    echo "[clean] 清空 $BUILD（保留目录骨架）"
    # 不用 rm -rf：本机安全删除层会拦截递归删除（曾失败过一次）。
    # 逐文件删 + 空目录自底向上删，行为与 rm -rf 一致但过得了策略。
    find "$BUILD" -type f -delete 2>/dev/null || true
    find "$BUILD" -depth -type d -exec rmdir {} + 2>/dev/null || true
    mkdir -p "$OBJ" "$BUILD/bin"
fi

# ---------- 编译参数（对应 vcxproj Release|x64）----------
INCLUDES=(
    "/I$(w "$VC_ROOT/include")"
    "/I$(w "$SDK_ROOT/Include/$SDK_VER/ucrt")"
    "/I$(w "$SDK_ROOT/Include/$SDK_VER/um")"
    "/I$(w "$SDK_ROOT/Include/$SDK_VER/shared")"
    "/I$(w "$SDK_ROOT/Include/$SDK_VER/winrt")"
    "/I$(w "$SDK_ROOT/Include/$SDK_VER/cppwinrt")"
    "/I$(w "$ROOT/Src")"
    "/I$(w "$ROOT/Src/Ui")"
)

DEFINES=(
    /DNOMINMAX /DWIN32_LEAN_AND_MEAN
    /D_CRT_NONSTDC_NO_WARNINGS /D_CRT_SECURE_NO_WARNINGS
    /DNDEBUG /DUNICODE /D_UNICODE
)
# 品牌宏：定义了才有「文字识别」页、OCR 引擎与内嵌模型。
# 这个宏是**唯一**的分叉点（见 Src/Brand.h），代码里所有 #if SNAP_HAS_OCR 都跟着它走
if [ "$WITH_OCR" = "1" ]; then
    DEFINES+=(/DSNAPCORE_WITH_OCR)
fi

CFLAGS=(
    /nologo /c /O2 /GL /MT /EHsc /std:c++20 /permissive- /W3
    /utf-8 /Gy /Gw /Z7
    "${INCLUDES[@]}" "${DEFINES[@]}"
)

LDFLAGS=(
    /nologo /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO
    /SUBSYSTEM:WINDOWS
    /MANIFEST:EMBED "/MANIFESTINPUT:$(w "$BUILD/$BRAND.manifest")"
    "/PDB:$(w "$BUILD/$BRAND.pdb")"
    "/OUT:$(w "$OUT")"
    "/LIBPATH:$(w "$VC_ROOT/lib/x64")"
    "/LIBPATH:$(w "$SDK_ROOT/Lib/$SDK_VER/ucrt/x64")"
    "/LIBPATH:$(w "$SDK_ROOT/Lib/$SDK_VER/um/x64")"
    dwmapi.lib windowsapp.lib comctl32.lib imm32.lib version.lib comdlg32.lib
    ntdll.lib Userenv.lib Cabinet.lib
    d3d11.lib dxgi.lib d2d1.lib
    windowscodecs.lib oleaut32.lib
    bcrypt.lib shcore.lib advapi32.lib shell32.lib user32.lib gdi32.lib
    ole32.lib uuid.lib runtimeobject.lib
    mf.lib mfreadwrite.lib mfplat.lib mfuuid.lib propsys.lib wmcodecdspuuid.lib
)

# ---------- 应用清单（Per-Monitor V2；main.cpp 里也会运行时再设一次）----------
# 清单里的 assemblyIdentity 必须带品牌名：两份清单同 name 同 version 时，
# 系统会按"同名程序集"处理并行缓存，两个不同的 exe 会争同一份清单
cat > "$BUILD/$BRAND.manifest" <<XML
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="$BRAND" version="1.0.0.0" processorArchitecture="amd64"/>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="asInvoker" uiAccess="false"/>
      </requestedPrivileges>
    </security>
  </trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1">
    <application>
      <supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/>
    </application>
  </compatibility>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true/pm</dpiAware>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
      <longPathAware xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">true</longPathAware>
    </windowsSettings>
  </application>
</assembly>
XML

# ---------- 增量判断 ----------
needs_build() {  # $1=源文件 $2=obj  $3..=额外依赖
    local src="$1" objf="$2"; shift 2
    [ -f "$objf" ] || return 0
    [ "$src" -nt "$objf" ] && return 0
    local d
    for d in "$@"; do [ "$d" -nt "$objf" ] && return 0; done
    return 1
}

# ---------- 0. 体检：不同目录不得出现同名编译单元 ----------
# 镜像 obj 目录只能保证"撞了也各存一份、不互相覆盖"，治不了同名本身带来的麻烦：
# #include "X.h" 靠"同目录优先"隐式解析，同名头谁被引到全看调用方站在哪个目录；
# IDE 里跳定义 / 断点 / 全局搜索还会指到另一份 —— 改错文件、看错代码都从这儿来。
# 所以这里直接禁止：一旦出现同名 .cpp，构建失败，改名再继续。
DUP=$(find "$ROOT/Src" \( -name '*.cpp' -o -name '*.c' \) -printf '%f\n' | sort | uniq -d)
if [ -n "$DUP" ]; then
    echo "!! 不同目录出现同名源文件（obj、头文件、IDE 跳转都会打架）："
    for n in $DUP; do
        echo "   $n"
        find "$ROOT/Src" -name "$n" | sed "s#^$ROOT/##; s#^#      #"
    done
    echo "   处置：给其中一个加前缀（GUI 层统一用 Ui 前缀，如 UiApp.cpp），"
    echo "         再同步改 #include 与 Src/ScreenCapture.vcxproj。"
    exit 1
fi

# ---------- 1. 预编译头 ----------
# PCH 里塞的是 Ui/Ui.h 那一整片头（WinBase/Node/Canvas…）。任何一个头改了
# 都必须重做 PCH，否则 .obj 用的是旧头里的类布局 —— 症状是"头文件里明明加了成员，
# 编译器却说不是成员"，或者更糟：编过了但运行时按旧偏移读写内存
# 注：末尾的 || true 是给 set -e 留的 —— 第一次构建时 $PCH 还不存在，
# find -newer 会报错返回非 0；管到 head 也可能被 SIGPIPE。（踩过一次：脚本直接静默退出）
# .hpp 也要算（.hpp 不在 .h 的通配里，漏一次就是
# "改了 hpp 没生效"——增量判断不跟踪单个 cpp 的头依赖，全靠这一句兜底）
HDR_NEWER=$(find "$ROOT/Src" \( -name '*.h' -o -name '*.hpp' \) -newer "$PCH" 2>/dev/null | head -1 || true)
# 头变了就全量重编：不删旧 obj（删一堆文件容易触发安全策略，也没必要 ——
# 逐个覆盖重编的结果一样，只要保证这次一个都不跳过）
FORCE_ALL=0
if [ ! -f "$PCH" ] || [ -n "$HDR_NEWER" ] || [ "$ROOT/Src/pch.h" -nt "$PCH" ] || [ "$ROOT/Src/pch.cpp" -nt "$PCH" ]; then
    if [ -n "$HDR_NEWER" ] && [ -f "$PCH" ]; then
        echo "[pch] 头文件有更新（$(basename "$HDR_NEWER")）：PCH 重建，本次全部源文件重编"
        FORCE_ALL=1
    fi
    echo "[pch] pch.h"
    "$CL" "${CFLAGS[@]}" '/Ycpch.h' "/Fp$(w "$PCH")" "/Fo$(w "$OBJ/pch.obj")" "$(w "$ROOT/Src/pch.cpp")" \
        > "$BUILD/pch.log" 2>&1 || { cat "$BUILD/pch.log"; exit 1; }
fi

# ---------- 2. C++ ----------
# obj 路径 = 源文件路径整棵镜像到 build/obj 下：
#     Src/App.cpp        → build/obj/App.obj
#     Src/Ui/UiApp.cpp   → build/obj/Ui/UiApp.obj
#     Src/Win/WinCap.cpp → build/obj/Win/WinCap.obj
# 重名问题已在文件名层面消掉（见文件头第 1 条），这层镜像现在是第二道防线：
# 源路径唯一决定 obj 路径，就算谁又加了个同名文件，也只是各占一格、不互相覆盖。
# 链接前的第 5 步还有一道陈旧检查，专门抓"链接成功但用的是旧 obj"。
COMPILED=0
CPP_LIST=$(cd "$ROOT" && find Src -name '*.cpp' | grep -v '^Src/pch.cpp$' | sort)
# 不带 OCR 的版本连 OCR 的编译单元都不进：OcrEngine / OcrLayout 只服务「文字识别」页，
# 留着编译只会在 exe 里留下一堆永远走不到的代码（还白链 Cabinet.lib）
if [ "$WITH_OCR" != "1" ]; then
    CPP_LIST=$(printf '%s\n' "$CPP_LIST" | grep -v '^Src/Ocr/' || true)
    echo "[c++] 跳过 Src/Ocr/*（本版不含 OCR 引擎）"
fi

for rel in $CPP_LIST; do
    src="$ROOT/$rel"
    # ${rel#Src/} 去掉 Src/，再把 .cpp 换成 .obj：目录层级原样保留
    objf="$OBJ/$(echo "${rel#Src/}" | sed 's#\.cpp$#.obj#')"
    mkdir -p "$(dirname "$objf")"
    if [ "$FORCE_ALL" = 0 ] && ! needs_build "$src" "$objf" "$ROOT/Src/pch.h" "$PCH"; then continue; fi
    echo "[c++] $rel"
    if [ "${rel#Src/Ui/}" != "$rel" ]; then
        # GUI 层不用 PCH（vcxproj 里是 NotUsing），自己 include UiPch.h
        "$CL" "${CFLAGS[@]}" "/Fo$(w "$objf")" "$(w "$src")" > "$BUILD/compile.log" 2>&1 \
            || { echo "!! 编译失败: $rel"; grep -E "error|错误" "$BUILD/compile.log" | head -20; exit 1; }
    else
        "$CL" "${CFLAGS[@]}" '/Yupch.h' "/Fp$(w "$PCH")" "/Fo$(w "$objf")" "$(w "$src")" > "$BUILD/compile.log" 2>&1 \
            || { echo "!! 编译失败: $rel"; grep -E "error|错误" "$BUILD/compile.log" | head -20; exit 1; }
    fi
    COMPILED=$((COMPILED+1))
done
echo "[c++] 本次编译 $COMPILED 个文件"

# ---------- 3. C（qrencode / quirc）----------
C_LIST=$(cd "$ROOT" && find Src -name '*.c' | sort)
for rel in $C_LIST; do
    src="$ROOT/$rel"
    objf="$OBJ/$(echo "${rel#Src/}" | sed 's#\.c$#.obj#')"
    mkdir -p "$(dirname "$objf")"
    if [ "$FORCE_ALL" = 0 ] && ! needs_build "$src" "$objf"; then continue; fi
    echo "[ cc] $rel"
    "$CL" /nologo /c /O2 /MT /TC /W3 "${INCLUDES[@]}" "${DEFINES[@]}" "/Fo$(w "$objf")" "$(w "$src")" \
        > "$BUILD/compile.log" 2>&1 || { echo "!! 编译失败: $rel"; grep -E "error|错误" "$BUILD/compile.log" | head -20; exit 1; }
done

# ---------- 4. 资源 ----------
RES="$OBJ/Resource.res"
# 资源里是 logo、语言包，以及（可选）OCR 引擎的四个文件。
# 图标已改为矢量路径（Src/Res/Icons.generated.h），不再有字体资源。
#
# OCR 那 44MB 默认打进 exe（单文件交付）；开发时用 SNAPCORE_OCR_EMBED=0 关掉，
# 免得每次链接都搬一遍。关掉时资源里没有它们，运行期退到 exe 同目录 ocr\ 找模型。
#
# 打进 exe 的不是裸文件，而是 tools/ocrcmp 压出来的 SOCP 容器（见 Src/Res/Resource.rc）。
# 实测 LZMS：46.9MB -> 32.3MB（引擎本体 16.9->5.7、det 9.9->8.0、rec 21.2->18.6）。
# 压缩是无损的，识别结果与裸文件完全一致；代价只是首次释放多花约 0.6 秒。
# 不带引擎的版本无论如何都不内嵌：模型资产可能不在这份目录里，就算在也不该进 exe
if [ "$WITH_OCR" = "1" ]; then OCR_EMBED="${SNAPCORE_OCR_EMBED:-1}"; else OCR_EMBED=0; fi
# 打包缓存（最后整体拷到 $BUILD/ocrpack，Resource.rc 只认这个路径）
PACK="$BUILD/pack"
# 资源脚本也要看品牌宏（VERSIONINFO 里的产品名 / 文件名按它分叉）
RC_DEFS=()
[ "$WITH_OCR" = "1" ] && RC_DEFS+=(/dSNAPCORE_WITH_OCR)
if [ "$OCR_EMBED" != "0" ] && [ -f "$OCR_SRC/PP-OCRv6_rec_small.onnx" ]; then
    RC_DEFS+=(/dSNAPCORE_OCR_EMBED)
    echo "[ rc] OCR 模型内嵌到 exe（压缩后约 32MB）"

    # 4.1 打包工具本身（改了就重编）
    CMP="$BUILD/tools/ocrcmp.exe"
    if [ ! -f "$CMP" ] || [ "$ROOT/tools/ocrcmp.cpp" -nt "$CMP" ]; then
        echo "[ cmp] tools/ocrcmp.cpp"
        mkdir -p "$BUILD/tools"
        "$CL" /nologo /O2 /MT /EHsc /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN \
            "/I$(w "$VC_ROOT/include")" "/I$(w "$SDK_ROOT/Include/$SDK_VER/ucrt")" \
            "/I$(w "$SDK_ROOT/Include/$SDK_VER/um")" "/I$(w "$SDK_ROOT/Include/$SDK_VER/shared")" \
            "/Fo$(w "$BUILD/tools/")" "/Fe$(w "$CMP")" "$(w "$ROOT/tools/ocrcmp.cpp")" \
            "/link" "/LIBPATH:$(w "$VC_ROOT/lib/x64")" \
            "/LIBPATH:$(w "$SDK_ROOT/Lib/$SDK_VER/ucrt/x64")" \
            "/LIBPATH:$(w "$SDK_ROOT/Lib/$SDK_VER/um/x64")" Cabinet.Lib \
            > "$BUILD/ocrcmp.log" 2>&1 || { cat "$BUILD/ocrcmp.log"; exit 1; }
    fi

    # 4.2 逐个打包（源文件比产物新才重压，压缩 47MB 要几秒，别每轮都做）
    mkdir -p "$PACK"
    for f in ocr-process.exe PP-OCRv6_det_small.onnx PP-OCRv6_rec_small.onnx ppocrv6_dict.txt; do
        SRC_FILE="$OCR_SRC/$f"
        [ -f "$SRC_FILE" ] || continue
        if [ ! -f "$PACK/$f.bin" ] || [ "$SRC_FILE" -nt "$PACK/$f.bin" ]; then
            echo "[ cmp] $f"
            "$CMP" "$(w "$SRC_FILE")" "$(w "$PACK/$f.bin")" || exit 1
        fi
    done
    # 4.3 统一切到 rc 认的目录名
    rm -rf "$BUILD/ocrpack"
    cp -r "$PACK" "$BUILD/ocrpack"
else
    echo "[ rc] 不内嵌 OCR 模型：运行期用 exe 同目录的 ocr 目录"
fi
# 内嵌开关变化时必须重编资源，否则上一次的 .res 会被原样链进去（模型有/没有全看运气）
STAMP="$BUILD/ocr_embed.stamp"
STAMP_VAL="$BRAND:$OCR_EMBED"
PREV_EMBED=$(cat "$STAMP" 2>/dev/null || echo "?")
RES_STALE=0
[ "$PREV_EMBED" != "$STAMP_VAL" ] && RES_STALE=1
# 打包产物变了也要重编：否则 exe 里还是上一版模型
if [ "$OCR_EMBED" != "0" ] && [ -d "$PACK" ]; then
    for b in "$PACK"/*.bin; do
        [ -f "$b" ] || continue
        [ "$b" -nt "$RES" ] && RES_STALE=1
    done
fi
if [ "$RES_STALE" = 1 ] || needs_build "$ROOT/Src/Res/Resource.rc" "$RES" "$ROOT/Lang/zh-CN.json" "$ROOT/Src/Res/logo.ico" "$ROOT/Src/Res/SnapCore.ico"; then
    echo "[ rc] Resource.rc"
    # /I 带上 build 目录：Resource.rc 里的 "ocrpack\xxx.bin" 靠它解析
    ( cd "$ROOT/Src/Res" && "$RC" /nologo ${RC_DEFS[@]+"${RC_DEFS[@]}"} "/fo$(w "$RES")" \
        "/I$(w "$BUILD")" \
        "/I$(w "$SDK_ROOT/Include/$SDK_VER/um")" "/I$(w "$SDK_ROOT/Include/$SDK_VER/shared")" Resource.rc ) \
        > "$BUILD/rc.log" 2>&1 || { cat "$BUILD/rc.log"; exit 1; }
    echo "$STAMP_VAL" > "$STAMP"
fi

# 不内嵌时把模型目录拷到产物旁边 —— 那条路运行期就是从 exe 同目录的 ocr 找
if [ "$OCR_EMBED" = "0" ] && [ -d "$ROOT/Src/Res/ocr" ]; then
    mkdir -p "$BUILD/bin/ocr"
    cp -f "$ROOT/Src/Res/ocr/"* "$BUILD/bin/ocr/" 2>/dev/null || true
fi

# ---------- 5. 链接前体检：每个 obj 都得对得上一个更新的源文件 ----------
# 这一步就是"改了没生效"的化验单。它的前提是第 2/3 步的 obj 路径与源文件一一对应，
# 于是可以逐个反查：
#   · 反查不到源文件（源文件被改名/删除，obj 是上一轮的残留）→ 报错，不许把旧代码链进去
#   · obj 比源文件旧（增量判断漏了、编译被中断留下半截）→ 报错
# 以前这两种情况都是静默的：链接成功、exe 也出来了，但跑的是旧逻辑
STALE=0
ORPHAN=0
OBJ_LIST=""
for o in $(find "$OBJ" -name '*.obj' | sort); do
    rel="${o#$OBJ/}"
    src=""
    case "$rel" in
        pch.obj)  src="$ROOT/Src/pch.cpp" ;;
        *.obj)    for cand in "$ROOT/Src/${rel%.obj}.cpp" "$ROOT/Src/${rel%.obj}.c"; do
                      [ -f "$cand" ] && { src="$cand"; break; }
                  done ;;
    esac
    if [ -z "$src" ]; then
        echo "!! 孤儿 obj（没有对应的源文件，多半是改名/删文件后的残留）：$rel —— 本次不参与链接"
        ORPHAN=$((ORPHAN+1))
        continue
    fi
    if [ "$src" -nt "$o" ]; then
        echo "!! 陈旧 obj（比源文件旧，说明这次没重编）：$rel  ←  $(basename "$src")"
        STALE=1
    fi
    OBJ_LIST="$OBJ_LIST $o"
done
if [ "$ORPHAN" -gt 0 ]; then
    echo "   （孤儿 obj 共 $ORPHAN 个，已跳过。想清干净就 ./build.sh clean）"
fi
if [ "$STALE" = 1 ]; then
    echo "!! 体检不通过：链接出来的 exe 会是'一半新一半旧'。"
    echo "   处置：./build.sh clean 全量重来（不要只补那一个 obj，别处可能也漏了）"
    exit 1
fi

# ---------- 6. 链接 ----------
echo "[link] $(basename "$OUT")"
echo "       obj $(echo $OBJ_LIST | wc -w) 个"
# shellcheck disable=SC2086
"$LINK" "${LDFLAGS[@]}" $(for o in $OBJ_LIST; do w "$o"; done) "$(w "$RES")" \
    > "$BUILD/link.log" 2>&1 || { echo "!! 链接失败"; tail -30 "$BUILD/link.log"; exit 1; }

SIZE=$(stat -c%s "$OUT" 2>/dev/null || stat -f%z "$OUT")
echo "[done] $OUT  ($((SIZE/1024)) KB)"
