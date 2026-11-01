#include "pch.h"
#include <algorithm>
#include <cwchar>
#include <iterator>
#include <map>
#include <set>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include "../Lang.h"
#include "../Util.h"
#include "RnList.h"
#include "RnPinyinData.h"
#include "WinQr.h"

// 「文件改名」页：工具栏 + 三行规则 + 一张列表 + 底部全选/反选。
//
// 界面全部自绘（按钮走主窗口的 onDrawItem，列表是 RnList），跟别的页同一个调子；
// 规则计算和改名/撤销这两件事是纯逻辑，照搬 zRenamer 那套两阶段改名：
// 先把选中的文件统统改成临时名，再改成目标名 —— 这样"a→b、b→a"这种互换
// 不会互相踩，中途失败也能整体退回去。
//
// 字号不用小字：所有标签、按钮、列表正文都走 FsBody（18px@96dpi）。
// 列表行高 22（字不裁边），一屏正好十行 —— 585×445 里的竖排预算就按这个倒推：
//   页签条 32 → 工具条 34..58 → 面板 72..142（四个切换标签压在它的上边框上
//   63..81）→ 列表 150..394（24 表头 + 10×22）→ 底行 402..426

namespace {
	// 配色一律对齐 WinQr.cpp 的基础色（C_TEXT / C_BORDER / C_WHITE / C_BG_TOP /
	// C_BTN_BLUE / C_TEXT_MUTED）。那几个常量在 WinQr.cpp 的匿名命名空间里，
	// 跨不过翻译单元，所以这里同值另起 RN_ 前缀，取值不许各写各的
	const COLORREF RN_TEXT_MUTED = RGB(120, 132, 150);   // = C_TEXT_MUTED
	const COLORREF RN_TEXT = RGB(45, 57, 74);            // = C_TEXT
	const COLORREF RN_BORDER = RGB(208, 216, 228);       // = C_BORDER
	const COLORREF RN_WHITE = RGB(255, 255, 255);        // = C_WHITE
	const COLORREF RN_PRIMARY = RGB(3, 83, 182);         // = C_BTN_BLUE（主色按钮 / 勾选块的那个蓝）
	const COLORREF RN_PAGE_BG = RGB(247, 249, 252);      // = C_BG_TOP（与其余五页同一个页面底色）
	constexpr int RN_W = 585;   // 与 WinQr.cpp 的 CW 同一个值：页宽

	// ———————————————— 布局（96dpi 基准，照参考图从上到下五段） ————————————————
	// 间距只走四档，不再逐处手调：
	//   RN_MARGIN  页边（14）—— 内容左右各离窗口边 14
	//   RN_GAP     并列分组之间的横向缝（10）
	//   RN_VGAP    上下分组之间的竖向缝（14）—— 分组的标题会往框线上探出 9，
	//              所以看到的是 5：再小上一组的底线就要贴上下一组的标题
	//   RN_INSET   控件到所在分组框线的左右留白（10）
	//   RN_BOX_VPAD 分组框内的上下留白（8）
	//   RN_BTN_GAP 同一行相邻控件的缝（8）—— 标签从四个字缩到两三个字以后，
	//              按钮之间才拉得开这一档
	constexpr int RN_MARGIN = 14;
	constexpr int RN_GAP = 10;
	constexpr int RN_VGAP = 14;
	constexpr int RN_INSET = 10;
	constexpr int RN_BOX_VPAD = 8;
	constexpr int RN_BTN_GAP = 10;

	constexpr int RN_TAB_BOTTOM = 32;  // 页签条 2..32，页内容一律从它下面开始
	constexpr int RN_TOOL_Y = 40;      // 八个动作按钮，整排按颜色分档（与文件校验页同一顶边）
	constexpr int RN_TOOL_H = 26;
	constexpr int RN_TOOL_GAP = 8;
	constexpr int RN_ROW_H = 26;       // 单行控件（按钮 / 单选 / 下拉 / 输入框）的高

	// 自上而下五段：工具条 40..66 → 规则面板 82..160 → 列表 168..412
	//              → 底行 419..443，页底只留 2
	// 表格十行是硬指标（24 表头 + 10×22），剩下的高度全给上半部分：
	// 页签底下留 8、工具条到面板留 16、面板内上下留 10、行缝 8 —— 呼吸感对着 v31
	// 那版来（工具条 44、面板 84、行缝 8、行高 26），只是十行比七行占得多，整体上提一档
	constexpr int RN_PANEL_Y = 82;
	constexpr int RN_PANEL_H = 80;
	// 四个切换标签不占独立一行 —— 它们就压在规则面板的上边框上（跟其余分组框的标题一样），
	// 选中的那颗换主色、其余保持正文色，不做按钮底、不做下划线
	constexpr int RN_PANEL_W = 436;                          // 14..450，右边留给「过滤」
	constexpr int RN_PANEL_IN_R = RN_MARGIN + RN_PANEL_W - RN_INSET; // 440（卡片内可用区的右边界）
	// 标题带：压在上边框线上，字高 18（与 group 标题同一档）
	constexpr int RN_TITLE_H = 18;
	constexpr int RN_TITLE_Y = RN_PANEL_Y - 9;               // 63..81
	constexpr int RN_TITLE_X = RN_MARGIN + 12;               // 26
	// 五个标签的左边和步距。中文字形左右各带 ~9px 空白（字身只有中间那一截），
	// 所以按字的推进宽算出来的缝会有 24px；这里让步距比字身宽出一档，
	// 字与字之间留出约 13px 的空（比一个空格位略宽，读起来是五个词而不是一串）
	constexpr int RN_TAB_FILE_X = RN_TITLE_X, RN_TAB_FILE_W = 44;
	constexpr int RN_TAB_EXT_X = RN_TAB_FILE_X + RN_TAB_FILE_W, RN_TAB_EXT_W = 44;
	constexpr int RN_TAB_REPL_X = RN_TAB_EXT_X + RN_TAB_EXT_W, RN_TAB_REPL_W = 33;
	constexpr int RN_TAB_SEQ_X = RN_TAB_REPL_X + RN_TAB_REPL_W, RN_TAB_SEQ_W = 33;
	constexpr int RN_TAB_DEL_X = RN_TAB_SEQ_X + RN_TAB_SEQ_W, RN_TAB_DEL_W = 46;
	// 五个压线标签：位置 / 槽宽 / 文案只维护这一张表 —— 绘制（drawRenamePage）
	// 和点击命中（rnTabHit）都从它取。两处各写一份的话，挪一个标签得记得改两遍，
	// 改漏了就是"看到的"和"点得到的"对不上
	const struct { int x; int w; const wchar_t* key; } kRnTabs[] = {
		{ RN_TAB_FILE_X, RN_TAB_FILE_W, L"rn.fileName" },
		{ RN_TAB_EXT_X,  RN_TAB_EXT_W,  L"rn.extLbl" },
		{ RN_TAB_REPL_X, RN_TAB_REPL_W, L"rn.replBox" },
		{ RN_TAB_SEQ_X,  RN_TAB_SEQ_W,  L"rn.tabAdd" },
		{ RN_TAB_DEL_X,  RN_TAB_DEL_W,  L"rn.delLbl" },
	};
	constexpr int kRnTabCount = static_cast<int>(std::size(kRnTabs));
	// 面板里两行控件的顶边：第一行离压线标签再让开 4（原来只隔 1，贴着标签带看着挤），
	// 第二行钉在 126 —— 它要和右侧「过滤」下拉同线，且面板底边留 10。
	// 于是第一行落在 96：上边离标签带 5、下边离第二行 4，两边的空差不多，读起来是居中一格
	constexpr int RN_ROW1_Y = RN_PANEL_Y + 14;                       // 96
	constexpr int RN_ROW2_Y = 126;                                   // = 内容区高 80 - 26 - 10

	// 文件名：左半边两列勾选（全部大写/首字大写、全部小写/数字递增），
	// 右半边「转换：/改为：」两行标签 + 下拉（标签画在 drawRenamePage，框贴在标签后面）。
	// 四字勾选项要容下"方框 14 + 缝 6 + 四个字"，所以宽 100
	constexpr int RN_RULE1_X = RN_MARGIN + RN_INSET;          // 24
	constexpr int RN_RULE1_W = 100;
	constexpr int RN_RULE2_X = RN_RULE1_X + RN_RULE1_W + RN_BTN_GAP;  // 132
	constexpr int RN_RULE2_W = 100;
	constexpr int RN_CONV_LBL_X = RN_RULE2_X + RN_RULE2_W + RN_BTN_GAP; // 240（两行标签同列）

	// 扩展名：左半边两列勾选（全部大写/不变、全部小写/删除），右半边「改为：」+ 下拉。
	// 左列宽 98 —— 右列（不变/删除）的方框落在 132，与文件名面板
	// 「首字大写/数字递增」那一列（24+100+10）对齐，切页签时方框不横跳
	constexpr int RN_EXT_UPPER_X = RN_MARGIN + RN_INSET;      // 24
	constexpr int RN_EXT_UPPER_W = 98;
	constexpr int RN_EXT_KEEP_X = RN_EXT_UPPER_X + RN_EXT_UPPER_W + RN_BTN_GAP;   // 132
	constexpr int RN_EXT_KEEP_W = 56;                         // 「不变」两字
	constexpr int RN_EXT_LOWER_X = RN_EXT_UPPER_X;            // 24（第二行）
	constexpr int RN_EXT_LOWER_W = 98;
	constexpr int RN_EXT_DROP_X = RN_EXT_LOWER_X + RN_EXT_LOWER_W + RN_BTN_GAP;   // 132
	constexpr int RN_EXT_DROP_W = 56;                         // 「删除」两字
	// 「改为：」跟在下拉前面（离框只留 2），整组挪到右边
	constexpr int RN_EXT_LBL_X = RN_EXT_DROP_X + RN_EXT_DROP_W + RN_BTN_GAP;      // 192
	constexpr int RN_EXT_LBL_W = 54;
	constexpr int RN_EXT_SET_X = RN_EXT_LBL_X + RN_EXT_LBL_W + 2;                 // 248
	constexpr int RN_EXT_SET_W = RN_PANEL_IN_R - RN_EXT_SET_X;                    // 192

	// 替换：第一行两颗开关，第二行「查找 / 替换为」两格（等宽分布，见 relayoutRename）
	constexpr int RN_REP_CASE_X = RN_MARGIN + RN_INSET;       // 24
	// 画成勾选项：方框 14 + 缝 6 + 文字，宽度按"方框那 20px 也算进去"给
	constexpr int RN_REP_CASE_W = 112;                        // 「区分大小写」五字 + 方框
	constexpr int RN_REP_ALL_X = RN_REP_CASE_X + RN_REP_CASE_W + RN_BTN_GAP; // 144
	constexpr int RN_REP_ALL_W = 94;                          // 「替换所有」四字 + 方框
	constexpr int RN_FIND_LBL_X = RN_MARGIN + RN_INSET;       // 24

	// 编号：两行都做均匀分布 —— 行内各组之间的间隔相等，框宽彼此一致。
	// 第一行「添加」开关 + 前缀 / 后缀（可手打下拉，候选是占位符 token；
	// 勾上「添加」才生效）；第二行「序号」开关 + 起始值 / 增量 / 位数
	constexpr int RN_SEQ_LBL_X = RN_MARGIN + RN_INSET;        // 24
	constexpr int RN_SEQ_ON_W = 56;                           // 「序号」两字 + 方框
	constexpr int RN_AFFIX_ON_X = RN_MARGIN + RN_INSET;       // 24（第一行开头）
	constexpr int RN_AFFIX_ON_W = 56;                         // 「添加」两字 + 方框

	// 删除：第一行 大写字母 / 空格 / 中文 / 特殊字符，第二行 小写字母 / 数字 + 「文本」框
	// 六颗都是勾选项（方框 14 + 缝 6 + 文字），四字的按 98 给宽 —— 右列方框
	// 落在 132，与文件名 / 扩展名两面板的第二列对齐，切页签时不横跳
	constexpr int RN_DEL_UPPER_X = RN_MARGIN + RN_INSET;      // 24
	constexpr int RN_DEL_UPPER_W = 98;                        // 四字 + 方框；右列落 132 与前两面板对齐
	constexpr int RN_DEL_SPACE_X = RN_DEL_UPPER_X + RN_DEL_UPPER_W + RN_BTN_GAP; // 132
	constexpr int RN_DEL_SPACE_W = 56;                        // 「空格」两字 + 方框
	constexpr int RN_DEL_CJK_X = RN_DEL_SPACE_X + RN_DEL_SPACE_W + RN_BTN_GAP;   // 198
	constexpr int RN_DEL_CJK_W = 56;                          // 「中文」两字 + 方框
	constexpr int RN_DEL_SPEC_X = RN_DEL_CJK_X + RN_DEL_CJK_W + RN_BTN_GAP;      // 264
	constexpr int RN_DEL_SPEC_W = 94;                         // 「特殊字符」四字 + 方框
	constexpr int RN_DEL_LOWER_X = RN_MARGIN + RN_INSET;      // 24（第二行）
	constexpr int RN_DEL_LOWER_W = 98;                        // 「小写字母」四字 + 方框
	constexpr int RN_DEL_DIGIT_X = RN_DEL_LOWER_X + RN_DEL_LOWER_W + RN_BTN_GAP; // 132
	constexpr int RN_DEL_DIGIT_W = 56;                        // 「数字」两字 + 方框
	// 「文本：」的框不写死位置：跟上面几处标签一样，按实测字宽 + 2 贴上去
	constexpr int RN_DEL_LBL_X = RN_DEL_DIGIT_X + RN_DEL_DIGIT_W + RN_BTN_GAP;   // 198

	// 「过滤」框：只包住一个下拉，高 45（9 + 26 + 10）。底边跟规则面板底边齐平，
	// 下拉与面板第二行收在同一条水平线上；到表格也留出与面板同样的一段空（8）
	constexpr int RN_G3_W = 111;                              // 460..571
	constexpr int RN_G3_X = RN_W - RN_MARGIN - RN_G3_W;
	constexpr int RN_G3_H = 45;
	constexpr int RN_G3_Y = RN_PANEL_Y + RN_PANEL_H - RN_G3_H; // 117..162，底边齐面板
	constexpr int RN_G3_ROW_Y = RN_G3_Y + 9;                  // 126，与 RN_ROW2_Y 同线
	constexpr int RN_FILTER_X = RN_G3_X + RN_INSET;           // 470
	constexpr int RN_FILTER_W = RN_G3_W - RN_INSET * 2;       // 91

