#include "pch.h"
#include <algorithm>
#include <commdlg.h>
#include <cstdio>
#include <cwchar>
#include <dwmapi.h>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>
#include "../App.h"
#include "../Lang.h"
#if SNAP_HAS_OCR
#include "../Ocr/OcrEngine.h"     // 只有带引擎的版本才编 OCR 页
#endif
#include "../Setting.h"
#include "../Util.h"
#include "WinCap.h"
#include "WinQr.h"
#include "ZipKill.h"

using Microsoft::WRL::ComPtr;

namespace {
	std::unique_ptr<WinQr> winQr;

	// ———————————————— 配色：与参考同一套值 ————————————————
	// 参考是"科学和谐的现代蓝体系"，#3867E0 主操作蓝、#E44848 危险红。
	// 这里全部原值照搬，连悬停/按下的三档都一致，才不会出现"颜色差不多但就是不像"
	const COLORREF C_PRIMARY = RGB(56, 103, 224);
	const COLORREF C_DANGER = RGB(228, 72, 72);
	const COLORREF C_TEXT = RGB(45, 57, 74);
	const COLORREF C_BORDER = RGB(208, 216, 228);
	const COLORREF C_FILL_GR = RGB(228, 234, 242);
	const COLORREF C_BG_TOP = RGB(247, 249, 252);
	const COLORREF C_TRACK = RGB(148, 168, 210);
	const COLORREF C_WHITE = RGB(255, 255, 255);
	const COLORREF C_BTN_GR = RGB(203, 213, 225);
	const COLORREF C_BTN_BLUE = RGB(3, 83, 182);
	const COLORREF C_TAB_BG = RGB(226, 232, 240);
	const COLORREF C_TAB_BAR = RGB(37, 99, 235);
	const COLORREF C_TAB_TX = RGB(71, 85, 105);
	// 效验结论用色：一致=绿、不一致=红、未定=灰（红绿只表达"通过/不通过"，
	// 与行情涨跌无关，所以不受红涨绿跌那套约定影响）
	const COLORREF C_OK_GREEN = RGB(21, 128, 61);
	const COLORREF C_WARN_RED = RGB(220, 38, 38);
	const COLORREF C_TEXT_MUTED = RGB(120, 132, 150);
	// 数字格里单位字（天/时/分/秒）的颜色。它是注释性的小字，得比数字弱一档，
	// 但用 C_TEXT_MUTED 又灰得像"禁用"；取一组比正文更淡、偏冷的石板灰，
	// 和蓝色按钮、灰色边框是同一色系，看着才是一套：
	// 比正文(45,57,74) 亮 25% 左右，比重一档的灰(120,132,150) 深得多
	const COLORREF C_UNIT_TX = RGB(71, 85, 105);

	// 悬停 / 按下的"压暗"：把原色按比例往黑里混。
	// 早先悬停是给每种颜色各挑一个更亮的色号（灰按钮变浅蓝灰、蓝按钮变亮蓝），
	// 结果浅色按钮反而"泛白"，像蒙了层雾。改成在原色上压一档，白、灰、蓝、红
	// 共用同一条规则 —— 这和工具条那边 setHoverBg(0x00000014) 的 8% 黑是同一档
	inline COLORREF dim(COLORREF c, int pct)
	{
		return RGB(GetRValue(c) * (100 - pct) / 100,
			GetGValue(c) * (100 - pct) / 100,
			GetBValue(c) * (100 - pct) / 100);
	}
	constexpr int kHoverDim = 8;    // 悬停压暗 8%（= 工具条的 0x14 透明度）
	constexpr int kPressDim = 20;   // 按下压 20%，明显比悬停深一档

	// ———————————————— 布局：96dpi 基准值 ————————————————
	constexpr int CW = 585;
	constexpr int CH = 445;
	constexpr int MARGIN = 14;
	constexpr int INPUT_H = 101;
	constexpr int TAB_H = 30;
	constexpr int QR_BTN_W = 132;
	constexpr int QR_BTN_H = 46;
	constexpr int QR_BTN_STEP = 58;          // 46 + 12 间隙
	constexpr int QR_SIDE = 5 * 46 + 4 * 12; // 278，正好等于按钮列总高
	constexpr int QR_TOP = 42 + INPUT_H + 10; // 153

	// 二维码卡片左边界：卡片是正方形，边长等于按钮列总高，贴右边距
	constexpr int QR_CARD_X = CW - MARGIN - QR_SIDE;        // 293

	// 左列按钮保持原来的 132 宽，页面上只剩这一列；
	// 160..279 那条原本摆着右列两颗（截图 / 关压缩软件）的空档就空着 ——
	// 按钮不拉长去填，短按钮在这一页读起来才是"一列动作"，拉满反而抢了卡片的视线

	// 标题框 72..128，字在其中竖直居中 —— 视觉中心落在 100。
	// 卡在标签栏底（30）与数字格顶（168）的正中：上下留白 47/40，
	// 眼睛看着才是"标题属于下面这一组"，原来 84 时下面只剩 34，整块被压得偏低。
	constexpr int SD_Y_TITLE = 72;
	constexpr int SD_TITLE_H = 56;
	// 竖向节奏：标题 72..128，往下到数字格 168、再 30 到按钮行、再 30 到状态条。
	// 块间步进 40/30/30（数字格与按钮/状态条之间原本就是 30，保持不动）；
	// 数字格 168..212、按钮 242..292、状态条 322..362
	constexpr int SD_Y_ED = 168;
	// 数字格：白底 + 1px 边框由父窗口画，EDIT 无边框、在格子里垂直居中
	constexpr int SD_ED_W = 87;   // 格子宽
	constexpr int SD_ED_H = 44;   // 格子高
	constexpr int SD_ED_PAD = 5;  // EDIT 离格子左右各缩进多少
	// 单位字（天/时/分/秒）从格子上方那条蓝底标签栏搬进了格子内部：
	// 贴着格子右边、在 ▲▼ 左边，和数字同一行 —— 这样"0 天"是一个整体，
	// 眼睛不用在上下两行之间来回对。宽度按单位字那一号字里最宽的"天"给：
	// 单位和数字现在是同一号（28）粗体，26 只够塞 22 号字，会把这行切成两截
	constexpr int SD_UNIT_W = 30;   // 单位字占的宽度
	constexpr int SD_UNIT_GAP = 4;  // 单位字与格子右边框的间距
	constexpr int SD_EDIT_W = SD_ED_W - SD_UNIT_W - SD_UNIT_GAP - SD_ED_PAD; // 48，数字自己那格
	constexpr int SD_Y_BTN = 242;
	constexpr int SD_Y_BAR = 322;
	constexpr int SD_BAR_H = 40;
	constexpr int SD_GX[4] = { 14, 157, 300, 443 };

	// —— 设置页 ——
	// 整页内容装进一张白卡片：页面底色是浅灰，控件直接散在上面时谈不上"分组"；
	// 卡片左右各留 42（页宽 585 → 卡片 500 居中），上边离页签 8，下边留 13。
	// 卡片内部按"组标题 + 分隔线 + 行"三段式排，三列贯穿全页：
	//   标签 58..190 / 值 194..440 / 动作 448..526
	// 快捷键行、保存位置行都落在这三条竖线上，看着才是一张表而不是三堆控件
	constexpr int SET_CARD_X = 42;
	constexpr int SET_CARD_Y = 40;
	constexpr int SET_CARD_W = 500;
	// 常规组六颗勾选排成 2 行 3 列：行距放开到 40，卡片相应收到 382
	constexpr int SET_CARD_H = 382;
	constexpr int SET_PAD = 16;
	constexpr int SET_LBL_X = SET_CARD_X + SET_PAD;             // 58
	constexpr int SET_LBL_W = 132;
	constexpr int SET_VAL_X = SET_LBL_X + SET_LBL_W + 4;        // 194
	constexpr int SET_VAL_W = 246;
	constexpr int SET_ACT_X = SET_VAL_X + SET_VAL_W + 8;        // 448
	constexpr int SET_ACT_W = 78;
	constexpr int SET_ROW_H = 28;
	constexpr int SET_CHK_H = 24;
	// 勾选块 3 列 2 行：内容宽 468，三等分正好 148 + 12 间隙（58 / 218 / 378）；
	// 行距 40（框高 24 + 16 空档），比原来的 30 松一档，两行之间不再贴着
	constexpr int SET_CHK_W = 148;
	constexpr int SET_CHK_PITCH_X = 160;
	constexpr int SET_CHK_PITCH = 40;
	constexpr int SET_TITLE_H = 24;
	constexpr int SET_GAP_TITLE = 4;   // 标题 → 分隔线
	constexpr int SET_GAP_LINE = 8;    // 分隔线 → 第一行
	// 三组各自的标题顶边。组内节奏一致（标题 24 + 4 + 分隔线 + 8 + 行），
	// 上一组最后一行底边到下一组标题顶边固定 12
	constexpr int SET_G1_Y = 56;    // 快捷键
	constexpr int SET_G2_Y = 200;   // 保存位置
	constexpr int SET_G3_Y = 306;   // 常规（六颗勾选 2 行 3 列）
	constexpr int SET_G1_ROW_Y = SET_G1_Y + SET_TITLE_H + SET_GAP_TITLE + SET_GAP_LINE; // 92
	constexpr int SET_G1_PITCH = 34;
	constexpr int SET_G2_ROW_Y = SET_G2_Y + SET_TITLE_H + SET_GAP_TITLE + SET_GAP_LINE; // 236
	constexpr int SET_G2_PITCH = 36;
	constexpr int SET_G3_ROW_Y = SET_G3_Y + SET_TITLE_H + SET_GAP_TITLE + SET_GAP_LINE; // 342

	// 静区只要 1 格：码本身贴边更紧凑，识别率靠对比度而不是靠白边撑着
	constexpr int kQuietModule = 1;
	constexpr int kQrPixels = 600;

	// 类名带品牌前缀（SnapCore_Main / SnapOCR_Main）：两版同时运行时同名类会让
	// 后启动那版 RegisterClassExW 失败，主界面直接建不出来
	const wchar_t* kClassName = SNAP_CLS_MAIN;
	const wchar_t* kToastClass = SNAP_CLS_TOAST;

	// 「界面收在托盘时按识别热键」的延后显示标记。
	// 置位期间 init() 不把窗口亮出来（连首次建窗都用 SW_HIDE），等识别结果落进框里
	// 再由 revealAfterGrab() 亮 —— 用户要的是"看到结果"，不是"先看到窗口再干等结果"。
	// 窗口本来就在屏幕上时不置位，走老路径：框选完把字直接填进对应的框，窗口不动
	static bool g_deferReveal = false;
	// 按识别热键那一刻界面在不在屏幕上。只用来决定"取消框选之后要不要把它放回去"：
	// 原本在屏幕上的，取消后回到原样；原本收在托盘里的，取消后继续待着，别突然冒出来
	static bool g_wasOnScreen = false;

	// ———————————————— 控件分组 ————————————————
	// 建控件、DPI 重排、切页显隐、悬停命中这四件事原来各抄一遍 id 字面量，
	// 加一个控件得改四处、漏一处就是"这一页从此不显示某个按钮"。这里一处定义，四处复用。
	// 顺序即布局顺序，重排时按下标取坐标
	// 页签顺序即从左到右的显示顺序，重排时按下标取坐标（tabX / tabW 同步）
	constexpr int kTabIds[] = {
#if SNAP_HAS_OCR
		WinQr::TabOcr,
#endif
		WinQr::TabQr, WinQr::TabVerify, WinQr::TabRename, WinQr::TabShutdown, WinQr::TabSetting
	};
#if SNAP_HAS_OCR
	// —— 文字识别页（主界面）——
	// 版面：顶部一条"结果内查找"的输入框 + 右侧查找/排版两颗按钮，
	// 中间一整块多行结果框，底下 识别 / 复制 / 清空 三颗 125×46 的按钮，
	// 第四格（原来是「关压缩软件」那颗）改放字数额。
	// 顶行少了字数额这一格，搜索框就一路撑到两颗按钮跟前：
	// 14..391 搜索，403..481 查找，493..571 排版 —— 右边界仍收在 571
	constexpr int OCR_SEARCH_W = 377;
	constexpr int OCR_FIND_X = 403;
	constexpr int OCR_FIND_W = 78;
	constexpr int OCR_LAYOUT_X = 493;
	constexpr int OCR_LAYOUT_W = 78;
	constexpr int OCR_EDIT_Y = 80;
	constexpr int OCR_EDIT_H = 295;
	constexpr int OCR_BTN_Y = 385;
	constexpr int OCR_BTN_W = 125;
	constexpr int OCR_BTN_H = 46;
	constexpr int OCR_BTN_GX[3] = { 14, 158, 302 };
	// 字数额住第四格：与三颗按钮同一行、同一高，右对齐贴页边 ——
	// 整块看着仍是"四格"，只是最后一格从按钮换成读数
	constexpr int OCR_COUNT_X = 446;
	constexpr int OCR_COUNT_W = 125;
	constexpr int kOcrIds[] = {
		WinQr::OcrSearch, WinQr::OcrBtnFind, WinQr::OcrEdit,
		WinQr::OcrBtnRec, WinQr::OcrBtnCopy, WinQr::OcrBtnClear,
		WinQr::OcrLblCount, WinQr::OcrBtnLayout
	};
#endif
	constexpr int kQrBtnIds[] = {
		WinQr::QrBtnClear, WinQr::QrBtnScreen, WinQr::QrBtnCopy,
		WinQr::QrBtnDownload, WinQr::QrBtnUpload
	};
	constexpr int kQrIds[] = {
		WinQr::QrEdit, WinQr::QrBtnClear, WinQr::QrBtnScreen, WinQr::QrBtnCopy,
		WinQr::QrBtnDownload, WinQr::QrBtnUpload
	};
	constexpr int kSdLblIds[] = {
		WinQr::SdLblDay, WinQr::SdLblHour, WinQr::SdLblMin, WinQr::SdLblSec
	};
	constexpr int kSdEditIds[] = {
		WinQr::SdEditDay, WinQr::SdEditHour, WinQr::SdEditMin, WinQr::SdEditSec
	};
	constexpr int kSdUpIds[] = {
		WinQr::SdUpDay, WinQr::SdUpHour, WinQr::SdUpMin, WinQr::SdUpSec
	};
	constexpr int kSdDnIds[] = {
		WinQr::SdDnDay, WinQr::SdDnHour, WinQr::SdDnMin, WinQr::SdDnSec
	};
	constexpr int kSdBtnIds[] = { WinQr::SdStart, WinQr::SdPause, WinQr::SdReset };
	// 八个箭头：▲▼ 只是步进方向不同，启用态、命中判定、重绘永远一起走，
	// 分成两个数组各写一遍循环等于同一件事做两遍
	constexpr int kSdArrowIds[] = {
		WinQr::SdUpDay, WinQr::SdDnDay, WinQr::SdUpHour, WinQr::SdDnHour,
		WinQr::SdUpMin, WinQr::SdDnMin, WinQr::SdUpSec, WinQr::SdDnSec
	};
	// 四格的手输上限统一 99（sdValue 就是按这个夹的，手打超了在 EN_CHANGE 里就按回 99），
	// 但 ▲▼ 的环绕上限按各自单位走：时 23、分/秒 59、天 99。按下标与 kSdEditIds 对齐
	constexpr int SdFieldMax = 99;
	constexpr int kSdFieldMax[] = { SdFieldMax, 23, 59, 59 };
	// 快捷键三行：截图 / 识别 / 压缩软件全关
	constexpr int kSetLblIds[] = { WinQr::SetLblCap, WinQr::SetLblQr, WinQr::SetLblZip };
	constexpr int kSetDispIds[] = { WinQr::SetDispCap, WinQr::SetDispQr, WinQr::SetDispZip };
	constexpr int kSetRecIds[] = { WinQr::SetRecCap, WinQr::SetRecQr, WinQr::SetRecZip };
	// 保存位置两行：截图 / 视频。顺序即从上到下
	constexpr int kSetDirLblIds[] = { WinQr::SetDirShotLbl, WinQr::SetDirVideoLbl };
	constexpr int kSetDirDispIds[] = { WinQr::SetDirShotDisp, WinQr::SetDirVideoDisp };
	constexpr int kSetDirBtnIds[] = { WinQr::SetDirShotBtn, WinQr::SetDirVideoBtn };
	constexpr int kSetChkIds[] = {
		WinQr::SetChkAutoStart, WinQr::SetChkZipGuard,
		WinQr::SetChkHideMain, WinQr::SetChkCloseTray,
		WinQr::SetChkSingleInstance, WinQr::SetChkShowMainOnStart
	};
	// 设置页三行的 shortcutKey 子键名，与 Setting::allShortcutTypes() 里的一致。
	// qrcap 这一项对外已经改叫「识别」：它在二维码页先当扫码用，其余情况当文字识别用
	constexpr int kSetRowCount = 3;
	constexpr const wchar_t* kSetRowTypes[kSetRowCount] = { L"cap", L"qrcap", L"zipkill" };
	constexpr int kSetIds[] = {
		WinQr::SetLblCap, WinQr::SetLblQr, WinQr::SetLblZip,
		WinQr::SetDispCap, WinQr::SetDispQr, WinQr::SetDispZip,
		WinQr::SetRecCap, WinQr::SetRecQr, WinQr::SetRecZip,
		WinQr::SetDirShotLbl, WinQr::SetDirVideoLbl,
		WinQr::SetDirShotDisp, WinQr::SetDirVideoDisp,
		WinQr::SetDirShotBtn, WinQr::SetDirVideoBtn,
		WinQr::SetChkAutoStart, WinQr::SetChkZipGuard, WinQr::SetChkHideMain, WinQr::SetChkCloseTray,
		WinQr::SetChkSingleInstance, WinQr::SetChkShowMainOnStart,
	};
	// 改名页那一整页的控件由 WinRename.cpp 建、由它显隐，这里只负责悬停命中
	// 只放自绘按钮：列表 / 编辑框 / 下拉不参与悬停高亮，混进来只会多刷几次屏
	constexpr int kRnIds[] = {
		WinQr::RnAddFiles, WinQr::RnAddFolder, WinQr::RnCopy, WinQr::RnClear,
		WinQr::RnReset, WinQr::RnUndo, WinQr::RnRun,
		WinQr::RnCaseUpper, WinQr::RnCaseLower, WinQr::RnCaseTitle, WinQr::RnNumEnable,
		WinQr::RnRepCase, WinQr::RnRepAll, WinQr::RnSeqOn, WinQr::RnAffixOn,
		WinQr::RnExtUpper, WinQr::RnExtLower, WinQr::RnExtKeep, WinQr::RnExtDrop,
		WinQr::RnCheckAll, WinQr::RnInvert
	};
	// 悬停高亮的候选：只要是自绘按钮就都在里面
	constexpr int kHoverIds[] = {
#if SNAP_HAS_OCR
		WinQr::TabOcr,
		WinQr::OcrBtnRec, WinQr::OcrBtnCopy, WinQr::OcrBtnClear, WinQr::OcrBtnFind,
		WinQr::OcrBtnLayout,
#endif
		WinQr::TabQr, WinQr::TabShutdown, WinQr::TabVerify, WinQr::TabRename, WinQr::TabSetting,
		WinQr::QrBtnClear, WinQr::QrBtnScreen, WinQr::QrBtnCopy,
		WinQr::QrBtnDownload, WinQr::QrBtnUpload,
		WinQr::SdUpDay, WinQr::SdDnDay, WinQr::SdUpHour, WinQr::SdDnHour,
		WinQr::SdUpMin, WinQr::SdDnMin, WinQr::SdUpSec, WinQr::SdDnSec,
		WinQr::SdStart, WinQr::SdPause, WinQr::SdReset,
		WinQr::SetRecCap, WinQr::SetRecQr, WinQr::SetRecZip,
		WinQr::SetDirShotBtn, WinQr::SetDirVideoBtn,
		WinQr::VfChkVer, WinQr::VfChkDate, WinQr::VfChkMd5, WinQr::VfChkSha256, WinQr::VfChkCrc,
		WinQr::SetChkAutoStart, WinQr::SetChkZipGuard, WinQr::SetChkHideMain, WinQr::SetChkCloseTray,
		WinQr::SetChkSingleInstance, WinQr::SetChkShowMainOnStart
	};
	// 设置页三行的类型名，与 Setting 里的 shortcutKey 子键同名。
	// 真正的快捷键类型表在 Setting::allShortcutTypes()（cap / qrcap / hide / zipkill），
	// 这里只服务设置页 UI

	template <size_t N>
	bool hasId(const int (&ids)[N], int id)
	{
		for (int v : ids) if (v == id) return true;
		return false;
	}

	// 唯一存活实例：截图回调持有的是裸 this，窗口先一步销毁时靠它识别野指针
	WinQr* g_live = nullptr;

	std::string toUtf8(const std::wstring& s)
	{
		if (s.empty()) return {};
		const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
		if (n <= 0) return {};
		std::string out((size_t)n, '\0');
		WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
		return out;
	}

	std::wstring fromUtf8(const std::string& s)
	{
		if (s.empty()) return {};
		// 先严格校验 UTF-8（MB_ERR_INVALID_CHARS）：合法就直接转。
		// 老二维码生成器常用 GBK/ANSI 编码，UTF-8 校验必然失败——这时按本地
		// ANSI 代码页再转一次，"识别出乱码"就变成"识别出中文"；
		// 真二进制载荷两次都转不出有意义内容，行为与从前一致（有损替换）
		const int nu = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
		if (nu > 0) {
			std::wstring out((size_t)nu, L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), out.data(), nu);
			return out;
		}
		const int na = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
		if (na <= 0) return {};
		std::wstring out((size_t)na, L'\0');
		MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), out.data(), na);
		return out;
	}

	bool isArrowCtrl(int id)
	{
		return hasId(kSdArrowIds, id);
	}

	// 快捷键行右侧那格"副编辑器"：外观做成输入框，行为仍是按钮（点一下开始录）
