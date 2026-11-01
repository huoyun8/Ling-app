#pragma once
#include <windows.h>
#include <deque>
#include <functional>
#include <string>
#include <vector>

// 文件校验页的结果框。
//
// 为什么不用系统 EDIT：多行只读 EDIT 在"程序反复 SetWindowText + 用户同时滚动"时，
// 会把新文字直接画在旧像素上（行距按缓存的行高算、退格重绘的区域不对），
// 表现就是文字上下叠成一片，而且越滚越花。这一页的结果是流式追加的，
// 正好踩在这个坑上。
//
// 所以这里自己画：行高由当前字体实测的 tmHeight 算，每帧整块重绘 + 裁剪，
// 滚动只改一个像素偏移量。结论不占框内任何地方：整圈外框染成绿（相同）
// 或红（不同）就够了 —— 原来那一整条竖栏会把 SHA256 那 72 个字符的尾巴挤掉。
class VfBox
{
public:
	static const wchar_t* className();
	// 建子窗口。失败返回空句柄
	HWND create(HWND parent, int id, int dpi);
	HWND hwnd() const { return wnd; }

	void setDpi(int value);
	// lineFontSmall：一行放不下时自动降一档字号用的（SHA256 那 72 个字符最容易顶格），
	// 传空就不降，直接省略
	void setFonts(HFONT lineFont, HFONT lineFontSmall);
	// 清空所有记录与结论
	void clear();
	// 追加一条记录（每个元素是一行文字）。只保留最新两条，新的顶掉最旧的
	void pushRecord(std::vector<std::wstring> lines);
	// 结论不外露文字，只把整圈外框染色 —— 不占正文宽度，也不跟内容抢地方：
	// 相同 = 绿框，不同 = 红框，没有结论（含首次 / 校验中）= 原灰框。
	// frame 传绿或红即表示"有结论"，text 只在 clear/paint 内部当有无所用
	void setVerdict(const std::wstring& text, COLORREF frame);
	// 复制 / 另存用：把当前记录按显示顺序拼成纯文本（不含结论）
	std::wstring allText() const;
	// 框内 Ctrl+V / WM_PASTE：把动作交给主窗口（它去读剪贴板，再把内容当一条记录塞回来）
	void setOnPaste(std::function<void()> cb) { onPaste = std::move(cb); }

	// ——— 文本选中 ———
	// 结果框是自绘的，系统给不了选中能力，所以自己记一份选区。
	// 坐标用"扁平行号 + 字符下标"：扁平行 = 所有记录的行首尾相接，
	// 记录之间那条分隔线不占行号，与 paint 里的行序一一对应。
	void clearSelection();
	bool hasSelection() const;
	std::wstring selectedText() const;

private:
	struct Rec { std::vector<std::wstring> lines; };
	struct SelPos { int row{ 0 }, ch{ 0 }; };

	static LRESULT CALLBACK procEntry(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT proc(UINT msg, WPARAM wp, LPARAM lp);
	void paint(HDC dc);
	void syncScrollBar(bool followBottom);
	int sc(int v) const { return MulDiv(v, dpi, 96); }
	int lineHeight() const;
	int contentHeight() const;
	int viewHeight() const;
	// 文字列可用的右边界（只给滚动条让位）。结论只染外框，不再从右边切掉一栏
	int textRight() const;
	void clampScroll();

	// 正文左边界（与 paint 里 textArea.left 同一套算法）
	int textLeft() const;
	// 扁平行 -> 该行顶边的客户区 y（已含滚动量）。分隔线占的那一行不进结果，
	// 但会在 y 上留出空档，所以必须和 paint 走同一套累加
	std::vector<int> rowTops() const;
	void rebuildFlat();
	void hitTest(int x, int y, SelPos& out) const;
	// 一行里第 ch 个字符相对行首的偏移（第 0 个是 0，第 size 个是整行宽）。
	// 必须用当前正文字体量，否则选区和字形对不齐
	int charOffset(HDC dc, const std::wstring& s, int ch) const;
	// 一行里第 ch 个字符的左边 x（第 0 个是行首）
	int charX(const std::wstring& s, int ch) const;
	// 两个选区端点排成 a <= b
	void orderedSel(SelPos& a, SelPos& b) const;
	void selectAll();
	void copySelection();

private:
	HWND wnd{ nullptr };
	int dpi{ 96 };
	HFONT fontLine{ nullptr };
	HFONT fontLineSmall{ nullptr };
	std::deque<Rec> records;
	// 所有记录的行摊平后的结果，选中与绘制都以它为准
	std::vector<std::wstring> flat;
	SelPos selAnchor{}, selCaret;
	bool selecting{ false };
	std::wstring verdict;
	// 外框颜色：中性（无结论 / 首次 / 校验中）就是边框那条浅灰，相同绿、不同红
	COLORREF verdictFrame{ RGB(208, 216, 228) };
	int scrollY{ 0 };
	// 只在"内容变了"时重设滚动条，避免鼠标拖动滚动条时被我们抢回去
	int lastContentH{ -1 };
	// 粘贴回调：框内接 Ctrl+V / WM_PASTE 时转给主窗口
	std::function<void()> onPaste;
};