	// 列表 170..416，高 246 = 26 表头 + 10×22 行：一屏正好十行整，不留半行残条。
	// 行高必须留 22 —— 正文是 18px 字号，行压到 20 会把字裁掉一条边
	constexpr int RN_LIST_Y = 170;
	constexpr int RN_LIST_H = 26 + 10 * 22;                   // 246
	constexpr int RN_LIST_X = RN_MARGIN;
	constexpr int RN_LIST_W = RN_W - RN_MARGIN * 2;           // 557
	// 底行紧贴表格底线（不留缝）：表格 168..414，全选/反选就压在 414 上，再往下 24 到 438
	constexpr int RN_BOT_Y = RN_LIST_Y + RN_LIST_H;           // 414
	constexpr int RN_BOT_H = 24;
	constexpr int RN_BOT_BTN_W = 44;
	constexpr int RN_BOT_BTN2_X = RN_MARGIN + RN_BOT_BTN_W + RN_BTN_GAP; // 66
	constexpr int RN_FMT_X = 122;      // 与「反选」右边界（112）之间正好留一道 10
	constexpr int RN_FMT_PITCH = 78;
	constexpr int RN_FMT_W = 68;
	// 格式勾选条只能排到这里为止：右边留给右下角那行「N 个」，
	// 再往右画就会跟它在窄屏 / 小字情况下叠在一起
	constexpr int RN_FMT_END = 508;

	// 工具栏七颗的宽（从左到右）。去掉「刷新」后不再凑 557：
	// 各颗宽度保持原样，右边的依次左移，右侧不再硬撑到表格右边界
	const struct { int id; int w; const wchar_t* key; } kRnTools[] = {
		{ WinQr::RnAddFiles, 56, L"rn.addFiles" },
		{ WinQr::RnAddFolder, 72, L"rn.addFolder" },
		{ WinQr::RnCopy, 56, L"rn.copy" },
		{ WinQr::RnClear, 56, L"rn.clear" },
		{ WinQr::RnReset, 56, L"rn.reset" },
		{ WinQr::RnUndo, 56, L"rn.undo" },
		{ WinQr::RnRun, 93, L"rn.run" },
	};

	std::wstring ExtOfName(const std::wstring& name)
	{
		const size_t dot = name.find_last_of(L'.');
		if (dot == std::wstring::npos || dot == 0 || dot + 1 == name.size()) return {};
		return name.substr(dot + 1);
	}

	std::wstring BaseOfName(const std::wstring& name)
	{
		const size_t dot = name.find_last_of(L'.');
		if (dot == std::wstring::npos || dot == 0 || dot + 1 == name.size()) return name;
		return name.substr(0, dot);
	}

	std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
	{
		if (dir.empty()) return name;
		if (dir.back() == L'\\') return dir + name;
		return dir + L"\\" + name;
	}

	std::wstring DirOf(const std::wstring& path)
	{
		const size_t p = path.find_last_of(L"\\/");
		return p == std::wstring::npos ? std::wstring() : path.substr(0, p + 1);
	}

	std::wstring NameOf(const std::wstring& path)
	{
		const size_t p = path.find_last_of(L"\\/");
		return p == std::wstring::npos ? path : path.substr(p + 1);
	}

	std::wstring Lower(std::wstring s)
	{
		for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
		return s;
	}

	std::wstring Trim(const std::wstring& s)
	{
		size_t a = 0, b = s.size();
		while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) a++;
		while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) b--;
		return s.substr(a, b - a);
	}

	// 文件名合法性与资源管理器一致：非法字符、结尾的点和空格都不行
	bool ValidName(const std::wstring& name)
	{
		if (name.empty() || name == L"." || name == L"..") return false;
		if (name.back() == L'.' || name.back() == L' ') return false;
		for (wchar_t c : name) {
			if (c < 32) return false;
			if (wcschr(L"<>:\"/\\|?*", c)) return false;
		}
		return true;
	}

	// 全部替换；find 为空直接返回原文
	std::wstring ReplaceAll(const std::wstring& src, const std::wstring& from, const std::wstring& to)
	{
		if (from.empty() || src.empty()) return src;
		std::wstring out;
		out.reserve(src.size());
		size_t pos = 0;
		while (true) {
			const size_t hit = src.find(from, pos);
			if (hit == std::wstring::npos) {
				out.append(src, pos, std::wstring::npos);
				break;
			}
			out.append(src, pos, hit - pos);
			out.append(to);
			pos = hit + from.size();
		}
		return out;
	}

	// 「查找 / 替换为」那一组：大小写与"换第一处还是全部"都是面板上的开关。
	// 不区分大小写时用两份小写副本找位置，切片仍取自原文 —— 换出来的字保留原写法
	std::wstring ReplaceText(const std::wstring& src, const std::wstring& from,
		const std::wstring& to, bool caseSensitive, bool all)
	{
		if (from.empty() || src.empty()) return src;
		const std::wstring hay = caseSensitive ? src : Lower(src);
		const std::wstring needle = caseSensitive ? from : Lower(from);
		std::wstring out;
		out.reserve(src.size());
		size_t pos = 0;
		while (true) {
			const size_t hit = hay.find(needle, pos);
			if (hit == std::wstring::npos) {
				out.append(src, pos, std::wstring::npos);
				break;
			}
			out.append(src, pos, hit - pos);
			out.append(to);
			pos = hit + from.size();
			if (!all) {
				out.append(src, pos, std::wstring::npos);
				break;
			}
		}
		return out;
	}

	std::wstring UpperAll(std::wstring s)
	{
		for (auto& c : s) c = static_cast<wchar_t>(towupper(c));
		return s;
	}

	// 首字母大写：只抬主文件名里第一个字母，其余原样（"abc def" → "Abc def"）
	std::wstring TitleFirst(std::wstring s)
	{
		for (auto& c : s) {
			if (iswalpha(c)) {
				c = static_cast<wchar_t>(towupper(c));
				break;
			}
		}
		return s;
	}

	std::wstring Pad(int value, int digits)
	{
		wchar_t buf[32]{};
		swprintf_s(buf, L"%0*d", digits, value);
		return buf;
	}

	// 临时名：同目录下的 ~rn<pid>_<n>.tmp，不存在的第一只就用它
	std::wstring TempNameIn(const std::wstring& dir)
	{
		const DWORD pid = GetCurrentProcessId();
		for (int i = 0; i < 10000; i++) {
			std::wstring name = L"~rn" + std::to_wstring(pid) + L"_" + std::to_wstring(i) + L".tmp";
			const std::wstring full = JoinPath(dir, name);
			if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) return name;
		}
		return L"~rn_tmp.tmp";
	}

	bool FileExists(const std::wstring& path)
	{
		return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
	}

	unsigned long long FileSizeOf(const std::wstring& path)
	{
		WIN32_FILE_ATTRIBUTE_DATA ad{};
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &ad)) return 0;
		return (static_cast<unsigned long long>(ad.nFileSizeHigh) << 32) | ad.nFileSizeLow;
	}

	// 一次改名动作：src 盘上现名 → dst 目标名，tmp 是中转名
	struct RnMove {
		int row{ -1 };
		std::wstring src, dst, tmp;
	};

	// 两阶段搬迁。失败时把已经动过的全部退回原位，返回 false，
	// 并把"卡在哪一条"记进 bad —— 只有这一条算失败，别把整批都标成失败
	bool MoveAll(std::vector<RnMove>& moves, size_t* bad = nullptr)
	{
		size_t staged = 0;
		for (; staged < moves.size(); staged++) {
			RnMove& m = moves[staged];
			// 中转名必须是"源文件所在目录 + 文件名"的完整路径。
			// 以前只取了文件名，MoveFileExW 会拿它当相对路径、按进程当前目录解析：
			// 当前目录不是源目录（尤其是被解压/安装到只读位置、或者当前目录根本没权限写）时，
			// 第一趟搬迁就整个失败 —— 表现为"执行改名全部失败"，跟改不改扩展名无关
			m.tmp = JoinPath(DirOf(m.src), TempNameIn(DirOf(m.src)));
			if (!MoveFileExW(m.src.c_str(), m.tmp.c_str(), 0)) break;
		}
		if (staged < moves.size()) {
			for (size_t i = 0; i < staged; i++) {
				MoveFileExW(moves[i].tmp.c_str(), moves[i].src.c_str(), 0);
			}
			if (bad) *bad = staged;
			return false;
		}

		size_t landed = 0;
		for (; landed < moves.size(); landed++) {
			RnMove& m = moves[landed];
			if (!MoveFileExW(m.tmp.c_str(), m.dst.c_str(), 0)) break;
		}
		if (landed < moves.size()) {
			for (size_t i = 0; i < landed; i++) {
				MoveFileExW(moves[i].dst.c_str(), moves[i].tmp.c_str(), 0);
			}
			for (auto& m : moves) {
				MoveFileExW(m.tmp.c_str(), m.src.c_str(), 0);
			}
			if (bad) *bad = landed;
			return false;
		}
		return true;
	}

	// 自然序：和资源管理器一致（(2) 排在 (10) 前面）。
	// 自己实现而不是用 StrCmpLogicalW：那个在 shlwapi.lib 里，本工程没链它，
	// 为一行排序多背一个库不划算。
	//
	// 这个比较器必须满足"严格弱序"，否则 std::stable_sort 的合并过程会算出越界的
	// 下标，把堆写坏 —— 表现就是"拖一堆文件进来，程序直接没了"。以前那版就踩了这个坑：
	// 数字段相等、但两边长度不同时，收尾只比字符串总长；于是 "1a" 与 "01" 判成相等，
	// 而它俩又都比 "1b" 小 —— 等号不传递，比较器就废了。
	// 现在的规则可以证明是严格弱序：先把名字切成"记号"（连续数字算一个，按数值比；
	// 其余字符按小写比），逐记号比；共同记号全相等时，记号先比完的更小。这就是字典序
	bool NaturalLess(const std::wstring& a, const std::wstring& b)
	{
		size_t i = 0, j = 0;
		while (i < a.size() && j < b.size()) {
			const bool digitA = iswdigit(a[i]) != 0;
			const bool digitB = iswdigit(b[j]) != 0;
			if (digitA && digitB) {
				size_t ia = i, jb = j;
				while (ia < a.size() && iswdigit(a[ia])) ia++;
				while (jb < b.size() && iswdigit(b[jb])) jb++;
				// 去掉前导 0 比位数，位数一样再逐位比 —— 等于按数值比大小
				size_t sa = i, sb = j;
				while (sa + 1 < ia && a[sa] == L'0') sa++;
				while (sb + 1 < jb && b[sb] == L'0') sb++;
				const size_t la = ia - sa, lb = jb - sb;
				if (la != lb) return la < lb;
				const int cmp = a.compare(sa, la, b, sb, lb);
				if (cmp != 0) return cmp < 0;
				// 数值相等（"1" 与 "01" 在这一段上就是相等的）：两边一起跳过去，
				// 先后由"谁还有记号"决定，不会出现互相都小的死结
				i = ia;
				j = jb;
				continue;
			}
			const wchar_t ca = static_cast<wchar_t>(towlower(a[i]));
			const wchar_t cb = static_cast<wchar_t>(towlower(b[j]));
			if (ca != cb) return ca < cb;
			i++;
			j++;
		}
		// 共同记号都比完了：谁还有剩谁就大；两边都用尽 = 相等
		return i == a.size() && j < b.size();
	}

	std::wstring ComboText(HWND parent, int id)
	{
		HWND combo = GetDlgItem(parent, id);
		if (!combo) return {};
		// 先读框里看得见的那行字。可编辑下拉里，用户手打的字就住在这儿；
		// 以前先看 CB_GETCURSEL，初始化时挂上的第 0 项一直没清，
		// 手打了 bmp 也被那一项盖掉 —— 表现就是"「改为」里写了 bmp，列表还是 jpg"
		const int len = GetWindowTextLengthW(combo);
		if (len > 0) {
			std::wstring text(static_cast<size_t>(len) + 1, L'\0');
			GetWindowTextW(combo, text.data(), len + 1);
			text.resize(static_cast<size_t>(len));
			return text;
		}
		// 框里没字（不可编辑的那种下拉没选中）：退回按选中项取
		const int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
		if (sel < 0) return {};
		const int itemLen = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN,
			static_cast<WPARAM>(sel), 0));
		if (itemLen <= 0) return {};
		std::wstring text(static_cast<size_t>(itemLen) + 1, L'\0');
		SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(sel),
			reinterpret_cast<LPARAM>(text.data()));
		text.resize(static_cast<size_t>(itemLen));
		return text;
	}

	// ———————————————— 文本转换（转录：只改名字的字面，不动扩展名） ————————————————

	// 简繁互转交给系统：LCMapStringEx 带 ZH 的语言参数时走整字映射表，
	// 比自己维护一张简繁对照表靠谱（"发/发"这种一简对多繁的，系统会按上下文挑）
	std::wstring MapZh(const std::wstring& s, bool toTraditional)
	{
		if (s.empty()) return s;
		const DWORD flag = toTraditional ? LCMAP_TRADITIONAL_CHINESE : LCMAP_SIMPLIFIED_CHINESE;
		const int need = LCMapStringEx(L"zh-CN", flag,
			s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr, 0);
		if (need <= 0) return s;
		std::wstring out(static_cast<size_t>(need), L'\0');
		const int got = LCMapStringEx(L"zh-CN", flag,
			s.c_str(), static_cast<int>(s.size()), out.data(), need, nullptr, nullptr, 0);
		if (got <= 0) return s;
		out.resize(static_cast<size_t>(got));
		return out;
	}

	const wchar_t kCnDigits[] = L"〇一二三四五六七八九";

	// 汉字数字 -> 阿拉伯数字。只做逐字映射（十百千万亿保持原样）：
	// 文件名里的数字多是序号（"第一版"→"第1版"），逐字足够用，也不会把"十二"拆坏
	std::wstring NumToDigit(const std::wstring& s)
	{
		std::wstring out;
		out.reserve(s.size());
		for (wchar_t c : s) {
			if (c == L'零' || c == L'〇') out += L'0';
			else if (c == L'两') out += L'2';
			else {
				const wchar_t* p = wcschr(kCnDigits + 1, c);
				out += p ? static_cast<wchar_t>(L'1' + (p - (kCnDigits + 1))) : c;
			}
		}
		return out;
	}

	std::wstring DigitToNum(const std::wstring& s)
	{
		std::wstring out;
		out.reserve(s.size());
		for (wchar_t c : s) {
			if (c >= L'0' && c <= L'9') out += kCnDigits[c - L'0'];
			else out += c;
		}
		return out;
	}

	// 汉字 -> 无声调全拼。查不到（生僻字 / 非 GB2312 / 非汉字）返回 null，调用方原样保留
	const wchar_t* PinyinOf(wchar_t c)
	{
		if (c < 0x80) return nullptr;
		char gb[4]{};
		const int n = WideCharToMultiByte(936, 0, &c, 1, gb, sizeof(gb), nullptr, nullptr);
		if (n != 2) return nullptr;
		const unsigned char hi = static_cast<unsigned char>(gb[0]);
		const unsigned char lo = static_cast<unsigned char>(gb[1]);
		if (lo < 0xA1 || lo > 0xFE) return nullptr;
		int idx = -1;
		if (hi >= 0xB0 && hi <= 0xD7) {
			// 一级汉字到 0xD7F9 为止，0xD7FA..FE 是空的
			if (hi == 0xD7 && lo > 0xF9) return nullptr;
			idx = (hi - 0xB0) * 94 + (lo - 0xA1);
		}
		else if (hi >= 0xD8 && hi <= 0xF7) {
			idx = 3755 + (hi - 0xD8) * 94 + (lo - 0xA1);
		}
		else {
			return nullptr;
		}
		if (idx < 0 || idx >= 6763) return nullptr;
		return RnPinyinData::kRnPinyinSyllables[RnPinyinData::kRnPinyinIndex[idx]];
	}

	// conv：1 简转繁 / 2 繁转简 / 3 汉字数字转阿拉伯 / 4 阿拉伯转汉字 /
	//       5 汉语转拼音 / 6 汉语转拼音首字母（0 = 不转换）
	std::wstring ConvertText(const std::wstring& s, int conv)
	{
		switch (conv) {
		case 1: return MapZh(s, true);
		case 2: return MapZh(s, false);
		case 3: return NumToDigit(s);
		case 4: return DigitToNum(s);
		case 5:
		case 6: {
			std::wstring out;
			out.reserve(s.size() * 4);
			for (wchar_t c : s) {
				const wchar_t* py = PinyinOf(c);
				if (!py) { out += c; continue; }
				if (conv == 6) {
					if (*py) out += *py;
				}
				else {
					out += py;
				}
			}
			return out;
		}
		default: return s;
		}
	}

	// 目录的"最后一段"：D:\照片\ → 照片；C:\ → C:
	std::wstring DirNameOf(const std::wstring& dir)
	{
		std::wstring d = dir;
		while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
		if (d.size() == 2 && d[1] == L':') return d;
		const size_t p = d.find_last_of(L"\\/");
		return p == std::wstring::npos ? d : d.substr(p + 1);
	}

	// 前缀 / 后缀里的占位符：
	//   <name> 原文件名（不含扩展名）  <self> 新文件名（规则 + 转换后的主名）
	//   <#> 序号  <##> 两位序号  <dir> 所在文件夹名
	//   <now> 日期+时间  <date> 日期  <time> 时间
	std::wstring ExpandTokens(const std::wstring& text, const std::wstring& origBase,
		const std::wstring& selfBase, const std::wstring& dirName, int ordinal, const SYSTEMTIME& now)
	{
		if (text.empty()) return text;
		wchar_t date[16]{}, time[16]{}, stamp[32]{};
		swprintf_s(date, L"%04u%02u%02u", now.wYear, now.wMonth, now.wDay);
		swprintf_s(time, L"%02u%02u%02u", now.wHour, now.wMinute, now.wSecond);
		swprintf_s(stamp, L"%s_%s", date, time);

		std::wstring out = text;
		out = ReplaceAll(out, L"<name>", origBase);
		out = ReplaceAll(out, L"<self>", selfBase);
		out = ReplaceAll(out, L"<dir>", dirName);
		out = ReplaceAll(out, L"<now>", stamp);
		out = ReplaceAll(out, L"<date>", date);
		out = ReplaceAll(out, L"<time>", time);
		out = ReplaceAll(out, L"<##>", Pad(ordinal, 2));
		out = ReplaceAll(out, L"<#>", std::to_wstring(ordinal));
		return out;
	}
}