int setRecIndex(int id)
{
	for (int i = 0; i < kSetRowCount; i++) if (kSetRecIds[i] == id) return i;
	return -1;
}

	// 候选主键。与 ui::Util::strToKey 认得的名字严格对齐，否则录下来的组合注册不上
	const int kCandidateVks[] = {
		'0','1','2','3','4','5','6','7','8','9',
		'A','B','C','D','E','F','G','H','I','J','K','L','M',
		'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
		VK_F1,VK_F2,VK_F3,VK_F4,VK_F5,VK_F6,VK_F7,VK_F8,VK_F9,VK_F10,VK_F11,VK_F12,
		VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,
		VK_INSERT,VK_DELETE,VK_HOME,VK_END,VK_PRIOR,VK_NEXT,
		VK_SNAPSHOT,VK_SCROLL,VK_PAUSE
	};

	std::wstring vkToName(int vk)
	{
		if (vk >= 'A' && vk <= 'Z') return std::wstring(1, (wchar_t)vk);
		if (vk >= '0' && vk <= '9') return std::wstring(1, (wchar_t)vk);
		if (vk >= VK_F1 && vk <= VK_F12) return L"F" + std::to_wstring(vk - VK_F1 + 1);
		switch (vk) {
		case VK_UP: return L"Up";
		case VK_DOWN: return L"Down";
		case VK_LEFT: return L"Left";
		case VK_RIGHT: return L"Right";
		case VK_INSERT: return L"Insert";
		case VK_DELETE: return L"Delete";
		case VK_HOME: return L"Home";
		case VK_END: return L"End";
		case VK_PRIOR: return L"PageUp";
		case VK_NEXT: return L"PageDown";
		case VK_SNAPSHOT: return L"PrintScreen";
		case VK_SCROLL: return L"ScrollLock";
		case VK_PAUSE: return L"Pause";
		default: return {};
		}
	}

	// 不配修饰键、单独按下也能当热键的那一类：它们本来就不承担输入职能，占着不碍事。
	// 字母、数字、方向键、Insert/Home/End/PageUp/PageDown 单独设上就等于把打字和导航抢走了 ——
	// 注册成功后用户一敲键盘就开始截图，而且他很难联想到是截图工具干的
	bool canBeUsedAlone(const std::wstring& name)
	{
		if (name.size() >= 2 && name[0] == L'F') {
			for (size_t i = 1; i < name.size(); i++) {
				if (name[i] < L'0' || name[i] > L'9') return false;
			}
			return true;
		}
		return name == L"PrintScreen" || name == L"ScrollLock" || name == L"Pause";
	}

	// 抢不得的那一小批。前一组是所有软件里手指记忆最强的编辑键，抢了用户会难受；
	// 后一组被系统独占，设上了也注册不成功（Ctrl+Alt+Delete 连消息都到不了应用）。
	// 其余组合一律放开，冲不冲突交给用户自己判断
	bool isReservedShortcut(const std::wstring& shortcut)
	{
		static const wchar_t* const reserved[] = {
			L"Ctrl+C", L"Ctrl+V", L"Ctrl+X", L"Ctrl+Z", L"Ctrl+Y", L"Ctrl+A", L"Ctrl+S",
			L"Ctrl+Alt+Delete", L"Ctrl+Shift+Esc", L"Ctrl+Esc",
			L"Alt+Tab", L"Alt+Esc", L"Alt+F4",
			L"Win+L", L"Win+D", L"Win+E", L"Win+R", L"Win+Tab",
		};
		for (const wchar_t* r : reserved) {
			if (_wcsicmp(r, shortcut.c_str()) == 0) return true;
		}
		return false;
	}

	std::wstring remainText(long long sec)
	{
		if (sec < 0) sec = 0;
		const long long d = sec / 86400;
		const long long h = (sec % 86400) / 3600;
		const long long m = (sec % 3600) / 60;
		const long long s = sec % 60;
		wchar_t buf[64]{};
		// 「天」这个单位走语言包（与页面上其它单位同一个键）
		const auto dayUnit = Lang::get(L"qr.day");
		if (d > 0) swprintf_s(buf, L"%lld %s %02lld:%02lld:%02lld", d, dayUnit.c_str(), h, m, s);
		else swprintf_s(buf, L"%02lld:%02lld:%02lld", h, m, s);
		return buf;
	}

	// LOGO 盖在二维码中央：先垫一块白，再最近邻贴图。
	// 不能用插值 —— 二维码模块边界一旦糊掉，识别率掉得比 LOGO 好看重要得多
	void overlayLogo(std::vector<BYTE>& dst, int dstW, int dstH,
		const std::vector<BYTE>& src, int srcW, int srcH, int side)
	{
		if (src.empty() || srcW <= 0 || srcH <= 0 || side <= 0) return;
		const int cx = dstW / 2, cy = dstH / 2;
		const int x0 = cx - side / 2, y0 = cy - side / 2;
		const int pad = side / 10;
		const int bx0 = x0 - pad, by0 = y0 - pad;
		const int bw = side + pad * 2, bh = side + pad * 2;
		for (int y = by0; y < by0 + bh; y++) {
			if (y < 0 || y >= dstH) continue;
			for (int x = bx0; x < bx0 + bw; x++) {
				if (x < 0 || x >= dstW) continue;
				BYTE* p = dst.data() + ((size_t)y * dstW + x) * 4;
				p[0] = p[1] = p[2] = 255; p[3] = 255;
			}
		}
		for (int y = 0; y < side; y++) {
			const int sy = (int)((long long)y * srcH / side);
			for (int x = 0; x < side; x++) {
				const int sx = (int)((long long)x * srcW / side);
				const int dx = x0 + x, dy = y0 + y;
				if (dx < 0 || dx >= dstW || dy < 0 || dy >= dstH) continue;
				const BYTE* s = src.data() + ((size_t)sy * srcW + sx) * 4;
				if (s[3] < 8) continue;
				BYTE* d = dst.data() + ((size_t)dy * dstW + dx) * 4;
				if (s[3] >= 248) {          // 不透明直接贴
					d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
				}
				else {                      // 半透明：over 混合到白底上，消除硬边
					const UINT a = s[3], ia = 255 - a;
					d[0] = (BYTE)((s[0] * a + d[0] * ia + 127) / 255);
					d[1] = (BYTE)((s[1] * a + d[1] * ia + 127) / 255);
					d[2] = (BYTE)((s[2] * a + d[2] * ia + 127) / 255);
				}
				d[3] = 255;
			}
		}
	}

	// WM_PRINT / PrintWindow 的转交：标准控件支持 WM_PRINT，但 DC 的原点是自己的客户区，
	// 所以要按控件在父窗口里的位置挪一次视口原点，否则所有子控件会叠在左上角。
	// 自绘按钮（BS_OWNERDRAW）是另一码事：Windows 的按钮过程在 WM_PRINT 里什么也不画，
	// 内容是父窗口在 WM_DRAWITEM 里画的。只转 WM_PRINT 的话，抓出来就是"背景 + 空白按钮"
	HDC g_printDc = nullptr;
	UINT g_printFlags = 0;
	BOOL CALLBACK printChildProc(HWND child, LPARAM)
	{
		if (!g_printDc || !IsWindowVisible(child)) return TRUE;
		HWND parent = GetParent(child);
		if (!parent) return TRUE;
		RECT cr{};
		if (!GetWindowRect(child, &cr)) return TRUE;
		POINT pt{ cr.left, cr.top };
		ScreenToClient(parent, &pt);
		const int w = cr.right - cr.left;
		const int h = cr.bottom - cr.top;

		POINT old{};
		SetViewportOrgEx(g_printDc, pt.x, pt.y, &old);
		const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
		if ((style & BS_TYPEMASK) == BS_OWNERDRAW) {
			// rcItem 从 0 起算：视口原点已经挪到控件左上角了
			DRAWITEMSTRUCT dis{};
			dis.CtlType = ODT_BUTTON;
			dis.CtlID = (UINT)GetDlgCtrlID(child);
			dis.itemAction = ODA_DRAWENTIRE;
			dis.hwndItem = child;
			dis.hDC = g_printDc;
			dis.rcItem = RECT{ 0, 0, w, h };
			SendMessageW(parent, WM_DRAWITEM, (WPARAM)dis.CtlID, reinterpret_cast<LPARAM>(&dis));
		}
		else {
			SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(g_printDc), g_printFlags);
		}
		SetViewportOrgEx(g_printDc, old.x, old.y, nullptr);
		return TRUE;
	}
}

// ———————————————— 生命周期 ————————————————

WinQr::WinQr()
{
	static bool classRegistered = false;
	if (!classRegistered) {
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = wndProcEntry;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hIcon = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
			GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.hbrBackground = nullptr;
		wc.lpszClassName = kClassName;
		wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
			GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
		if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
		if (!wc.hIconSm) wc.hIconSm = wc.hIcon;
		RegisterClassExW(&wc);
		classRegistered = true;
	}

	// 参考用整机 DPI 起步，窗口跟着主屏缩放；后面 WM_DPICHANGED 再按实际屏修正
	HDC screenDc = GetDC(nullptr);
	if (screenDc) {
		const int d = GetDeviceCaps(screenDc, LOGPIXELSX);
		if (d > 0) dpi = d;
		ReleaseDC(nullptr, screenDc);
	}
	if (dpi <= 0) dpi = 96;

	// 固定大小、不可拉伸：布局是一格一格钉死的，允许改尺寸只会让画面东倒西歪
	const DWORD style = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_MINIMIZEBOX;
	RECT rc{ 0, 0, sc(CW), sc(CH) };
	AdjustWindowRectEx(&rc, style, FALSE, 0);
	const int winW = rc.right - rc.left;
	const int winH = rc.bottom - rc.top;
	const int x = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
	const int y = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

	const auto title = Lang::get(L"qr.title");
	hwnd = CreateWindowExW(0, kClassName, title.c_str(), style,
		x, y, winW, winH, nullptr, nullptr, GetModuleHandleW(nullptr), this);
	if (!hwnd) return;

	// 这里不给主窗口挂 WDA_EXCLUDEFROMCAPTURE：那会让窗口对所有截屏工具隐身，
	// 用户拿别的软件截图时界面直接消失。识别二维码那一路自己在抓屏前 Hide 过了，
	// 不需要牺牲主界面的可见性
	// Win11 给顶层窗口自动加 8px 圆角，跟"全直角"的版式冲突。
	// 属性只在 Win11 上有效，老系统上调用失败即可忽略
	const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

	// 接收拖进来的文件（文件校验页的主要用法：把文件拖进窗口直接看哈希）
	DragAcceptFiles(hwnd, TRUE);

	// 托盘态触发识别时静默建窗：控件全建好（结果得有地方填），但先不露面，
	// 等识别结果落进框里再亮（见 decodeFromScreen / revealAfterGrab）
	ShowWindow(hwnd, g_deferReveal ? SW_HIDE : SW_SHOW);
	if (!g_deferReveal) UpdateWindow(hwnd);
	g_live = this;
}

WinQr::~WinQr()
{
	if (g_live == this) g_live = nullptr;
	destroyWindow();
	if (qrHbmp) {
		DeleteObject(qrHbmp);
		qrHbmp = nullptr;
		qrHbmpSide = 0;
		qrDibBits = nullptr;
	}
	for (auto& kv : fontCache) {
		if (kv.second) DeleteObject(kv.second);
	}
	fontCache.clear();
	for (auto& kv : brushCache) {
		if (kv.second) DeleteObject(kv.second);
	}
	brushCache.clear();
	setFont = editFont = sdEditFont = sdLabelFont = nullptr;
}

void WinQr::destroyWindow()
{
	// 先让校验线程收手并等它退出：它会在结束后 PostMessage 到 hwnd，
	// 窗口先没了倒不至于崩，但 this 会变成野指针
	vfCancel = true;
	vfJoinWorker();
	if (toastHwnd && IsWindow(toastHwnd)) {
		DestroyWindow(toastHwnd);
	}
	toastHwnd = nullptr;
	// 双缓冲画布跟着窗口一起收：它是挂在窗口 DC 上的兼容位图，留着没意义还占约 1MB
	if (bufDc) { DeleteDC(bufDc); bufDc = nullptr; }
	if (bufBmp) { DeleteObject(bufBmp); bufBmp = nullptr; }
	bufSize = SIZE{};
	if (hwnd && IsWindow(hwnd)) {
		// 先把 userdata 摘掉：DestroyWindow 会发 WM_DESTROY / WM_NCDESTROY，
		// 留着 this 的话窗口过程会在半析构对象上再走一遍成员函数
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		DestroyWindow(hwnd);
	}
	hwnd = nullptr;
}

void WinQr::init()
{
	if (winQr) {
		if (winQr->hwnd) {
			// 托盘态触发的识别：窗口本来就该保持隐身，别在这里提前亮出来。
			// 其余调用方（双击 exe、托盘"打开主界面"）标记是假，照旧拉到前台
			if (g_deferReveal) return;
			if (IsIconic(winQr->hwnd)) ShowWindow(winQr->hwnd, SW_RESTORE);
			ShowWindow(winQr->hwnd, SW_SHOW);
			SetForegroundWindow(winQr->hwnd);
			return;
		}
		// 上一次 CreateWindowEx 失败留下的空壳：不清掉会永远卡在"对象存在但没窗口"，
		// 之后每次 init 都进上面那个分支、什么也不做，界面再也出不来
		winQr.reset();
	}
	winQr.reset(new WinQr());
	// 【这里**不**预热 D2D】主界面是纯 GDI 绘制，压根不碰 deviceContext；在这里拉一把
	// 能把首次截图的设备创建成本（30ms 起）提前到启动时，但代价是整套 D2D + D3D + DXGI
	// 常驻：实测私有内存 +4.7MB（主界面开着时 8.1 → 12.8MB）。
	// 现在改成按需创建（第一次截图 / 识码时才建）。失败路径不用另做 —— WinCap::init
	// 建不出设备时会关窗返回 false，startCapture 已接住并弹"图形设备初始化失败"的提示
}

void WinQr::dispose()
{
	if (!winQr) return;
	winQr->destroyWindow();
	winQr.reset();
}

void WinQr::decodeFromScreen()
{
	// 流程：按下识别键 → 框选 → 识别 → 结果落进对应的框 → 界面带着结果弹出。
	// 这里**不再**无条件藏窗：要不要把主界面收起来、还是只是把它排除出捕获，
	// 由「截图时隐藏主界面」那条设置决定，统一交给 grabRecognize() → beginGrab() 处理
	// （那边会先试捕获亲和性，窗口不闪；亲和性不支持才降级 SW_HIDE）。
	// 早先这里写死了一句 SW_HIDE，结果前台按 F2 必然"窗口先消失一下"，
	// 而且把设置项架空 —— 按钮路径听设置的、热键路径不听。
	// 先置标记再 init：窗口还没建时 init 只会静默建出来（控件齐全，结果才有地方填），
	// 不会顺手 ShowWindow —— 亮不亮由 revealAfterGrab() 统一决定
	g_deferReveal = true;
	g_wasOnScreen = winQr && winQr->hwnd
		&& IsWindowVisible(winQr->hwnd) && !IsIconic(winQr->hwnd);
	init();
	if (winQr && winQr->hwnd) winQr->grabRecognize();
	else g_deferReveal = false;   // 窗口没建起来，别把这个标记留到下一次
}

void WinQr::revealAfterGrab()
{
	if (!g_deferReveal) return;
	// 先清标记再 showMain：showMain 里的 init() 看到标记为假才会真的显示
	g_deferReveal = false;
	g_wasOnScreen = false;
	showMain();
}

void WinQr::showSettingPage()
{
	// 用户明确要看界面（托盘菜单点进来的）：把"等识别结果再亮"的挂起状态作废，
	// 否则上一次识别被 Esc 取消后残留的标记会让这里点了没反应
	g_deferReveal = false;
	init();
	if (!winQr || !winQr->hwnd) return;
	// 托盘菜单进来时主界面可能是收起来的状态：只切页不显示，用户会以为点了没反应
	if (IsIconic(winQr->hwnd)) ShowWindow(winQr->hwnd, SW_RESTORE);
	ShowWindow(winQr->hwnd, SW_SHOW);
	SetForegroundWindow(winQr->hwnd);
	winQr->setPage(PageSetting);
}

void WinQr::showMain()
{
	// 防多开把"又启动一次"的请求转到这里：不该新开窗口，只是把已有的亮出来。
	// 主界面可能被托盘收起来了、也可能最小化了，两种都要还原。
	// 这是"明确要显示"的入口：顺手作废延后显示标记，免得被上一次取消的识别挂住
	g_deferReveal = false;
	init();
	if (!winQr || !winQr->hwnd) return;
	if (IsIconic(winQr->hwnd)) ShowWindow(winQr->hwnd, SW_RESTORE);
	ShowWindow(winQr->hwnd, SW_SHOW);
	SetForegroundWindow(winQr->hwnd);
}

bool WinQr::hasInstance()
{
	return winQr && winQr->hwnd;
}

void WinQr::toggleMainVisible()
{
	// 只认"窗口此刻在不在屏幕上"这一个状态：最小化也算不在 —— 用户按一下是想让它回来，
	// 不是想让它最小化得更彻底。不用 hideForCapture / restoreAfterCapture 那一对：
	// 那两个带着"截图前临时藏"的记账，截图收尾会自动放回来，而用户主动收起来的窗口
	// 不该在下次截图结束时自己弹出来
	if (winQr && winQr->hwnd && IsWindowVisible(winQr->hwnd) && !IsIconic(winQr->hwnd)) {
		ShowWindow(winQr->hwnd, SW_HIDE);
		return;
	}
	showMain();
}

// "截图前把主界面从捕获里摘出去"的记账。这一对标记只在截图前真的动作了才生效：
//   g_mainHiddenForCap —— 是否处于"为截图而隐身"状态；
//   g_mainExcludedByAffinity —— 这次隐身是用「捕获亲和性」做的（窗口还留在屏幕上，
//                              只是抓屏拿不到它），还是降级成 SW_HIDE（窗口真没了）。
// 前者是主路径：窗口全程可见，用户看不到任何"消失又恢复"的闪动 —— 这正是闪一下的根因
static bool g_mainHiddenForCap = false;
static bool g_mainExcludedByAffinity = false;
// 降级路径（老系统不支持 affinity）专用的两个标记：
//   g_mainWasMinimized —— 藏之前是最小化的，放回来还得是最小化的；
//   g_mainHideTick     —— 藏起来的那一刻，放回去时算"离藏起来过了多久"
static bool g_mainWasMinimized = false;
static ULONGLONG g_mainHideTick = 0;

// 降级路径里，从"藏起来"到"放回去"至少隔这么久（ms）。只有老系统才走得到这里：
// 截图窗口建失败、或者用户按了热键又立刻按 Esc，藏和放会落在同一条消息里，
// 屏幕表现就是主界面闪一下。隔开一档之后用户看到的要么是收起来、要么是还在，不会抖
static constexpr ULONGLONG kRestoreGapMs = 200;

bool WinQr::hideForCapture()
{
	if (g_mainHiddenForCap) return true;              // 已经隐过身了，别再覆盖原始状态
	if (!Setting::get()->getHideMainOnCap()) return false;
	if (!winQr || !winQr->hwnd) return false;
	if (!IsWindowVisible(winQr->hwnd)) return false;  // 本来就不在屏幕上，跟"隐藏"无关
	// 主路径：用「捕获亲和性」把主界面从抓屏里摘出去，但窗口照常显示。
	// 用户点热键的瞬间主界面纹丝不动，闪一下的根因在这里被连根拔掉
	if (App::setCaptureAffinity(winQr->hwnd, true)) {
		g_mainExcludedByAffinity = true;
		g_mainHiddenForCap = true;
		return true;
	}
	// 降级路径（老系统 <19041 不支持 affinity）：退回旧的 SW_HIDE 藏窗
	g_mainWasMinimized = IsIconic(winQr->hwnd) != FALSE;
	ShowWindow(winQr->hwnd, SW_HIDE);
	// ShowWindow 只改状态位，DWM 真正把它从屏幕上撤下来要等下一次合成。
	// 不等这一下，紧接着的全屏抓图常常还能抓到主界面的残影
	DwmFlush();
	g_mainHideTick = GetTickCount64();
	g_mainHiddenForCap = true;
	return true;
}

bool WinQr::mainHiddenForCap()
{
	return g_mainHiddenForCap;
}

void WinQr::restoreAfterCapture()
{
	if (!g_mainHiddenForCap) return;
	g_mainHiddenForCap = false;
	if (!winQr || !winQr->hwnd) return;
	// 主路径：窗口本来就没离开过屏幕，摘掉捕获标记就完事，无 show、无定时器、无抖动
	if (g_mainExcludedByAffinity) {
		g_mainExcludedByAffinity = false;
		App::setCaptureAffinity(winQr->hwnd, false);
		return;
	}
	// 降级路径：藏起来到现在还没过 kRestoreGapMs：不当场放，交给定时器到点再放。
	// 这一档就是老系统上"闪一下"的来源 —— 同一条消息里藏了又放，用户只看到窗口抖了一下
	const ULONGLONG now = GetTickCount64();
	const ULONGLONG elapsed = now - g_mainHideTick;
	if (elapsed < kRestoreGapMs) {
		SetTimer(winQr->hwnd, TidRestoreMain, (UINT)(kRestoreGapMs - elapsed), nullptr);
		return;
	}
	restoreMainNow();
}

// 真正把主界面摆回来（仅降级路径用到）。延后路径（TidRestoreMain）与立即路径共用这一份，
// 免得两边对"最小化的窗口怎么放回来"各写一半、以后改一处漏一处
void WinQr::restoreMainNow()
{
	if (!winQr || !winQr->hwnd) return;
	// SW_SHOWNA：放回来但别抢焦点 —— 截图刚结束，焦点该留给用户接下来要点的东西
	ShowWindow(winQr->hwnd, g_mainWasMinimized ? SW_SHOWMINNOACTIVE : SW_SHOWNA);
}

int WinQr::sc(int v) const
{
	return MulDiv(v, dpi, 96);
}

WinQr* WinQr::instanceOf(HWND h)
{
	return reinterpret_cast<WinQr*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

LRESULT CALLBACK WinQr::wndProcEntry(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_NCCREATE) {
		auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
		SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
	}
	auto self = instanceOf(h);
	if (!self) return DefWindowProcW(h, msg, wp, lp);
	if (!self->hwnd) self->hwnd = h;
	return self->wndProc(msg, wp, lp);
}

LRESULT CALLBACK WinQr::hintProcEntry(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_NCCREATE) {
		auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
		SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
	}
	auto self = instanceOf(h);
	if (!self) return DefWindowProcW(h, msg, wp, lp);
	return self->hintProc(h, msg, wp, lp);
}

