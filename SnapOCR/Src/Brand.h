#pragma once
// ───────────────────────────── 编译期品牌 ─────────────────────────────
// 一份源码、两个产品，只靠一个宏分叉：
//
//   SNAPCORE_WITH_OCR 未定义 → SnapCore  不带 OCR 引擎
//                              页签：二维码 / 文件校验 / 定时关机 / 设置
//   SNAPCORE_WITH_OCR 已定义 → SnapOCR   带 PP-OCRv6 本地识别
//                              页签：文字识别 / 二维码 / 文件校验 / 定时关机 / 设置
//
// 下面的名字全部由品牌派生，两个版本因此可以**同机共存**：
//   窗口类名（SnapCore_Main / SnapOCR_Main …）、单实例标识（appID）、
//   配置目录 %APPDATA%\<brand>、OCR 模型缓存 %LOCALAPPDATA%\<brand>\ocr、
//   注册表启动项名、任务栏 AppUserModelID、托盘提示。
//
// 字面量拼接是编译期完成的（相邻字符串字面量自动相接），没有运行期开销：
//   SNAP_CLS_MAIN → L"SnapCore_Main" 或 L"SnapOCR_Main"
#if defined(SNAPCORE_WITH_OCR)
#	define SNAP_HAS_OCR 1
#	define SNAP_BRAND_W L"SnapOCR"
#	define SNAP_BRAND_A "SnapOCR"
#else
#	define SNAP_HAS_OCR 0
#	define SNAP_BRAND_W L"SnapCore"
#	define SNAP_BRAND_A "SnapCore"
#endif

// 窗口类名。每只窗口类都带品牌前缀 —— 两版同时运行时，同名类会撞在
// RegisterClassExW 上（第二个注册失败返回 0，窗口直接建不出来）
#define SNAP_CLS_MAIN   SNAP_BRAND_W L"_Main"
#define SNAP_CLS_TOAST  SNAP_BRAND_W L"_Toast"
#define SNAP_CLS_TOASTX SNAP_BRAND_W L"Toast"
#define SNAP_CLS_VFBOX  SNAP_BRAND_W L"_VfBox"
#define SNAP_CLS_UI     SNAP_BRAND_W L"_Ui"

// 配置目录 / 缓存目录
#define SNAP_DIR_W      SNAP_BRAND_W

// 版本号：产品信息与 build.sh 的 FILEVERSION 同源
// 两个产品各自独立编号：SnapCore 1.0（纯工具）、SnapOCR 2.0（带识别引擎）
#if defined(SNAPCORE_WITH_OCR)
#	define SNAP_VERSION_W L"2.0"
#else
#	define SNAP_VERSION_W L"1.0"
#endif