// ———————————————— 建控件 ————————————————

void WinQr::createRenamePage()
{
	// 页签本身在 createControls 的页签表里建，这里不再建一遍：
	// 同一个 id 建两个按钮，后建的那个会盖掉 map 里的句柄，先建的就永远不会被重排到
	for (const auto& t : kRnTools) mkButton(t.id, Lang::get(t.key).c_str());

	// 文件名四档：点第二下取消（再次点同一档 = 这条规则不启用）
	mkButton(RnCaseUpper, Lang::get(L"rn.caseUpper").c_str());
	mkButton(RnCaseLower, Lang::get(L"rn.caseLower").c_str());
	mkButton(RnCaseTitle, Lang::get(L"rn.caseTitle").c_str());
	mkButton(RnNumEnable, Lang::get(L"rn.numInc").c_str());

	// 扩展名四档
	mkButton(RnExtUpper, Lang::get(L"rn.extUpper").c_str());
	mkButton(RnExtLower, Lang::get(L"rn.extLower").c_str());
	mkButton(RnExtKeep, Lang::get(L"rn.extKeep").c_str());
	mkButton(RnExtDrop, Lang::get(L"rn.extDel").c_str());

	// 「转换：/改为：」两行：转换是七项只读列表；改为可手打 —— 输入后主名整体换成它
	mkButton(RnRepCase, Lang::get(L"rn.caseSensitive").c_str());
	mkButton(RnRepAll, Lang::get(L"rn.replaceAll").c_str());

	// 下拉：转换为只读列表；改为 / 前缀 / 后缀 / 过滤 可手打
	auto combo = [this](int id, bool editable) {
		DWORD style = WS_CHILD | WS_VISIBLE | WS_VSCROLL |
			(editable ? (CBS_DROPDOWN | CBS_AUTOHSCROLL) : CBS_DROPDOWNLIST);
		return CreateWindowExW(0, L"COMBOBOX", L"", style, 0, 0, 0, 0, hwnd,
			reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
	};
	combo(RnConvert, false);
	combo(RnConvTo, true);
	// 「改为」也要能手打：候选只有十种常见格式，遇到 mkv / tar 这种只能自己敲
	combo(RnExtSet, true);
	combo(RnFilter, true);
	combo(RnPrefix, true);
	combo(RnSuffix, true);

	auto fill = [this](int id, const std::vector<std::wstring>& items, bool selectFirst = true) {
		HWND c = GetDlgItem(hwnd, id);
		if (!c) return;
		for (const auto& s : items) {
			SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
		}
		// 「改为」一开始不该挂着候选里的第一项：默认是"不变"，框里空着才对。
		// 挂着选中项还有一个副作用 —— 手打字时它还留在那儿（见 ComboText）
		if (selectFirst) SendMessageW(c, CB_SETCURSEL, 0, 0);
		else {
			SendMessageW(c, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
			SetWindowTextW(c, L"");
		}
	};
	fill(RnConvert, rnConvertItems());
	fill(RnExtSet, rnExtItems(), false);
	fill(RnFilter, { Lang::get(L"rn.allFmt") });

	// 前缀 / 后缀的候选是占位符 token：点一下把 token 本身填进框（后半句说明留给下拉里看）
	for (int id : { static_cast<int>(RnPrefix), static_cast<int>(RnSuffix) }) {
		HWND c = GetDlgItem(hwnd, id);
		for (const auto& s : rnAffixItems()) {
			SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s.c_str()));
		}
		SetWindowTextW(c, L"");
	}

	// 下拉展开的条数单独定（控件本身只占一行高，见 relayoutRename 里的注释）
	for (int id : { static_cast<int>(RnConvert), static_cast<int>(RnExtSet),
		static_cast<int>(RnFilter), static_cast<int>(RnPrefix), static_cast<int>(RnSuffix) }) {
		if (HWND c = GetDlgItem(hwnd, id)) SendMessageW(c, 0x0401 /*CB_SETMINVISIBLE*/, 10, 0);
	}

	// 这一页五只下拉都只占一行高，点开前得先把窗口抻高，不然列表高度算成 0、点开是空的
	// （见 rnComboProc）。以前只挂了「转换 / 前缀 / 后缀」三只叠格的，漏了「改为」「过滤」
	for (int id : { static_cast<int>(RnConvert), static_cast<int>(RnPrefix),
		static_cast<int>(RnSuffix), static_cast<int>(RnExtSet), static_cast<int>(RnFilter) }) {
		HWND c = GetDlgItem(hwnd, id);
		WNDPROC prev = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(c, GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(rnComboProcEntry)));
		if (!rnComboOld) rnComboOld = prev;
	}

	// 面板控件的显隐统一由 rnSyncPanel 管（文件名页签现在常显「转换 / 改为」两格）

	// 替换双栏（参考图里是两颗开关 + 查找 / 替换为 两格）
	mkEdit(RnFind, L"", WS_BORDER | ES_AUTOHSCROLL);
	mkEdit(RnReplace, L"", WS_BORDER | ES_AUTOHSCROLL);

	// 追加面板：一行收齐（序号开关 + 起始值 / 增量 / 位数），第二行 添加开关 + 前缀 / 后缀
	mkButton(RnSeqOn, Lang::get(L"rn.seqLbl").c_str());
	mkButton(RnAffixOn, Lang::get(L"rn.affixOn").c_str());
	// 数字框文字水平居中：框只有两位数宽，"1" 贴左看着空一块，居中才齐
	mkEdit(RnSeqStart, L"1", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER | ES_CENTER);
	mkEdit(RnSeqStep, L"1", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER | ES_CENTER);
	mkEdit(RnSeqDigits, L"1", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER | ES_CENTER);

	// 删除面板：六类字符各一颗开关（选中才生效），加一格「文本：」随手抹掉一串字
	mkButton(RnDelSpace, Lang::get(L"rn.delSpace").c_str());
	mkButton(RnDelLower, Lang::get(L"rn.delLower").c_str());
	mkButton(RnDelCjk, Lang::get(L"rn.delCjk").c_str());
	mkButton(RnDelSpecial, Lang::get(L"rn.delSpecial").c_str());
	mkButton(RnDelDigit, Lang::get(L"rn.delDigit").c_str());
	mkButton(RnDelUpper, Lang::get(L"rn.delUpper").c_str());
	mkEdit(RnDelText, L"", WS_BORDER | ES_AUTOHSCROLL);

	// 底部全选 / 反选；格式勾选不走控件，画在页面上、点命中在 WM_LBUTTONDOWN 里判
	mkButton(RnCheckAll, Lang::get(L"rn.checkAll").c_str());
	mkButton(RnInvert, Lang::get(L"rn.invert").c_str());

	// 列表：自绘控件，数据指针在窗口这边（rnRows）
	rnb.create(hwnd, RnListView, dpi);
	rnb.setSource(&rnRows);
	rnb.onClick = [this](int rowIndex, int col) {
		if (rowIndex < 0 || rowIndex >= static_cast<int>(rnRows.size())) return;
		RnRow& row = rnRows[rowIndex];
		// 点哪儿都先由列表把整行选中（高亮在列表里画）。
		// 「新文件名」那一格进编辑只认双击（下面的 onDblClick）——
		// 单击时什么都不做，编辑框自然不会在这一对点击中间冒出来抢走后续消息
		if (col == 0) {
			row.checked = !row.checked;
			rnb.invalidateRow(rowIndex);
			// 扩展名那条规则只认勾选的行，勾一变当场重算
			rnApplyRules();
		}
	};
	// 双击（系统给的那一下）：进编辑，进去就是全选
	rnb.onDblClick = [this](int rowIndex, int col) {
		if (col == 2) rnBeginEdit(rowIndex);
	};
	rnb.onScroll = [this]() { rnHideEditor(); };
	// 拖表头把列宽改了：编辑框还按老格子摆着，先收起来
	rnb.onResizeColumns = [this]() { rnHideEditor(); };

	// 原地编辑框：平时藏着，点「新文件名」那格才挪过去
	if (auto ed = mkEdit(RnEdit, L"", WS_BORDER | ES_AUTOHSCROLL)) {
		rnEditOld = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(ed, GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(rnEditProcEntry)));
		ShowWindow(ed, SW_HIDE);
	}
}

// ———————————————— 摆位置 ————————————————

int WinQr::rnLabelW(const std::wstring& s)
{
	// 标签后面紧跟输入框的那几处（查找 / 替换为 / 起始值 / 增量 / 位数 / 改为），
	// 位置不再写死：量一下这串字实际占多宽，框就贴着它后面摆。
	// 汉字字形两侧自带空白，按"字数×18"估出来的宽总会多出十来像素，
	// 框和标签之间就空出一大块 —— 量出来的前进宽才是字真正占的地方
	if (!hwnd || s.empty()) return 0;
	HDC dc = GetDC(hwnd);
	if (!dc) return 0;
	HGDIOBJ old = SelectObject(dc, fontOf(FsBody, 500));
	SIZE sz{};
	GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
	SelectObject(dc, old);
	ReleaseDC(hwnd, dc);
	return dpi > 0 ? MulDiv(sz.cx, 96, dpi) : sz.cx;
}