LRESULT WinQr::wndProc(UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		createControls();
		relayout();
		// 这里必须走 applyPage，不能 setPage：page 的初值就是首页（带引擎是
		// PageOcr，不带是 PageQr，见 WinQr.h 的声明），setPage 见到
		// "目标页 == 当前页"会直接提前返回，于是各页控件一颗都藏不住 ——
		// 窗口第一次亮出来时六页内容全叠在一起，直到用户点了某个页签才恢复
		applyPage();
		SetTimer(hwnd, TidHover, 120, nullptr);
		return 0;

	case WM_NCACTIVATE:
		// 截图会话期间，截图窗口抢前台会让主界面失活（wp=FALSE），默认行为是
		// 重绘标题栏成非激活色。遮罩盖住时用户看不见，可截图结束遮罩一消失，
		// 露出来的是"灰标题栏"，紧接着才重新激活变蓝 —— 这一下就是"跳一下"。
		// 这里在截图会话期间屏蔽失活：标题栏始终按激活态画，遮罩消失即无缝衔接
		if (wp == FALSE && WinCap::get() && !WinCap::get()->isDead()) {
			return DefWindowProcW(hwnd, WM_NCACTIVATE, TRUE, lp);
		}
		break;

	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT:
		onPaint();
		return 0;

	case WM_PRINT:
		printTo(reinterpret_cast<HDC>(wp), (UINT)lp);
		return 0;

	case WM_PRINTCLIENT:
		// PrintWindow(PW_CLIENTONLY) 走这条：自己画内容，再把每个子控件画上来
		printTo(reinterpret_cast<HDC>(wp), PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
		return 0;

	case WM_DRAWITEM:
		onDrawItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
		return 0;

	case WM_KILLFOCUS: {
		// 录快捷键时焦点被夺走（Alt+Tab 切走、点了别的程序）：必须收尾。
		// 录入期间全局热键是注销的，人一走神没回来，整套快捷键就一直不响，
		// 界面上却看不出来任何异常。
		// 先把 prevFocus 清掉：endCapture 会 SetFocus(prevFocus) 把焦点抢回来，
		// 而此刻用户是主动要离开这个窗口
		if (captureRow >= 0) {
			// 例外：焦点只是落到那几颗「录入 / 取消」按钮上。点「取消」时按钮先抢走焦点，
			// 这里把录入收掉，紧接着那条 WM_COMMAND 就判成"这一行还没在录 → 开始录"，
			// 又起一轮 —— 按钮上明明写着"取消"，点下去反而录得更起劲。
			// 这两颗按钮的去留交给 WM_COMMAND，这里放行
			const HWND to = reinterpret_cast<HWND>(wp);
			if (to && IsChild(hwnd, to) && setRecIndex(GetDlgCtrlID(to)) >= 0) return 0;
			prevFocus = nullptr;
			endCapture();
		}
		return 0;
	}

	case WM_SETCURSOR:
		// 改名页那五个压线标签和底部那排格式勾选都是画出来的（没有 HWND），
		// 默认只有一支箭头；它们点得动，鼠标移上去就换成手形
		if (page == PageRename && LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == hwnd) {
			POINT pt{};
			GetCursorPos(&pt);
			ScreenToClient(hwnd, &pt);
			if (rnTabHit(pt.x, pt.y) >= 0 || rnFmtHit(pt.x, pt.y) >= 0) {
				SetCursor(LoadCursorW(nullptr, IDC_HAND));
				return TRUE;   // 吃掉：不再回落到窗口类的箭头
			}
		}
		break;

	case WM_LBUTTONDOWN: {
		// 改名页：底部那排格式勾选是画出来的（没有 HWND），命中在这里判
		if (page == PageRename) {
			const POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
			if (const int i = rnTabHit(pt.x, pt.y); i >= 0) {
				rnSetPanel(i);
				return 0;
			}
			if (const int i = rnFmtHit(pt.x, pt.y); i >= 0) {
				rnToggleFmt(i);
				return 0;
			}
		}
		// 数字格的白底/边框是父窗口画的，EDIT 比格子小一圈（字太高就居不了中）。
		// 点在格子留白上也得聚焦进框里，否则用户会觉得"这一圈点不动"。
		// 倒计时跑着的时候例外：格子是只读读数，抢来焦点会整串反白，看着像
		// "选中了就能改"，实际下一拍就被覆盖
		if (page == PageShutdown && !sdRunning) {
			const POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
			for (int i = 0; i < 4; i++) {
				const RECT cell{ sc(SD_GX[i]), sc(SD_Y_ED),
					sc(SD_GX[i] + SD_ED_W), sc(SD_Y_ED + SD_ED_H) };
				if (!PtInRect(&cell, pt)) continue;
				if (HWND ed = GetDlgItem(hwnd, kSdEditIds[i])) {
					SetFocus(ed);
					SendMessageW(ed, EM_SETSEL, 0, -1);   // 全选：直接打字就是替换
				}
				return 0;
			}
		}
		break;
	}

	case WM_DROPFILES: {
		// 改名页：拖进来的是"待改名的素材"，就地收进列表，不切页 ——
		// 用户正盯着规则那一行调，把页面切走等于把上下文抢掉
		if (page == PageRename) {
			rnDropFiles(reinterpret_cast<HDROP>(wp));
			return 0;
		}
		// 其余页：把文件拖到窗口上就开始校验：这类工具最常见的用法就是"拖进来看看哈希"

		auto drop = reinterpret_cast<HDROP>(wp);
		const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
		std::vector<std::wstring> files;
		files.reserve(n);
		for (UINT i = 0; i < n; i++) {
			const UINT len = DragQueryFileW(drop, i, nullptr, 0);
			if (len == 0) continue;
			// cch 是含结尾 0 的容量：缓冲必须留出那一格，否则多写的一个 0 落在
			// string 的堆块外面（和 D2D 里 GetString 那处是同一类越界）
			std::wstring path(len + 1, L'\0');
			DragQueryFileW(drop, i, path.data(), len + 1);
			path.resize(len);
			files.push_back(std::move(path));
		}
		DragFinish(drop);
		if (!files.empty()) {
			setPage(PageVerify);
			vfStart(std::move(files));
		}
		return 0;
	}

	case WM_CTLCOLORSTATIC: {
		const int id = GetDlgCtrlID((HWND)lp);
		HDC dc = reinterpret_cast<HDC>(wp);
		SetBkMode(dc, TRANSPARENT);
		if (id == SdLblDay || id == SdLblHour || id == SdLblMin || id == SdLblSec) {
			// 单位字在数字格里（右下角），底色得跟格子一致 ——
			// 不然会在这块白底上糊出一小条蓝。文字用 C_UNIT_TX：
			// 比数字浅一档，主次分明，又不至于灰成"禁用"
			SetBkColor(dc, C_WHITE);
			SetTextColor(dc, C_UNIT_TX);
			return reinterpret_cast<LRESULT>(brushOf(C_WHITE));
		}
		// 只读编辑框（快捷键显示框、保存位置路径框）也走 STATIC 这条通知：
		// 背景必须刷白，否则会跟页面底色糊成一片，看着像标签而不是文本框
		if (hasId(kSetDispIds, id) || hasId(kSetDirDispIds, id)) {
			SetBkColor(dc, C_WHITE);
			SetTextColor(dc, C_TEXT);
			return reinterpret_cast<LRESULT>(brushOf(C_WHITE));
		}
		// 设置页的标签站在白卡片上，底色也得跟着卡片走 —— 用页面底色会在
		// 卡片里留一块灰斑
		if (hasId(kSetLblIds, id) || hasId(kSetDirLblIds, id)) {
			SetBkColor(dc, C_WHITE);
			SetTextColor(dc, C_TEXT);
			return reinterpret_cast<LRESULT>(brushOf(C_WHITE));
		}
		SetTextColor(dc, C_TEXT);
		return reinterpret_cast<LRESULT>(brushOf(C_BG_TOP));
	}

	case WM_CTLCOLORBTN: {
		// 复选框：不刷底色就会在浅色页面上留一块系统灰
		HDC dc = reinterpret_cast<HDC>(wp);
		SetBkColor(dc, C_BG_TOP);
		return reinterpret_cast<LRESULT>(brushOf(C_BG_TOP));
	}

	case WM_CTLCOLOREDIT: {
		HDC dc = reinterpret_cast<HDC>(wp);
		SetBkColor(dc, C_WHITE);
		SetTextColor(dc, C_TEXT);
		return reinterpret_cast<LRESULT>(brushOf(C_WHITE));
	}

	case WM_COMMAND: {
		const int id = LOWORD(wp);
		const int code = HIWORD(wp);
		if (id == QrEdit && code == EN_CHANGE) {
			refreshQr();
			return 0;
		}
		if (id == QrEdit && code == EN_SETFOCUS) {
			if (auto e = GetDlgItem(hwnd, QrEdit)) SendMessageW(e, EM_SETSEL, 0, -1);
			return 0;
		}
#if SNAP_HAS_OCR
		// 结果框是用户能直接改的（识别完顺手修两个错字是最常见的用法），
		// 所以字数额不能只在"填文字"那几条路径上同步 —— 手打、粘贴、退格都得跟着走。
		// EN_CHANGE 是改完才发，读数不会落后于框里看得见的字
		if (id == OcrEdit && code == EN_CHANGE) {
			ocrSyncScrollbar();
			ocrSyncCount();
			return 0;
		}
		// 搜索框边打边找，不用再点「查找」。
		// 这里绝对不能让焦点跑到结果框去 —— 每敲一个键抢一次焦点，第二键就打不进来。
		// 所以只做「选中 + 滚动」，焦点留在搜索框；选中的高亮由结果框的
		// ES_NOHIDESEL 负责在失焦状态下照常画出来（见 createOcrPage）
		if (id == OcrSearch && code == EN_CHANGE) {
			ocrFindLive();
			return 0;
		}
#endif
		// —— 改名页：规则一改就重算整表 ——
		// 这一段必须挂在 WM_COMMAND 下面独立判断。以前它被套进了「定时关机」那排
		// 数字框的 EN_KILLFOCUS 分支里，等于改名页的所有输入（前缀 / 后缀 / 查找 /
		// 替换为 / 删除文本）只有在那几个框失去焦点时才算一次 —— 表现就是
		// "改了不实时显示"，列表要等别的动作才刷新
		if (page == PageRename) {
			if (code == CBN_SELCHANGE || code == CBN_EDITCHANGE) {
				// 从列表里点了一项：这一瞬间编辑框里还挂着上一次的字
				// （系统要等这条 CBN_SELCHANGE 返回之后才把选中项回填进编辑框），
				// 而「改为」「过滤」的取值口径都是"先读框里看得见的字" —— 直接算就等于
				// 算在旧值上。表现就是"手打了 123，再从列表挑 png，列表一动不动"。
				// 所以先按刚选中的那一项把文本摆正，下面各分支再去取值。
				// 加 rnSyncing 是因为 SetWindowText 会立刻回来一次 CBN_EDITCHANGE，
				// 那一趟里 rnApplyRules 会被挡掉，避免同一件事算两遍
				if (code == CBN_SELCHANGE && (id == RnExtSet || id == RnFilter)) {
					if (HWND c = GetDlgItem(hwnd, id)) {
						const int sel = static_cast<int>(SendMessageW(c, CB_GETCURSEL, 0, 0));
						if (sel >= 0) {
							wchar_t item[128]{};
							if (SendMessageW(c, CB_GETLBTEXT, static_cast<WPARAM>(sel),
								reinterpret_cast<LPARAM>(item)) != CB_ERR) {
								rnSyncing = true;
								SetWindowTextW(c, item);
								rnSyncing = false;
							}
						}
					}
				}
				if (id == RnFilter) { rnSyncFilter(); return 0; }
				if (id == RnConvert) { rnApplyRules(); return 0; }
				// 「改为」：每敲一个字重算一遍 —— 主名整体换成输入的内容
				if (id == RnConvTo) { rnApplyRules(); return 0; }
				// 改了「改为」那一档就等于选了它（这一组互斥）。
				// 手打也算：框里一旦有了字，「改为」就该亮起来并立刻重算
				if (id == RnExtSet) {
					// 只在"框已经被删空"的时候清一次选中项：这时清选中没有字可丢。
					// 绝不能每敲一个字都发 CB_SETCURSEL(-1) —— 对可编辑下拉来说那句
					// 会连编辑框里的字一起清空，而 CBN_EDITCHANGE 是每键一次，
					// 表现就是字打不进去（按一下就没了）。
					// 反过来说，留着一个旧选中项也不行：框被删空后 ComboText 会退回
					// 去读选中项，框里明明没字却还在套用上一个格式
					if (code == CBN_EDITCHANGE) {
						if (auto c = GetDlgItem(hwnd, RnExtSet)) {
							if (GetWindowTextLengthW(c) == 0) {
								SendMessageW(c, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
							}
						}
					}
					rnPickExtSet();
					return 0;
				}
				// 前缀 / 后缀：从候选里挑一项时只把 token 填进框；手打则直接重算
				if (id == RnPrefix || id == RnSuffix) {
					if (code == CBN_SELCHANGE) rnPickAffixToken(id);
					else rnApplyRules();
					return 0;
				}
			}
			if (code == EN_CHANGE && (id == RnFind || id == RnReplace || id == RnDelText)) {
				rnApplyRules();
				return 0;
			}
			// 序号：只收数字，边打边算。离开框时把框里的字换回"夹过界的规范值"，
			// 免得界面上挂着 999 而实际按 99 编号
			if (id == RnSeqStart || id == RnSeqStep || id == RnSeqDigits) {
				if (code == EN_CHANGE) {
					rnSeqStart = rnSeqInt(RnSeqStart, rnSeqStart, 0, 999999);
					rnSeqStep = rnSeqInt(RnSeqStep, rnSeqStep, 1, 999);   // 下限 1：框里显示的增量就是实际用的增量，不再有"挂着 0 按 1 编号"
					rnSeqDigits = rnSeqInt(RnSeqDigits, 0, 0, 9);
					rnApplyRules();
					return 0;
				}
				if (code == EN_KILLFOCUS) {
					auto put = [this](int editId, int v) {
						if (auto c = GetDlgItem(hwnd, editId)) {
							rnSyncing = true;
							SetWindowTextW(c, std::to_wstring(v).c_str());
							rnSyncing = false;
						}
					};
					put(RnSeqStart, rnSeqStart);
					put(RnSeqStep, rnSeqStep);
					put(RnSeqDigits, rnSeqDigits);
					return 0;
				}
				return 0;
			}
		}
		// 四格手输上限统一 99：打着打着超了就地夹回 99，光标停在末尾 ——
		// 再往下敲也只会重新夹成 99，数字不再变化。
		// 不能等到"离开输入框那一下"才纠正：那时屏幕上还挂着 999
		for (int sdId : kSdEditIds) {
			if (id != sdId) continue;
			auto ed = GetDlgItem(hwnd, id);
			if (code == EN_CHANGE) {
				wchar_t buf[32]{};
				if (ed) {
					GetWindowTextW(ed, buf, 32);
					if (wcstol(buf, nullptr, 10) > SdFieldMax) {
						sdSetValue(id, SdFieldMax);
						// SetWindowText 会把插入点丢回开头，不摆回来第二个键就插到前面去了
						SendMessageW(ed, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
					}
				}
				return 0;
			}
			// 离开输入框时把 "5" 补成 "05"
			if (code == EN_KILLFOCUS) {
				sdSetValue(id, sdValue(id));
				sdUpdateStatus();
				return 0;
			}
		}
		switch (id) {
#if SNAP_HAS_OCR
		case TabOcr: setPage(PageOcr); return 0;
		case OcrBtnRec: startScreenOcr(); return 0;
		case OcrBtnCopy: ocrCopy(); return 0;
		case OcrBtnClear: ocrClear(); return 0;
		case OcrBtnLayout: ocrCycleLayout(); return 0;
		case OcrBtnFind: ocrFind(); return 0;
#endif
		case TabQr: setPage(PageQr); return 0;
		case TabShutdown: setPage(PageShutdown); return 0;
		case TabVerify: setPage(PageVerify); return 0;
		case TabRename: setPage(PageRename); return 0;
		case TabSetting: setPage(PageSetting); return 0;
		// —— 文件改名页（逻辑在 WinRename.cpp） ——
		case RnAddFiles: rnPickFiles(); return 0;
		case RnAddFolder: rnPickFolder(); return 0;
		case RnCopy: rnCopyNames(); return 0;
		case RnClear: rnClear(); return 0;
		case RnReset: rnReset(); return 0;
		case RnUndo: rnUndo(); return 0;
		case RnRun: rnRun(); return 0;
		case RnCaseUpper: rnSetCase(0); return 0;
		case RnCaseLower: rnSetCase(1); return 0;
		case RnCaseTitle: rnSetCase(2); return 0;
		case RnNumEnable: rnToggleSeq(); return 0;
		case RnSeqOn: rnToggleSeq(); return 0;
		case RnAffixOn: rnToggleAffix(); return 0;
		case RnExtUpper: rnSetExtMode(1); return 0;
		case RnExtLower: rnSetExtMode(2); return 0;
		case RnExtKeep: rnSetExtMode(0); return 0;
		case RnExtDrop: rnSetExtMode(3); return 0;
		case RnRepCase: rnToggleRepCase(); return 0;
		case RnRepAll: rnToggleRepAll(); return 0;
		case RnDelSpace: rnToggleDel(0); return 0;
		case RnDelLower: rnToggleDel(1); return 0;
		case RnDelUpper: rnToggleDel(2); return 0;
		case RnDelDigit: rnToggleDel(3); return 0;
		case RnDelCjk: rnToggleDel(4); return 0;
		case RnDelSpecial: rnToggleDel(5); return 0;
		case RnCheckAll: rnCheckAll(false); return 0;
		case RnInvert: rnCheckAll(true); return 0;
		case QrBtnScreen: qrGrabWithFallback(); return 0;
		case QrBtnCopy: copyImage(); return 0;
		case QrBtnDownload: saveImage(); return 0;
		case QrBtnUpload: pickLogo(); return 0;
		case QrBtnClear: clearInput(); return 0;
		case SdUpDay: sdNudge(SdEditDay, 1); return 0;
		case SdDnDay: sdNudge(SdEditDay, -1); return 0;
		case SdUpHour: sdNudge(SdEditHour, 1); return 0;
		case SdDnHour: sdNudge(SdEditHour, -1); return 0;
		case SdUpMin: sdNudge(SdEditMin, 1); return 0;
		case SdDnMin: sdNudge(SdEditMin, -1); return 0;
		case SdUpSec: sdNudge(SdEditSec, 1); return 0;
		case SdDnSec: sdNudge(SdEditSec, -1); return 0;
		case VfBrowse: vfBrowse(); return 0;
		case VfClear: vfClear(); return 0;
		case VfCopy: vfCopyText(); return 0;
		case VfSave: vfSave(); return 0;
		case VfStop: vfStop(); return 0;
		// 自绘勾选不会自己翻状态（BS_OWNERDRAW 没有 BS_AUTOCHECKBOX 的自动切换），
		// 状态由窗口自己拿（vfChk），翻完重画一格
		case VfChkVer: case VfChkDate: case VfChkMd5: case VfChkSha256: case VfChkCrc: {
			const int i = vfChkIndex(id);
			if (i >= 0) {
				vfChk[i] = !vfChk[i];
				if (auto ck = GetDlgItem(hwnd, id)) InvalidateRect(ck, nullptr, TRUE);
			}
			return 0;
		}
		case SdStart: sdStart(); return 0;
		case SdPause: sdPause(); return 0;
		case SdReset: sdClear(); return 0;
		case SetRecCap:
			if (captureRow == 0) endCapture(); else beginCapture(0);
			return 0;
		case SetRecQr:
			if (captureRow == 1) endCapture(); else beginCapture(1);
			return 0;
		case SetRecZip:
			if (captureRow == 2) endCapture(); else beginCapture(2);
			return 0;
		case SetDirShotBtn: chooseSaveDir(false); return 0;
		case SetDirVideoBtn: chooseSaveDir(true); return 0;
		// 自绘勾选不会自己翻状态，这里手动翻 + 重画一格
		case SetChkZipGuard: {
			setChkZipGuard = !setChkZipGuard;
			Setting::get()->setZipGuard(setChkZipGuard);
			// 立即生效：打开就起后台看护，关掉就停（想正常多开同一个包的人靠这个）
			if (setChkZipGuard) ZipKill::startGuard(); else ZipKill::stopGuard();
			if (auto ck = GetDlgItem(hwnd, SetChkZipGuard)) InvalidateRect(ck, nullptr, TRUE);
			showToast(Lang::get(setChkZipGuard ? L"qr.zipGuardOn" : L"qr.zipGuardOff"));
			return 0;
		}
		case SetChkAutoStart: {
			const bool want = !setChkAutoStart;
			// 以注册表的实际写入结果为准：被组策略 / 安全软件拦下时保持原状态并提示。
			// 早先是"写完再读一遍配置"，而配置无论成败都会被写成新值，那个回写等于自欺欺人
			if (Setting::get()->setAutoStart(want)) {
				setChkAutoStart = want;
				showToast(Lang::get(want ? L"qr.autoStartOn" : L"qr.autoStartOff"));
			}
			else {
				showToast(Lang::get(L"qr.autoStartFail"));
			}
			if (auto ck = GetDlgItem(hwnd, SetChkAutoStart)) InvalidateRect(ck, nullptr, TRUE);
			return 0;
		}
		case SetChkHideMain: {
			setChkHideMain = !setChkHideMain;
			Setting::get()->setHideMainOnCap(setChkHideMain);
			if (auto ck = GetDlgItem(hwnd, SetChkHideMain)) InvalidateRect(ck, nullptr, TRUE);
			showToast(Lang::get(setChkHideMain ? L"qr.hideMainOn" : L"qr.hideMainOff"));
			return 0;
		}
		case SetChkCloseTray: {
			setChkCloseTray = !setChkCloseTray;
			Setting::get()->setCloseToTray(setChkCloseTray);
			if (auto ck = GetDlgItem(hwnd, SetChkCloseTray)) InvalidateRect(ck, nullptr, TRUE);
			showToast(Lang::get(setChkCloseTray ? L"qr.closeTrayOn" : L"qr.closeTrayOff"));
			return 0;
		}
		case SetChkSingleInstance: {
			setChkSingleInstance = !setChkSingleInstance;
			Setting::get()->setSingleInstance(setChkSingleInstance);
			// 只影响之后启动的进程：本进程已经持有的互斥体不动（放掉反而让
			// 别人钻空子），关掉后新开的实例走各自的托盘 / 热键
			if (auto ck = GetDlgItem(hwnd, SetChkSingleInstance)) InvalidateRect(ck, nullptr, TRUE);
			showToast(Lang::get(setChkSingleInstance ? L"qr.singleOn" : L"qr.singleOff"));
			return 0;
		}
		case SetChkShowMainOnStart: {
			// 只影响下一次启动：本进程的界面已经开着了，这里只落配置
			setChkShowMainOnStart = !setChkShowMainOnStart;
			Setting::get()->setShowMainOnStart(setChkShowMainOnStart);
			if (auto ck = GetDlgItem(hwnd, SetChkShowMainOnStart)) InvalidateRect(ck, nullptr, TRUE);
			showToast(Lang::get(setChkShowMainOnStart ? L"qr.showMainOn" : L"qr.showMainOff"));
			return 0;
		}
		default:
			break;
		}
		return 0;
	}

	case MsgVfProgress:
		vfOnProgress();
		return 0;

	case MsgVfDone:
		vfOnDone();
		return 0;

#if SNAP_HAS_OCR
	// 工作线程识别完 → 回 UI 线程填结果。控件只在这里动，线程里一个都不碰
	case MsgOcrDone:
		ocrOnDone();
		return 0;
#endif

	case WM_TIMER:
		if (wp == TidShutdown) {
			sdTick();
			return 0;
		}
#if SNAP_HAS_OCR
		if (wp == TidOcrIdle) {
			ocrIdleCheck();
			return 0;
		}
#endif
		if (wp == TidRestoreMain) {
			// 藏起来不足 200ms 的"放回去"被推到了这里（见 restoreAfterCapture）：
			// 到点才真正摆回来，藏与放之间总算隔开了一帧以上
			KillTimer(hwnd, TidRestoreMain);
			restoreMainNow();
			return 0;
		}
		if (wp == TidGrabCheck) {
			if (!grabPending) {
				KillTimer(hwnd, TidGrabCheck);
				return 0;
			}
			auto cap = WinCap::get();
			// 截图窗口没了 = 用户取消了框选，完成回调不会再来。把主界面放回去，
			// 否则程序看着像"按了扫码就消失了"
			if (!cap || cap->isDead()) {
				KillTimer(hwnd, TidGrabCheck);
				grabPending = false;
				// 用户自己取消了框选，没有结果可看：原本在屏幕上的界面放回原样，
				// 原本收在托盘里的就继续待着（别"按一下键界面就冒出来"）
				const bool restore = g_deferReveal ? g_wasOnScreen : true;
				g_deferReveal = false;
				g_wasOnScreen = false;
				if (grabExcludedByAffinity) {
					App::setCaptureAffinity(hwnd, false);
					grabExcludedByAffinity = false;
				}
				else if (restore) {
					ShowWindow(hwnd, SW_SHOW);
					SetForegroundWindow(hwnd);
				}
			}
			return 0;
		}
		if (wp == TidCapture) {
			pollCaptureKeys();
			return 0;
		}
		// 副编辑器里"已保存"只亮一下，随后回到"录入"
		if (wp == TidRecSaved) {
			KillTimer(hwnd, TidRecSaved);
			recSavedRow = -1;
			refreshShortcutRows();
			return 0;
		}
		if (wp == TidHover) {
			POINT pt{};
			GetCursorPos(&pt);
			auto under = [this, &pt](int id) {
				auto c = GetDlgItem(hwnd, id);
				if (!c || !IsWindowVisible(c)) return false;
				RECT r{};
				return GetWindowRect(c, &r) && PtInRect(&r, pt);
			};
			int over = 0;
			for (int id : kHoverIds) {
				if (under(id)) { over = id; break; }
			}
			// 改名页那一排按钮单独一张表（见 kRnIds）：两页的控件互不相见，
			// 合成一张大表只会让"这页的 id 混进了别的页"更难查
			if (!over) {
				for (int id : kRnIds) {
					if (under(id)) { over = id; break; }
				}
			}
			if (over != hoverCtrl) {
				auto old = GetDlgItem(hwnd, hoverCtrl);
				hoverCtrl = over;
				if (old) InvalidateRect(old, nullptr, TRUE);
				auto now = GetDlgItem(hwnd, hoverCtrl);
				if (now) InvalidateRect(now, nullptr, TRUE);
			}
			return 0;
		}
		break;

	case WM_KEYDOWN:
		// 录入快捷键期间键盘归录入器：吃掉所有按键，免得空格/回车顺手把"取消"按了
		if (captureRow >= 0) {
			if (wp == VK_ESCAPE) endCapture();
			return 0;
		}
		if (wp == VK_ESCAPE) {
			ShowWindow(hwnd, SW_HIDE);
			return 0;
		}
		// 校验页：焦点不在结果框里（落在按钮或窗口上）也要能粘贴。
		// 结果框自己有焦点时由 VfBox 接，不会重复
		if (page == PageVerify && (GetKeyState(VK_CONTROL) & 0x8000)
			&& (wp == 'V' || wp == 'v')) {
			vfPasteCompare();
			return 0;
		}
		break;

	case WM_CLOSE:
		// 关窗前先把快捷键录入收掉：热键还处于注销状态，配置也没落，窗口一藏就再没人收尾
		if (captureRow >= 0) endCapture();
		// 关窗行为由设置里那颗勾选决定：默认收进托盘，不是杀进程 ——
		// 定时关机的倒计时还得继续走；用户在设置里关掉"关闭时最小化到托盘"
		// 之后，点关闭就是真退出。
		// 还要再确认一次托盘图标真的在（hasTray）：图标没挂上时把窗口藏起来，
		// 就等于把唯一的入口藏掉 —— 进程留在后台、快捷键和托盘都没反应，
		// 用户只能去任务管理器杀。没图标就直接退出，宁可少一个常驻也别留一个僵尸
		if (setChkCloseTray && ui::App::get()->hasTray()) {
			ShowWindow(hwnd, SW_HIDE);
		}
		else {
			ui::App::get()->quit(0);
		}
		return 0;

	case WM_GETMINMAXINFO: {
		auto mmi = reinterpret_cast<MINMAXINFO*>(lp);
		RECT rc{ 0, 0, sc(CW), sc(CH) };
		const DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
		const DWORD ex = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
		AdjustWindowRectEx(&rc, style, FALSE, ex);
		mmi->ptMinTrackSize.x = mmi->ptMaxTrackSize.x = rc.right - rc.left;
		mmi->ptMinTrackSize.y = mmi->ptMaxTrackSize.y = rc.bottom - rc.top;
		return 0;
	}

	case WM_DPICHANGED: {
		dpi = HIWORD(wp);
		if (dpi <= 0) dpi = 96;
		// 字体是按 sc(height) 建的，dpi 一变就得重新取一遍。旧字体留在缓存里，
		// 由析构统一 DeleteObject，不在这里删 —— 可能有控件还挂着它
		setFont = fontOf(FsBody, 500);
		editFont = fontOf(FsCtrl, 400);
		sdEditFont = fontOf(FsNum, 700);
		// 单位字（天/时/分/秒）与下方「开始定时器」那颗按钮同号同重：
		// 自绘按钮（高 ≥40）用的就是 FsCtrl / 700，这里照它来，一行里才是同一套字
		sdLabelFont = fontOf(FsCtrl, 700);
		auto prc = reinterpret_cast<RECT*>(lp);
		if (prc) {
			SetWindowPos(hwnd, nullptr, prc->left, prc->top,
				prc->right - prc->left, prc->bottom - prc->top,
				SWP_NOZORDER | SWP_NOACTIVATE);
		}
		relayout();
		InvalidateRect(hwnd, nullptr, TRUE);
		return 0;
	}

	case WM_DESTROY:
		KillTimer(hwnd, TidShutdown);
		KillTimer(hwnd, TidCapture);
		KillTimer(hwnd, TidHover);
		KillTimer(hwnd, TidGrabCheck);
		KillTimer(hwnd, TidRecSaved);
#if SNAP_HAS_OCR
		KillTimer(hwnd, TidOcrIdle);
		// 引擎是独立进程，窗口没了就得收掉，否则它会挂在后台占着 100MB 内存
		ocrJoinWorker();
		OcrEngine::shutdown();
#endif
		return 0;

	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// ———————————————— 控件 ————————————————

HWND WinQr::mkButton(int id, const wchar_t* text, bool ownerDraw)
{
	const DWORD style = WS_CHILD | WS_VISIBLE | (ownerDraw ? BS_OWNERDRAW : BS_PUSHBUTTON);
	return CreateWindowExW(0, L"BUTTON", text, style, 0, 0, 0, 0, hwnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

HWND WinQr::mkEdit(int id, const wchar_t* text, DWORD extra)
{
	// WS_CLIPSIBLINGS：改名页的原地编辑框要浮在自绘列表上面，
	// 两者重画时必须互相绕开，谁也不能把谁的像素抹掉
	return CreateWindowExW(0, L"EDIT", text,
		WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | extra, 0, 0, 0, 0, hwnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

HWND WinQr::mkStatic(int id, const wchar_t* text, DWORD extra)
{
	return CreateWindowExW(0, L"STATIC", text,
		WS_CHILD | WS_VISIBLE | extra, 0, 0, 0, 0, hwnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

void WinQr::createControls()
{
	// 字体在这里取一次，挂到控件上是 relayout() 的事 —— 早先两边都设一遍，
	// 每个控件白挂两次 WM_SETFONT
	setFont = fontOf(FsBody, 500);
	editFont = fontOf(FsCtrl, 400);
	sdEditFont = fontOf(FsNum, 700);
	// 单位字与「开始定时器」按钮同号同重（FsCtrl / 700），与 WM_DPICHANGED 里那份一致
	sdLabelFont = fontOf(FsCtrl, 700);

	// 页签：带引擎的版本最左边多一颗「文字识别」，它就是首页（见 PageOcr）
#if SNAP_HAS_OCR
	mkButton(TabOcr, Lang::get(L"qr.tabOcr").c_str());
#endif
	mkButton(TabQr, Lang::get(L"qr.tabQr").c_str());
	mkButton(TabVerify, Lang::get(L"qr.tabVerify").c_str());
	mkButton(TabRename, Lang::get(L"rn.tab").c_str());
	mkButton(TabShutdown, Lang::get(L"qr.tabShutdown").c_str());
	mkButton(TabSetting, Lang::get(L"qr.tabSetting").c_str());

#if SNAP_HAS_OCR
	createOcrPage();
#endif

	// —— 二维码页 ——
	if (auto ed = mkEdit(QrEdit, L"", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN)) {
		// EM_SETCUEBANNER：空框时的灰色提示；系统没装 comctl32 v6 时这条会被忽略，不影响功能
		SendMessageW(ed, EM_SETCUEBANNER, TRUE,
			reinterpret_cast<LPARAM>(Lang::get(L"qr.placeholder").c_str()));
	}
	mkButton(QrBtnClear, Lang::get(L"qr.btnClear").c_str());
	mkButton(QrBtnScreen, Lang::get(L"qr.btnDecode").c_str());
	mkButton(QrBtnCopy, Lang::get(L"qr.btnCopy").c_str());
	mkButton(QrBtnDownload, Lang::get(L"qr.btnSave").c_str());
	mkButton(QrBtnUpload, Lang::get(L"qr.btnLogo").c_str());
	// 右列那颗「截图」原来在这里（与 F1 同功能），已经去掉：热键够用，
	// 那条空档就空着，左列按钮不再拉长（见 QR_BTN_W 的注释）

	// —— 定时关机页 ——
	// 单位字贴右：它现在住在数字格的右半边，紧挨着数字，右对齐才贴得住格子右边框
	mkStatic(SdLblDay, Lang::get(L"qr.day").c_str(), SS_RIGHT | SS_CENTERIMAGE);
	mkStatic(SdLblHour, Lang::get(L"qr.hour").c_str(), SS_RIGHT | SS_CENTERIMAGE);
	mkStatic(SdLblMin, Lang::get(L"qr.minute").c_str(), SS_RIGHT | SS_CENTERIMAGE);
	mkStatic(SdLblSec, Lang::get(L"qr.second").c_str(), SS_RIGHT | SS_CENTERIMAGE);

	const wchar_t* sdInit[4] = { L"0", L"00", L"00", L"00" };
	for (int i = 0; i < 4; i++) {
		// 不带 WS_BORDER：边框由父窗口画（见 drawShutdownPage），这样 EDIT 才能
		// 缩到"只有一行字那么高"，再在格子里居中 —— 带边框的 44 高单行 EDIT
		// 永远把字顶在上边。
		// 右对齐（不是居中）：单位字就贴在它右边，数字靠右才读得出"0 天"是一个词；
		// 居中的话两个数字和单位之间会空出二十多像素，看着是两截
		if (auto ed = mkEdit(kSdEditIds[i], sdInit[i], ES_RIGHT | ES_NUMBER)) {
			// 限 3 位：手输上限 99，多给一位让"100"这种能完整敲进来，再由
			// sdValue 夹回 99（截成"10"是无声改数，更难理解）
			SendMessageW(ed, EM_SETLIMITTEXT, 3, 0);
		}
		mkButton(kSdUpIds[i], L"");
		mkButton(kSdDnIds[i], L"");
	}

	mkButton(SdStart, Lang::get(L"qr.sdStart").c_str());
	mkButton(SdPause, Lang::get(L"qr.sdPause").c_str());
	mkButton(SdReset, Lang::get(L"qr.sdReset").c_str());

	// —— 设置页 ——
	// SS_CENTERIMAGE：单行文字竖直居中。行高 28 的输入框里字是居中的，
	// 标签靠"框顶 + 2px"去凑永远差一像素，交给 STATIC 自己居中最省事
	const DWORD lblStyle = SS_LEFTNOWORDWRAP | SS_CENTERIMAGE;
	mkStatic(SetLblCap, Lang::get(L"qr.setShot").c_str(), lblStyle);
	mkStatic(SetLblQr, Lang::get(L"qr.setScan").c_str(), lblStyle);
	mkStatic(SetLblZip, Lang::get(L"qr.setZip").c_str(), lblStyle);
	for (int i = 0; i < kSetRowCount; i++) {
		mkEdit(kSetDispIds[i], L"", WS_BORDER | ES_CENTER | ES_READONLY);
		// 自绘：看起来是紧挨着的第二只编辑框（副编辑器），点下去其实是"开始录入"
		mkButton(kSetRecIds[i], Lang::get(L"qr.setRecord").c_str());
	}

	// 保存位置两行：路径框只读显示当前目录（ES_AUTOHSCROLL 让长路径能横向滚），
	// 「更改…」是同一列上的次级按钮 —— 与快捷键行的「录入」共用一条竖线
	mkStatic(SetDirShotLbl, Lang::get(L"qr.setShotDir").c_str(), lblStyle);
	mkStatic(SetDirVideoLbl, Lang::get(L"qr.setVideoDir").c_str(), lblStyle);
	for (int i = 0; i < 2; i++) {
		if (auto ed = mkEdit(kSetDirDispIds[i], L"", WS_BORDER | ES_READONLY | ES_AUTOHSCROLL)) {
			// 路径两边留 6：贴着边框起笔，长盘符路径看着像被切掉了一截
			SendMessageW(ed, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
				MAKELPARAM(sc(6), sc(6)));
		}
		mkButton(kSetDirBtnIds[i], Lang::get(L"qr.setChange").c_str());
	}

	// 两颗勾选都用自绘（BS_OWNERDRAW）：主题复选框是透明背景画字的，主窗口又是
	// WS_CLIPCHILDREN + 整幅双缓冲，被页签/别的窗口盖过之后控件矩形里会留别人的
	// 残影（看起来就是"叠字"）。自绘每帧先整块填底色，物理上不可能残留。
	// 勾选状态由窗口自己持有（setChkAutoStart / setChkZipGuard），不从按钮读
	setChkAutoStart = Setting::get()->getAutoStart();
	CreateWindowExW(0, L"BUTTON", Lang::get(L"qr.setAutoStart").c_str(),
		WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(SetChkAutoStart)),
		GetModuleHandleW(nullptr), nullptr);

	// 后台「压缩包防多开」：开着时同一个压缩包被开出多个窗口，只留最新打开的那个；
	// 想正常多开同一个包的人在这里关掉即可
	setChkZipGuard = Setting::get()->getZipGuard();
	CreateWindowExW(0, L"BUTTON", Lang::get(L"qr.setZipGuard").c_str(),
		WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(SetChkZipGuard)),
		GetModuleHandleW(nullptr), nullptr);

	// 「截图时隐藏主界面」：开着的时候，按截图 / 扫码热键（以及点「识别二维码」）
	// 会先把主窗口收起来，截完再放回来 —— 免得自己的窗口出现在选区里，识码时更糟，
	// 屏幕上那张大二维码会被当成识别结果
	setChkHideMain = Setting::get()->getHideMainOnCap();
	CreateWindowExW(0, L"BUTTON", Lang::get(L"qr.setHideMain").c_str(),
		WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(SetChkHideMain)),
		GetModuleHandleW(nullptr), nullptr);

	// 「关闭时最小化到托盘」：开着点关闭只是收进托盘（进程留着，定时关机的
	// 倒计时才不会被一扇窗户带走）；关掉点关闭就是真退出
	setChkCloseTray = Setting::get()->getCloseToTray();
	CreateWindowExW(0, L"BUTTON", Lang::get(L"qr.setCloseTray").c_str(),
		WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(SetChkCloseTray)),
		GetModuleHandleW(nullptr), nullptr);

	// 「只允许单实例」：默认勾上，与历来"双击图标只出一个窗口"一致
	setChkSingleInstance = Setting::get()->getSingleInstance();
	CreateWindowExW(0, L"BUTTON", Lang::get(L"qr.setSingleInstance").c_str(),
		WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(SetChkSingleInstance)),
		GetModuleHandleW(nullptr), nullptr);

	// 「启动显示主界面」：勾上双击直接出界面；不勾就是老手感，只落托盘。
	// 只影响下一次启动，本进程界面已经开着了
	setChkShowMainOnStart = Setting::get()->getShowMainOnStart();
	CreateWindowExW(0, L"BUTTON", Lang::get(L"qr.setShowMainOnStart").c_str(),
		WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(SetChkShowMainOnStart)),
		GetModuleHandleW(nullptr), nullptr);

	createVerifyPage();
	createRenamePage();

	refreshShortcutRows();
	refreshSaveDirs();
	sdWriteRemain(0);
	sdUpdateStatus();

	// 参考打开就是一张"请输入文本内容！"的示例码，不是一张空白卡片。
	// 只画码，不往输入框里塞字 —— 输入框保持空态，占位提示才看得见
	if (qrEncode(toUtf8(Lang::get(L"qr.placeholder")), qrBmp)) invalidateQrBitmap();
}

void WinQr::relayout()
{
	auto place = [this](int id, int x, int y, int w, int h) {
		if (auto c = GetDlgItem(hwnd, id)) {
			SetWindowPos(c, nullptr, sc(x), sc(y), sc(w), sc(h), SWP_NOZORDER | SWP_NOACTIVATE);
		}
	};

	// 页签：从 14 起，宽 84×N + 58（「设置」那颗窄），间隙 6，高 30。
	// 位置按 kTabIds 的下标取，所以顺序一改就是"页签换了位置"；
	// 不带引擎的版本少一颗「文字识别」，整排左移一格，右边空出来的地方不补 —— 多一颗
	// 空白页签比"少一颗"更难看
#if SNAP_HAS_OCR
	static const int tabX[6] = { 14, 104, 194, 284, 374, 464 };
	static const int tabW[6] = { 84, 84, 84, 84, 84, 58 };
#else
	// 不带引擎：二维码 / 文件校验 / 文件改名 / 定时关机 / 设置 —— 五颗，与带引擎那一版同宽
	static const int tabX[5] = { 14, 104, 194, 284, 374 };
	static const int tabW[5] = { 84, 84, 84, 84, 58 };
#endif
	for (int i = 0; i < (int)std::size(kTabIds); i++) place(kTabIds[i], tabX[i], 2, tabW[i], TAB_H);

#if SNAP_HAS_OCR
	// 文字识别页自己带一套几何（含字体挂载），见 relayoutOcr
	relayoutOcr();
#endif

	// 二维码页：输入框顶到左右边距，下面五个 132×46 的按钮一列到底，行距 58。
	// （原来按钮和二维码卡片之间还挤着一小列「截图 / 关压缩软件」，两颗都去掉后
	// 那条空档就空着了 —— 左列按钮保持 132 宽，不拉长去填，见 QR_BTN_W 的注释）
	place(QrEdit, MARGIN, 42, CW - MARGIN * 2, INPUT_H);
	for (int i = 0; i < (int)std::size(kQrBtnIds); i++) {
		place(kQrBtnIds[i], MARGIN, QR_TOP + QR_BTN_STEP * i, QR_BTN_W, QR_BTN_H);
	}

	// 定时关机页：四列（14 / 157 / 300 / 443），列内「标签 + 输入框 + 上下箭头」
	// 单行 EDIT 是把字顶在客户区上沿画的 —— 框给到 44 高，那串数字也贴在上边，
	// 看上去永远"偏上"。所以白底和 1px 边框改由父窗口画（drawShutdownPage），
	// 里面只放一只**无边框**的 EDIT，高度正好等于这一行字的行高，再在格子里居中摆。
	// 这样居中是算出来的，跟字体、DPI 无关
	int sdLineH = sc(FsNum);
	if (sdEditFont) {
		if (HDC dc = GetDC(hwnd)) {
			HGDIOBJ oldFont = SelectObject(dc, sdEditFont);
			TEXTMETRICW tm{};
			if (GetTextMetricsW(dc, &tm) && tm.tmHeight > 0) sdLineH = tm.tmHeight;
			SelectObject(dc, oldFont);
			ReleaseDC(hwnd, dc);
		}
	}
	// +sc(4)：给笔画上下留一点气，卡得死紧反而像被切过
	const int sdEdH = sdLineH + sc(4);
	// 单位字（天/时/分/秒）的行高。STATIC 要贴到格子右下角，就得知道"一行字多高"，
	// 否则只能靠占满整格再居中，那看着和数字不同高
	int sdUnitH = 0;
	if (HDC dc = GetDC(hwnd)) {
		HGDIOBJ oldFont = SelectObject(dc, sdLabelFont);
		TEXTMETRICW tm{};
		if (GetTextMetricsW(dc, &tm) && tm.tmHeight > 0) sdUnitH = tm.tmHeight;
		SelectObject(dc, oldFont);
		ReleaseDC(hwnd, dc);
	}
	if (sdUnitH <= 0) sdUnitH = SD_ED_H / 2;
	const int sdEdY = SD_Y_ED + (SD_ED_H - sdEdH) / 2;
	for (int i = 0; i < 4; i++) {
		const int gx = SD_GX[i];
		// 单位字在格子右下角：右边贴格子边框（留 SD_UNIT_GAP），下边离格子底边框 3px。
		// Win32 的 STATIC 没有"底部对齐"样式（只有 SS_CENTERIMAGE 管竖直居中），
		// 所以"往下压"是靠**把 STATIC 自己的框压矮、贴着格子下沿放**做到的：
		// 框高正好一行字高，竖直居中等价于填满，位置就由这个框决定了。
		// 下边留 3px：STATIC 会把自己那块刷白，贴着边框就会被压掉一条线
		place(kSdLblIds[i], gx + SD_ED_W - SD_UNIT_W - SD_UNIT_GAP,
			SD_Y_ED + SD_ED_H - 3 - sdUnitH, SD_UNIT_W, sdUnitH);
		place(kSdEditIds[i], gx + SD_ED_PAD, sdEdY, SD_EDIT_W, sdEdH);
		place(kSdUpIds[i], gx + SD_ED_W, SD_Y_ED, 40, 21);
		place(kSdDnIds[i], gx + SD_ED_W, SD_Y_ED + 23, 40, 21);
	}
	static const int sdBtnX[3] = { 14, 246, 478 };
	static const int sdBtnW[3] = { 220, 220, 93 };
	for (int i = 0; i < 3; i++) {
		place(kSdBtnIds[i], sdBtnX[i], SD_Y_BTN, sdBtnW[i], 50);
	}

	// 设置页：一张白卡片里三组内容（卡片与组标题、分隔线由 drawSettingPage 画），
	// 三列贯穿 —— 标签 / 值 / 动作。快捷键行与保存位置行共用同一套 x，
	// 竖着看是三条干净的线；常规组六颗勾选一律从 SET_LBL_X 起排 2 行 3 列
	for (int i = 0; i < kSetRowCount; i++) {
		const int y = SET_G1_ROW_Y + SET_G1_PITCH * i;
		place(kSetLblIds[i], SET_LBL_X, y, SET_LBL_W, SET_ROW_H);
		place(kSetDispIds[i], SET_VAL_X, y, SET_VAL_W, SET_ROW_H);
		place(kSetRecIds[i], SET_ACT_X, y, SET_ACT_W, SET_ROW_H);
	}
	for (int i = 0; i < 2; i++) {
		const int y = SET_G2_ROW_Y + SET_G2_PITCH * i;
		place(kSetDirLblIds[i], SET_LBL_X, y, SET_LBL_W, SET_ROW_H);
		place(kSetDirDispIds[i], SET_VAL_X, y, SET_VAL_W, SET_ROW_H);
		place(kSetDirBtnIds[i], SET_ACT_X, y, SET_ACT_W, SET_ROW_H);
	}
	// 勾选块：3 列 2 行，横向按内容宽三等分（58 / 218 / 378），纵向行距 40。
	// 六颗依次填格，一格一个 —— 循环按 kSetChkIds 的实际长度走，
	// 漏掉一格的表现是"那颗勾选框建出来了却零尺寸"，界面上看不见、也没法点
	for (int i = 0; i < (int)std::size(kSetChkIds); i++) {
		place(kSetChkIds[i], SET_LBL_X + SET_CHK_PITCH_X * (i % 3),
			SET_G3_ROW_Y + SET_CHK_PITCH * (i / 3), SET_CHK_W, SET_CHK_H);
	}

	// 字体是按 sc(height) 建的：换 DPI 之后必须重挂，否则控件还是旧尺寸的字
	auto mountFont = [this](int id, HFONT f) {
		if (!f) return;
		if (auto c = GetDlgItem(hwnd, id)) {
			SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
		}
	};
	mountFont(QrEdit, editFont);
	for (int id : kSdEditIds) mountFont(id, sdEditFont);
	for (int id : kSdLblIds) mountFont(id, sdLabelFont);
	for (int id : kSetDispIds) mountFont(id, setFont);
	for (int id : kSetLblIds) mountFont(id, setFont);
	for (int id : kSetRecIds) mountFont(id, setFont);   // 之前漏了，录入按钮用的是系统默认字体
	for (int id : kSetDirLblIds) mountFont(id, setFont);
	for (int id : kSetDirDispIds) mountFont(id, setFont);
	for (int id : kSetDirBtnIds) mountFont(id, setFont);
	for (int id : kSetChkIds) mountFont(id, setFont);

	// 文件校验页自己带一套网格（列数多、按钮矮），几何放在 WinQrVerify.cpp 里，
	// 由它自己排 —— 免得两处常量各改一半
	relayoutVerify();
	// 文件改名页同理：几何常量都在 WinRename.cpp 里
	relayoutRename();
}

void WinQr::applyPage()
{
	auto show = [this](const int* ids, size_t n, bool visible) {
		for (size_t i = 0; i < n; i++) {
			if (auto c = GetDlgItem(hwnd, ids[i])) {
				ShowWindow(c, visible ? SW_SHOW : SW_HIDE);
			}
		}
	};
#if SNAP_HAS_OCR
	show(kOcrIds, std::size(kOcrIds), page == PageOcr);
#endif
	show(kQrIds, std::size(kQrIds), page == PageQr);
	const bool sdPage = (page == PageShutdown);
	show(kSdLblIds, std::size(kSdLblIds), sdPage);
	show(kSdEditIds, std::size(kSdEditIds), sdPage);
	show(kSdArrowIds, std::size(kSdArrowIds), sdPage);
	show(kSdBtnIds, std::size(kSdBtnIds), sdPage);
	show(kSetIds, std::size(kSetIds), page == PageSetting);
	showVerifyPage(page == PageVerify);
	showRenamePage(page == PageRename);
	if (sdPage) {
		sdUpdateStatus();
		// 三颗按钮的颜色是按 sdRunning/sdPaused 现算的：倒计时可能在别的页上
		// 起停过，切回来的这一下必须按当前状态重画一遍，否则留着旧色
		for (int id : kSdBtnIds) {
			if (auto b = GetDlgItem(hwnd, id)) InvalidateRect(b, nullptr, TRUE);
		}
	}
}

void WinQr::setPage(int p)
{
	if (p < 0 || p > PageSetting || p == page) return;
	// 正在录快捷键时切页：先把录入收掉。录入期间全局热键是注销状态的
	// （setHotkeysEnabled(false)），人一走神切到别的页，热键就一直不恢复 ——
	// 表现出来就是"所有快捷键突然都不好使了"。这一句是那道兜底
	if (captureRow >= 0) endCapture();
	// 离开文件校验页就等于按了"清空"：切走的这一刻把记录、对比基线、结论一起丢掉，
	// 正在跑的校验也一并停掉 —— 再切回来是干净的一页，不会拿上次的摘要接着比
	if (page == PageVerify) vfClear();
	// 切页是一次"整屏换内容"：旧页的控件要一起藏、新页的一起露，中间还夹着父窗口
	// 重画一次背景。这三件事如果分散在几次消息里做，屏幕上就会抓到中间态 ——
	// 新控件已经画上去了、旧页的卡片/按钮还没被擦掉，也就是看到的"残影"。
	// 所以先把重绘冻住，全部改完再一次性把父窗口 + 所有子控件同步重画
	SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
	page = p;
	applyPage();
	for (int id : kTabIds) {
		if (auto c = GetDlgItem(hwnd, id)) InvalidateRect(c, nullptr, TRUE);
	}
	SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
	// RDW_UPDATENOW：这一趟就走 WM_PAINT，别等消息队列轮空；RDW_ALLCHILDREN：
	// 子控件跟着一起刷，否则新页上刚 ShowWindow 的按钮会先是空白、随后才补画
	RedrawWindow(hwnd, nullptr, nullptr,
		RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

// ———————————————— 绘制 ————————————————

HFONT WinQr::fontOf(int height, int weight, bool negative, int face)
{
	// key 各字段错开比特：height < 256、weight < 1024、face < 8、dpi < 4096
	const long long key = ((long long)dpi << 40) | ((long long)face << 32)
		| ((long long)height << 20) | ((long long)weight << 8) | (negative ? 1LL : 0LL);
	auto it = fontCache.find(key);
	if (it != fontCache.end()) return it->second;

	LOGFONTW lf{};
	lf.lfHeight = negative ? -sc(height) : sc(height);
	lf.lfWeight = weight;
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = CLEARTYPE_QUALITY;
	lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
	// 结果框用等宽字体，数字才能一列一列对齐
	const wchar_t* faceName = face == FaceMono ? L"Consolas" : L"Microsoft YaHei";
	wcscpy_s(lf.lfFaceName, faceName);
	HFONT f = CreateFontIndirectW(&lf);
	// 自建字体失败时退回系统 stock 字体。stock 对象不缓存也不 DeleteObject：
	// 它是系统共享句柄，删了会破坏系统状态，且缓存析构时统一 DeleteObject 会误删它
	if (!f) return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	fontCache[key] = f;
	return f;
}

HBRUSH WinQr::brushOf(COLORREF color)
{
	auto it = brushCache.find(color);
	if (it != brushCache.end()) return it->second;
	HBRUSH b = CreateSolidBrush(color);
	brushCache[color] = b;
	return b;
}

void WinQr::onPaint()
{
	PAINTSTRUCT ps{};
	HDC dc = BeginPaint(hwnd, &ps);
	if (!dc) return;

	RECT rc{};
	GetClientRect(hwnd, &rc);
	const int w = rc.right > 0 ? rc.right : sc(CW);
	const int h = rc.bottom > 0 ? rc.bottom : sc(CH);

	// 双缓冲：页面上要画卡片、进度条、大标题，直接往窗口 DC 上怼会闪。
	// 后台画布复用（见 bufDc 成员说明），只有尺寸变了才重建
	const bool needBuf = !bufDc || !bufBmp || bufSize.cx != w || bufSize.cy != h;
	if (needBuf) {
		if (bufDc) { DeleteDC(bufDc); bufDc = nullptr; }
		if (bufBmp) { DeleteObject(bufBmp); bufBmp = nullptr; }
		bufDc = CreateCompatibleDC(dc);
		bufBmp = bufDc ? CreateCompatibleBitmap(dc, w, h) : nullptr;
		bufSize = SIZE{ w, h };
	}
	if (bufDc && bufBmp) {
		HGDIOBJ old = SelectObject(bufDc, bufBmp);
		RECT full{ 0, 0, w, h };
		FillRect(bufDc, &full, brushOf(C_BG_TOP));
		if (page == PageQr) drawQrPage(bufDc);
		else if (page == PageShutdown) drawShutdownPage(bufDc);
		else if (page == PageVerify) drawVerifyPage(bufDc);
		else if (page == PageRename) drawRenamePage(bufDc);
		else if (page == PageSetting) drawSettingPage(bufDc);
		BitBlt(dc, 0, 0, w, h, bufDc, 0, 0, SRCCOPY);
		if (old) SelectObject(bufDc, old);
	}

	EndPaint(hwnd, &ps);
}

void WinQr::printTo(HDC dc, UINT flags)
{
	if (!hwnd || !dc) return;
	RECT rc{};
	GetClientRect(hwnd, &rc);
	FillRect(dc, &rc, brushOf(C_BG_TOP));
	if (page == PageQr) drawQrPage(dc);
	else if (page == PageShutdown) drawShutdownPage(dc);
	else if (page == PageVerify) drawVerifyPage(dc);
	else if (page == PageRename) drawRenamePage(dc);
	else if (page == PageSetting) drawSettingPage(dc);

	g_printDc = dc;
	g_printFlags = flags;
	EnumChildWindows(hwnd, printChildProc, 0);
	g_printDc = nullptr;
	g_printFlags = 0;
}

void WinQr::drawQrPage(HDC dc)
{
	// 右侧正方形卡片：x 贴右边距，边长 = 左侧按钮列总高
	RECT area{ sc(QR_CARD_X), sc(QR_TOP),
			   sc(QR_CARD_X + QR_SIDE), sc(QR_TOP + QR_SIDE) };
	FillRect(dc, &area, brushOf(C_BORDER));
	RECT inner{ area.left + sc(2), area.top + sc(2), area.right - sc(2), area.bottom - sc(2) };
	FillRect(dc, &inner, brushOf(C_WHITE));

	if (!qrHbmp) return;
	const int iw = inner.right - inner.left;
	const int ih = inner.bottom - inner.top;
	int side = std::min(iw, ih) - sc(8);
	if (side < sc(24)) return;
	const int x = inner.left + (iw - side) / 2;
	const int y = inner.top + (ih - side) / 2;

	HDC src = CreateCompatibleDC(dc);
	if (!src) return;
	HGDIOBJ old = SelectObject(src, qrHbmp);
	// 显示尺寸变了（首帧 / DPI 变化）：预览位图按显示尺寸重渲 —— 位图与 blit 严格
	// 1:1，屏幕上的码和编码结果逐像素一致。不再用 StretchBlt 缩放：COLORONCOLOR
	// 的最近邻缩列会把高版本码的整列模块抹掉，屏幕上那张码本身就残了，截屏识别
	// （qrGrabWithFallback）解不出就是这么来的
	if (qrHbmpSide != side) {
		renderQrDib(side);
		SelectObject(src, qrHbmp);   // renderQrDib 可能重建了 HBITMAP，重新选中
	}
	if (!qrHbmp) { SelectObject(src, old); DeleteDC(src); return; }
	BitBlt(dc, x, y, side, side, src, 0, 0, SRCCOPY);
	SelectObject(src, old);
	DeleteDC(src);
}

void WinQr::drawSettingPage(HDC dc)
{
	// 白卡片：浅灰页面上唯一的实心块，所有控件都落在这张卡片里
	RECT card{ sc(SET_CARD_X), sc(SET_CARD_Y),
			   sc(SET_CARD_X + SET_CARD_W), sc(SET_CARD_Y + SET_CARD_H) };
	FillRect(dc, &card, brushOf(C_WHITE));
	FrameRect(dc, &card, brushOf(C_BORDER));

	SetBkMode(dc, TRANSPARENT);
	// 组的标题由页签那条主蓝的小竖条领着，字号只到正文档、加粗一档 ——
	// 标题比正文大一号就会和页签抢视线，这里是"分组"不是"标题"
	const wchar_t* keys[3] = { L"qr.setSecShortcut", L"qr.setSecDir", L"qr.setSecCommon" };
	const int titleY[3] = { SET_G1_Y, SET_G2_Y, SET_G3_Y };
	HGDIOBJ oldFont = SelectObject(dc, fontOf(FsBody, 700));
	SetTextColor(dc, C_TEXT);
	for (int i = 0; i < 3; i++) {
		const int ty = sc(titleY[i]);
		const int th = sc(SET_TITLE_H);
		const int bh = sc(14);
		RECT bar{ sc(SET_LBL_X), ty + (th - bh) / 2, sc(SET_LBL_X) + sc(3), ty + (th + bh) / 2 };
		FillRect(dc, &bar, brushOf(C_TAB_BAR));

		RECT tr{ sc(SET_LBL_X + 10), ty, sc(SET_CARD_X + SET_CARD_W - SET_PAD), ty + th };
		DrawTextW(dc, Lang::get(keys[i]).c_str(), -1, &tr,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

		// 组下方一条 1px 分隔线，撑满卡片内容宽
		const int ly = sc(titleY[i] + SET_TITLE_H + SET_GAP_TITLE);
		RECT line{ sc(SET_CARD_X + SET_PAD), ly,
				   sc(SET_CARD_X + SET_CARD_W - SET_PAD), ly + sc(1) };
		FillRect(dc, &line, brushOf(C_BORDER));
	}
	SelectObject(dc, oldFont);
}

void WinQr::drawVerifyPage(HDC dc)
{
	// 结论（相同 / 不同）不再画在这儿：它只由 VfBox 给结果框外框染色表示。
	// 父窗口只负责两条进度条，画完就收工
	const auto pctOf = [](unsigned long long done, unsigned long long total) -> double {
		if (total == 0) return 0.0;
		const double v = (double)done * 100.0 / (double)total;
		return v > 100.0 ? 100.0 : v;
		};
	const double p1 = pctOf(vfProg.fileDone.load(), vfProg.fileTotal.load());
	const double p2 = pctOf(vfProg.allDone.load(), vfProg.allTotal.load());

	const RECT r1{ sc(VfBarX), sc(VfRowFile + 3), sc(VfBarX + VfBarW), sc(VfRowFile + 3 + VfBarH) };
	const RECT r2{ sc(VfBarX), sc(VfRowTotal + 3), sc(VfBarX + VfBarW), sc(VfRowTotal + 3 + VfBarH) };
	// 一条进度条里两块信息：左边「文件:」「总计:」标签，正中百分比。
	// 标签贴左边不动、数字居中 —— 两行条子的数字上下对齐成一条竖线，
	// 眼睛从左往中间扫一次就读完，不像原来数字吊在最右端要横跨整条
	drawProgressBar(dc, r1, p1, Lang::get(L"vf.file") + L": ",
		std::to_wstring((int)(p1 + 0.5)) + L"%");
	drawProgressBar(dc, r2, p2, Lang::get(L"vf.total") + L": ",
		std::to_wstring((int)(p2 + 0.5)) + L"%");
}

void WinQr::drawProgressBar(HDC dc, const RECT& bar, double pct,
	const std::wstring& left, const std::wstring& center)
{
	FillRect(dc, &bar, brushOf(C_FILL_GR));
	RECT fill = bar;
	fill.right = bar.left + (LONG)((bar.right - bar.left) * pct / 100.0);
	if (fill.right > bar.right) fill.right = bar.right;
	const bool hasFill = fill.right > bar.left;
	if (hasFill) FillRect(dc, &fill, brushOf(C_BTN_BLUE));

	SetBkMode(dc, TRANSPARENT);
	HGDIOBJ old = SelectObject(dc, fontOf(FsBody, 600));
	const int pad = sc(12);

	// 文字横跨两种底色：已填充段用白字、未填充段用深字，各自裁一次再画，
	// 看起来就是一个字被进度条"淹"过去。
	// 裁之前必须判空：pct=0 时填充段宽度为 0，IntersectClipRect 得到一个空区，
	// 两段都画不出来 —— 原来刚开始校验时两条条子是一整条空白（连 0% 都不显示）
	const auto seg = [&](const std::wstring& s, UINT align, LONG l, LONG r, COLORREF color) {
		if (s.empty() || r <= l) return;   // 空裁剪区画不出东西，直接跳过
		int saved = SaveDC(dc);
		IntersectClipRect(dc, l, bar.top, r, bar.bottom);
		SetTextColor(dc, color);
		RECT t = bar;
		if (align & DT_LEFT) t.left += pad;   // 靠左的标签离边框留一截
		DrawTextW(dc, s.c_str(), -1, &t, align | DT_VCENTER | DT_SINGLELINE);
		RestoreDC(dc, saved);
	};
	// 每段都画两遍：深字段（填充终点 → 右边界，没填充就是整条）+ 白字段（左边 → 填充终点）
	const auto paint = [&](const std::wstring& s, UINT align) {
		seg(s, align, hasFill ? fill.right : bar.left, bar.right, C_TEXT);
		if (hasFill) seg(s, align, bar.left, fill.right, C_WHITE);
	};
	paint(left, DT_LEFT);
	paint(center, DT_CENTER);

	SelectObject(dc, old);
}

void WinQr::drawShutdownPage(HDC dc)
{
	SetBkMode(dc, TRANSPARENT);

	// 标题：与下方 14..571 的内容区同宽居中
	RECT tr{ sc(MARGIN), sc(SD_Y_TITLE), sc(CW - MARGIN), sc(SD_Y_TITLE + SD_TITLE_H) };
	HGDIOBJ old = SelectObject(dc, fontOf(FsTitle, 700, true));
	SetTextColor(dc, C_TEXT);
	const auto title = Lang::get(L"qr.sdTitle");
	DrawTextW(dc, title.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	SelectObject(dc, old);

	// 四个数字格：白底 + 1px 边框。里面的 EDIT 是无边框的（见 relayout），
	// 只负责画字，边框和底色归这里 —— 这样"字在格子里居中"才成立
	for (int i = 0; i < 4; i++) {
		RECT cell{ sc(SD_GX[i]), sc(SD_Y_ED),
			sc(SD_GX[i] + SD_ED_W), sc(SD_Y_ED + SD_ED_H) };
		FillRect(dc, &cell, brushOf(C_WHITE));
		FrameRect(dc, &cell, brushOf(C_BORDER));
	}

	// 状态行兼进度条：一条轨道 + 蓝色已完成部分 + 白色状态文字。
	// 暂停时也要画已走过的比例，否则一按暂停进度条就"清零"，看着像什么都没发生过
	RECT bar{ sc(MARGIN), sc(SD_Y_BAR), sc(CW - MARGIN), sc(SD_Y_BAR + SD_BAR_H) };
	FillRect(dc, &bar, brushOf(C_TRACK));
	if (sdMax > 1) {
		double frac = 1.0 - (double)sdRemain / (double)sdMax;
		if (frac < 0.0) frac = 0.0;
		if (frac > 1.0) frac = 1.0;
		const int fillW = (int)((bar.right - bar.left) * frac);
		if (fillW > 2) {
			RECT f{ bar.left, bar.top, bar.left + fillW, bar.bottom };
			FillRect(dc, &f, brushOf(C_PRIMARY));
		}
	}

	std::wstring status;
	if (sdRunning) {
		status = Lang::get(sdPaused ? L"qr.sdPaused" : L"qr.sdCounting");
		status += L"  " + remainText(sdRemain);
	}
	else if (sdDone) status = Lang::get(L"qr.sdDone");
	else status = Lang::get(L"qr.sdNotStart");

	old = SelectObject(dc, fontOf(FsCtrl, 700));
	SetTextColor(dc, C_WHITE);
	RECT sr{ bar.left + sc(4), bar.top + sc(1), bar.right - sc(4), bar.bottom - sc(1) };
	DrawTextW(dc, status.c_str(), -1, &sr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	SelectObject(dc, old);
}

void WinQr::drawTab(DRAWITEMSTRUCT* dis)
{
	const int id = (int)dis->CtlID;
	const bool selected = 
#if SNAP_HAS_OCR
		(id == TabOcr && page == PageOcr) ||
#endif
		(id == TabQr && page == PageQr)
		|| (id == TabShutdown && page == PageShutdown)
		|| (id == TabVerify && page == PageVerify)
		|| (id == TabRename && page == PageRename)
		|| (id == TabSetting && page == PageSetting);

	RECT r = dis->rcItem;
	FillRect(dis->hDC, &r, brushOf(C_TAB_BG));
	if (selected) {
		RECT bar{ r.left, r.bottom - sc(4), r.right, r.bottom };
		FillRect(dis->hDC, &bar, brushOf(C_TAB_BAR));
	}

	SetBkMode(dis->hDC, TRANSPARENT);
	SetTextColor(dis->hDC, C_TAB_TX);
	HGDIOBJ old = SelectObject(dis->hDC, fontOf(FsBody, 500));
	wchar_t text[64]{};
	GetWindowTextW(dis->hwndItem, text, 64);
	// 文字按"整格"居中，不要为底部那条 4px 指示条让位：
	// 让了之后中心整体上移 2px，和同一排的其它页签看着不齐（18 号字在 30 高的格里
	// 上下各留 4px，本来就碰不到指示条）
	DrawTextW(dis->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	SelectObject(dis->hDC, old);
}

void WinQr::drawItemLabel(const DRAWITEMSTRUCT* dis, const std::wstring& label)
{
	if (!dis) return;
	RECT r = dis->rcItem;
	SetBkMode(dis->hDC, TRANSPARENT);
	SetTextColor(dis->hDC, C_WHITE);
	// 自绘按钮不经过系统的助记符处理，"浏览(&B)…"里的 & 会原样画出来，得自己筛掉
	std::wstring text;
	text.reserve(label.size());
	for (wchar_t ch : label) {
		if (ch != L'&') text.push_back(ch);
	}
	// 按钮矮（< 40px）就换小一号字，否则 22px 的字在 30 高的按钮里顶到边
	const int btnH = dis->rcItem.bottom - dis->rcItem.top;
	HGDIOBJ old = SelectObject(dis->hDC, fontOf(btnH < sc(40) ? FsBody : FsCtrl, btnH < sc(40) ? 500 : 700));
	// 二维码页右列只有 119 宽，「关闭压缩软件」这种六字标签按 22 号画会顶出右边被裁。
	// 量一下实际宽度，装不下就退到正文号 —— 宁可小一号，不能缺字
	SIZE ts{};
	if (GetTextExtentPoint32W(dis->hDC, text.c_str(), (int)text.size(), &ts)
		&& ts.cx > r.right - r.left - sc(8)) {
		SelectObject(dis->hDC, fontOf(FsBody, 500));
	}
	DrawTextW(dis->hDC, text.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	SelectObject(dis->hDC, old);
}

int WinQr::vfChkIndex(int id)
{
	switch (id) {
	case VfChkVer: return 0;
	case VfChkDate: return 1;
	case VfChkMd5: return 2;
	case VfChkSha256: return 3;
	case VfChkCrc: return 4;
	default: return -1;
	}
}

void WinQr::onDrawItem(DRAWITEMSTRUCT* dis)
{
	if (!dis || !dis->hDC) return;
	const int id = (int)dis->CtlID;

	if (id == TabQr || id == TabShutdown || id == TabVerify || id == TabRename || id == TabSetting
#if SNAP_HAS_OCR
		|| id == TabOcr
#endif
		) {
		drawTab(dis);
		return;
	}

	// —— 改名页的按钮，与其余页同一套画法 ——
	// 动作按钮：除「清空」红底外全是蓝底白字；规则按钮：选中填蓝底白字，未选中白底细框
	{
		const bool isRule = id == RnCaseUpper || id == RnCaseLower || id == RnCaseTitle
			|| id == RnNumEnable || id == RnExtUpper || id == RnExtLower
			|| id == RnExtKeep || id == RnExtDrop
			|| id == RnRepCase || id == RnRepAll
			|| id == RnSeqOn || id == RnAffixOn
			|| id == RnDelSpace || id == RnDelLower || id == RnDelUpper
			|| id == RnDelDigit || id == RnDelCjk || id == RnDelSpecial;
		const bool isTool = id == RnAddFiles || id == RnAddFolder || id == RnCopy
			|| id == RnClear || id == RnReset || id == RnUndo
			|| id == RnRun || id == RnCheckAll || id == RnInvert;
		if (isRule || isTool) {
			RECT r = dis->rcItem;
			const bool hot = (hoverCtrl == id);
			// 按下压一档、悬停压一档：跟页面上其它按钮同一条规则（见本函数尾部）
			const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
			wchar_t label[64]{};
			GetWindowTextW(dis->hwndItem, label, 64);
			SetBkMode(dis->hDC, TRANSPARENT);

			if (isTool) {
				// 底部「全选 / 反选」只是两个词，不带按钮底；它们直接落在页面底色上
				const bool plain = id == RnCheckAll || id == RnInvert;
				if (plain) {
					FillRect(dis->hDC, &r, brushOf(hot ? dim(C_BG_TOP, 4) : C_BG_TOP));
					SetTextColor(dis->hDC, C_TEXT);
					HGDIOBJ f = SelectObject(dis->hDC, fontOf(FsBody, 500));
					DrawTextW(dis->hDC, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
					SelectObject(dis->hDC, f);
					return;
				}
				// 除「清空」外一律跟「文件 / 文件夹」同色：蓝底白字平涂，不加描边，
				// 「清空」是红底（会丢东西的那颗才用红）
				const bool dangerAct = id == RnClear;
				const COLORREF base = dangerAct ? C_DANGER : C_BTN_BLUE;
				const COLORREF bg = pressed ? dim(base, kPressDim) : (hot ? dim(base, kHoverDim) : base);
				FillRect(dis->hDC, &r, brushOf(bg));
				SetTextColor(dis->hDC, C_WHITE);
				HGDIOBJ oldFont = SelectObject(dis->hDC, fontOf(FsBody, 500));
				DrawTextW(dis->hDC, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
				SelectObject(dis->hDC, oldFont);
				return;
			}

			// 这几颗都是勾选项，不是按钮：白底 + 方框 + 对勾 + 文字。
			// 勾上才生效这条规则 —— 和设置页那几颗勾选用同一套画法，一眼就知道"只管开关"
			if (id == RnRepCase || id == RnRepAll || id == RnSeqOn || id == RnAffixOn
				|| id == RnCaseUpper || id == RnCaseTitle || id == RnCaseLower || id == RnNumEnable
				|| id == RnExtUpper || id == RnExtLower || id == RnExtKeep || id == RnExtDrop
				|| id == RnDelSpace || id == RnDelLower || id == RnDelUpper
				|| id == RnDelDigit || id == RnDelCjk || id == RnDelSpecial) {
				RECT r = dis->rcItem;
				bool on = false;
				switch (id) {
				case RnRepCase: on = rnRepCase; break;
				case RnRepAll: on = rnRepAll; break;
				case RnSeqOn: on = rnSeqOn; break;
				case RnAffixOn: on = rnAffixOn; break;
				case RnCaseUpper: on = (rnCase == 0); break;
				case RnCaseTitle: on = (rnCase == 2); break;
				case RnCaseLower: on = (rnCase == 1); break;
				case RnNumEnable: on = rnSeqOn; break;
				case RnExtUpper: on = (rnExtMode == 1); break;
				case RnExtLower: on = (rnExtMode == 2); break;
				case RnExtKeep: on = (rnExtMode == 0); break;
				case RnExtDrop: on = (rnExtMode == 3); break;
				case RnDelSpace: on = rnDelSpace; break;
				case RnDelLower: on = rnDelLower; break;
				case RnDelUpper: on = rnDelUpper; break;
				case RnDelDigit: on = rnDelDigit; break;
				case RnDelCjk: on = rnDelCjk; break;
				case RnDelSpecial: on = rnDelSpecial; break;
				default: break;
				}
				const bool hot = (hoverCtrl == id);
				FillRect(dis->hDC, &r, brushOf(hot ? C_BG_TOP : C_WHITE));
				const int bs = sc(14);
				const int by = r.top + (r.bottom - r.top - bs) / 2;
				RECT box{ r.left + sc(1), by, r.left + sc(1) + bs, by + bs };
				FillRect(dis->hDC, &box, brushOf(on ? C_BTN_BLUE : C_WHITE));
				FrameRect(dis->hDC, &box, brushOf(on ? C_BTN_BLUE : C_BORDER));
				if (on) {
					HPEN pen = CreatePen(PS_SOLID, sc(2), C_WHITE);
					HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
					const POINT pts[3] = {
						{ box.left + bs * 3 / 10, by + bs * 52 / 100 },
						{ box.left + bs * 44 / 100, by + bs * 74 / 100 },
						{ box.left + bs * 76 / 100, by + bs * 28 / 100 },
					};
					Polyline(dis->hDC, pts, 3);
					SelectObject(dis->hDC, oldPen);
					DeleteObject(pen);
				}
				SetTextColor(dis->hDC, C_TEXT);
				HGDIOBJ f = SelectObject(dis->hDC, fontOf(FsBody, 500));
				RECT tr{ box.right + sc(6), r.top, r.right, r.bottom };
				DrawTextW(dis->hDC, label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
				SelectObject(dis->hDC, f);
				return;
			}
		}
	}

	// 勾选统一自绘：整块填底 + 方框 + 对勾。
	// 文件校验页五颗 + 设置页六颗共用这一段，长得一致、也都没了残影问题
	bool* chkState = nullptr;
	if (vfChkIndex(id) >= 0) chkState = &vfChk[vfChkIndex(id)];
	else if (id == SetChkAutoStart) chkState = &setChkAutoStart;
	else if (id == SetChkZipGuard) chkState = &setChkZipGuard;
	else if (id == SetChkHideMain) chkState = &setChkHideMain;
	else if (id == SetChkCloseTray) chkState = &setChkCloseTray;
	else if (id == SetChkSingleInstance) chkState = &setChkSingleInstance;
	else if (id == SetChkShowMainOnStart) chkState = &setChkShowMainOnStart;
	if (chkState) {
		RECT r = dis->rcItem;
		const bool on = *chkState;
		const bool hot = (hoverCtrl == id);
		// 整块填底是这段存在的理由：不填就留别人的旧像素（见 createVerifyPage 的注释）。
		// 悬停同样走"压暗"：浅底提亮只会更泛白，压一档反而能把整行从背景里拎出来。
		// 底色跟着所在页走：校验页在页面底色上，设置页在白色卡片上
		const COLORREF chkBg = hasId(kSetChkIds, id) ? C_WHITE : C_BG_TOP;
		FillRect(dis->hDC, &r, brushOf(hot ? dim(chkBg, kHoverDim) : chkBg));

		const int bs = sc(14);
		const int by = r.top + (r.bottom - r.top - bs) / 2;
		RECT box{ r.left + sc(1), by, r.left + sc(1) + bs, by + bs };
		FillRect(dis->hDC, &box, brushOf(on ? C_BTN_BLUE : C_WHITE));
		FrameRect(dis->hDC, &box, brushOf(on ? C_BTN_BLUE : C_BORDER));
		if (on) {
			// 白色对勾：三点折线，方头线帽，跟"一律直角"的调子对齐
			HPEN pen = CreatePen(PS_SOLID, sc(2), C_WHITE);
			HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
			const POINT pts[3] = {
				{ box.left + bs * 3 / 10, by + bs * 52 / 100 },
				{ box.left + bs * 44 / 100, by + bs * 74 / 100 },
				{ box.left + bs * 76 / 100, by + bs * 28 / 100 },
			};
			Polyline(dis->hDC, pts, 3);
			SelectObject(dis->hDC, oldPen);
			DeleteObject(pen);
		}

		wchar_t label[64]{};
		GetWindowTextW(dis->hwndItem, label, 64);
		SetBkMode(dis->hDC, TRANSPARENT);
		SetTextColor(dis->hDC, C_TEXT);
		// 行距 20、行高 20，标签用正文的 18 号：行距是为了让右列五颗勾选
		// 跟左边「按钮行 + 两条进度条」上下对齐，降一档后仍比字身略高，
		// 视觉上不挤；字号跟旁边的「浏览/清除」按钮同一号，整块看着才是一个整体
		HGDIOBJ oldFont = SelectObject(dis->hDC, fontOf(FsBody, 500));
		RECT tr{ box.right + sc(6), r.top, r.right, r.bottom };
		DrawTextW(dis->hDC, label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		SelectObject(dis->hDC, oldFont);
		return;
	}

	const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
	const bool arrow = isArrowCtrl(id);
	// 禁用态：倒计时跑着的时候 ▲▼ 被锁（格子里是剩余时间读数，改了下一拍就被覆盖），
	// 「清空」在运行中也锁。禁用的按钮不再响应悬停高亮，箭头转灰 —— 否则看着能点、点了没反应
	const bool locked = (dis->itemState & ODS_DISABLED) != 0;
	const bool hover = !locked && (hoverCtrl == id);

	// 静止态底色。悬停 / 按下都在这一个色上压暗，不再各维护一套亮色号和深色号
	COLORREF base = C_BTN_BLUE;
	// 红色留给"清空"那几颗：二维码页、校验页各一颗，带引擎的版本还有文字识别页那颗
	if (id == QrBtnClear || id == VfClear
#if SNAP_HAS_OCR
		|| id == OcrBtnClear
#endif
		) base = C_DANGER;
	else if (arrow) base = C_BTN_GR;
	// 只有"正在倒计时且没暂停"时才显示成可用的红色，其它时候是灰的
	else if (id == SdPause) base = (sdRunning && !sdPaused) ? C_DANGER : C_BTN_GR;
	// 没在算的时候按了也没意义，直接画成禁用灰
	else if (id == VfStop) base = vfBusy ? C_DANGER : C_BTN_GR;
	else if (id == SdReset) base = (sdRunning && !sdPaused) ? C_BTN_GR : C_DANGER;

	const COLORREF bg = pressed ? dim(base, kPressDim) : (hover ? dim(base, kHoverDim) : base);

	RECT r = dis->rcItem;

	// 快捷键行右侧的"副编辑器"：画成一只小输入框，跟左边显示当前组合的编辑框并排，
	// 三行上下排开。录入中描边转蓝、文字变"取消"；未解锁时整格置灰
	const int recIdx = setRecIndex(id);
	if (recIdx >= 0) {
		const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
		const bool live = (captureRow == recIdx);
		FillRect(dis->hDC, &r, brushOf(disabled ? C_FILL_GR : (hover ? dim(C_WHITE, kHoverDim) : C_WHITE)));
		FrameRect(dis->hDC, &r, brushOf(live ? C_BTN_BLUE : C_BORDER));
		wchar_t label[64]{};
		GetWindowTextW(dis->hwndItem, label, 64);
		SetBkMode(dis->hDC, TRANSPARENT);
		SetTextColor(dis->hDC, disabled ? C_TEXT_MUTED : (live ? C_BTN_BLUE : C_TEXT));
		HGDIOBJ oldFont = SelectObject(dis->hDC, fontOf(FsBody, 500));
		DrawTextW(dis->hDC, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(dis->hDC, oldFont);
		return;
	}

	// 保存位置的「更改…」：次级按钮，白底 + 细边框。它右边是整页的主操作区，
	// 这里用蓝底会把"改目录"抬到和保存同等的分量，留白描边才合主次
	if (hasId(kSetDirBtnIds, id)) {
		FillRect(dis->hDC, &r, brushOf(hover ? dim(C_WHITE, kHoverDim) : C_WHITE));
		FrameRect(dis->hDC, &r, brushOf(hover ? C_PRIMARY : C_BORDER));
		wchar_t label[64]{};
		GetWindowTextW(dis->hwndItem, label, 64);
		SetBkMode(dis->hDC, TRANSPARENT);
		SetTextColor(dis->hDC, C_TEXT);
		HGDIOBJ oldFont = SelectObject(dis->hDC, fontOf(FsBody, 500));
		DrawTextW(dis->hDC, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(dis->hDC, oldFont);
		return;
	}

	// 一律直角：Win32 的圆角只能靠区域（CreateRoundRectRgn）硬裁，
	// 边缘没有抗锯齿，小尺寸上不是缺角就是发毛，还不如方方正正
	FillRect(dis->hDC, &r, brushOf(bg));

	if (arrow) {
		// 箭头用几何三角形画，不用字体字形：▲/▼ 在不同系统上的符号字体里
		// 基线、行高、字重都不一样，21px 高的按钮里经常偏上偏下甚至被裁掉半截
		const bool up = hasId(kSdUpIds, id);
		const int cx = (r.left + r.right) / 2;
		const int cy = (r.top + r.bottom) / 2;
		const int hw = sc(5);
		const int hh = sc(4);
		POINT pts[3]{};
		if (up) {
			pts[0] = { cx, cy - hh };
			pts[1] = { cx - hw, cy + hh };
			pts[2] = { cx + hw, cy + hh };
		}
		else {
			pts[0] = { cx, cy + hh };
			pts[1] = { cx - hw, cy - hh };
			pts[2] = { cx + hw, cy - hh };
		}
		HGDIOBJ oldBrush = SelectObject(dis->hDC, brushOf(locked ? C_TEXT_MUTED : C_TEXT));
		HGDIOBJ oldPen = SelectObject(dis->hDC, GetStockObject(NULL_PEN));
		Polygon(dis->hDC, pts, 3);
		SelectObject(dis->hDC, oldPen);
		SelectObject(dis->hDC, oldBrush);
		return;
	}

	wchar_t text[128]{};
	GetWindowTextW(dis->hwndItem, text, 128);
	drawItemLabel(dis, text);
}

// ———————————————— 二维码 ————————————————

void WinQr::refreshQr()
{
	if (!hwnd) return;
	auto ed = GetDlgItem(hwnd, QrEdit);
	if (!ed) return;
	// 缓冲区要多留一个字符位：GetWindowTextW 的 nMaxCount 含结尾 0，
	// 正好按长度分配会让最后那个 0 写到 size() 之后
	const int len = GetWindowTextLengthW(ed);
	std::wstring text;
	if (len > 0) {
		text.resize((size_t)len + 1);
		const int got = GetWindowTextW(ed, text.data(), len + 1);
		text.resize(got > 0 ? (size_t)got : 0);
	}

	// 空输入不是"没有二维码"：卡片上始终留一张示例码（内容就是输入框的占位提示）。
	// 点清空之后回到这张示例码，而不是变成一块白板
	if (text.empty()) text = Lang::get(L"qr.placeholder");

	// 容错级别照草料的策略：加了 Logo 自动切 H（30%，Logo 遮挡也能扫）；
	// 没 Logo 走 Auto（草料同款「优先紧凑码型」，在 M 15% / Q 25% 间自动选，见 QrCodec.cpp）
	const int ec = logoPixels.empty() ? QrEcAuto : QrEcH;
	const bool ok = qrEncode(toUtf8(text), qrBmp, ec);
	// qrEncode 失败时不动 qrBmp：这里读到的 size 就是"上一张还在不在"，
	// 用它决定要不要提示 —— 输入超长时每敲一个键都弹一遍太吵
	const bool hadCode = qrBmp.size > 0;
	if (!ok) {
		qrBmp.size = 0;
		qrBmp.modules.clear();
	}
	invalidateQrBitmap();
	// 会变的只有右侧那张卡片：整页重绘把输入框、按钮、页签一起刷一遍，
	// 而这里每敲一个键都会走到，纯属白刷
	RECT card{ sc(QR_CARD_X - 2), sc(QR_TOP - 2),
		sc(QR_CARD_X + QR_SIDE + 2), sc(QR_TOP + QR_SIDE + 2) };
	InvalidateRect(hwnd, &card, FALSE);
	if (!ok && hadCode) showToast(Lang::get(L"qr.encodeFail"));
}

void WinQr::invalidateQrBitmap()
{
	if (qrBmp.size <= 0) {
		// 输入被清空 / 长度超限：这里的旧图必须扔掉，否则界面上还挂着上一串内容的码
		if (qrHbmp) { DeleteObject(qrHbmp); qrHbmp = nullptr; }
		qrDibBits = nullptr;
		qrPx.clear();
		qrHbmpSide = 0;
		return;
	}
	// 预览位图按显示尺寸（qrHbmpSide，drawQrPage 置位）1:1 渲染。曾经固定渲 600
	// 再用 COLORONCOLOR 缩到显示尺寸 —— 最近邻缩放会整列整行地丢采样，版本一高
	// 模块就被抹掉，屏幕上的码本身就残了，截屏识别自然解不出。
	// 首帧还没有显示尺寸时先按 kQrPixels 渲一版占位，drawQrPage 马上会按实际尺寸重建
	renderQrDib(qrHbmpSide > 0 ? qrHbmpSide : kQrPixels);
}

// 按 side 渲染 qrBmp 并落到缓存 DIB 上。side 变了才重建 DIB，
// 否则只重渲染像素再拷进去 —— 输入框每敲一个字都会走到，分配要省着花
void WinQr::renderQrDib(int side)
{
	if (qrBmp.size <= 0 || side <= 0) return;
	if (!qrPx.empty() && qrPx.size() != (size_t)side * side * 4) qrPx.clear();
	if (!renderQrToPixels(side, qrPx)) return;

	// 必须用 CreateDIBSection + 负高度：CreateBitmap 建的 32bpp 位图是自下而上的，
	// 直接把 top-down 的 BGRA 丢进去会让二维码上下翻转，定位图案一翻就解不出来了
	void* bits = nullptr;
	if (!qrHbmp || qrHbmpSide != side) {
		if (qrHbmp) { DeleteObject(qrHbmp); qrHbmp = nullptr; }
		BITMAPINFO bmi{};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = side;
		bmi.bmiHeader.biHeight = -side;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		qrHbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!qrHbmp) { qrDibBits = nullptr; qrHbmpSide = 0; return; }
		qrHbmpSide = side;
	}
	else {
		bits = qrDibBits;
	}
	if (!bits) return;
	qrDibBits = bits;
	CopyMemory(bits, qrPx.data(), qrPx.size());
}

bool WinQr::renderQrToPixels(int side, std::vector<BYTE>& out)
{
	if (qrBmp.size <= 0 || side <= 0) return false;
	out.assign((size_t)side * side * 4, 255);

	const int total = qrBmp.size + kQuietModule * 2;
	for (int py = 0; py < side; py++) {
		const int my = (int)(((long long)py * total) / side) - kQuietModule;
		if (my < 0 || my >= qrBmp.size) continue;
		BYTE* row = out.data() + (size_t)py * side * 4;
		for (int px = 0; px < side; px++) {
			const int mx = (int)(((long long)px * total) / side) - kQuietModule;
			if (mx < 0 || mx >= qrBmp.size) continue;
			if (!qrBmp.modules[(size_t)my * qrBmp.size + mx]) continue;
			BYTE* p = row + (size_t)px * 4;
			p[0] = p[1] = p[2] = 0;
			p[3] = 255;
		}
	}

	if (!logoPixels.empty() && logoW > 0 && logoH > 0) {
		overlayLogo(out, side, side, logoPixels, logoW, logoH, side * 22 / 100);
	}
	return true;
}

void WinQr::copyImage()
{
	std::vector<BYTE> px;
	if (!renderQrToPixels(kQrPixels, px)) {
		showToast(Lang::get(L"qr.encodeFail"));
		return;
	}
	const bool ok = Util::saveToClipboard(kQrPixels, kQrPixels, px.data());
	showToast(Lang::get(ok ? L"qr.copied" : L"qr.copyFail"));
}

void WinQr::saveImage()
{
	std::vector<BYTE> px;
	if (!renderQrToPixels(kQrPixels, px)) {
		showToast(Lang::get(L"qr.encodeFail"));
		return;
	}
	// 不再弹另存为：直接存桌面（时间戳命名，不会重名）。
	// 取桌面失败这种极端情况才回退到对话框，保证总能存出去
	auto path = Util::desktopSavePath(L"png");
	if (path.empty()) path = Util::getSaveFilePath(hwnd);
	if (path.empty()) return;
	if (Util::saveToFile(path, kQrPixels, kQrPixels, px.data())) showToast(Lang::get(L"qr.saved"));
	else showToast(Lang::get(L"qr.saveFail"));
}

void WinQr::pickLogo()
{
	OPENFILENAMEW ofn{};
	wchar_t buf[MAX_PATH]{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hwnd;
	ofn.lpstrFile = buf;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrFilter = L"PNG\0*.png\0All Files\0*.*\0";
	// OFN_NOCHANGEDIR：对话框切目录不改动进程当前目录（程序其他地方不依赖相对路径，纯加固）
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&ofn)) return;

	// 从 COM 创建到解码、缩放、取像素，失败原因对用户来说是同一件事：
	// 这张图用不了。统一一句话，不在七处各抄一遍
	const auto fail = [this] { showToast(Lang::get(L"qr.logoFail")); };

	ComPtr<IWICImagingFactory> factory;
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(factory.GetAddressOf()));
	if (FAILED(hr)) { fail(); return; }

	ComPtr<IWICBitmapDecoder> decoder;
	hr = factory->CreateDecoderFromFilename(buf, nullptr, GENERIC_READ,
		WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf());
	if (FAILED(hr)) { fail(); return; }

	ComPtr<IWICBitmapFrameDecode> frame;
	hr = decoder->GetFrame(0, frame.GetAddressOf());
	if (FAILED(hr)) { fail(); return; }

	ComPtr<IWICBitmapSource> converted;
	hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.Get(), converted.GetAddressOf());
	if (FAILED(hr)) { fail(); return; }

	UINT w = 0, h = 0;
	if (FAILED(converted->GetSize(&w, &h)) || w == 0 || h == 0) {
		fail();
		return;
	}
	// LOGO 最终只贴在二维码中央那块（画布边长的 22%），按最大的二维码画布算也就
	// 两三百像素见方 —— 原图是 4000² 还是 400² 在成品上一个像素都看不出区别。
	// 所以选图时就直接缩到 ≤256²：常驻内存从最多 64MB 降到 256KB，
	// 而且每敲一次键重建二维码时贴图源也小了，刷新更快
	constexpr UINT kLogoMax = 256;
	ComPtr<IWICBitmapSource> src{ converted };
	ComPtr<IWICBitmapScaler> scaler;
	if (w > kLogoMax || h > kLogoMax) {
		hr = factory->CreateBitmapScaler(scaler.GetAddressOf());
		if (FAILED(hr)) { fail(); return; }
		const double f = (double)kLogoMax / (double)(w > h ? w : h);
		const UINT nw = (UINT)std::max(1.0, w * f), nh = (UINT)std::max(1.0, h * f);
		hr = scaler->Initialize(converted.Get(), nw, nh, WICBitmapInterpolationModeFant);
		if (FAILED(hr)) { fail(); return; }
		src = scaler;
		w = nw;
		h = nh;
	}
	std::vector<BYTE> pixels((size_t)w * h * 4, 0);
	const UINT stride = w * 4;
	// WIC 的 CopyPixels 要的缓冲区大小 = stride * h，传 vector 的实际字节数更保险
	hr = src->CopyPixels(nullptr, stride, (UINT)pixels.size(), pixels.data());
	if (FAILED(hr)) {
		fail();
		return;
	}
	logoPixels = std::move(pixels);
	logoW = (int)w;
	logoH = (int)h;
	// Logo 到位后必须重新编码（容错从 Auto 切到 H），只把图盖上去不够：
	// 之前那张还是低容错的码，中心再挡一块 Logo 就顶到容错上限了
	refreshQr();
	showToast(Lang::get(L"qr.logoed"));
}

void WinQr::clearInput()
{
	// 清空 = 清掉文字和 LOGO，然后回到"请输入文本内容！"的示例码。
	// 先把 LOGO 清掉再清文字：反过来的话那一次 EN_CHANGE 会拿着还没清的 logo
	// 先画一张带 LOGO 的示例码，紧接着再被这张干净的覆盖 —— 白画一次
	logoPixels.clear();
	logoW = logoH = 0;
	// 直接发 WM_SETTEXT：它的 EN_CHANGE 会触发 refreshQr 重建二维码
	if (auto ed = GetDlgItem(hwnd, QrEdit)) {
		SendMessageW(ed, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L""));
	}
	refreshQr();   // 输入框本来就是空的时候 EN_CHANGE 不触发，这里补一次
}

void WinQr::killZipNow()
{
	// 纯动作：按一次热键扫一次。不做常驻开关、不写配置、不挂定时器 ——
	// 用户要的是"按一下把压缩软件关掉"，而不是一个要记住状态的开关。
	// closeAll 内部只对 GUI 主程序发 WM_CLOSE，且同一 PID 只发一次
	const int n = ZipKill::closeAll();
	if (n > 0) {
		showToast(Lang::get(L"qr.zipKilledPre") + std::to_wstring(n) + Lang::get(L"qr.zipKilledSuf"));
	}
	else {
		showToast(Lang::get(L"qr.zipNothing"));
	}
}

void WinQr::killZipByHotkey()
{
	// 热键入口（设置页「压缩软件全关」）。界面可能正藏着 —— 主界面不在时不建窗、
	// 不抢焦点，直接干活：这个键的意义就是"人不在主界面上也能一键收摊"
	if (winQr && winQr->hwnd) winQr->killZipNow();
	else ZipKill::closeAll();
}

// 截图启动失败的提示。用静态变量记"上次有没有提示过"，同一个失败原因
// 连续触发时只弹一次 —— 用户按十次热键不该看十条一模一样的消息框
void WinQr::showCapFailTip()
{
    static bool tipShown = false;
    if (tipShown) return;
    tipShown = true;
    // 挂在主界面窗口上：它此刻已被 restore 放回屏幕，正好当提示的宿主
    HWND host = (winQr && winQr->hwnd) ? winQr->hwnd : nullptr;
    MessageBoxW(host, Lang::get(L"cap.failTip").c_str(),
        Lang::get(L"qr.title").c_str(), MB_OK | MB_ICONWARNING);
}

// ———————————————— 抓一块屏幕像素（识码 / 文字识别共用） ————————————————
// 两条路对"隐身多久、什么时候恢复、Esc 取消后主界面怎么回来"的要求完全一样，
// 只有拿到像素之后干什么不同，所以把这一段抽出来，work 里放各自的后处理。
bool WinQr::beginGrab(std::function<void(std::vector<BYTE>&&, int, int)> work)
{
	if (!hwnd) return false;

	// 先把自己从捕获里摘出去再抓屏：主界面要是压在选区里，识别到的就是自己那张脸。
	// 抓屏在 initForGrab 内部同步完成，所以隐身与恢复之间不会漏进用户操作。
	// 设置页可以关掉这一步（有人就是想让窗口留在原地），关掉时 wasVisible 直接为假，
	// 后面那一串恢复也就整套跳过。
	// 优先用「捕获亲和性」（窗口留在屏幕，不闪）；老系统降级 SW_HIDE
	const bool wasVisible = Setting::get()->getHideMainOnCap() && IsWindowVisible(hwnd) != FALSE;
	grabExcludedByAffinity = false;
	if (wasVisible) {
		if (App::setCaptureAffinity(hwnd, true)) {
			grabExcludedByAffinity = true;
		}
		else {
			ShowWindow(hwnd, SW_HIDE);
			// ShowWindow 只是把窗口标记成隐藏，DWM 真正把它从屏幕上撤下来要等下一帧合成。
			// 不等这一下，抓屏常常还能抓到主界面的残影
			DwmFlush();
		}
	}

	// 会话进行中一律置位：热键那一路上来就把窗口收起来了（wasVisible 为假），
	// 但"用户按 Esc 取消框选"这条路同样得有人盯着 —— 否则主界面会停在收起状态
	grabPending = true;

	auto handler = [this, wasVisible, work = std::move(work)](std::vector<BYTE>&& pixels, int w, int h) {
		// 回调可能在主窗口已经销毁之后才来（截图窗口比主界面活得久），
		// 这时 this 已经是野指针。g_live 是唯一存活的实例，对不上就直接放弃结果
		if (g_live != this || !this->hwnd) return;
		this->grabPending = false;
		KillTimer(this->hwnd, TidGrabCheck);
		// 先把主界面放回来再做后处理：文字识别要一两秒，窗口不能一直不见
		if (wasVisible) {
			if (this->grabExcludedByAffinity) {
				App::setCaptureAffinity(this->hwnd, false);
				this->grabExcludedByAffinity = false;
			}
			else {
				ShowWindow(this->hwnd, SW_SHOW);
				SetForegroundWindow(this->hwnd);
			}
		}
		if (work) work(std::move(pixels), w, h);
	};

	if (!WinCap::initForGrab(handler)) {
		grabPending = false;
		if (wasVisible) {
			if (grabExcludedByAffinity) {
				App::setCaptureAffinity(hwnd, false);
				grabExcludedByAffinity = false;
			}
			else ShowWindow(hwnd, SW_SHOW);
		}
		showToast(Lang::get(L"qr.capBusy"));
		return false;
	}
	// 用户按 Esc / 右键取消框选时，完成回调不会来，主界面就永远隐身。
	// 用一个定时器盯着截图窗口的生死：它没了而回调没跑过，就把主界面放回来
	SetTimer(hwnd, TidGrabCheck, 300, nullptr);
	return true;
}

// 「识别」热键：一条热键管两件事。
//   · 在二维码页 —— 先当扫码用（这是老行为，用户手就在二维码页上，要的就是码）；
//     这一块里没有码时回落文字识别，识别结果直接填进输入框，顺手把二维码画出来；
//   · 在别的页 —— 就是纯文字识别，结果落「文字识别」页。
// 之所以用"当前页"当判据：热键只有一条，用户的意图看他站在哪一页最准，
// 比再开一条热键/弹个选择框都省事。
void WinQr::grabRecognize()
{
	if (!hwnd) { g_deferReveal = false; return; }
#if !SNAP_HAS_OCR
	// 不带引擎的版本：这条热键就是「截一段屏幕认二维码」，
	// 没有文字识别可回落（页面上也没有那一页可切）
	qrGrabWithFallback();
#else
	// 站在二维码页：交给二维码页那一套（先识码，没码就回落文字并填进输入框）
	if (page == PageQr) {
		qrGrabWithFallback();
		return;
	}
	if (ocrBusy.load()) {
		showToast(Lang::get(L"ocr.busy"));
		revealAfterGrab();   // 托盘态：界面还没露面，这句提示得跟着界面一起出来
		return;
	}
	// 不在二维码页：纯文字识别。页不在这里切 —— 按键就跳页，用户看到的是"还没框选界面就换了一页"
	// （站在设置页按 F2 时，想截的那一页当场就没了）。标记留到结果回来（ocrOnDone）再切
	if (!OcrEngine::available()) {
		ocrSwitchOnDone = false;
		showToast(Lang::get(L"ocr.noModel"));
		revealAfterGrab();
		return;
	}
	ocrToQrEdit = false;
	ocrSwitchOnDone = page != PageOcr;
	// 抓屏窗没起来（上一轮没收干净、图形设备异常）：提示同样得让用户看见
	if (!beginGrab([this](std::vector<BYTE>&& pixels, int w, int h) {
		ocrStartWorker(std::move(pixels), w, h);
	})) {
		ocrSwitchOnDone = false;
		revealAfterGrab();
	}
#endif
}

// 二维码页的唯一入口：按钮和识别热键都走这里。
// 先当码认；这一块里没有码时，同一个选区改走文字识别，结果折成一行填进输入框，
// 顺手把二维码画出来 —— 等于「截一段文字 → 变成二维码」一步到位。
// 之前按钮只认码（没码就弹"没找到二维码"），热键会回落，两条路行为不一致，这里统一。
void WinQr::qrGrabWithFallback()
{
	if (!hwnd) { g_deferReveal = false; return; }
#if SNAP_HAS_OCR
	// 这条路径的结果进二维码输入框，页当然留在二维码页：不参与"结果回来再切页"那套
	ocrSwitchOnDone = false;
	if (ocrBusy.load()) {
		showToast(Lang::get(L"ocr.busy"));
		revealAfterGrab();
		return;
	}
	ocrToQrEdit = false;   // 先按识码走，真回落了再置位
#endif
	if (!beginGrab([this](std::vector<BYTE>&& pixels, int w, int h) {
		// 小块区域先放大再解：屏幕上的码常常只有十几像素一个模块
		const int scale = (w < 200 || h < 200) ? 3 : 1;
		const std::string text = qrDecode(pixels.data(), w, h, scale);
		if (!text.empty()) {
			if (auto ed = GetDlgItem(hwnd, QrEdit)) {
				SetWindowTextW(ed, fromUtf8(text).c_str());
				refreshQr();      // 不依赖 EN_CHANGE，直接重画一次
			}
			showToast(Lang::get(L"qr.decoded"));
			revealAfterGrab();    // 结果已经进框了，托盘态这时候才亮界面
			return;
		}
#if SNAP_HAS_OCR
		// 没码：回落文字识别，结果回填输入框生成二维码
		if (!OcrEngine::available()) {
			showToast(Lang::get(L"qr.notFound"));
			revealAfterGrab();
			return;
		}
		ocrToQrEdit = true;
		showToast(Lang::get(L"qr.ocrFallback"));
		// 文字识别这一轮会晚一点回来，界面留到 ocrOnDone 里再亮
		ocrStartWorker(std::move(pixels), w, h);
#else
		// 不带引擎：没码就到此为止，不编一句"已按文字识别填充"的假话
		(void)scale;
		showToast(Lang::get(L"qr.notFound"));
		revealAfterGrab();
#endif
	})) revealAfterGrab();
}

// ═══════════════════ 文字识别（只有带引擎的版本才编进来）═══════════════════
// SnapCore（SNAPCORE_WITH_OCR 未定义）整段不参与编译：没有这一页、没有工作线程、
// 也没有 OcrEngine 的任何调用 —— exe 里连一个符号都不剩。以后接引擎把宏打开即可。
#if SNAP_HAS_OCR

// ———————————————— 文字识别页（主界面） ————————————————
// 版面与参考那一页一致：结果内查找 + 大结果框 + 识别/复制/清空 + 字数额。
// 识别本身在 Src/Ocr/OcrEngine.cpp（引擎是独立进程），这里只做界面与线程调度。

// 引擎给的失败原因是一串短代号，翻成人话在界面层做 —— 引擎里不出现任何界面文案
static std::wstring ocrErrorText(const std::wstring& code)
{
	if (code == L"noModel") return Lang::get(L"ocr.noModel");
	if (code == L"empty") return Lang::get(L"ocr.empty");
	if (code == L"busy") return Lang::get(L"ocr.busy");
	return Lang::get(L"ocr.fail");
}

// 不区分大小写地在 hay 里找 needle（从 from 开始）。中文没有大小写概念，
// 这一步只对英文有效；用 _wcsnicmp 逐位比，比自己转小写再找少一次全串拷贝
static size_t findNoCase(const std::wstring& hay, const std::wstring& needle, size_t from)
{
	if (needle.empty() || hay.size() < needle.size()) return std::wstring::npos;
	if (from > hay.size() - needle.size()) return std::wstring::npos;
	for (size_t i = from; i + needle.size() <= hay.size(); i++) {
		if (_wcsnicmp(hay.c_str() + i, needle.c_str(), needle.size()) == 0) return i;
	}
	return std::wstring::npos;
}

void WinQr::createOcrPage()
{
	// 查找条：空框时挂一条灰色提示。EM_SETCUEBANNER 需要 comctl32 v6，
	// 系统没装就静默无效，不影响功能
	if (auto ed = mkEdit(OcrSearch, L"", WS_BORDER | ES_AUTOHSCROLL)) {
		SendMessageW(ed, EM_SETCUEBANNER, TRUE,
			reinterpret_cast<LPARAM>(Lang::get(L"ocr.searchCue").c_str()));
		SendMessageW(ed, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(sc(6), sc(6)));
	}
	mkButton(OcrBtnFind, Lang::get(L"ocr.find").c_str());
	// 结果框：多行 + 竖向滚动 + 回车换行。WS_VSCROLL 建的时候就带上，
	// 内容不多时由 ocrSyncScrollbar 动态收起来（ShowScrollBar 只能改显隐，加不上样式）
	// ES_NOHIDESEL：失焦时也把选中段画出来。搜索框是边打边找的，焦点一直在搜索框上，
	// 没有这个样式的话找到的那一段在结果框里根本看不见（默认失焦即隐藏选中）
	if (auto ed = mkEdit(OcrEdit, L"",
		WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | ES_NOHIDESEL)) {
		SendMessageW(ed, EM_SETCUEBANNER, TRUE,
			reinterpret_cast<LPARAM>(Lang::get(L"ocr.placeholder").c_str()));
		SendMessageW(ed, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(sc(6), sc(6)));
	}
	mkButton(OcrBtnLayout, Lang::get(L"ocr.layout").c_str());
	mkButton(OcrBtnRec, Lang::get(L"ocr.recognize").c_str());
	mkButton(OcrBtnCopy, Lang::get(L"ocr.copy").c_str());
	mkButton(OcrBtnClear, Lang::get(L"ocr.clear").c_str());
	// 档位跟着配置走：老配置里没这个键，就落在 auto
	ocrLayout = Layout::fromKey(Setting::get()->getOcrLayout());
	// 字数额：右对齐、竖向居中，住在底部第四格（原来「关压缩软件」那颗的位置）
	mkStatic(OcrLblCount, L"", SS_RIGHT | SS_CENTERIMAGE);

	// 滚动条的显隐不在这里同步，交给 relayoutOcr 的末尾 —— 这一步控件还是 0×0，
	// 而 ocrSyncScrollbar 的判据是「行高×行数 > 客户区高-6」：0 高的框必然命中，
	// 于是空结果框反而被挂上一根竖滚动条，之后又没人收（实测：布局完客户端宽 526
	// 而不是 543，滚动条白占 17px，还一直是可见的禁用态）
}

void WinQr::relayoutOcr()
{
	auto place = [this](int id, int x, int y, int w, int h) {
		if (auto c = GetDlgItem(hwnd, id)) {
			SetWindowPos(c, nullptr, sc(x), sc(y), sc(w), sc(h), SWP_NOZORDER | SWP_NOACTIVATE);
		}
	};
	place(OcrSearch, MARGIN, 42, OCR_SEARCH_W, 28);
	place(OcrBtnFind, OCR_FIND_X, 42, OCR_FIND_W, 28);
	place(OcrBtnLayout, OCR_LAYOUT_X, 42, OCR_LAYOUT_W, 28);
	place(OcrEdit, MARGIN, OCR_EDIT_Y, CW - MARGIN * 2, OCR_EDIT_H);
	place(OcrBtnRec, OCR_BTN_GX[0], OCR_BTN_Y, OCR_BTN_W, OCR_BTN_H);
	place(OcrBtnCopy, OCR_BTN_GX[1], OCR_BTN_Y, OCR_BTN_W, OCR_BTN_H);
	place(OcrBtnClear, OCR_BTN_GX[2], OCR_BTN_Y, OCR_BTN_W, OCR_BTN_H);
	// 字数额住第四格：与三颗按钮同一行同一高，右对齐贴页边 ——
	// 空出来的那格原来是「关压缩软件」，现在由读数占着，底部这一排仍是齐的四格
	place(OcrLblCount, OCR_COUNT_X, OCR_BTN_Y, OCR_COUNT_W, OCR_BTN_H);

	// 结果框用正文号（18）：比按钮上的 22 小一档，整屏文字不会挤成一团；
	// 查找框跟着结果框走，同一个字号
	const HFONT body = fontOf(FsBody, 400);
	if (auto c = GetDlgItem(hwnd, OcrEdit)) SendMessageW(c, WM_SETFONT, (WPARAM)body, TRUE);
	if (auto c = GetDlgItem(hwnd, OcrSearch)) SendMessageW(c, WM_SETFONT, (WPARAM)body, TRUE);
	if (auto c = GetDlgItem(hwnd, OcrBtnLayout)) SendMessageW(c, WM_SETFONT, (WPARAM)setFont, TRUE);
	if (auto c = GetDlgItem(hwnd, OcrLblCount)) SendMessageW(c, WM_SETFONT, (WPARAM)sdLabelFont, TRUE);

	// 摆放与字体都定了，才轮得到「要不要竖滚动条」：这个判断要看客户区真实高度，
	// 也得用最终字号量出来的行高。放在这里还有一个好处：DPI 切换 / 窗口尺寸变化
	// 都会重走 relayoutOcr，滚动条会按新尺寸重算 —— 原来只在建窗口那一次算，
	// 缩放后原地不动，长文本在小窗里可能就没滚动条了
	ocrSyncScrollbar();
	ocrSyncCount();
}

void WinQr::startScreenOcr()
{
	if (!hwnd) return;
	if (ocrBusy.load()) {
		showToast(Lang::get(L"ocr.busy"));
		return;
	}
	// 模型不在（既没内嵌、exe 旁边也没有 ocr\）时先说清楚，别让用户框完才看见一句失败
	if (!OcrEngine::available()) {
		showToast(Lang::get(L"ocr.noModel"));
		return;
	}
	ocrToQrEdit = false;   // 这页上的「识别」按钮只往结果框里写
	ocrSwitchOnDone = false;   // 本来就在这一页，不用等结果再切
	beginGrab([this](std::vector<BYTE>&& pixels, int w, int h) {
		ocrStartWorker(std::move(pixels), w, h);
	});
}

std::wstring WinQr::ocrText()
{
	auto ed = GetDlgItem(hwnd, OcrEdit);
	if (!ed) return {};
	const int len = GetWindowTextLengthW(ed);
	if (len <= 0) return {};
	// GetWindowTextW 的 nMaxCount 含结尾 0：缓冲要按 len+1 开，取完再裁回 len
	std::wstring out((size_t)len + 1, L'\0');
	GetWindowTextW(ed, out.data(), len + 1);
	out.resize(len);
	return out;
}

void WinQr::ocrFillText(const std::wstring& text)
{
	auto ed = GetDlgItem(hwnd, OcrEdit);
	if (!ed) return;
	SetWindowTextW(ed, text.c_str());
	// 新结果从第一行开始看，光标记在末尾会滚到最后一行
	SendMessageW(ed, EM_SETSEL, 0, 0);
	SendMessageW(ed, EM_SCROLLCARET, 0, 0);
	ocrSyncScrollbar();
	ocrSyncCount();
}

// 结果框右侧滚动条：内容装得下就收起来。行数必须用 EM_GETLINECOUNT ——
// 只读/可编辑多行 EDIT 会自动折行，数 \r\n 得到的永远是"逻辑行"，
// 长一行折成三行时它说装得下，实际却滚不动
void WinQr::ocrSyncScrollbar()
{
	auto ed = GetDlgItem(hwnd, OcrEdit);
	if (!ed) return;
	const int lines = (int)SendMessageW(ed, EM_GETLINECOUNT, 0, 0);
	int lineH = sc(FsBody);
	if (HDC dc = GetDC(ed)) {
		HGDIOBJ old = SelectObject(dc, fontOf(FsBody, 400));
		TEXTMETRICW tm{};
		if (GetTextMetricsW(dc, &tm) && tm.tmHeight > 0) lineH = tm.tmHeight;
		SelectObject(dc, old);
		ReleaseDC(ed, dc);
	}
	RECT rc{};
	GetClientRect(ed, &rc);
	const bool need = lines > 0 && lines * lineH > (rc.bottom - rc.top) - sc(6);
	ShowScrollBar(ed, SB_VERT, need);
}

void WinQr::ocrSyncCount()
{
	auto lbl = GetDlgItem(hwnd, OcrLblCount);
	if (!lbl) return;
	const size_t n = ocrText().size();
	std::wstring text;
	if (n > 0) text = std::to_wstring(n) + L" " + Lang::get(L"ocr.count");
	SetWindowTextW(lbl, text.c_str());
}

void WinQr::ocrCopy()
{
	const auto text = ocrText();
	if (text.empty()) return;
	showToast(Util::copyText(text) ? Lang::get(L"ocr.copied") : Lang::get(L"ocr.copyFail"));
}

void WinQr::ocrClear()
{
	// 清空就把缓存的行一起丢掉，否则点"排版"会把刚清掉的文字又排回来
	{
		std::lock_guard<std::mutex> g(ocrMutex);
		ocrLines.clear();
		ocrResult.clear();
	}
	if (auto ed = GetDlgItem(hwnd, OcrEdit)) SetWindowTextW(ed, L"");
	if (auto ed = GetDlgItem(hwnd, OcrSearch)) SetWindowTextW(ed, L"");
	ocrSyncScrollbar();
	ocrSyncCount();
	if (auto ed = GetDlgItem(hwnd, OcrEdit)) SetFocus(ed);
}

// 换排版档位：文本和坐标都在缓存里，重排一遍就行 —— 不重新调引擎，所以是瞬时的。
// 没有识别结果时只切档位（下次识别直接按新档位出结果）。
void WinQr::ocrCycleLayout()
{
	ocrLayout = Layout::next(ocrLayout);
	Setting::get()->setOcrLayout(Layout::keyOf(ocrLayout));
	switch (ocrLayout) {
	case Layout::Mode::Line: showToast(Lang::get(L"ocr.layoutLine")); break;
	case Layout::Mode::Flat: showToast(Lang::get(L"ocr.layoutFlat")); break;
	case Layout::Mode::Raw: showToast(Lang::get(L"ocr.layoutRaw")); break;
	default: showToast(Lang::get(L"ocr.layoutAuto")); break;
	}

	std::vector<OcrLine> lines;
	{
		std::lock_guard<std::mutex> g(ocrMutex);
		lines = ocrLines;
	}
	if (!lines.empty()) ocrFillText(Layout::format(lines, ocrLayout));
}

// 查找：从当前光标处往后找，到头了绕回开头再找一遍（第二次还找不到才算没有）
void WinQr::ocrFind()
{
	auto ed = GetDlgItem(hwnd, OcrEdit);
	auto box = GetDlgItem(hwnd, OcrSearch);
	if (!ed || !box) return;
	const int nlen = GetWindowTextLengthW(box);
	if (nlen <= 0) { SetFocus(box); return; }
	std::wstring needle((size_t)nlen + 1, L'\0');
	GetWindowTextW(box, needle.data(), nlen + 1);
	needle.resize(nlen);
	const auto hay = ocrText();
	if (hay.empty()) { showToast(Lang::get(L"ocr.empty")); return; }

	DWORD selStart = 0, selEnd = 0;
	SendMessageW(ed, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
	size_t pos = findNoCase(hay, needle, (size_t)selEnd);
	if (pos == std::wstring::npos) pos = findNoCase(hay, needle, 0);   // 绕回开头
	if (pos == std::wstring::npos) {
		showToast(Lang::get(L"ocr.notFound") + L"：" + needle);
		return;
	}
	SendMessageW(ed, EM_SETSEL, (WPARAM)pos, (LPARAM)(pos + needle.size()));
	SendMessageW(ed, EM_SCROLLCARET, 0, 0);
	SetFocus(ed);
}

// 边打边找：输入框一变就定位到第一处命中并选中。
// 与 ocrFind() 的分工是「输入时定位第一处，点查找时往后跳下一处」——
// 定位一律从 0 开始，不接上一次的光标，否则改一个字母就跑到文档末尾去了。
// 找不到就静默（连打三个字母弹三次提示没人受得了），只把上一次的选中收掉，
// 免得界面上留着一段"上一轮的命中"误导人。
void WinQr::ocrFindLive()
{
	auto ed = GetDlgItem(hwnd, OcrEdit);
	auto box = GetDlgItem(hwnd, OcrSearch);
	if (!ed || !box) return;
	const auto hay = ocrText();

	const int nlen = GetWindowTextLengthW(box);
	std::wstring needle;
	if (nlen > 0) {
		needle.resize((size_t)nlen + 1, L'\0');
		GetWindowTextW(box, needle.data(), nlen + 1);
		needle.resize(nlen);
	}

	size_t hit = needle.empty() ? std::wstring::npos : findNoCase(hay, needle, 0);
	if (hit == std::wstring::npos) {
		// 撤掉选中但保持插入点原地不动：把选中收成它自己的起点即可
		DWORD selStart = 0, selEnd = 0;
		SendMessageW(ed, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
		if (selEnd > selStart) SendMessageW(ed, EM_SETSEL, (WPARAM)selStart, (LPARAM)selStart);
		return;
	}
	SendMessageW(ed, EM_SETSEL, (WPARAM)hit, (LPARAM)(hit + needle.size()));
	SendMessageW(ed, EM_SCROLLCARET, 0, 0);
}

void WinQr::ocrJoinWorker()
{
	if (ocrThread.joinable()) ocrThread.join();
}

void WinQr::ocrStartWorker(std::vector<BYTE>&& pixels, int w, int h)
{
	if (ocrBusy.exchange(true)) {
		// 上一拍还在识别：这批像素不要了，但得回一声 —— 两条入口（按钮 / 二维码回落）
		// 都是先查 busy 再 beginGrab，中间隔着用户框选那几秒，连按两次热键就能越过
		// 那道检查走到这里。静默丢弃会让人以为"识别按钮没反应"
		showToast(Lang::get(L"ocr.busy"));
		return;
	}
	ocrJoinWorker();          // 收掉上一轮的线程对象（它早跑完了，join 立即返回）
	showToast(Lang::get(L"ocr.recognizing"));

	HWND self = hwnd;
	std::vector<BYTE> px = std::move(pixels);
	// 起线程这一步也可能失败（句柄或内存耗尽）。失败后忙标记再不复位，OCR 就永久停在
	// "正在识别"：识别按钮点了只弹一条 busy，再也认不了任何东西
	try {
		ocrThread = std::thread([this, px = std::move(px), w, h, self]() {
			std::wstring text, err;
			std::vector<OcrLine> lines;
			const bool ok = OcrEngine::recognize(px.data(), w, h, text, err, &lines);
			{
				std::lock_guard<std::mutex> g(ocrMutex);
				ocrResult = std::move(text);
				ocrLines = std::move(lines);
				ocrOk = ok;
				ocrFail = std::move(err);
			}
			ocrBusy.store(false);
			if (self && IsWindow(self)) PostMessageW(self, MsgOcrDone, 0, 0);
			});
	}
	catch (...) {
		ocrBusy.store(false);
		// 托盘态"等结果出来再亮界面"的标记也要消费掉：正常路径靠 ocrOnDone
		// 开头的 Reveal 兜底，线程根本没起成的这一路不会发消息 —— 不补一句，
		// 主界面就一直藏在托盘里，用户只能再按一次热键才能把它叫出来
		revealAfterGrab();
		showToast(Lang::get(L"ocr.fail"));
	}
}

void WinQr::ocrOnDone()
{
	// 这一轮识别到此结束（成功 / 空结果 / 失败都算）。托盘态触发的，界面就是等着
	// 这个时刻才亮的：用 guard 兜住下面几个 return 出口，省得以后加分支又忘了亮
	struct Reveal { ~Reveal() { WinQr::revealAfterGrab(); } } revealGuard;
	// 这条消息可能是"上一轮"的迟到件：工作线程先置 ocrBusy=false 再 PostMessage，
	// 而框选回调（松开鼠标）也是队列消息 —— 两者挨得够近时，新一轮已经用 exchange
	// 抢到了忙标记，这条旧消息才被取出来。照原样往下走会 join 掉**新一轮**的线程，
	// 主线程在等一次完整识别，界面假死几秒；填进去的还会是旧结果。
	// busy 为真就说明有更新的一轮在跑，它完成时自己会发消息，这条丢掉即可
	if (ocrBusy.load()) return;
	ocrJoinWorker();
	std::wstring text, err;
	std::vector<OcrLine> lines;
	bool ok = false;
	{
		std::lock_guard<std::mutex> g(ocrMutex);
		text = ocrResult;
		lines = ocrLines;
		err = ocrFail;
		ok = ocrOk;
	}
	if (!hwnd) return;
	// 热键在别的页按下的：到这一刻（有结果了）才切到文字识别页 ——
	// 成功 / 空结果 / 失败都切，用户至少能看到"识别过了、结果是这个"
	if (ocrSwitchOnDone) {
		ocrSwitchOnDone = false;
		if (page != PageOcr) setPage(PageOcr);
	}
	// 结果已经出来了，"正在识别…"这条进度提示没有存在意义，当场收掉；
	// 下面报错/空结果会另起一条，成功就直接什么都不提示
	hideToast();
	// 这一轮识别到此结束（成功 / 空结果 / 失败都算）：先把空闲回收的定时器挂上。
	// 引擎常驻着，主界面收进托盘之后没人再调识别，100MB 的推理进程不该一直占着。
	// 以前这行只在"填文字结果框"那条出口上，二维码回落这条路径（包括回落拿到空结果
	// 直接 return 的分支）根本没挂 —— 回落一次之后引擎就永久驻留
	SetTimer(hwnd, TidOcrIdle, 30000, nullptr);
	if (!ok) {
		// 二维码页回落这一轮失败：静默 —— 识码没中在 qr.ocrFallback 里已经说过，
		// 再补一句"文字识别失败"是多余的（用户只要码，回落只是顺带一试）
		const bool wasFallback = ocrToQrEdit;
		ocrToQrEdit = false;   // 这一轮没结果，别让残留标记把下一次识别结果填进二维码框
		if (!wasFallback)
			showToast(err.empty() ? Lang::get(L"ocr.fail") : ocrErrorText(err));
		return;
	}
	// 排版解析：坐标和文本都在手上，档位不同只是重排一遍，不花识别时间
	if (!lines.empty()) text = Layout::format(lines, ocrLayout);

	if (ocrToQrEdit) {
		ocrToQrEdit = false;
		// 一段多行文字做成二维码不好扫、还更容易超容量，折成一行再填
		std::wstring flat;
		flat.reserve(text.size());
		for (wchar_t ch : text) flat += (ch == L'\r' || ch == L'\n' || ch == L'\t') ? L' ' : ch;
		if (flat.empty()) {
			// 回落也没识别出文字：静默 —— "已按文字识别"那句已经提示过，
			// 再弹一句"没有识别到文字"是多余的
			return;
		}
		if (auto ed = GetDlgItem(hwnd, QrEdit)) {
			SetWindowTextW(ed, flat.c_str());
			refreshQr();               // 立刻把二维码画出来（内容太长时它会自己提示）
			SendMessageW(ed, EM_SETSEL, 0, (LPARAM)flat.size());
			SetFocus(ed);
		}
		return;
	}
	ocrFillText(text);
}

void WinQr::ocrIdleCheck()
{
	const ULONGLONG last = OcrEngine::lastUseTick();
	if (ocrBusy.load() || last == 0) return;
	if (GetTickCount64() - last > kOcrIdleMs) {
		OcrEngine::shutdown();
		KillTimer(hwnd, TidOcrIdle);
	}
}

#endif  // SNAP_HAS_OCR

// ———————————————— 定时关机 ————————————————

// 提权是纯 Win32 调用，不碰 this，没必要挂在类上（原来还得在头文件里留声明）。
// 放在这一段的最前面：sdTick 到点时要调，定义得先于使用
namespace {
	bool enableShutdownPrivilege()
	{
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
			return false;
		}
		TOKEN_PRIVILEGES tp{};
		LUID luid{};
		BOOL ok = FALSE;
		if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &luid)) {
			tp.PrivilegeCount = 1;
			tp.Privileges[0].Luid = luid;
			tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
			ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
		}
		// 返回 TRUE 只代表调用被接受：权限到底有没有加上要看 GetLastError
		const DWORD err = GetLastError();
		CloseHandle(token);
		return ok && err == ERROR_SUCCESS;
	}
}

int WinQr::sdValue(int editId) const
{
	auto c = GetDlgItem(hwnd, editId);
	if (!c) return 0;
	wchar_t buf[32]{};
	GetWindowTextW(c, buf, 32);
	// 空串 / 非数字都按 0；负数（ES_NUMBER 下敲不出来，但粘贴可能）也按 0
	const long v = wcstol(buf, nullptr, 10);
	if (v < 0) return 0;
	return static_cast<int>(v > SdFieldMax ? SdFieldMax : v);
}

void WinQr::sdSetValue(int editId, long long v)
{
	auto c = GetDlgItem(hwnd, editId);
	if (!c) return;
	wchar_t buf[32]{};
	if (editId == SdEditDay) swprintf_s(buf, L"%lld", v);
	else swprintf_s(buf, L"%02lld", v);
	// 值没变就不写：倒计时每秒回写四格，天/时基本不动，而每次 SetWindowTextW
	// 都要触发一次 EN_CHANGE 和一次重绘 —— 白烧
	wchar_t cur[32]{};
	GetWindowTextW(c, cur, 32);
	if (wcscmp(cur, buf) == 0) return;
	SetWindowTextW(c, buf);
}

void WinQr::sdNudge(int editId, int dir)
{
	int idx = 0;
	while (idx < 4 && kSdEditIds[idx] != editId) idx++;
	if (idx == 4) return;
	const int maxV = kSdFieldMax[idx];

	// 步进到顶就绕回 0、到底就绕到顶。手输允许到 99（比本格上限大）时，
	// 越界值按一次就落到 0 —— 和"59 再往上"是同一个环绕动作
	int v = sdValue(editId) + dir;
	if (v < 0) v = maxV;
	else if (v > maxV) v = 0;
	sdSetValue(editId, v);
	sdUpdateStatus();
}

long long WinQr::sdTotalSeconds() const
{
	return (long long)sdValue(SdEditDay) * 86400
		+ (long long)sdValue(SdEditHour) * 3600
		+ (long long)sdValue(SdEditMin) * 60
		+ (long long)sdValue(SdEditSec);
}

// 「开始」与「暂停」两条恢复路径共用这一段：以"现在 + 剩余"重新立截止时刻，
// 暂停期间的时间不计入。
// 提醒标志一律复位：暂停时若已进入最后 10 秒，标志位是 true，恢复完到点就会
// 静悄悄关机；复位后下一拍按 remain <= 10 重新判定，用户至少还能看到一次窗口
void WinQr::sdResume()
{
	sdPaused = false;
	sdWarned = false;
	sdDeadline = GetTickCount64() + (ULONGLONG)sdRemain * 1000;
	SetTimer(hwnd, TidShutdown, 1000, nullptr);
	sdUpdateStatus();
}

void WinQr::sdStart()
{
	// 正在跑且没暂停时再按"开始"没有意义，而原来会按当前剩余重开一轮：
	// 进度条从已走的比例瞬间跳回 0%
	if (sdRunning && !sdPaused) return;
	// 暂停状态下按"开始"是恢复，不是拿输入框的值重新起算
	if (sdRunning) {
		sdResume();
		return;
	}
	const long long total = sdTotalSeconds();
	if (total <= 0) {
		showToast(Lang::get(L"qr.timeInvalid"));
		return;
	}
	sdRunning = true;
	sdPaused = false;
	sdDone = false;
	sdWarned = false;
	sdRemain = total;
	sdMax = total;
	sdDeadline = GetTickCount64() + (ULONGLONG)total * 1000;
	// 顺手把四个格子规范化（用户可能输的是 "1" / " 5"），也让"这次真的开始跑了"
	// 有个看得见的变化
	sdWriteRemain(sdRemain);
	SetTimer(hwnd, TidShutdown, 1000, nullptr);
	sdUpdateStatus();
}

void WinQr::sdPause()
{
	if (!sdRunning) return;
	if (!sdPaused) {
		// 暂停时把剩余量现算一遍：tick 是整秒跳的，拿上一次的读数当基准，
		// 恢复后总时长会平白多出将近一秒。
		// 算完立刻回写四格 —— 状态条和格子读的是同一个 sdRemain，只更一处会出现
		// "格子多一秒、下面少一秒"的错位
		const ULONGLONG now = GetTickCount64();
		const long long r = sdDeadline > now
			? (long long)((sdDeadline - now + 999) / 1000) : 0;
		if (r != sdRemain) {
			sdRemain = r;
			sdWriteRemain(sdRemain);
		}
		sdPaused = true;
		KillTimer(hwnd, TidShutdown);
		sdUpdateStatus();
		return;
	}
	sdResume();
}

void WinQr::sdClear()
{
	// 倒计时正在跑的时候"清空"是禁用的：先暂停或等它结束再清，
	// 否则误点一下就把还有几小时的关机任务取消了
	if (sdRunning && !sdPaused) return;
	KillTimer(hwnd, TidShutdown);
	// 一次点击就要一步到位回到「未开始」，不再让某个中间状态亮两秒
	sdRunning = false;
	sdPaused = false;
	sdDone = false;
	sdWarned = false;
	sdRemain = 0;
	sdMax = 1;
	sdDeadline = 0;
	sdWriteRemain(0);
	sdUpdateStatus();
}

// 剩余秒数 → 天/时/分/秒。倒计时过程中每秒回写一次，四个格子里看到的就是
// "还剩多久"；暂停时停在当前值上，不会跳回当初设的数字
void WinQr::sdWriteRemain(long long secs)
{
	if (secs < 0) secs = 0;
	sdSetValue(SdEditDay, secs / 86400);
	sdSetValue(SdEditHour, (secs % 86400) / 3600);
	sdSetValue(SdEditMin, (secs % 3600) / 60);
	sdSetValue(SdEditSec, secs % 60);
}

void WinQr::sdWarnWindow()
{
	if (!hwnd) return;
	// 最后 10 秒才弹：早了用户会关掉继续干别的，晚了他来不及反应。
	// 窗口可能被收进托盘、也可能被最小化，两种都要还原
	if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
	ShowWindow(hwnd, SW_SHOW);
	// 切到关机页：暂停、重设都在这一页上，弹出来却停在二维码页等于没弹
	if (page != PageShutdown) setPage(PageShutdown);
	// 前台正被别的程序占着时系统会拦 SetForegroundWindow，抢不到就闪任务栏，
	// 至少让用户知道这儿有事发生
	if (!SetForegroundWindow(hwnd)) {
		FLASHWINFO fi{};
		fi.cbSize = sizeof(fi);
		fi.hwnd = hwnd;
		fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
		FlashWindowEx(&fi);
	}
}

void WinQr::sdTick()
{
	if (!sdRunning || sdPaused) return;
	// 用截止时刻反算剩余秒数，而不是"每秒减一"：
	//   · 定时器会被系统合并到相邻的调度片上，实际周期常常是 1015~1050ms，
	//     逐次减一跑一分钟能跑成六十五秒；
	//   · 界面重绘、弹提醒窗口也都在这个线程上，逐次减一还会再被吃掉一点。
	// 向上取整（+999）保证"3 秒"这一档完整显示满一秒，不会刚显示就跳。
	const ULONGLONG now = GetTickCount64();
	long long remain = 0;
	if (sdDeadline > now) remain = (long long)((sdDeadline - now + 999) / 1000);
	if (remain != sdRemain) {
		sdRemain = remain;
		// 四个数字格跟着走 —— 用户要的"自动减少时间"就是这一句：
		// 格子里显示的是剩余量，和下面状态条上的读数是同一个数
		sdWriteRemain(sdRemain);
	}
	// 剩 10 秒时把主界面顶出来，让用户能临时暂停或改时间。
	// 没暂停就照原计划关机 —— 这个提醒只是给个机会，不是取消
	if (!sdWarned && sdRemain <= 10) {
		sdWarned = true;
		sdWarnWindow();
	}
	if (sdRemain > 0) {
		sdUpdateStatus();
		return;
	}

	sdRemain = 0;
	sdRunning = false;
	sdDone = true;
	KillTimer(hwnd, TidShutdown);
	sdUpdateStatus();

	if (!enableShutdownPrivilege()) {
		sdAbort(L"qr.privilegeFail");
		return;
	}
	// EWX_FORCE 用来顶掉"有程序未响应，是否仍要关机"的等待框
	if (!ExitWindowsEx(EWX_POWEROFF | EWX_FORCE,
		SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER)) {
		sdAbort(L"qr.shutdownFail");
	}
}

// 关机没成：机器还开着，界面退回「未开始」。只清 sdDone 会留下
// "满格进度条 + 未开始"的自相矛盾，所以进度条分母一并归零；输入格保持原样，
// 用户补好权限后能直接再点一次开始
void WinQr::sdAbort(const wchar_t* msg)
{
	sdDone = false;
	sdMax = 1;
	sdUpdateStatus();
	showToast(Lang::get(msg));
}

void WinQr::sdUpdateStatus()
{
	if (!hwnd) return;
	const bool busy = sdRunning && !sdPaused;
	// 运行中（且没暂停）才锁「清空」：暂停之后要能一键清掉，倒计时结束也要能清
	if (auto b = GetDlgItem(hwnd, SdReset)) {
		const BOOL enable = busy ? FALSE : TRUE;
		if (IsWindowEnabled(b) != enable) EnableWindow(b, enable);
	}
	// 倒计时跑起来之后，四个数字格从"输入框"变成"剩余时间读数"：每秒回写一次。
	// 这时必须设成只读，否则用户改完数字下一拍就被覆盖，看着像"改了没反应"；
	// ▲▼ 同理（sdNudge 写进去也立刻被覆盖），一起禁用。
	// 用 EM_SETREADONLY 而不是 EnableWindow：禁用的 EDIT 会把字画成灰色，
	// 而这里是正在跳动的读数，灰掉像是"坏了"
	const BOOL readOnly = sdRunning ? TRUE : FALSE;
	for (int id : kSdEditIds) {
		auto c = GetDlgItem(hwnd, id);
		if (!c) continue;
		// EDIT 没有 EM_GETREADONLY（winuser.h 只给了 EM_SETREADONLY），当前状态只能从窗口样式读
		const bool isRO = (GetWindowLongW(c, GWL_STYLE) & ES_READONLY) != 0;
		if (isRO != (readOnly != 0)) {
			SendMessageW(c, EM_SETREADONLY, (WPARAM)readOnly, 0);
		}
	}
	// ▲▼ 也是自绘子窗口：只 EnableWindow 不会让它重画，得自己置脏（同上面三只按钮）
	for (int id : kSdArrowIds) {
		auto c = GetDlgItem(hwnd, id);
		if (!c) continue;
		if (IsWindowEnabled(c) != !readOnly) {
			EnableWindow(c, !readOnly);
			RedrawWindow(c, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
		}
	}
	if (page != PageShutdown) return;

	RECT r{ sc(MARGIN), sc(SD_Y_TITLE), sc(CW - MARGIN), sc(SD_Y_BAR + SD_BAR_H) };
	InvalidateRect(hwnd, &r, FALSE);
	// 三只按钮（开始 / 暂停 / 清空）的颜色是自绘时按 sdRunning/sdPaused 现算的，
	// 而 InvalidateRect(hwnd, ...) 只让**父窗口**重画，自绘按钮是子窗口，
	// 它那块无效区根本不会被置脏 —— 于是"点了开始，暂停还是灰的，
	// 把鼠标移上去才亮"。状态一变就得逐个把按钮自己置脏并当场画掉。
	// 只在真的变了才做：tick 每秒都会调到这里，无条件重画等于每秒白烧三次
	const bool changed = (sdUiBusy != busy);
	sdUiBusy = busy;
	if (!changed) return;
	for (int id : kSdBtnIds) {
		if (auto b = GetDlgItem(hwnd, id)) {
			RedrawWindow(b, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
		}
	}
}

// ———————————————— 设置页 / 快捷键 ————————————————

void WinQr::refreshShortcutRows()
{
	auto s = Setting::get();
	const auto none = Lang::get(L"qr.setNone");
	const auto record = Lang::get(L"qr.setRecord");
	const auto cancel = Lang::get(L"qr.setCancel");
	const auto saved = Lang::get(L"qr.setSaved");

	for (int i = 0; i < kSetRowCount; i++) {
		if (auto d = GetDlgItem(hwnd, kSetDispIds[i])) {
			auto v = s->getShortcutKey(kSetRowTypes[i]);
			SetWindowTextW(d, v.empty() ? none.c_str() : v.c_str());
		}
		if (auto b = GetDlgItem(hwnd, kSetRecIds[i])) {
			const std::wstring& text = (captureRow == i) ? cancel
				: (recSavedRow == i ? saved : record);
			SetWindowTextW(b, text.c_str());
		}
	}
}

void WinQr::refreshSaveDirs()
{
	// 显示的是"实际会落盘的那个目录"：配置里空着（没设过）、或者当初选的目录
	// 已经不在了（被删 / 盘拔了），Util::resolveSaveDir 都会回落到桌面。
	// 显示与落盘走同一个判定，才不会出现"设置页写着 D:\，文件却落在桌面"
	auto s = Setting::get();
	const std::wstring dirs[] = {
		Util::resolveSaveDir(s->getShotDir()),
		Util::resolveSaveDir(s->getVideoDir()),
	};
	for (int i = 0; i < 2; i++) {
		if (auto e = GetDlgItem(hwnd, kSetDirDispIds[i])) {
			SetWindowTextW(e, dirs[i].c_str());
		}
	}
}

void WinQr::chooseSaveDir(bool video)
{
	auto s = Setting::get();
	// 对话框从当前生效的目录开始 —— 用户多半只是往里挑一层子目录，
	// 让他每次都从"此电脑"翻起是纯折腾
	const auto cur = Util::resolveSaveDir(video ? s->getVideoDir() : s->getShotDir());
	const auto picked = Util::pickFolder(hwnd, cur);
	if (picked.empty()) return;   // 用户取消
	// 目录存在不等于能写：只读目录、写保护的 U 盘、断连的网络盘都过得了对话框的
	// FOS_PATHMUSTEXIST，但存下来之后截图 / 录屏会静默落不了盘。当场拦下让用户重选
	if (!Util::isDirWritable(picked)) {
		showToast(Lang::get(L"qr.dirNotWritable"));
		return;
	}
	if (video) s->setVideoDir(picked);
	else s->setShotDir(picked);
	refreshSaveDirs();
	showToast(Lang::get(video ? L"qr.videoDirChanged" : L"qr.shotDirChanged"));
}

// 设置页的显示框是只读 EDIT：焦点在它上面时按键消息直接进 EDIT 自己的窗口过程，
// 主窗口的 WM_KEYDOWN 收不到。所以在消息循环这一层截一道 ——
// 焦点落在某一行显示框上按 Delete / 退格，就把这一行的快捷键清掉，
// 不用先点「录制」再按一次 Delete（老路径要两步，还容易按错行）。
bool WinQr::handleAppKey(const MSG& msg)
{
	if (!winQr || !winQr->hwnd) return false;
	if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
	if (msg.wParam != VK_DELETE && msg.wParam != VK_BACK) return false;

	// 录入进行中：键归 pollCaptureKeys 管（那里也会把 Delete 当成"清空这一行"），
	// 这里让开，免得同一次按键被处理两遍
	WinQr* self = winQr.get();
	if (self->captureRow >= 0) return false;

	for (int i = 0; i < kSetRowCount; i++) {
		if (GetDlgItem(self->hwnd, kSetDispIds[i]) != msg.hwnd) continue;
		// 本来就是空的：吃掉按键就行，不必再写一遍配置、也不必闪"已清空"
		if (Setting::get()->getShortcutKey(kSetRowTypes[i]).empty()) return true;
		self->clearShortcutRow(i);
		return true;
	}
	return false;
}

// 清空某一行 = 这一项不设热键。两条入口（录入中按退格 / Delete、焦点停在显示框上按
// Delete）走同一段，落盘与"已保存"提示只有一个来源
void WinQr::clearShortcutRow(int row)
{
	if (row < 0 || row >= kSetRowCount) return;
	// setShortcutKey 认空组合：写空串 + 注销这一组的注册，重启也不会把默认值填回来
	Setting::get()->setShortcutKey(kSetRowTypes[row], {});
	recSavedRow = row;
	refreshShortcutRows();
	SetTimer(hwnd, TidRecSaved, 1500, nullptr);
}

void WinQr::beginCapture(int row)
{
	if (row < 0 || row >= kSetRowCount) return;
	// 已经有一行在录：先收干净那一行，再把目标换到这一行
	if (captureRow >= 0) endCapture();

	captureRow = row;
	capturedMods = 0;
	for (int& t : modHoldTicks) t = 0;
	prevFocus = GetFocus();

	// 排空候选键与几个"功能键"的"按下沿"：上一次按键残留的状态不能立刻被当成本次输入。
	// 清空键（退格 / Delete）和取消键（Esc）也要排 —— 别处按过它们会留下"按过"标记，
	// 不清掉的话一进录入就会被当成"用户要清空 / 要取消"，这一行看着就录不进去
	for (int vk : kCandidateVks) GetAsyncKeyState(vk);
	for (UINT vk : { (UINT)VK_CONTROL, (UINT)VK_SHIFT, (UINT)VK_MENU,
		(UINT)VK_LWIN, (UINT)VK_RWIN, (UINT)VK_BACK, (UINT)VK_DELETE, (UINT)VK_ESCAPE }) {
		GetAsyncKeyState(vk);
	}

	// 录入期间先注销全局热键：否则按下正在用的组合会直接触发截图，界面根本收不到
	Setting::get()->setHotkeysEnabled(false);
	refreshShortcutRows();

	if (auto d = GetDlgItem(hwnd, kSetDispIds[row])) {
		SetWindowTextW(d, Lang::get(L"shortcut.pressKey").c_str());
	}
	SetTimer(hwnd, TidCapture, 40, nullptr);
	// 焦点给主窗口而不是按钮：录入期间按空格/回车才不会被默认按钮吃掉
	SetFocus(hwnd);
}

void WinQr::endCapture()
{
	if (captureRow < 0) return;
	KillTimer(hwnd, TidCapture);
	captureRow = -1;
	capturedMods = 0;

	// 录入期间热键是注销状态的，这里统一恢复。放在 setShortcutKey 之后调用时，
	// 它会按刚保存的组合重新注册所有热键。
	// 注册不上一律是"被别的程序先占了"：不提示的话，界面写着已保存、按下去毫无反应
	if (!Setting::get()->setHotkeysEnabled(true)) {
		showToast(Lang::get(L"qr.hotkeyFail"));
	}
	refreshShortcutRows();

	if (prevFocus && IsWindow(prevFocus)) SetFocus(prevFocus);
	prevFocus = nullptr;
}

void WinQr::pollCaptureKeys()
{
	if (captureRow < 0) return;

	// 修饰键用边沿累积：快速点按 Ctrl+Shift+A 时，等到检测到 A 的那一刻
	// Ctrl 可能已经松了，瞬时态会漏掉。
	// 但累积不能只增不减：用户按错一次 Ctrl，之后松开它再按 F8，录出来会变成
	// Ctrl+F8。所以每个修饰键再记一个"松开多少拍"，连续 5 拍（约 200ms）都没按，
	// 就从累积里清掉 —— 快速点按的窗口（几十毫秒）不受影响
	{
		const UINT modVks[4]{ VK_CONTROL, VK_SHIFT, VK_MENU, (UINT)(VK_LWIN) };
		const DWORD modBits[4]{ MOD_CONTROL, MOD_SHIFT, MOD_ALT, MOD_WIN };
		for (int i = 0; i < 4; i++) {
			bool down{};
			if (i == 3) down = (GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000;
			else down = GetAsyncKeyState(modVks[i]) & 0x8000;
			if (down) {
				capturedMods |= modBits[i];
				modHoldTicks[i] = 0;
			}
			else if (capturedMods & modBits[i]) {
				if (++modHoldTicks[i] >= 5) {
					capturedMods &= ~modBits[i];
					modHoldTicks[i] = 0;
				}
			}
		}
	}

	// Esc = 放弃这次录入（不写配置）。没有这个出口的话，用户想取消只能按退格把原组合
	// 清掉；而录入期间全局热键是注销的，停在"请按键…"里越久越像整套快捷键都坏了
	if (GetAsyncKeyState(VK_ESCAPE) & 0x8001) {
		recSavedRow = -1;
		endCapture();
		return;
	}

	// 退格 / Delete = 把这行清空（= 这项不设热键）。低字节 0x0001 表示"上次查询之后按过"，
	// 快速点一下也不会被 40ms 轮询漏掉。先落配置再收录入 —— 收尾时按配置重注册热键，
	// 清掉的那一组就不会又被挂回去
	if ((GetAsyncKeyState(VK_BACK) & 0x8001) || (GetAsyncKeyState(VK_DELETE) & 0x8001)) {
		const int row = captureRow;
		clearShortcutRow(row);
		endCapture();
		return;
	}

	int vk = 0;
	for (int cand : kCandidateVks) {
		if (GetAsyncKeyState(cand) & 0x8000) { vk = cand; break; }
	}
	if (!vk) return;

	// 无修饰键时只允许 F1~F12 / PrintScreen / ScrollLock / Pause（默认值就是 F1 这类单键），
	// 别的单键等于把打字和系统导航抢走；系统保留组合也一并拒绝。
	// 拒绝时保持录入状态等用户换一个：结束录入会把全局热键整套注册回来，白折腾一趟
	const auto name = vkToName(vk);
	if (name.empty()) return;
	std::vector<std::wstring> keys;
	if (capturedMods & MOD_CONTROL) keys.push_back(L"Ctrl");
	if (capturedMods & MOD_SHIFT) keys.push_back(L"Shift");
	if (capturedMods & MOD_ALT) keys.push_back(L"Alt");
	if (capturedMods & MOD_WIN) keys.push_back(L"Win");
	keys.push_back(name);
	std::wstring joined;
	for (const auto& k : keys) {
		if (!joined.empty()) joined += L'+';
		joined += k;
	}
	const bool hasModifier = (capturedMods & (MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_WIN)) != 0;
	if ((!hasModifier && !canBeUsedAlone(name)) || isReservedShortcut(joined)) {
		if (auto d = GetDlgItem(hwnd, kSetDispIds[captureRow])) {
			const auto tip = Lang::get(L"qr.setInvalid");
			// 按住不放时每 40ms 都会判一次，文本没变就别再 SetWindowTextW —— 会闪
			wchar_t cur[128]{};
			GetWindowTextW(d, cur, 128);
			if (tip != cur) SetWindowTextW(d, tip.c_str());
		}
		return;
	}

	const wchar_t* type = kSetRowTypes[captureRow];
	const int row = captureRow;
	// setShortcutKey 内部负责排他：同一个组合若已被别的功能占用，会把那一边清空
	Setting::get()->setShortcutKey(type, keys);

	// 结果直接落回那一格副编辑器里闪一下，不再弹独立浮层
	recSavedRow = row;
	endCapture();
	SetTimer(hwnd, TidRecSaved, 1500, nullptr);
}

// ———————————————— 轻提示 ————————————————

void WinQr::showToast(const std::wstring& text)
{
	if (!hwnd || text.empty()) return;

	static bool toastClassRegistered = false;
	if (!toastClassRegistered) {
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = hintProcEntry;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.lpszClassName = kToastClass;
		RegisterClassExW(&wc);
		toastClassRegistered = true;
	}

	if (!toastHwnd || !IsWindow(toastHwnd)) {
		toastHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
			kToastClass, L"", WS_POPUP, 0, 0, 10, 10, hwnd, nullptr,
			GetModuleHandleW(nullptr), this);
		if (!toastHwnd) return;
	}

	SetWindowTextW(toastHwnd, text.c_str());

	// 按文字实测宽度定窗口大小，长提示不会被截成半句话
	HDC dc = GetDC(toastHwnd);
	int textW = 0, textH = 0;
	if (dc) {
		HGDIOBJ old = SelectObject(dc, fontOf(FsBody, 500));
		SIZE sz{};
		GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
		textW = sz.cx;
		textH = sz.cy;
		SelectObject(dc, old);
		ReleaseDC(toastHwnd, dc);
	}
	const int w = std::max(sc(160), textW + sc(44));
	const int h = std::max(sc(40), textH + sc(24));

	RECT owner{};
	GetWindowRect(hwnd, &owner);
	const int x = owner.left + ((owner.right - owner.left) - w) / 2;
	const int y = owner.bottom - h - sc(54);

	SetWindowPos(toastHwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	// 提示条同样不做圆角：区域裁切在浅色背景上会留出毛边
	InvalidateRect(toastHwnd, nullptr, TRUE);
	SetTimer(toastHwnd, 1, 1200, nullptr);
}

void WinQr::hideToast()
{
	if (toastHwnd && IsWindow(toastHwnd)) DestroyWindow(toastHwnd);
	toastHwnd = nullptr;
}

LRESULT WinQr::hintProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT: {
		PAINTSTRUCT ps{};
		HDC dc = BeginPaint(h, &ps);
		if (dc) {
			RECT rc{};
			GetClientRect(h, &rc);
			FillRect(dc, &rc, brushOf(RGB(45, 57, 74)));
			wchar_t buf[512]{};
			GetWindowTextW(h, buf, 512);
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, C_WHITE);
			HGDIOBJ old = SelectObject(dc, fontOf(FsBody, 500));
			RECT tr{ sc(12), 0, rc.right - sc(12), rc.bottom };
			DrawTextW(dc, buf, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
			SelectObject(dc, old);
			EndPaint(h, &ps);
		}
		return 0;
	}

	case WM_TIMER:
		KillTimer(h, 1);
		DestroyWindow(h);
		return 0;

	case WM_LBUTTONDOWN:
		DestroyWindow(h);
		return 0;

	case WM_NCDESTROY: {
		auto self = instanceOf(h);
		if (self && self->toastHwnd == h) self->toastHwnd = nullptr;
		break;
	}

	default:
		break;
	}
	return DefWindowProcW(h, msg, wp, lp);
}