void WinQr::relayoutRename()
{
	auto place = [this](int id, int x, int y, int w, int h) {
		if (auto c = GetDlgItem(hwnd, id)) {
			SetWindowPos(c, nullptr, sc(x), sc(y), sc(w), sc(h), SWP_NOZORDER | SWP_NOACTIVATE);
		}
	};

	int x = RN_MARGIN;
	for (const auto& t : kRnTools) {
		place(t.id, x, RN_TOOL_Y, t.w, RN_TOOL_H);
		x += t.w + RN_TOOL_GAP;
	}

	// 「文件名」面板左半边：两列勾选
	place(RnCaseUpper, RN_RULE1_X, RN_ROW1_Y, RN_RULE1_W, RN_ROW_H);
	place(RnCaseTitle, RN_RULE2_X, RN_ROW1_Y, RN_RULE2_W, RN_ROW_H);
	place(RnCaseLower, RN_RULE1_X, RN_ROW2_Y, RN_RULE1_W, RN_ROW_H);
	place(RnNumEnable, RN_RULE2_X, RN_ROW2_Y, RN_RULE2_W, RN_ROW_H);
	// 右半边「转换：/改为：」两行：标签画在 drawRenamePage，框贴在标签后面，
	// 宽度一路铺到面板内右边界（和旧长框同款，展开条数靠 CB_SETMINVISIBLE 单独定）
	rnConvX = RN_CONV_LBL_X + rnLabelW(Lang::get(L"rn.convert")) + 2;
	rnConvToX = RN_CONV_LBL_X + rnLabelW(Lang::get(L"rn.convTo")) + 2;
	place(RnConvert, rnConvX, RN_ROW1_Y, RN_PANEL_IN_R - rnConvX, RN_ROW_H);
	place(RnConvTo, rnConvToX, RN_ROW2_Y, RN_PANEL_IN_R - rnConvToX, RN_ROW_H);

	// 「替换」面板：第一行两颗开关，第二行「查找 / 替换为」两格。
	// 两格等宽：标签宽是量出来的（「替换为：」比「查找：」宽），
	// 先按预算整除出框宽，除不尽的余量摊进组间 —— 末框右缘正好收到面板右缘
	rnFindX = RN_FIND_LBL_X + rnLabelW(Lang::get(L"rn.findLbl")) + 2;
	const int lRepl = rnLabelW(Lang::get(L"rn.replLbl"));
	const int budget = RN_PANEL_IN_R - rnFindX - lRepl - 2 - RN_BTN_GAP;
	const int boxW = std::max(40, budget / 2);               // 框宽兜底 40
	rnReplLblX = rnFindX + boxW + RN_BTN_GAP + (budget - boxW * 2);
	rnReplaceX = rnReplLblX + lRepl + 2;
	place(RnRepCase, RN_REP_CASE_X, RN_ROW1_Y, RN_REP_CASE_W, RN_ROW_H);
	place(RnRepAll, RN_REP_ALL_X, RN_ROW1_Y, RN_REP_ALL_W, RN_ROW_H);
	place(RnFind, rnFindX, RN_ROW2_Y, boxW, RN_ROW_H);
	place(RnReplace, rnReplaceX, RN_ROW2_Y, boxW, RN_ROW_H);

	// 「编号」面板：「添加」行在上、「序号」行在下。序号行三框等宽 ——
	// 标签宽是量出来的（起始值三字、增量/位数两字，各不相同），
	// 若按列均分再让框铺到列尾，框宽就会跟着标签参差；改成先定三框同宽，
	// 余量全摊进列间（间隔仍以 12 为基准）。两行左边界看齐、右缘都收到
	// 面板右缘，左右两头的对齐一点不动。标签宽语言无关
	{
		const int contentL = RN_SEQ_LBL_X;
		// 两行共用的间隔统一定 12：序号行列间、添加行前后缀之间都是它
		const int gapBase = 12;
		// 第一行（上）：「添加」开关 + 「前缀：/后缀：」两组，两框等宽平分。
		// 「前缀：」的 x 与下面「起始值：」同列 —— 两行左边界看齐才齐整
		const int lp1 = rnLabelW(Lang::get(L"rn.prefix"));
		const int lp2 = rnLabelW(Lang::get(L"rn.suffix"));
		const int row1L = contentL + RN_AFFIX_ON_W + gapBase;
		int boxW = (RN_PANEL_IN_R - row1L - gapBase - lp1 - lp2 - 4) / 2;
		if (boxW < 100) boxW = 100;               // 太窄的手打框不好用，兜底
		rnPfxLblX = row1L;
		rnPfxX = rnPfxLblX + lp1 + 2;
		rnSufLblX = rnPfxX + boxW + gapBase;
		rnSufX = rnSufLblX + lp2 + 2;
		place(RnAffixOn, RN_AFFIX_ON_X, RN_ROW1_Y, RN_AFFIX_ON_W, RN_ROW_H);
		place(RnPrefix, rnPfxX, RN_ROW1_Y, boxW, RN_ROW_H);
		place(RnSuffix, rnSufX, RN_ROW1_Y, std::min(boxW, RN_PANEL_IN_R - rnSufX), RN_ROW_H);
		// 第二行（下）：「序号」开关 + 起始值 / 增量 / 位数，三框同宽。
		// 先按"标签总宽 + 6 贴缝 + 两道基准间隔"的预算整除出框宽，
		// 除不尽的余量摊进两道列间 —— 末框右缘仍然正好收到面板右缘
		const int l1 = rnLabelW(Lang::get(L"rn.seqStart"));
		const int l2 = rnLabelW(Lang::get(L"rn.seqStep"));
		const int l3 = rnLabelW(Lang::get(L"rn.seqDigits"));
		const int colL = contentL + RN_SEQ_ON_W + gapBase;        // 首列标签起点（与「前缀：」同列）
		const int budget = RN_PANEL_IN_R - colL - (l1 + l2 + l3) - 6;
		const int boxW2 = std::max(40, (budget - gapBase * 2) / 3); // 框宽兜底 40：极窄语言下标签一宽也不能把框挤没
		const int gap2 = std::max(4, (budget - boxW2 * 3) / 2);     // 列间兜底 4：框宽被兜底顶上时列间不再算负
		rnSeqStartLblX = colL;
		rnSeqStartX = colL + l1 + 2;
		rnSeqStepLblX = rnSeqStartX + boxW2 + gap2;
		rnSeqStepX = rnSeqStepLblX + l2 + 2;
		rnSeqDigLblX = rnSeqStepX + boxW2 + gap2;
		rnSeqDigX = rnSeqDigLblX + l3 + 2;
		place(RnSeqOn, RN_SEQ_LBL_X, RN_ROW2_Y, RN_SEQ_ON_W, RN_ROW_H);
		place(RnSeqStart, rnSeqStartX, RN_ROW2_Y, boxW2, RN_ROW_H);
		place(RnSeqStep, rnSeqStepX, RN_ROW2_Y, boxW2, RN_ROW_H);
		place(RnSeqDigits, rnSeqDigX, RN_ROW2_Y, boxW2, RN_ROW_H);
	}

	// 「扩展名」面板：左两列勾选（大写/不变、小写/删除），右「改为：」+ 下拉。
	// 下拉不再一路铺到面板右缘（太长了），收成固定 140 —— 与「替换」面板的
	// 「查找」框同一个宽度档
	rnExtSetX = RN_EXT_LBL_X + rnLabelW(Lang::get(L"rn.extSetLbl")) + 2;
	rnExtSetW = 140;
	place(RnExtUpper, RN_EXT_UPPER_X, RN_ROW1_Y, RN_EXT_UPPER_W, RN_ROW_H);
	place(RnExtKeep, RN_EXT_KEEP_X, RN_ROW1_Y, RN_EXT_KEEP_W, RN_ROW_H);
	place(RnExtLower, RN_EXT_LOWER_X, RN_ROW2_Y, RN_EXT_LOWER_W, RN_ROW_H);
	place(RnExtDrop, RN_EXT_DROP_X, RN_ROW2_Y, RN_EXT_DROP_W, RN_ROW_H);
	place(RnExtSet, rnExtSetX, RN_ROW2_Y, rnExtSetW, RN_ROW_H);

	// 「删除」面板：第一行 大写字母 / 空格 / 中文 / 特殊字符，第二行 小写字母 / 数字 + 「文本：」框
	place(RnDelUpper, RN_DEL_UPPER_X, RN_ROW1_Y, RN_DEL_UPPER_W, RN_ROW_H);
	place(RnDelSpace, RN_DEL_SPACE_X, RN_ROW1_Y, RN_DEL_SPACE_W, RN_ROW_H);
	place(RnDelCjk, RN_DEL_CJK_X, RN_ROW1_Y, RN_DEL_CJK_W, RN_ROW_H);
	place(RnDelSpecial, RN_DEL_SPEC_X, RN_ROW1_Y, RN_DEL_SPEC_W, RN_ROW_H);
	place(RnDelLower, RN_DEL_LOWER_X, RN_ROW2_Y, RN_DEL_LOWER_W, RN_ROW_H);
	place(RnDelDigit, RN_DEL_DIGIT_X, RN_ROW2_Y, RN_DEL_DIGIT_W, RN_ROW_H);
	rnDelTxtX = RN_DEL_LBL_X + rnLabelW(Lang::get(L"rn.delTextLbl")) + 2;
	place(RnDelText, rnDelTxtX, RN_ROW2_Y, RN_PANEL_IN_R - rnDelTxtX, RN_ROW_H);

	// 「过滤」框
	place(RnFilter, RN_FILTER_X, RN_G3_ROW_Y, RN_FILTER_W, RN_ROW_H);

	place(RnListView, RN_LIST_X, RN_LIST_Y, RN_LIST_W, RN_LIST_H);
	rnb.setDpi(dpi);

	// 底部两颗是"文字链接"的样子（参考图里没有按钮底），宽度按字给，不撑满
	place(RnCheckAll, RN_MARGIN, RN_BOT_Y, RN_BOT_BTN_W, RN_BOT_H);
	place(RnInvert, RN_BOT_BTN2_X, RN_BOT_Y, RN_BOT_BTN_W, RN_BOT_H);

	auto mountFont = [this](int id, HFONT f) {
		if (f) if (auto c = GetDlgItem(hwnd, id)) {
			SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
		}
	};
	const int all[] = {
		RnConvert, RnPrefix, RnSuffix, RnExtSet, RnFilter, RnFind, RnReplace, RnEdit,
		RnSeqOn, RnSeqStart, RnSeqStep, RnSeqDigits,
		RnDelSpace, RnDelLower, RnDelCjk, RnDelSpecial, RnDelDigit, RnDelUpper, RnDelText,
		RnCheckAll, RnInvert,
		RnCaseUpper, RnCaseLower, RnCaseTitle, RnNumEnable,
		RnConvert, RnConvTo,
		RnRepCase, RnRepAll,
		RnExtUpper, RnExtLower, RnExtKeep, RnExtDrop,
	};
	for (int id : all) mountFont(id, setFont);
	for (const auto& t : kRnTools) mountFont(t.id, setFont);

	rnb.setFonts(fontOf(FsBody, 400), fontOf(FsBody, 700));
}

void WinQr::showRenamePage(bool visible)
{
	const int ids[] = {
		// 页签不在这里：它跟其它四颗页签一样常驻，由页签那套逻辑统一管显隐。
		// 早先把它一起藏了，症状是"文件改名"这颗在别的页上是空白、也点不动
		RnAddFiles, RnAddFolder, RnCopy, RnClear, RnReset, RnUndo, RnRun,
		RnFilter, RnCheckAll, RnInvert, RnEdit,
	};
	for (int id : ids) {
		if (auto c = GetDlgItem(hwnd, id)) {
			// 编辑框单独处理：切回来时就地编辑框还是藏着的
			ShowWindow(c, (visible && c != GetDlgItem(hwnd, RnEdit)) ? SW_SHOW : SW_HIDE);
		}
	}
	// 规则面板里的控件（含长框三态）显隐全交给 rnSyncPanel：它按 rnPanel 决定露哪一组，
	// 长框还多一层 page == PageRename 的门槛 —— 切走的这一刻 page 已经不是改名页
	// （applyPage 是先定 page 再进来），三只长框在这里就被顺带藏掉了
	rnSyncPanel();
	if (rnb.hwnd()) ShowWindow(rnb.hwnd(), visible ? SW_SHOW : SW_HIDE);
	if (!visible) rnHideEditor();
}

// 规则面板五选一：文件名 / 扩展名 / 替换 / 序号 / 删除。
// 标签本身是画出来的，这里负责把它们对应的控件露出来、其余藏掉；
// 五组规则彼此独立、同时生效，切换只改变"露哪一组"，不重算也不清值
void WinQr::rnSyncPanel()
{
	auto show = [this](int id, bool on) {
		if (auto c = GetDlgItem(hwnd, id)) {
			ShowWindow(c, (on && page == PageRename) ? SW_SHOW : SW_HIDE);
		}
	};
	const bool f = (rnPanel == 0), e = (rnPanel == 1), r = (rnPanel == 2),
		q = (rnPanel == 3), d = (rnPanel == 4);
	for (int id : { static_cast<int>(RnCaseUpper), static_cast<int>(RnCaseLower),
		static_cast<int>(RnCaseTitle), static_cast<int>(RnNumEnable),
		static_cast<int>(RnConvert), static_cast<int>(RnConvTo) }) show(id, f);
	for (int id : { static_cast<int>(RnExtUpper), static_cast<int>(RnExtLower),
		static_cast<int>(RnExtKeep), static_cast<int>(RnExtDrop),
		static_cast<int>(RnExtSet) }) show(id, e);
	for (int id : { static_cast<int>(RnRepCase), static_cast<int>(RnRepAll),
		static_cast<int>(RnFind), static_cast<int>(RnReplace) }) show(id, r);
	for (int id : { static_cast<int>(RnSeqOn), static_cast<int>(RnSeqStart),
		static_cast<int>(RnSeqStep), static_cast<int>(RnSeqDigits),
		static_cast<int>(RnAffixOn), static_cast<int>(RnPrefix),
		static_cast<int>(RnSuffix) }) show(id, q);
	for (int id : { static_cast<int>(RnDelSpace), static_cast<int>(RnDelLower),
		static_cast<int>(RnDelCjk), static_cast<int>(RnDelSpecial),
		static_cast<int>(RnDelDigit), static_cast<int>(RnDelUpper),
		static_cast<int>(RnDelText) }) show(id, d);
	if (page == PageRename) InvalidateRect(hwnd, nullptr, FALSE);
}

void WinQr::rnSetPanel(int panel)
{
	if (panel < 0 || panel > 4 || panel == rnPanel) return;
	// 切面板等于"离开这一组输入框"：就地编辑框先落定，免得焦点换到别的控件上还留着半截
	rnHideEditor();
	rnPanel = panel;
	rnSyncPanel();
}

// 标签行的命中：y 必须先落在标签带里，再按四段的 x 区间判。
// 坐标是客户区物理像素（LPARAM 里的原值），跟绘制用的 sc() 一致
int WinQr::rnTabHit(int x, int y) const
{
	if (y < sc(RN_TITLE_Y) || y >= sc(RN_TITLE_Y + RN_TITLE_H)) return -1;
	for (int i = 0; i < kRnTabCount; i++) {
		if (x >= sc(kRnTabs[i].x) && x < sc(kRnTabs[i].x + kRnTabs[i].w)) return i;
	}
	return -1;
}

// ———————————————— 规则 ————————————————

std::vector<std::wstring> WinQr::rnConvertItems() const
{
	// 长框「转换」那七项：第一项是不转换，后面是六种转录
	return {
		Lang::get(L"rn.convertNone"),
		Lang::get(L"rn.convS2T"),
		Lang::get(L"rn.convT2S"),
		Lang::get(L"rn.convNum2Digit"),
		Lang::get(L"rn.convDigit2Num"),
		Lang::get(L"rn.convPinyin"),
		Lang::get(L"rn.convPinyinFirst"),
	};
}

std::vector<std::wstring> WinQr::rnAffixItems() const
{
	// 前缀 / 后缀的候选：token 本身 + 一句说明。选中时只取 token（第一个空格前那段），
	// 所以下拉里能看懂，填进框的又干净
	return {
		Lang::get(L"rn.tokenName"),
		Lang::get(L"rn.tokenSelf"),
		Lang::get(L"rn.tokenNum1"),
		Lang::get(L"rn.tokenNum2"),
		Lang::get(L"rn.tokenDir"),
		Lang::get(L"rn.tokenNow"),
		Lang::get(L"rn.tokenDate"),
		Lang::get(L"rn.tokenTime"),
	};
}

std::vector<std::wstring> WinQr::rnExtItems() const
{
	// 「改为」只放最常用的十种：列表短了才好选，一屏就能看完。
	// 表头曾经压着一条空字符串当"不变"，下拉一展开第一行是空白 —— 候选里干脆不留空项，
	// "不变"由"框里没字"来表达（rnCompose 里 set 为空就不动扩展名）；
	// 框本身是可编辑下拉，列表里没有的格式直接手打
	return {
		L"png", L"jpg", L"jpeg", L"webp", L"bmp",
		L"gif", L"mp4", L"pdf", L"txt", L"zip",
	};
}

std::wstring WinQr::rnText(int id) const
{
	HWND c = GetDlgItem(hwnd, id);
	if (!c) return {};
	const int len = GetWindowTextLengthW(c);
	if (len <= 0) return {};
	std::wstring text(static_cast<size_t>(len) + 1, L'\0');
	GetWindowTextW(c, text.data(), len + 1);
	text.resize(static_cast<size_t>(len));
	return text;
}

int WinQr::rnConvertMode() const
{
	HWND combo = GetDlgItem(hwnd, RnConvert);
	if (!combo) return 0;
	return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
}

WinQr::RnRuleSet WinQr::rnReadRules() const
{
	// 整表重算只读一遍控件：rnCompose 每行都回读的话，十行就是六十次跨窗口取值。
	// 时间只取一次（整表同一批名字该用同一个 <now>），「删除文本」「改为」在这里
	// 就裁好空白 —— 与原来在 rnCompose 里裁的结果一样，只是不再按行重复
	RnRuleSet r;
	r.find = rnText(RnFind);
	r.replace = rnText(RnReplace);
	r.delText = Trim(rnText(RnDelText));
	// 「改为」用户可能敲成 ".png" / "*.png"：这里一次剥干净，rnCompose 直接用
	r.extSet = Trim(ComboText(hwnd, RnExtSet));
	while (!r.extSet.empty() && (r.extSet.front() == L'.' || r.extSet.front() == L'*')) {
		r.extSet.erase(r.extSet.begin());
	}
	// 「改为」：输入了内容 = 主名整体换成它。顺手剥掉 Windows 文件名禁用字符
	// （\/:*?"<>|），剥完为空就当没填 —— 不至于整批名字全被判成非法
	r.convTo = Trim(rnText(RnConvTo));
	r.convTo.erase(std::remove_if(r.convTo.begin(), r.convTo.end(), [](wchar_t c) {
		return wcschr(L"\\/:*?\"<>|", c) != nullptr;
	}), r.convTo.end());
	r.prefix = rnText(RnPrefix);
	r.suffix = rnText(RnSuffix);
	r.conv = rnConvertMode();
	GetLocalTime(&r.now);
	return r;
}

std::wstring WinQr::rnCompose(const RnRow& row, int ordinal, const RnRuleSet& rules) const
{
	// 手工改过的行：规则一概不碰，用户写什么就是什么
	if (!row.manualName.empty()) return row.manualName;

	const std::wstring origBase = BaseOfName(row.name);

	// 序号段：起始值 + 增量 × 行号。「位数」0 和 1 都是不补零 ——
	// 0 是 1 位、10 是 2 位，各按各的真实长度排；想整批补零对齐，
	// 把「位数」填成 2、3……（之前"按最大号自动补零"会让 97 个文件
	// 从 00 编起，与"0 就是 1 位"的直觉相悖，已删）
	auto seqPad = [this](int ordinal) -> std::wstring {
		const int step = rnSeqStep == 0 ? 1 : rnSeqStep;   // 增量 0 会让整表同一个号
		const int digits = std::max(rnSeqDigits, 1);
		const long long value = static_cast<long long>(rnSeqStart)
			+ static_cast<long long>(step) * (ordinal - 1);
		const int shown = static_cast<int>(std::clamp<long long>(
			value, 0LL, 999999999LL));
		return Pad(shown, digits);
	};

	std::wstring base;
	if (!rules.convTo.empty()) {
		// 「改为」：框里有字 = 主名整体换成它（大小写 / 替换 / 删除那几条规则
		// 都是洗"原来的名字"的，不再往上叠）。「序号」勾着时编号接在名字后面 ——
		// 整批同名必然冲突，带上编号每个名字才唯一
		base = rules.convTo;
		if (rnSeqOn) base += seqPad(ordinal);
	} else {
		base = origBase;
		if (rnCase == 0) base = UpperAll(base);
		else if (rnCase == 1) base = Lower(base);
		else if (rnCase == 2) base = TitleFirst(base);

		if (rnSeqOn) base = seqPad(ordinal);

		// 转换：长框里选的那一项，只改名字的字面（简繁 / 数字 / 拼音），跟左边四颗规则钮无关
		if (rules.conv > 0) base = ConvertText(base, rules.conv);

		// 替换：查找 / 替换为 两格，大小写与"只换第一处 / 全部"由面板上的两颗开关定
		if (!rules.find.empty()) base = ReplaceText(base, rules.find, rules.replace, rnRepCase, rnRepAll);

		// 删除：名字里挑几类字符抹掉（空格 / 小写 / 大写 / 数字 / 中文 / 特殊字符），
		// 再顺带抹掉「文本」框里那串字。六颗钮都是"选中才生效"，跟左边的规则钮一个道理；
		// 这一步压在替换之后、前缀后缀之前 —— 前缀后缀是用户自己写的，不该被规则洗掉
		if (rnDelSpace || rnDelLower || rnDelUpper || rnDelDigit || rnDelCjk || rnDelSpecial) {
			std::wstring kept;
			kept.reserve(base.size());
			for (wchar_t c : base) {
				if (rnDelSpace && iswspace(c)) continue;
				if (rnDelLower && iswlower(c) && c < 0x2E80) continue;
				if (rnDelUpper && iswupper(c) && c < 0x2E80) continue;
				if (rnDelDigit && iswdigit(c) && c < 0x2E80) continue;
				if (rnDelCjk && c >= 0x3000 && c <= 0x9FFF) continue;
				// 特殊字符：既不是字母数字、也不是空格、也不是中文的那一类（标点、符号）
				if (rnDelSpecial && c < 0x2E80 && !iswalnum(c) && !iswspace(c)) continue;
				kept.push_back(c);
			}
			base = kept;
		}
		if (!rules.delText.empty()) base = ReplaceText(base, rules.delText, L"", true, true);
	}

	// 前缀 / 后缀：文本里可以夹占位符，展开时以"当前这一行"为上下文。
	// 「添加」开关不勾时整条规则跳过 —— 框里留着字也不套用，与其它"选中才生效"的规则一致
	const std::wstring dirName = DirNameOf(row.dir);
	const std::wstring prefix = ExpandTokens(rules.prefix, origBase, base, dirName, ordinal, rules.now);
	const std::wstring suffix = ExpandTokens(rules.suffix, origBase, base, dirName, ordinal, rules.now);
	if (rnAffixOn && (!prefix.empty() || !suffix.empty())) base = prefix + base + suffix;

	// 扩展名：四档规则 + 「改为」。只对勾选的行生效 —— 表格左边那颗勾没打上的行
	// 原样保留自己的扩展名；四条规则一颗都没选时也等于没动
	std::wstring ext = ExtOfName(row.name);
	if (row.checked) {
		switch (rnExtMode) {
		case 1: ext = UpperAll(ext); break;
		case 2: ext = Lower(ext); break;
		case 3: ext.clear(); break;
		case 4: {
			if (!rules.extSet.empty()) ext = rules.extSet;
			break;
		}
		default: break;
		}
	}
	if (ext.empty()) return base;
	return base + L"." + ext;
}

void WinQr::rnRecalcStatus()
{
	// 一遍算新名 + 一遍判状态：判"同名冲突"要知道全场的新名，
	// 所以不能一边算一边判（a→b 与 b→b 的先后顺序会给出不同结论）
	const RnRuleSet rules = rnReadRules();
	std::map<std::wstring, int> dup;          // 目录|新名(小写) → 出现次数
	std::set<std::wstring> sources;           // 目录|现名(小写)：这些名字"马上会腾出来"
	for (int i = 0; i < static_cast<int>(rnRows.size()); i++) {
		RnRow& row = rnRows[i];
		if (row.done) {
			// 已经改完的行：盘上就是新名，不再参与规则
			row.newName = row.name;
			continue;
		}
		row.newName = rnCompose(row, i + 1, rules);
	}
	// 第二遍收"确定会动"的行：没勾选的行执行时直接跳过（见 rnRun），新名与现名
	// 相同的行也不会动；源文件已不在（Missing）的行没东西可挪、新名非法（Bad）
	// 的行 MoveFile 必败 —— 这两类同样进不了 rnRun 的 moves，既占不了目标名、
	// 也腾不出原名。早先只排前两类，于是：
	//   · 源已不在的行占着目标名，同批真正要改的行被误判"冲突"，执行时被跳过；
	//   · 新名非法的行占着原名，别的行取这个名字被判"可以改"，二阶段搬迁时
	//     撞上没腾出来的名字，整批回滚、只标一行失败，用户看不出是哪行挡的
	for (int i = 0; i < static_cast<int>(rnRows.size()); i++) {
		const RnRow& row = rnRows[i];
		if (row.done || !row.checked || row.newName == row.name) continue;
		if (!FileExists(JoinPath(row.dir, row.name))) continue;
		if (!ValidName(row.newName)) continue;
		dup[Lower(row.dir) + L"|" + Lower(row.newName)]++;
		sources.insert(Lower(row.dir) + L"|" + Lower(row.name));
	}

	for (int i = 0; i < static_cast<int>(rnRows.size()); i++) {
		RnRow& row = rnRows[i];
		if (row.done) {
			row.status = RnDone;
			continue;
		}
		const std::wstring key = Lower(row.dir) + L"|" + Lower(row.newName);
		if (!FileExists(JoinPath(row.dir, row.name))) {
			row.status = RnMissing;
		}
		else if (!ValidName(row.newName)) {
			row.status = RnBad;
		}
		else if (dup[key] > 1) {
			row.status = RnConflict;
		}
		// 这里必须逐字比，不能忽略大小写：Windows 文件名大小写不敏感但保留原样，
		// a.png → a.PNG 是能改的（全大写扩展名就靠这个）。以前用小写比，
		// 这种"只改大小写"的行被判成"同名"直接跳过，表现就是"全大写没效果、
		// 批量改扩展名失败"
		else if (row.newName == row.name) {
			row.status = RnSame;
		}
		else if (FileExists(JoinPath(row.dir, row.newName)) && sources.count(key) == 0) {
			row.status = RnConflict;
		}
		else {
			row.status = RnReady;
		}
	}
}

void WinQr::rnApplyRules()
{
	if (rnSyncing) return;
	rnHideEditor();
	rnRecalcStatus();
	rnb.refresh();
	// 底部摘要跟着变：这里不重画整页，只把页面空白处刷一下（列表上面那一带）
	InvalidateRect(hwnd, nullptr, FALSE);
}

// ———————————————— 加文件 ————————————————

void WinQr::rnAddPaths(const std::vector<std::wstring>& paths)
{
	// 先把还开着的编辑框落定：下面要往 rnRows 里塞行、还要整体重排，
	// 等排完再提交的话 rnEditRow 指向的已经是别的行了，手工名会写到别人头上
	rnHideEditor();
	std::set<std::wstring> seen;
	for (const auto& row : rnRows) seen.insert(Lower(JoinPath(row.dir, row.name)));

	int added = 0;
	for (const auto& p : paths) {
		const DWORD attr = GetFileAttributesW(p.c_str());
		if (attr == INVALID_FILE_ATTRIBUTES) continue;

		if (attr & FILE_ATTRIBUTE_DIRECTORY) {
			// 文件夹：递归收里面的文件（子文件夹一并收，省得一层层拖）。
			// 已进过的目录记下来不再进：目录联接（junction）/符号链接成环时
			// 没有这道墙会无限遍历，表现就是"拖一个文件夹进来，程序卡死"
			std::vector<std::wstring> stack{ p };
			std::set<std::wstring> visited{ Lower(p) };
			while (!stack.empty()) {
				const std::wstring dir = stack.back();
				stack.pop_back();
				WIN32_FIND_DATAW fd{};
				HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
				if (h == INVALID_HANDLE_VALUE) continue;
				do {
					if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
					const std::wstring full = JoinPath(dir, fd.cFileName);
					if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
						const std::wstring key = Lower(full);
						if (visited.insert(key).second) stack.push_back(full);
						continue;
					}
					const std::wstring key = Lower(full);
					if (seen.count(key)) continue;
					seen.insert(key);
					RnRow row;
					row.dir = DirOf(full);
					row.name = fd.cFileName;
					row.newName = row.name;
					row.size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
					rnRows.push_back(std::move(row));
					added++;
				} while (FindNextFileW(h, &fd));
				FindClose(h);
			}
			continue;
		}

		const std::wstring key = Lower(p);
		if (seen.count(key)) continue;
		seen.insert(key);
		RnRow row;
		row.dir = DirOf(p);
		row.name = NameOf(p);
		row.newName = row.name;
		row.size = FileSizeOf(p);
		rnRows.push_back(std::move(row));
		added++;
	}

	if (added <= 0) {
		showToast(Lang::get(L"rn.noNew"));
		return;
	}

	// 按目录 + 自然名排序：跟资源管理器里看到的顺序一致，编号也顺眼
	std::stable_sort(rnRows.begin(), rnRows.end(), [](const RnRow& a, const RnRow& b) {
		if (Lower(a.dir) != Lower(b.dir)) return Lower(a.dir) < Lower(b.dir);
		return NaturalLess(a.name, b.name);
	});

	rnRebuildFilter();
	rnApplyRules();
}

void WinQr::rnDropFiles(HDROP drop)
{
	if (!drop) return;
	const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
	std::vector<std::wstring> paths;
	paths.reserve(n);
	for (UINT i = 0; i < n; i++) {
		const UINT len = DragQueryFileW(drop, i, nullptr, 0);
		if (len == 0) continue;
		// cch 含结尾 0，缓冲要多留一格
		std::wstring p(static_cast<size_t>(len) + 1, L'\0');
		DragQueryFileW(drop, i, p.data(), len + 1);
		p.resize(len);
		paths.push_back(std::move(p));
	}
	DragFinish(drop);
	if (!paths.empty()) rnAddPaths(paths);
}

// ———————————————— 对话框挑选 ————————————————

void WinQr::rnPickFiles()
{
	// 系统"打开"对话框，多选；筛选用"所有文件" —— 这一页处理的是任意格式
	std::wstring buf(32768, L'\0');
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hwnd;
	ofn.lpstrFilter = L"所有文件\0*.*\0";
	ofn.lpstrFile = buf.data();
	ofn.nMaxFile = static_cast<DWORD>(buf.size());
	ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY
		| OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&ofn)) return;

	// 多选时 lpstrFile 是 "目录\0文件1\0文件2\0\0"；单选时只有一条完整路径
	std::vector<std::wstring> paths;
	const wchar_t* p = buf.c_str();
	const std::wstring first = p;
	p += first.size() + 1;
	if (*p == L'\0') {
		paths.push_back(first);
	}
	else {
		const std::wstring dir = first;
		while (*p) {
			paths.push_back(dir + L"\\" + p);
			p += wcslen(p) + 1;
		}
	}
	rnAddPaths(paths);
}

void WinQr::rnPickFolder()
{
	IFileOpenDialog* dlg = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&dlg))) || !dlg) {
		// 这里原来弹的是"没有新增文件" —— 对话框压根没打开，那句话是错的。
		// 这条路径在正常系统上走不到（COM 没初始化才会这样），静默返回，
		// 别用一句不对的提示盖住真正的原因
		return;
	}
	DWORD opts = 0;
	dlg->GetOptions(&opts);
	dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT);

	std::vector<std::wstring> paths;
	if (SUCCEEDED(dlg->Show(hwnd))) {
		IShellItemArray* arr = nullptr;
		if (SUCCEEDED(dlg->GetResults(&arr)) && arr) {
			DWORD count = 0;
			arr->GetCount(&count);
			for (DWORD i = 0; i < count; i++) {
				IShellItem* item = nullptr;
				if (SUCCEEDED(arr->GetItemAt(i, &item)) && item) {
					PWSTR psz = nullptr;
					if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
						paths.emplace_back(psz);
						CoTaskMemFree(psz);
					}
					item->Release();
				}
			}
			arr->Release();
		}
	}
	dlg->Release();
	if (!paths.empty()) rnAddPaths(paths);
}

// ———————————————— 过滤 ————————————————

void WinQr::rnRebuildFilter()
{
	// 格式表来自"已经拉进来的文件"：这一页的过滤就是按这些格式筛，不预设任何格式
	std::vector<std::wstring> next;
	for (const auto& row : rnRows) {
		const std::wstring e = Lower(ExtOfName(row.name));
		if (!e.empty() && std::find(next.begin(), next.end(), e) == next.end()) next.push_back(e);
	}
	std::sort(next.begin(), next.end());
	// 用 swap 换掉旧表，不逐条拷贝字符串：以前这里是 rnFmtAll = exts，
	// 一条一条复制 string 的编译器内联代码正是崩溃日志里那段
	// （basic_string 拷贝构造 +0x1A 访问违例）。换指针没有任何元素级读写
	rnFmtAll.swap(next);

	// 勾选状态只留还在的格式（文件被清掉后，那个格式的勾也跟着失效）
	std::set<std::wstring> keepOn;
	for (const auto& e : rnFmtOn) {
		if (std::find(rnFmtAll.begin(), rnFmtAll.end(), e) != rnFmtAll.end()) keepOn.insert(e);
	}
	rnFmtOn.swap(keepOn);

	HWND combo = GetDlgItem(hwnd, RnFilter);
	if (combo) {
		// 候选跟着列表里的文件自动长：加进来新格式，下拉里就多一项。
		// 框里的字原样留着（用户可能正打着一半关键字，重建候选不能把它冲掉）
		const std::wstring typed = rnText(RnFilter);
		rnSyncing = true;
		SendMessageW(combo, CB_RESETCONTENT, 0, 0);
		SendMessageW(combo, CB_ADDSTRING, 0,
			reinterpret_cast<LPARAM>(Lang::get(L"rn.allFmt").c_str()));
		for (const auto& e : rnFmtAll) {
			SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(e.c_str()));
		}
		SetWindowTextW(combo, typed.c_str());
		rnSyncing = false;
	}
	rnApplyFmt();
}

void WinQr::rnApplyFmt()
{
	// 手打了关键字：格式表就不参与了，交给列表按"扩展名 / 文件名含它"筛
	if (!rnFmtText.empty()) {
		rnb.setFilterList({});
		rnb.setKeyword(rnFmtText);
		InvalidateRect(hwnd, nullptr, FALSE);
		return;
	}
	rnb.setKeyword({});
	// rnFmtOn 为空 = 全选（默认状态）；否则只保留勾上的格式
	std::vector<std::wstring> on;
	for (const auto& e : rnFmtAll) {
		if (rnFmtOn.empty() || rnFmtOn.count(e) != 0) on.push_back(e);
	}
	rnb.setFilterList(rnFmtOn.empty() ? std::vector<std::wstring>() : on);
	InvalidateRect(hwnd, nullptr, FALSE);
}

void WinQr::rnSyncFilter()
{
	if (rnSyncing) return;
	// 过滤框既可手打、也能从自动候选里挑一项。挑的那一项就是精确格式，
	// 手打的字则按"包含"匹配（打 p 就把 png/jpg/pdf 一起筛出来）
	const std::wstring pick = Lower(Trim(rnText(RnFilter)));
	rnFmtText.clear();
	rnFmtOn.clear();
	if (pick.empty() || pick == Lower(Lang::get(L"rn.allFmt"))) {
		rnApplyFmt();
		return;
	}
	const bool exact = std::find(rnFmtAll.begin(), rnFmtAll.end(), pick) != rnFmtAll.end();
	if (exact) rnFmtOn.insert(pick);
	else rnFmtText = pick;
	rnApplyFmt();
}

int WinQr::rnFmtHit(int x, int y) const
{
	if (y < sc(RN_BOT_Y) || y >= sc(RN_BOT_Y + RN_BOT_H)) return -1;
	int fx = RN_FMT_X;
	for (int i = 0; i < static_cast<int>(rnFmtAll.size()); i++) {
		// 命中范围必须和绘制用同一个右边界，否则"看到的"和"点得到的"对不上
		if (fx + RN_FMT_W > RN_FMT_END) break;
		if (x >= sc(fx) && x < sc(fx + RN_FMT_W)) return i;
		fx += RN_FMT_PITCH;
	}
	return -1;
}

void WinQr::rnToggleFmt(int index)
{
	if (index < 0 || index >= static_cast<int>(rnFmtAll.size())) return;
	const std::wstring e = rnFmtAll[index];
	if (rnFmtOn.empty()) {
		// 原本"全勾"，点掉一个 = 除它以外都勾上
		for (const auto& x : rnFmtAll) {
			if (x != e) rnFmtOn.insert(x);
		}
	} else if (rnFmtOn.count(e) != 0) {
		rnFmtOn.erase(e);
	} else {
		rnFmtOn.insert(e);
	}
	// 勾满就等于"全部"，回到空集，下拉也显示全部格式
	if (rnFmtOn.size() >= rnFmtAll.size()) rnFmtOn.clear();

	// 勾选和下拉是同一份状态的俩入口：勾完把下拉里的字也改过来。
	// 顺手把手打的关键字清掉 —— 勾了具体格式，就按勾选走
	rnFmtText.clear();
	if (HWND combo = GetDlgItem(hwnd, RnFilter)) {
		rnSyncing = true;
		std::wstring text = Lang::get(L"rn.allFmt");
		if (rnFmtOn.size() == 1) {
			const std::wstring only = *rnFmtOn.begin();
			for (const auto& e : rnFmtAll) {
				if (e == only) { text = e; break; }
			}
		}
		SetWindowTextW(combo, text.c_str());
		rnSyncing = false;
	}
	rnApplyFmt();
}

// ———————————————— 按钮 ————————————————

void WinQr::rnPickAffixToken(int id)
{
	HWND c = GetDlgItem(hwnd, id);
	if (!c) return;
	const int sel = static_cast<int>(SendMessageW(c, CB_GETCURSEL, 0, 0));
	if (sel < 0) return;
	wchar_t buf[160]{};
	SendMessageW(c, CB_GETLBTEXT, static_cast<WPARAM>(sel), reinterpret_cast<LPARAM>(buf));
	std::wstring token = buf;
	const size_t cut = token.find(L' ');
	if (cut != std::wstring::npos) token.resize(cut);
	SetWindowTextW(c, token.c_str());
	rnApplyRules();
}

void WinQr::rnSetCase(int mode)
{
	// 四颗是同一组：全部大写 / 全部小写 / 首字大写 / 数字递增（= 序号）互斥，
	// 选一颗另外三颗就灭；再点同一颗自己就灭，回到"四颗都不选"
	rnCase = (rnCase == mode) ? -1 : mode;
	if (rnCase >= 0) rnSeqOn = false;   // 编号那条规则让出位置（序号面板那颗一起灭）
	// 自绘按钮不会自己重画，得显式刷一遍四颗
	for (int id : { static_cast<int>(RnCaseUpper), static_cast<int>(RnCaseLower),
		static_cast<int>(RnCaseTitle), static_cast<int>(RnNumEnable),
		static_cast<int>(RnSeqOn) }) {
		if (auto c = GetDlgItem(hwnd, id)) InvalidateRect(c, nullptr, TRUE);
	}
	rnApplyRules();
}

void WinQr::rnToggleSeq()
{
	// 「序号」/「数字递增」是同一颗开关的两个入口：选中 = 编号这条规则生效。
	// 它跟大小写三颗同组互斥 —— 亮起来时那三颗全灭；再点一下自己就取消
	rnSeqOn = !rnSeqOn;
	if (rnSeqOn) rnCase = -1;
	for (int id : { static_cast<int>(RnSeqOn), static_cast<int>(RnNumEnable),
		static_cast<int>(RnCaseUpper), static_cast<int>(RnCaseLower),
		static_cast<int>(RnCaseTitle) }) {
		if (auto c = GetDlgItem(hwnd, id)) InvalidateRect(c, nullptr, TRUE);
	}
	rnApplyRules();
}

// 删除面板六颗开关：每颗管一类字符，选中就把这一类从名字里抹掉，再点自己就取消
void WinQr::rnToggleDel(int which)
{
	switch (which) {
	case 0: rnDelSpace = !rnDelSpace; break;
	case 1: rnDelLower = !rnDelLower; break;
	case 2: rnDelUpper = !rnDelUpper; break;
	case 3: rnDelDigit = !rnDelDigit; break;
	case 4: rnDelCjk = !rnDelCjk; break;
	default: rnDelSpecial = !rnDelSpecial; break;
	}
	const int ids[] = { RnDelSpace, RnDelLower, RnDelUpper, RnDelDigit, RnDelCjk, RnDelSpecial };
	if (const int id = ids[which]; auto c = GetDlgItem(hwnd, id)) InvalidateRect(c, nullptr, TRUE);
	rnApplyRules();
}

// 替换面板的两颗开关：再点自己就取消，跟左边四颗规则钮同一套"点亮/熄灭"的画法
void WinQr::rnToggleAffix()
{
	// 「添加」开关独立成颗：勾上 = 前缀 / 后缀两条参与改名，不勾 = 框里留着字也不套用
	rnAffixOn = !rnAffixOn;
	if (auto c = GetDlgItem(hwnd, RnAffixOn)) InvalidateRect(c, nullptr, TRUE);
	rnApplyRules();
}

void WinQr::rnToggleRepCase()
{
	rnRepCase = !rnRepCase;
	if (auto c = GetDlgItem(hwnd, RnRepCase)) InvalidateRect(c, nullptr, TRUE);
	rnApplyRules();
}

void WinQr::rnToggleRepAll()
{
	rnRepAll = !rnRepAll;
	if (auto c = GetDlgItem(hwnd, RnRepAll)) InvalidateRect(c, nullptr, TRUE);
	rnApplyRules();
}

// 序号三个数字框都只收数字；空、非数字、越界一律退回"上一次的合法值"
int WinQr::rnSeqInt(int id, int fallback, int lo, int hi) const
{
	const std::wstring s = rnText(id);
	if (s.empty()) return fallback;
	for (wchar_t ch : s) if (ch < L'0' || ch > L'9') return fallback;
	long long v = 0;
	for (wchar_t ch : s) {
		v = v * 10 + (ch - L'0');
		if (v > 999999999LL) return hi;
	}
	return static_cast<int>(std::clamp<long long>(v, lo, hi));
}

void WinQr::rnSetExtMode(int mode)
{
	// 全大写 / 全小写 / 不变 / 删除 / 改为：五选一，再点同一颗就取消（全不选）
	rnExtMode = (rnExtMode == mode) ? -1 : mode;
	// 没停在「改为」这一档，就把下拉的选择清掉 —— 不然它还挂着上一次的值，
	// 让人以为两档同时亮着
	if (rnExtMode != 4) {
		if (auto c = GetDlgItem(hwnd, RnExtSet)) {
			SendMessageW(c, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
			SetWindowTextW(c, L"");
		}
	}
	for (int id : { static_cast<int>(RnExtUpper), static_cast<int>(RnExtLower),
		static_cast<int>(RnExtKeep), static_cast<int>(RnExtDrop) }) {
		if (auto c = GetDlgItem(hwnd, id)) InvalidateRect(c, nullptr, TRUE);
	}
	rnApplyRules();
}

void WinQr::rnPickExtSet()
{
	// 在「改为」下拉里挑了一项 = 这一档亮，前面四颗自动灭
	if (rnExtMode == 4) {
		// 已经是「改为」这一档，只是换了一项：照样得重算，不然列表还停在上一项
		rnApplyRules();
		return;
	}
	rnExtMode = 4;
	for (int id : { static_cast<int>(RnExtUpper), static_cast<int>(RnExtLower),
		static_cast<int>(RnExtKeep), static_cast<int>(RnExtDrop) }) {
		if (auto c = GetDlgItem(hwnd, id)) InvalidateRect(c, nullptr, TRUE);
	}
	rnApplyRules();
}

void WinQr::rnCheckAll(bool invert)
{
	for (auto& row : rnRows) {
		if (row.done) continue;
		row.checked = invert ? !row.checked : true;
	}
	// 扩展名规则只作用在勾选的行上：勾一改就得重算一遍新名和状态
	rnApplyRules();
}

void WinQr::rnCopyNames()
{
	std::wstring text;
	int count = 0;
	for (const auto& row : rnRows) {
		if (!row.checked) continue;
		text += row.newName;
		text += L"\r\n";
		count++;
	}
	if (count == 0) {
		for (const auto& row : rnRows) {
			text += row.newName;
			text += L"\r\n";
			count++;
		}
	}
	if (count == 0) return;
	// 复制失败（另一个程序正占着剪贴板）就别报"已复制"：见到的和做到的一致，
	// 用户重按一次就是了。走 Util::copyText，跟文件校验页的「复制」同一条路径
	if (Util::copyText(text)) showToast(Lang::get(L"rn.copied"));
}

void WinQr::rnClear()
{
	if (rnRows.empty()) return;
	rnRows.clear();
	rnUndoPairs.clear();
	// 过滤跟着清：它是"看哪一部分"的视图状态，留着旧关键字的话，清空之后再拉进来的
	// 文件会被那个关键字筛掉，列表看着还是空的，像是根本没加进来
	rnFmtOn.clear();
	rnFmtText.clear();
	rnSyncing = true;
	if (auto c = GetDlgItem(hwnd, RnFilter)) SetWindowTextW(c, L"");
	rnSyncing = false;
	rnRebuildFilter();
	rnApplyRules();
	InvalidateRect(hwnd, nullptr, FALSE);   // 右下角那个「共 N 个文件」跟着一起重画
}

// 规则全清：全部开关、下拉、文本框回到刚导入时的样子（只管规则，不动行数据）。
// 重置和撤销共用这一份 —— 撤销原先只清前缀/后缀，大小写、转换、序号、替换、删除
// 还挂在界面上，接着一算「新文件名」列还是旧规则的结果，看着就像撤销没生效
void WinQr::rnClearRules()
{
	rnCase = -1;
	rnSeqOn = false;
	rnDelSpace = rnDelLower = rnDelUpper = false;
	rnDelDigit = rnDelCjk = rnDelSpecial = false;
	rnExtMode = -1;   // 与初始状态一致：扩展名默认一颗都不选
	// 替换面板的两颗开关也回到默认（区分大小写开→关、替换所有→开）：
	// 它们同样是"规则"，重置不清的话，下次替换的行为跟刚导入时不一样，
	// 而界面上那两颗按钮还亮着，看不出是上一轮留下的
	rnRepCase = false;
	rnRepAll = true;
	rnAffixOn = false;   // 「添加」开关一并回默认：前缀 / 后缀重新变回"填了也不生效"
	rnSyncing = true;
	if (auto c = GetDlgItem(hwnd, RnConvert)) SendMessageW(c, CB_SETCURSEL, 0, 0);
	if (auto c = GetDlgItem(hwnd, RnExtSet)) {
		// 和初始化一致：清掉选中项、框里留空，免得重置后又挂着上一轮的扩展名
		SendMessageW(c, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
		SetWindowTextW(c, L"");
	}
	// 「改为」现在是可手打的输入框：重置要把框里的字一并清掉
	if (auto c = GetDlgItem(hwnd, RnConvTo)) {
		SendMessageW(c, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
		SetWindowTextW(c, L"");
	}
	// 前缀 / 后缀一并清空
	if (auto c = GetDlgItem(hwnd, RnPrefix)) SetWindowTextW(c, L"");
	if (auto c = GetDlgItem(hwnd, RnSuffix)) SetWindowTextW(c, L"");
	SetWindowTextW(GetDlgItem(hwnd, RnFind), L"");
	SetWindowTextW(GetDlgItem(hwnd, RnReplace), L"");
	SetWindowTextW(GetDlgItem(hwnd, RnDelText), L"");
	// 序号三格也回初始值（起始 1 / 增量 1 / 位数 0）。留着上一轮的数字，
	// 下次一勾「序号」就按旧值编号，而界面上看不出那是上一轮留下的
	if (auto c = GetDlgItem(hwnd, RnSeqStart)) SetWindowTextW(c, L"1");
	if (auto c = GetDlgItem(hwnd, RnSeqStep)) SetWindowTextW(c, L"1");
	if (auto c = GetDlgItem(hwnd, RnSeqDigits)) SetWindowTextW(c, L"1");
	rnSeqStart = 1;
	rnSeqStep = 1;
	rnSeqDigits = 1;
	rnSyncing = false;
	for (int id : { static_cast<int>(RnDelSpace), static_cast<int>(RnDelLower),
		static_cast<int>(RnDelUpper), static_cast<int>(RnDelDigit),
		static_cast<int>(RnDelCjk), static_cast<int>(RnDelSpecial) }) {
		if (auto c = GetDlgItem(hwnd, id)) InvalidateRect(c, nullptr, TRUE);
	}
	for (int id : { static_cast<int>(RnCaseUpper), static_cast<int>(RnCaseLower),
		static_cast<int>(RnCaseTitle), static_cast<int>(RnNumEnable),
		static_cast<int>(RnRepCase), static_cast<int>(RnRepAll),
		static_cast<int>(RnAffixOn),
		static_cast<int>(RnExtUpper), static_cast<int>(RnExtLower),
		static_cast<int>(RnExtKeep), static_cast<int>(RnExtDrop) }) {
		if (auto c = GetDlgItem(hwnd, id)) InvalidateRect(c, nullptr, TRUE);
	}
}

void WinQr::rnReset()
{
	// 重置 = 回到刚导入的样子：规则全清、手工名全清、改过的行重新算作"起点"，
	// 勾选也全恢复。这样重置完不用重新拖文件，直接选行改就行
	rnHideEditor();
	rnClearRules();
	for (auto& row : rnRows) {
		// 已经改过名的行：盘上现在叫 name（rnRun 里已换过来），把它当成新的原始名
		if (row.done) {
			row.done = false;
			row.size = FileSizeOf(JoinPath(row.dir, row.name));
		}
		row.manualName.clear();
		row.checked = true;
		row.status = RnReady;
	}
	// 起点换了，上一轮的撤销记录就对不上了，一并作废
	rnUndoPairs.clear();
	rnApplyRules();
	InvalidateRect(hwnd, nullptr, FALSE);
}

void WinQr::rnRun()
{
	rnHideEditor();   // 执行会重写行里的名字，编辑框先落定，免得写进已经换了名的行
	std::vector<RnMove> moves;
	int skipped = 0;
	for (int i = 0; i < static_cast<int>(rnRows.size()); i++) {
		const RnRow& row = rnRows[i];
		if (row.done || !row.checked) { skipped++; continue; }
		if (row.status != RnReady) { skipped++; continue; }
		// 逐字比：只改大小写的（a.png → a.PNG）也是真改名，不能当"没变化"漏掉
		if (row.newName == row.name) { skipped++; continue; }
		RnMove m;
		m.row = i;
		m.src = JoinPath(row.dir, row.name);
		m.dst = JoinPath(row.dir, row.newName);
		moves.push_back(std::move(m));
	}

	if (moves.empty()) {
		showToast(Lang::get(L"rn.nothing"));
		return;
	}

	size_t bad = 0;
	if (!MoveAll(moves, &bad)) {
		// 只把真正卡住的那一行标失败：其余行已被整体退回原样，仍然是"可以改"
		for (size_t i = 0; i < moves.size(); i++) {
			rnRows[moves[i].row].status = (i == bad) ? RnFail : RnReady;
		}
		rnb.refresh();
		showToast(Lang::get(L"rn.fail"));
		return;
	}

	rnUndoPairs.clear();
	for (auto& m : moves) {
		RnRow& row = rnRows[m.row];
		row.dir = DirOf(m.dst);
		row.name = NameOf(m.dst);
		row.newName = row.name;
		row.manualName.clear();
		row.done = true;
		row.status = RnDone;
		row.size = FileSizeOf(m.dst);
		rnUndoPairs.emplace_back(m.dst, m.src);
	}

	// 改名后仍留在列表里，状态列显示"成功"—— 用户一眼能看到这一批的结果；
	// 这些行不再参与规则（rnRecalcStatus 里 done 的行直接跳过）
	rnRebuildFilter();
	rnApplyRules();

	wchar_t buf[96]{};
	swprintf_s(buf, Lang::get(L"rn.doneFmt").c_str(), static_cast<int>(moves.size()), skipped);
	showToast(buf);
}

void WinQr::rnUndo()
{
	rnHideEditor();   // 撤销会改行里的名字，先把还开着的编辑框落定
	if (rnUndoPairs.empty()) {
		showToast(Lang::get(L"rn.undoEmpty"));
		return;
	}
	std::vector<RnMove> moves;
	moves.reserve(rnUndoPairs.size());
	// 逆序还原：后改的先退，避免中途互相占名
	for (auto it = rnUndoPairs.rbegin(); it != rnUndoPairs.rend(); ++it) {
		RnMove m;
		m.src = it->first;    // 现在的名字（= 上次改完的名字）
		m.dst = it->second;   // 原来的名字
		moves.push_back(std::move(m));
	}

	if (!MoveAll(moves)) {
		showToast(Lang::get(L"rn.fail"));
		return;
	}
	rnUndoPairs.clear();
	// 行还在列表里（改完留着"成功"那条记录）：按"撤销前它在盘上的名字"倒着找回来。
	// 找的键必须是 m.src —— 列表里的 row.name 存的正是上次改完的新名字；
	// 早先拿 m.dst（原名）去找，永远找不到，撤销完了列表还挂着"成功"
	for (auto& m : moves) {
		const std::wstring dir = DirOf(m.src);
		const std::wstring name = NameOf(m.src);
		for (auto& row : rnRows) {
			if (Lower(row.dir) == Lower(dir) && Lower(row.name) == Lower(name)) {
				row.dir = DirOf(m.dst);       // 退回原名（目录一般没变，照写更稳）
				row.name = NameOf(m.dst);
				row.newName = row.name;
				row.done = false;
				row.manualName.clear();
				row.size = FileSizeOf(m.dst);
				break;
			}
		}
	}
	// 扩展名可能跟着回来了，格式表也要重建
	rnRebuildFilter();
	// 撤销后把所有规则一并清空：规则还挂在界面上，下面 rnApplyRules 会立刻按旧规则
	// 重算一遍，「新文件名」列看着跟没撤销一模一样（其实行已经退回原名了），
	// 用户会以为撤销没生效。只清前缀 / 后缀不够 —— 大小写、转换、序号、替换、删除
	// 一样能把名字重新拼出来。全清之后列表才真回到刚导入的样子，肉眼就能确认
	rnClearRules();
	rnApplyRules();
	showToast(Lang::get(L"rn.undone"));
}

// ———————————————— 原地编辑 ————————————————

void WinQr::rnBeginEdit(int rowIndex)
{
	auto ed = GetDlgItem(hwnd, RnEdit);
	if (!ed) return;
	RECT cell{};
	if (!rnb.cellRect(rowIndex, 2, cell)) return;

	// 把编辑框挪到那一格上（左右各缩 1px，别压住列线），高矮跟行齐平。
	// cellRect 给的是列表客户区坐标；编辑框是主窗口的子窗口，SetWindowPos 要的是
	// 主窗口客户区坐标 —— 所以用 MapWindowPoints 从列表换到主窗口，不能换成屏幕
	// 坐标（子窗口按屏幕坐标摆会被再叠上一次窗口原点，整块跑到窗口外面看不见）
	POINT tl{ cell.left + 1, cell.top + 1 };
	MapWindowPoints(rnb.hwnd(), hwnd, &tl, 1);
	// 进编辑时框里只放主名，扩展名不进去：一进去就是全选，退格一下清空重打，
	// 也不会顺手把 .png 删掉；列表那格照旧显示带扩展名的完整名字
	SetWindowTextW(ed, BaseOfName(rnRows[rowIndex].newName).c_str());
	SetWindowPos(ed, HWND_TOP, tl.x, tl.y,
		cell.right - cell.left - 2, cell.bottom - cell.top - 2,
		SWP_SHOWWINDOW | SWP_NOACTIVATE);
	rnEditRow = rowIndex;
	// 列表带了 WS_CLIPSIBLINGS（见 RnList::create）：这一行的待重画不会盖到编辑框上，
	// 进去就是全选，全选也稳得住
	SetFocus(ed);
	SendMessageW(ed, EM_SETSEL, 0, -1);
}

void WinQr::rnCommitEdit()
{
	if (rnEditRow < 0) return;
	auto ed = GetDlgItem(hwnd, RnEdit);
	const int rowIndex = rnEditRow;
	rnEditRow = -1;
	ShowWindow(ed, SW_HIDE);
	if (rowIndex < 0 || rowIndex >= static_cast<int>(rnRows.size())) return;

	std::wstring text = Trim(rnText(RnEdit));
	if (text.empty() || !ValidName(text)) {
		// 不合法就丢掉这次输入，列表上还留着原来那行 —— 比塞进一个非法名字强
		showToast(Lang::get(L"rn.badName"));
		return;
	}
	// 框里只有主名，落定时把它接回这一行原有的扩展名（扩展名归上面那排规则管，不在这儿改）；
	// 用户自己把扩展名也打进去了（比如真想要 a.tar.gz），就别再接一遍
	std::wstring full = text;
	const std::wstring ext = ExtOfName(rnRows[rowIndex].newName);
	if (!ext.empty()) {
		const std::wstring tail = L"." + ext;
		const bool typedTail = full.size() >= tail.size()
			&& Lower(full.substr(full.size() - tail.size())) == Lower(tail);
		if (!typedTail) full += tail;
	}
	// 名字跟原来一模一样（用户点进来又原样确认）：不算"改过名"，
	// 否则这一行会一直挂着红色，让人以为动过手
	if (full != rnRows[rowIndex].newName) {
		rnRows[rowIndex].manualName = full;
	}
	rnApplyRules();
}

void WinQr::rnCancelEdit()
{
	rnEditRow = -1;
	if (auto ed = GetDlgItem(hwnd, RnEdit)) ShowWindow(ed, SW_HIDE);
}

void WinQr::rnHideEditor()
{
	if (rnEditRow < 0) {
		if (auto ed = GetDlgItem(hwnd, RnEdit)) {
			if (IsWindowVisible(ed)) ShowWindow(ed, SW_HIDE);
		}
		return;
	}
	// 正在编辑时被滚动 / 切页打断：先提交（用户的输入不该悄悄丢）
	rnCommitEdit();
}

LRESULT CALLBACK WinQr::rnEditProcEntry(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	WinQr* self = instanceOf(GetParent(h));
	if (!self) return DefWindowProcW(h, msg, wp, lp);
	return self->rnEditProc(h, msg, wp, lp);
}

LRESULT WinQr::rnEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_KEYDOWN:
		if (wp == VK_RETURN) {
			rnCommitEdit();
			return 0;
		}
		if (wp == VK_ESCAPE) {
			rnCancelEdit();
			rnb.refresh();
			return 0;
		}
		break;
	case WM_KILLFOCUS:
		// 点到别处 = 落定（不提交就白输了）
		rnCommitEdit();
		break;
	case WM_GETDLGCODE:
		return DLGC_WANTALLKEYS;
	case WM_NCDESTROY:
		if (rnEditOld) SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(rnEditOld));
		rnEditOld = nullptr;
		break;
	default:
		break;
	}
	return rnEditOld ? CallWindowProcW(rnEditOld, h, msg, wp, lp)
		: DefWindowProcW(h, msg, wp, lp);
}

// ———————————————— 叠格下拉：点开前先抻高 ————————————————
// 「转换 / 前缀 / 后缀」三只下拉叠在同一格（谁亮着显示谁）。窗口高度只给一行，
// 是为了不盖住下面的「扩展名」那排按钮；可系统的下拉列表高度算的正是
// "窗口高 - 一行"，一行高的窗口算出来是 0 —— 点开自然是空的
// （CB_SETMINVISIBLE 只管最多展开几条，管不了控件自己的高）。
// 这里在按下的那一瞬把窗口往下抻高，列表收起后再缩回一行：抻出来的那一截
// 始终被弹出的列表盖着，看不到。
LRESULT CALLBACK WinQr::rnComboProcEntry(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	WinQr* self = instanceOf(GetParent(h));
	if (!self) return DefWindowProcW(h, msg, wp, lp);
	return self->rnComboProc(h, msg, wp, lp);
}

LRESULT WinQr::rnComboProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	constexpr UINT_PTR kPollTimer = 7;   // 收起后靠它把窗口缩回一行

	auto resizeH = [h](int newH) {
		RECT wr{};
		GetWindowRect(h, &wr);
		// 顺带抬到同级最上层：抻出来的那截会压在下面的控件上，不提上来就看不见（也就没得闪）
		SetWindowPos(h, HWND_TOP, 0, 0, wr.right - wr.left, newH,
			SWP_NOMOVE | SWP_NOACTIVATE);
	};

	switch (msg) {
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_KEYDOWN: {
		bool wantDrop = msg != WM_KEYDOWN;
		if (msg == WM_KEYDOWN) {
			wantDrop = (wp == VK_F4) || (wp == VK_DOWN && (GetKeyState(VK_MENU) & 0x8000) != 0);
		}
		if (wantDrop && rnComboTall != h) {
			int items = static_cast<int>(SendMessageW(h, CB_GETCOUNT, 0, 0));
			if (items < 1) items = 1;
			if (items > 8) items = 8;
			resizeH(sc(RN_ROW_H) + sc(22) * items);
			rnComboTall = h;
			SetTimer(h, kPollTimer, 150, nullptr);
		}
		break;
	}
	case WM_TIMER:
		if (wp == kPollTimer) {
			// 列表一收起（CB_GETDROPPEDSTATE=0）就缩回一行，那截空白不能一直盖着下面
			if (!SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0)) {
				KillTimer(h, kPollTimer);
				rnComboTall = nullptr;
				resizeH(sc(RN_ROW_H));
			}
			return 0;
		}
		break;
	case WM_SHOWWINDOW:
		// 切到别的那一格时这只被藏起来，顺手缩回去
		if (!wp && rnComboTall == h) {
			KillTimer(h, kPollTimer);
			rnComboTall = nullptr;
			resizeH(sc(RN_ROW_H));
		}
		break;
	case WM_NCDESTROY:
		KillTimer(h, kPollTimer);
		if (rnComboTall == h) rnComboTall = nullptr;
		if (rnComboOld) SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(rnComboOld));
		break;
	default:
		break;
	}
	return rnComboOld ? CallWindowProcW(rnComboOld, h, msg, wp, lp)
		: DefWindowProcW(h, msg, wp, lp);
}

// ———————————————— 页面上的字 ————————————————
// 这一页照参考图是"白底 + 细灰分组框 + 框上压标题"：
//   文件名（单选 4 颗 + 转换 / 前缀）  替换（查找 / 替换为 两格）
//   扩展名（单选 4 颗 + 改为）         过滤（一个下拉）
// 框、标题、单选圆圈、底部格式勾选都没有 HWND，全画在这里；点击命中走 rnFmtHit
void WinQr::drawRenamePage(HDC dc)
{
	auto fill = [this, dc](int x, int y, int w, int h, COLORREF c) {
		RECT r{ sc(x), sc(y), sc(x + w), sc(y + h) };
		HBRUSH b = CreateSolidBrush(c);
		FillRect(dc, &r, b);
		DeleteObject(b);
	};
	auto frame = [this, dc](int x, int y, int w, int h) {
		RECT r{ sc(x), sc(y), sc(x + w), sc(y + h) };
		HBRUSH b = CreateSolidBrush(RN_BORDER);
		FrameRect(dc, &r, b);
		DeleteObject(b);
	};
	auto text = [this, dc](int x, int y, int w, int h, const std::wstring& s, COLORREF c, int weight) {
		RECT r{ sc(x), sc(y), sc(x + w), sc(y + h) };
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, c);
		HGDIOBJ oldFont = SelectObject(dc, fontOf(FsBody, weight));
		DrawTextW(dc, s.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
		SelectObject(dc, oldFont);
	};
	// 分组框：白色卡片 + 灰框，标题压在上边框线上（把那一小段擦成卡片白底，
	// 字才不被线穿过）。白色卡片这一层是照着基础配色来的 —— 其余五页都是
	// 浅灰页面上浮白卡片，这一页若整页铺白，分组框就成了"白底上画线"，格格不入
	// 擦白的宽度按标题实际像素量出来 —— 写死 68 的话，「过滤」这种两个字会被擦出
	// 一段比字宽得多的空白，上边框看着像被挖了个洞；六字标题又会裁掉半个字
	auto group = [&](int x, int y, int w, int h, const wchar_t* title) {
		fill(x, y, w, h, RN_WHITE);
		frame(x, y, w, h);
		if (!title || !*title) return;
		HGDIOBJ old = SelectObject(dc, fontOf(FsBody, 700));
		SIZE sz{};
		GetTextExtentPoint32W(dc, title, static_cast<int>(wcslen(title)), &sz);
		SelectObject(dc, old);
		const int tw = MulDiv(sz.cx, 96, dpi) + 2;   // 物理像素量出的字宽换回逻辑单位
		fill(x + 8, y - 9, 4 + tw + 8, 18, RN_WHITE);
		text(x + 12, y - 9, 4 + tw, 18, title, RN_TEXT, 700);
	};

	// 整页铺浅灰底（与其余五页同一色），分组框和格式勾选落在上面
	fill(0, RN_TAB_BOTTOM, RN_W, 445 - RN_TAB_BOTTOM, RN_PAGE_BG);

	// 面板卡片：位置固定，切换只换里面露出来的那组控件。
	// 卡片不写标题 —— 四个切换标签就压在这张卡片的上边框线上，
	// 跟其余分组框的标题同一个位置、同一号字，只是选中的那颗换成主色
	group(RN_MARGIN, RN_PANEL_Y, RN_PANEL_W, RN_PANEL_H, L"");
	group(RN_G3_X, RN_G3_Y, RN_G3_W, RN_G3_H, Lang::get(L"rn.filterLbl").c_str());

	// 五个切换标签：压在卡片的上边框线上 —— 每颗只把自己底下那截线擦成卡片白底，
	// 中间留出缝，上边框还是连贯的一条（整段擦白会让框顶看着缺一块）。
	// 选中 → 黑色加粗，其余 → 黑色常规；不做按钮底、不做下划线 —— 只是笔画轻重差一档
	// 它们没有 HWND，点击命中在 rnTabHit，位置同取 kRnTabs
	// 标签字比正文大一号（18 → 19）：按 FsBody 的字形另取一份，只给这五颗用。
	// 选中那颗用 700，其余 500 —— 只是笔画轻重差一档，颜色始终是正文黑
	auto tabFont = [this](int weight) -> HFONT {
		static HFONT cached[2]{};
		static int cachedH = 0;
		const int slot = weight >= 700 ? 1 : 0;
		const int h = sc(19);
		if (cached[slot] && cachedH == h) return cached[slot];
		if (cachedH != h) {
			// DPI 变了要重建：旧句柄先删掉再置空，直接置空就是每次改缩放漏 2 个字体
			if (cached[0]) DeleteObject(cached[0]);
			if (cached[1]) DeleteObject(cached[1]);
			cached[0] = cached[1] = nullptr;
			cachedH = h;
		}
		LOGFONTW lf{};
		GetObjectW(fontOf(FsBody, 500), sizeof(lf), &lf);
		lf.lfHeight = lf.lfHeight < 0 ? -h : h;
		lf.lfWeight = weight >= 700 ? 700 : 500;
		cached[slot] = CreateFontIndirectW(&lf);
		return cached[slot];
	};
	// 擦白必须跟"字实际占多宽"一样宽：槽宽是给点击命中用的（44 / 33 / 46），
	// 和字宽并不相等 —— 按槽宽擦，白块就比字探出一截，框线上留下一块块错位的白斑；
	// 比字窄时又会让线从字身上穿过去。这里按实测字宽擦，左右各让 2
	for (int i = 0; i < kRnTabCount; i++) {
		const std::wstring label = Lang::get(kRnTabs[i].key);
		HGDIOBJ old = SelectObject(dc, tabFont(rnPanel == i ? 700 : 500));
		SIZE sz{};
		GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &sz);
		SelectObject(dc, old);
		const int tw = MulDiv(sz.cx, 96, dpi);
		fill(kRnTabs[i].x - 2, RN_TITLE_Y, tw + 4, RN_TITLE_H, RN_WHITE);
	}
	for (int i = 0; i < kRnTabCount; i++) {
		const std::wstring label = Lang::get(kRnTabs[i].key);
		RECT r{ sc(kRnTabs[i].x), sc(RN_TITLE_Y), sc(kRnTabs[i].x + kRnTabs[i].w + 24), sc(RN_TITLE_Y + RN_TITLE_H) };
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RN_TEXT);
		HGDIOBJ oldTabFont = SelectObject(dc, tabFont(rnPanel == i ? 700 : 500));
		DrawTextW(dc, label.c_str(), -1, &r,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		SelectObject(dc, oldTabFont);
	}

	// 各面板画出来的行标签（控件不自带文字，见 relayoutRename 里算好的贴合位置）
	if (rnPanel == 0) {
		text(RN_CONV_LBL_X, RN_ROW1_Y, rnConvX - RN_CONV_LBL_X, RN_ROW_H, Lang::get(L"rn.convert"), RN_TEXT, 500);
		text(RN_CONV_LBL_X, RN_ROW2_Y, rnConvToX - RN_CONV_LBL_X, RN_ROW_H, Lang::get(L"rn.convTo"), RN_TEXT, 500);
	} else if (rnPanel == 1) {
		text(RN_EXT_LBL_X, RN_ROW2_Y, RN_EXT_LBL_W, RN_ROW_H, Lang::get(L"rn.extSetLbl"), RN_TEXT, 500);
	} else if (rnPanel == 2) {
		text(RN_FIND_LBL_X, RN_ROW2_Y, rnFindX - RN_FIND_LBL_X, RN_ROW_H, Lang::get(L"rn.findLbl"), RN_TEXT, 500);
		text(rnReplLblX, RN_ROW2_Y, rnReplaceX - rnReplLblX, RN_ROW_H, Lang::get(L"rn.replLbl"), RN_TEXT, 500);
	} else if (rnPanel == 3) {
		// 「添加」行在上、「序号」行在下（与 relayoutRename 的 place 同一次对调）
		text(rnPfxLblX, RN_ROW1_Y, rnPfxX - rnPfxLblX, RN_ROW_H, Lang::get(L"rn.prefix"), RN_TEXT, 500);
		text(rnSufLblX, RN_ROW1_Y, rnSufX - rnSufLblX, RN_ROW_H, Lang::get(L"rn.suffix"), RN_TEXT, 500);
		text(rnSeqStartLblX, RN_ROW2_Y, rnSeqStartX - rnSeqStartLblX, RN_ROW_H, Lang::get(L"rn.seqStart"), RN_TEXT, 500);
		text(rnSeqStepLblX, RN_ROW2_Y, rnSeqStepX - rnSeqStepLblX, RN_ROW_H, Lang::get(L"rn.seqStep"), RN_TEXT, 500);
		text(rnSeqDigLblX, RN_ROW2_Y, rnSeqDigX - rnSeqDigLblX, RN_ROW_H, Lang::get(L"rn.seqDigits"), RN_TEXT, 500);
	} else if (rnPanel == 4) {
		text(RN_DEL_LBL_X, RN_ROW2_Y, rnDelTxtX - RN_DEL_LBL_X, RN_ROW_H, Lang::get(L"rn.delTextLbl"), RN_TEXT, 500);
	}

	// 底部：勾上的格式才进列表（全勾 = 不过滤）。和"过滤"下拉是同一份状态的两个入口
	int fx = RN_FMT_X;
	for (const auto& e : rnFmtAll) {
		if (fx + RN_FMT_W > RN_FMT_END) break;
		const bool on = rnFmtOn.empty() || rnFmtOn.count(e) != 0;
		const int by = RN_BOT_Y + (RN_BOT_H - 14) / 2;
		RECT box{ sc(fx), sc(by), sc(fx + 14), sc(by + 14) };
		HBRUSH bg = CreateSolidBrush(on ? RN_PRIMARY : RN_WHITE);
		FillRect(dc, &box, bg);
		DeleteObject(bg);
		HBRUSH bd = CreateSolidBrush(on ? RN_PRIMARY : RN_BORDER);
		FrameRect(dc, &box, bd);
		DeleteObject(bd);
		if (on) {
			// 对勾和面板里那几颗勾选项用同一组比例坐标（方头线帽、2px）——
			// 这里早先按"相对格子左边 3/6/11、上边 7/10/4"的绝对像素画，
			// 一换 DPI / 一换框大小，勾就成了歪歪扭扭的一折
			HPEN pen = CreatePen(PS_SOLID, sc(2), RN_WHITE);
			HGDIOBJ oldPen = SelectObject(dc, pen);
			const POINT pts[3] = {
				{ sc(fx + 14 * 3 / 10), sc(by + 14 * 52 / 100) },
				{ sc(fx + 14 * 44 / 100), sc(by + 14 * 74 / 100) },
				{ sc(fx + 14 * 76 / 100), sc(by + 14 * 28 / 100) },
			};
			Polyline(dc, pts, 3);
			SelectObject(dc, oldPen);
			DeleteObject(pen);
		}
		text(fx + 18, by - 2, RN_FMT_W - 18, 18, L"." + e, RN_TEXT, 400);
		fx += RN_FMT_PITCH;
	}

	// 右下角：这份一共导入了多少个文件 —— 不管上面勾了哪几个格式，总数就摆在这儿
	// （格式勾选只决定哪些行进列表，跟"装进来多少"是两件事，容易混，所以写明）
	if (!rnRows.empty()) {
		wchar_t buf[64]{};
		swprintf_s(buf, Lang::get(L"rn.fileCount").c_str(), static_cast<int>(rnRows.size()));
		RECT r{ sc(RN_W - RN_MARGIN - 56), sc(RN_BOT_Y), sc(RN_W - RN_MARGIN), sc(RN_BOT_Y + RN_BOT_H) };
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RN_TEXT_MUTED);
		HGDIOBJ oldFont = SelectObject(dc, fontOf(FsBody, 400));
		DrawTextW(dc, buf, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		SelectObject(dc, oldFont);
	}
}
