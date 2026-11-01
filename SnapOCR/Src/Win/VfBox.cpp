#include "pch.h"
#include "VfBox.h"
#include <algorithm>
#include <climits>

namespace {
	constexpr int kMaxRecords = 2;
	// 与主窗口同一套配色（页面底色偏冷白，边框 208,216,228）
	const COLORREF C_BORDER = RGB(208, 216, 228);
	const COLORREF C_TEXT = RGB(45, 57, 74);
	const COLORREF C_SEP = RGB(214, 222, 232);
	const COLORREF C_WHITE = RGB(255, 255, 255);
	// 选中底色：比正文浅很多、比白底明显，压在字形下面不抢字
	const COLORREF C_SEL = RGB(204, 226, 255);

	// 自绘控件命中测试要用的固定类名（带品牌前缀，两版可同机运行）
	const wchar_t* kVfClass = SNAP_CLS_VFBOX;
}

const wchar_t* VfBox::className()
{
	return kVfClass;
}

HWND VfBox::create(HWND parent, int id, int dpiValue)
{
	dpi = dpiValue > 0 ? dpiValue : 96;
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	// CS_DBLCLKS 不需要；关键是把背景擦除交给 WM_ERASEBKGND 直接吃掉，避免闪白
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = procEntry;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.lpszClassName = kVfClass;
	wc.hbrBackground = nullptr;
	RegisterClassExW(&wc);   // 已注册时返回 0，忽略即可

	wnd = CreateWindowExW(0, kVfClass, L"",
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS,
		0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
		GetModuleHandleW(nullptr), this);
	return wnd;
}

void VfBox::setDpi(int value)
{
	dpi = value > 0 ? value : 96;
	InvalidateRect(wnd, nullptr, TRUE);
}

void VfBox::setFonts(HFONT lineFont, HFONT lineFontSmall)
{
	fontLine = lineFont;
	fontLineSmall = lineFontSmall;
	// 换了字体行高就变了，滚动位置要重新夹一次
	clampScroll();
	InvalidateRect(wnd, nullptr, TRUE);
}

void VfBox::clear()
{
	records.clear();
	verdict.clear();
	scrollY = 0;
	rebuildFlat();
	syncScrollBar(false);
	InvalidateRect(wnd, nullptr, TRUE);
}

void VfBox::pushRecord(std::vector<std::wstring> lines)
{
	while (!lines.empty() && lines.back().empty()) lines.pop_back();
	if (lines.empty()) return;
	records.push_back(Rec{ std::move(lines) });
	while (records.size() > (size_t)kMaxRecords) records.pop_front();
	rebuildFlat();
	// 新结果进来看最新那条：直接滚到底
	syncScrollBar(true);
	InvalidateRect(wnd, nullptr, TRUE);
}

void VfBox::setVerdict(const std::wstring& text, COLORREF frame)
{
	if (verdict == text && verdictFrame == frame) return;
	verdict = text;
	verdictFrame = frame;
	InvalidateRect(wnd, nullptr, TRUE);
}

std::wstring VfBox::allText() const
{
	std::wstring out;
	for (size_t r = 0; r < records.size(); r++) {
		if (r > 0) out += L"--------------------------------\r\n";
		for (const auto& line : records[r].lines) {
			out += line;
			out += L"\r\n";
		}
	}
	return out;
}

// ————————————————— 选中 —————————————————

void VfBox::rebuildFlat()
{
	flat.clear();
	for (const auto& rec : records) {
		for (const auto& line : rec.lines) flat.push_back(line);
	}
	// 记录被顶掉后行号会整体平移，越界的端点直接夹回来，别让它指到不存在的行
	if ((int)flat.size() <= selAnchor.row) { selAnchor.row = 0; selAnchor.ch = 0; }
	if ((int)flat.size() <= selCaret.row) { selCaret.row = 0; selCaret.ch = 0; }
	if (!flat.empty()) {
		selAnchor.ch = std::min(selAnchor.ch, (int)flat[selAnchor.row].size());
		selCaret.ch = std::min(selCaret.ch, (int)flat[selCaret.row].size());
	}
}

int VfBox::textLeft() const
{
	return sc(8);   // 客户区左上角起算，与 paint 的 textArea.left 一致
}

int VfBox::charOffset(HDC dc, const std::wstring& s, int ch) const
{
	if (ch <= 0 || !dc || s.empty()) return 0;
	if (ch > (int)s.size()) ch = (int)s.size();
	SIZE sz{};
	if (!GetTextExtentPoint32W(dc, s.c_str(), ch, &sz)) return 0;
	return sz.cx;
}

int VfBox::charX(const std::wstring& s, int ch) const
{
	if (!wnd) return textLeft();
	HDC dc = GetDC(wnd);
	if (!dc) return textLeft();
	HGDIOBJ old = fontLine ? SelectObject(dc, fontLine) : nullptr;
	const int x = textLeft() + charOffset(dc, s, ch);
	if (old) SelectObject(dc, old);
	ReleaseDC(wnd, dc);
	return x;
}

std::vector<int> VfBox::rowTops() const
{
	std::vector<int> tops;
	if (!wnd || flat.empty()) return tops;
	const int lh = lineHeight();
	tops.reserve(flat.size());
	// 起点和累加方式必须和 paint 完全一样：正文从 textArea.top（rc.top + sc(8)）开始，
	// 滚动只是整体平移，记录之间那条分隔线占一行高但不占行号。
	// 这里曾经写 sc(6)，比 paint 的 sc(8) 高 2 像素，点在某行顶端两像素内会选中上一行
	int y = sc(8) - scrollY;
	for (size_t r = 0; r < records.size(); r++) {
		for (size_t i = 0; i < records[r].lines.size(); i++) {
			tops.push_back(y);
			y += lh;
		}
		if (r + 1 < records.size()) y += lh;
	}
	return tops;
}

void VfBox::hitTest(int x, int y, SelPos& out) const
{
	out.row = 0;
	out.ch = 0;
	if (flat.empty() || !wnd) return;
	const std::vector<int> tops = rowTops();
	if (tops.size() != flat.size()) return;
	const int lh = lineHeight();

	int row = 0;
	if (y < tops.front()) {
		row = 0;
	} else if (y >= tops.back() + lh) {
		row = (int)flat.size() - 1;
	} else {
		// 落在记录之间那条分隔线的空档里时仍算上一行 —— 拖选跨记录时手感才连续
		for (int i = 0; i < (int)tops.size(); i++) {
			if (y >= tops[i]) row = i;
			if (y >= tops[i] && y < tops[i] + lh) break;
		}
	}
	out.row = row;

	// 找离鼠标最近的字符边界；量宽度用正文字体，否则高 DPI 下会偏好几个字符
	const std::wstring& s = flat[row];
	HDC dc = GetDC(wnd);
	if (!dc) return;
	HGDIOBJ old = fontLine ? SelectObject(dc, fontLine) : nullptr;
	int best = 0;
	int bestD = INT_MAX;
	for (int c = 0; c <= (int)s.size(); c++) {
		const int cx = textLeft() + charOffset(dc, s, c);
		const int d = cx > x ? cx - x : x - cx;
		if (d < bestD) { bestD = d; best = c; }
	}
	if (old) SelectObject(dc, old);
	ReleaseDC(wnd, dc);
	out.ch = best;
}

void VfBox::orderedSel(SelPos& a, SelPos& b) const
{
	if (b.row < a.row || (b.row == a.row && b.ch < a.ch)) {
		SelPos t = a;
		a = b;
		b = t;
	}
}

bool VfBox::hasSelection() const
{
	if (flat.empty()) return false;
	if (selAnchor.row < 0 || selAnchor.row >= (int)flat.size()) return false;
	if (selCaret.row < 0 || selCaret.row >= (int)flat.size()) return false;
	return selAnchor.row != selCaret.row || selAnchor.ch != selCaret.ch;
}

void VfBox::clearSelection()
{
	selAnchor = SelPos{ 0, 0 };
	selCaret = SelPos{ 0, 0 };
	selecting = false;
	if (wnd) InvalidateRect(wnd, nullptr, FALSE);
}

void VfBox::selectAll()
{
	if (flat.empty()) {
		clearSelection();
		return;
	}
	selAnchor = SelPos{ 0, 0 };
	selCaret = SelPos{ (int)flat.size() - 1, (int)flat.back().size() };
	if (wnd) InvalidateRect(wnd, nullptr, FALSE);
}

std::wstring VfBox::selectedText() const
{
	if (!hasSelection()) return std::wstring();
	SelPos a = selAnchor;
	SelPos b = selCaret;
	orderedSel(a, b);
	std::wstring out;
	for (int r = a.row; r <= b.row; r++) {
		const std::wstring& s = flat[r];
		int from = (r == a.row) ? a.ch : 0;
		int to = (r == b.row) ? b.ch : (int)s.size();
		if (from < 0) from = 0;
		if (to > (int)s.size()) to = (int)s.size();
		if (to > from) out.append(s, from, to - from);
		if (r < b.row) out += L"\r\n";
	}
	return out;
}

void VfBox::copySelection()
{
	const std::wstring text = selectedText();
	if (text.empty() || !wnd) return;
	if (!OpenClipboard(wnd)) return;
	EmptyClipboard();
	const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
	if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
		void* p = GlobalLock(mem);
		if (p) {
			memcpy(p, text.c_str(), bytes);
			GlobalUnlock(mem);
			if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
		} else {
			GlobalFree(mem);
		}
	}
	CloseClipboard();
}

int VfBox::lineHeight() const
{
	int h = sc(19);
	if (fontLine) {
		HDC dc = GetDC(wnd);
		if (dc) {
			HGDIOBJ old = SelectObject(dc, fontLine);
			TEXTMETRICW tm{};
			if (GetTextMetricsW(dc, &tm)) {
				// 行距 = 字身高度 + 1：多给一点，上一行的下缘不会压到下一行；
				// 给太多两条记录就装不进框里了（271 高的框 ≈ 13 行）
				h = tm.tmHeight + sc(1);
			}
			SelectObject(dc, old);
			ReleaseDC(wnd, dc);
		}
	}
	return h > sc(12) ? h : sc(19);
}

int VfBox::viewHeight() const
{
	RECT rc{};
	if (!wnd || !GetClientRect(wnd, &rc)) return 0;
	return rc.bottom - rc.top - sc(12);   // 上下各留 6
}

int VfBox::contentHeight() const
{
	const int lh = lineHeight();
	int h = sc(6);
	for (size_t r = 0; r < records.size(); r++) {
		h += (int)records[r].lines.size() * lh;
		if (r + 1 < records.size()) h += lh;   // 记录之间的分隔线占一行高
	}
	h += sc(6);
	return h;
}

int VfBox::textRight() const
{
	RECT rc{};
	if (!wnd || !GetClientRect(wnd, &rc)) return 0;
	// 结论边框最粗 5px，文字内缩也跟着放到 10，免得首尾字符压在色边上
	int right = rc.right - sc(10);
	// 结论只染外框，框里一整条宽度都归正文
	// 滚动条是系统画的，占掉右侧一条
	if (GetWindowLongPtrW(wnd, GWL_STYLE) & WS_VSCROLL) {
		if (contentHeight() > viewHeight()) right -= GetSystemMetrics(SM_CXVSCROLL) - sc(2);
	}
	return right;
}

void VfBox::clampScroll()
{
	const int maxScroll = std::max(0, contentHeight() - viewHeight());
	if (scrollY > maxScroll) scrollY = maxScroll;
	if (scrollY < 0) scrollY = 0;
}

void VfBox::syncScrollBar(bool followBottom)
{
	if (!wnd) return;
	const int content = contentHeight();
	const int view = viewHeight();
	const bool needBar = content > view;
	if (followBottom) scrollY = std::max(0, content - view);
	clampScroll();

	SCROLLINFO si{};
	si.cbSize = sizeof(si);
	si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	si.nMin = 0;
	si.nMax = std::max(0, content - 1);
	si.nPage = (UINT)std::max(0, view);
	si.nPos = scrollY;
	SetScrollInfo(wnd, SB_VERT, &si, TRUE);

	// 不需要滚动条时把 WS_VSCROLL 摘掉：否则内容不满一屏也挂着一条灰槽
	const LONG_PTR style = GetWindowLongPtrW(wnd, GWL_STYLE);
	if (needBar && !(style & WS_VSCROLL)) {
		SetWindowLongPtrW(wnd, GWL_STYLE, style | WS_VSCROLL);
		SetWindowPos(wnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	}
	else if (!needBar && (style & WS_VSCROLL)) {
		SetWindowLongPtrW(wnd, GWL_STYLE, style & ~WS_VSCROLL);
		SetWindowPos(wnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	}
	lastContentH = content;
}

void VfBox::paint(HDC dc)
{
	RECT rc{};
	GetClientRect(wnd, &rc);

	// 双缓冲：一次流式刷新只重绘这一块，直接怼窗口 DC 会闪
	HDC mem = CreateCompatibleDC(dc);
	HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
	HGDIOBJ oldBmp = nullptr;
	HDC target = dc;
	if (mem && bmp) {
		oldBmp = SelectObject(mem, bmp);
		target = mem;
	}

	// ——— 底色 + 边框 ———
	HBRUSH white = CreateSolidBrush(C_WHITE);
	FillRect(target, &rc, white);
	DeleteObject(white);
	// 结论不外露文字，整圈外框染色就是"隐藏式提醒"：
	// 相同 = 绿框、不同 = 红框，比原来那一整条竖栏省地方，正文也不用让宽度。
	// 中性态仍是原来那条 1px 浅灰；有结论时加粗到 5px —— 1px 彩线在小窗口里
	// 容易被白底吃掉，5px 不用凑近看就能判断；正文再往里缩一点也还有富余
	const bool hasVerdictColor = verdictFrame != C_BORDER;
	const int frameW = sc(hasVerdictColor ? 5 : 1);
	{
		HBRUSH fb = CreateSolidBrush(hasVerdictColor ? verdictFrame : C_BORDER);
		const RECT strips[4] = {
			{ rc.left, rc.top, rc.right, rc.top + frameW },
			{ rc.left, rc.bottom - frameW, rc.right, rc.bottom },
			{ rc.left, rc.top, rc.left + frameW, rc.bottom },
			{ rc.right - frameW, rc.top, rc.right, rc.bottom },
		};
		for (const RECT& s : strips) FillRect(target, &s, fb);
		DeleteObject(fb);
	}

	// ——— 正文：整块裁剪到文字区，滚动只改起始 y ———
	const int lh = lineHeight();
	const int right = textRight();
	RECT textArea{ rc.left + sc(10), rc.top + sc(8), right, rc.bottom - sc(8) };
	if (textArea.right <= textArea.left) {
		// 窗口太窄（布局还没跑）时不要硬画
		if (mem && bmp) { BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY); SelectObject(mem, oldBmp); }
		if (bmp) DeleteObject(bmp);
		if (mem) DeleteDC(mem);
		return;
	}

	int saved = SaveDC(target);
	IntersectClipRect(target, textArea.left, textArea.top, textArea.right, textArea.bottom);
	SetBkMode(target, TRANSPARENT);
	SetTextColor(target, C_TEXT);
	HGDIOBJ oldFont = SelectObject(target, fontLine);

	int y = textArea.top - scrollY;
	// 选区端点先排好序，逐行判断本行落在选区的哪一段。
	// 底色必须画在字形之前 —— DrawText 用 TRANSPARENT 背景，先画字就被盖住了
	SelPos s0 = selAnchor;
	SelPos s1 = selCaret;
	orderedSel(s0, s1);
	const bool showSel = hasSelection();
	int flatRow = 0;

	for (size_t r = 0; r < records.size(); r++) {
		for (const auto& line : records[r].lines) {
			if (y + lh >= textArea.top && y <= textArea.bottom) {
				// 一行放不下就先降一档字号：SHA256 那行是 "SHA256: " + 64 位十六进制，
				// 共 72 个字符，窗口一窄（或 DPI 一变）15 号字就顶格了。
				// 行高仍按统一值排，只是这行的字形小一号，整行宽度必然收回来；
				// 降档还放不下才交给 DrawText 省略，不会出现半截值
				const int avail = right - textArea.left;
				SelectObject(target, fontLine);
				if (fontLineSmall && charOffset(target, line, (int)line.size()) > avail) {
					SelectObject(target, fontLineSmall);
				}
				if (showSel && flatRow >= s0.row && flatRow <= s1.row) {
					int a = (flatRow == s0.row) ? s0.ch : 0;
					int b = (flatRow == s1.row) ? s1.ch : (int)line.size();
					if (a < 0) a = 0;
					if (b > (int)line.size()) b = (int)line.size();
					if (b > a) {
						// 此刻 target 上选着这一行实际用的字体，量出来的和字形宽度一致
						const int x0 = textArea.left + charOffset(target, line, a);
						const int x1 = textArea.left + charOffset(target, line, b);
						RECT hr{ x0, y, x1, y + lh };
						HBRUSH sb = CreateSolidBrush(C_SEL);
						FillRect(target, &hr, sb);
						DeleteObject(sb);
					}
				}
				RECT lr{ textArea.left, y, right, y + lh };
				DrawTextW(target, line.c_str(), -1, &lr,
					DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
				SelectObject(target, fontLine);
			}
			y += lh;
			flatRow++;
		}
		if (r + 1 < records.size()) {
			// 记录之间画一条细横线，而不是再打一串减号：
			// 减号在等宽字体里是实线，看着像结果的一部分
			if (y + lh >= textArea.top && y <= textArea.bottom) {
				HPEN pen = CreatePen(PS_SOLID, 1, C_SEP);
				HGDIOBJ op = SelectObject(target, pen);
				const int ly = y + lh / 2;
				MoveToEx(target, textArea.left, ly, nullptr);
				LineTo(target, textArea.right - sc(4), ly);
				SelectObject(target, op);
				DeleteObject(pen);
			}
			y += lh;
		}
	}
	SelectObject(target, oldFont);
	RestoreDC(target, saved);

	if (target == mem) {
		BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
		if (oldBmp) SelectObject(mem, oldBmp);
	}
	if (bmp) DeleteObject(bmp);
	if (mem) DeleteDC(mem);
}

LRESULT CALLBACK VfBox::procEntry(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_NCCREATE) {
		auto cs = reinterpret_cast<CREATESTRUCTW*>(lp);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
	}
	auto self = reinterpret_cast<VfBox*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
	if (!self->wnd) self->wnd = hwnd;
	return self->proc(msg, wp, lp);
}

LRESULT VfBox::proc(UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT: {
		PAINTSTRUCT ps{};
		HDC dc = BeginPaint(wnd, &ps);
		if (dc) {
			paint(dc);
			EndPaint(wnd, &ps);
		}
		return 0;
	}

	// 主窗口的 WM_PRINT（PrintWindow / 系统截图）会转成这一条：
	// 自绘控件不认 WM_PRINTCLIENT 的话，抓出来就是一块空白
	case WM_PRINTCLIENT:
		if (wp) paint(reinterpret_cast<HDC>(wp));
		return 0;

	case WM_SIZE:
		clampScroll();
		syncScrollBar(false);
		InvalidateRect(wnd, nullptr, FALSE);
		return 0;

	case WM_MOUSEWHEEL: {
		const int delta = GET_WHEEL_DELTA_WPARAM(wp);
		scrollY -= delta * lineHeight() * 3 / WHEEL_DELTA;
		clampScroll();
		syncScrollBar(false);
		InvalidateRect(wnd, nullptr, FALSE);
		return 0;
	}

	case WM_VSCROLL: {
		const int lh = lineHeight();
		const int maxScroll = std::max(0, contentHeight() - viewHeight());
		int pos = scrollY;
		switch (LOWORD(wp)) {
		case SB_LINEUP: pos -= lh; break;
		case SB_LINEDOWN: pos += lh; break;
		case SB_PAGEUP: pos -= viewHeight(); break;
		case SB_PAGEDOWN: pos += viewHeight(); break;
		case SB_TOP: pos = 0; break;
		case SB_BOTTOM: pos = maxScroll; break;
		case SB_THUMBTRACK:
		case SB_THUMBPOSITION: {
			SCROLLINFO si{};
			si.cbSize = sizeof(si);
			si.fMask = SIF_TRACKPOS;
			if (GetScrollInfo(wnd, SB_VERT, &si)) pos = si.nTrackPos;
			break;
		}
		default: return 0;
		}
		scrollY = pos < 0 ? 0 : (pos > maxScroll ? maxScroll : pos);
		syncScrollBar(false);
		InvalidateRect(wnd, nullptr, FALSE);
		return 0;
	}

	case WM_LBUTTONDOWN: {
		// 点一下把焦点拿过来，滚轮消息才会落到这里（父窗口的控件群里有 EDIT）
		SetFocus(wnd);
		SelPos p;
		hitTest((short)LOWORD(lp), (short)HIWORD(lp), p);
		// Shift + 点击从原锚点续选，不按 Shift 就在点击处另起一段
		const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
		if (!shift) selAnchor = p;
		selCaret = p;
		selecting = true;
		SetCapture(wnd);
		InvalidateRect(wnd, nullptr, FALSE);
		return 0;
	}

	case WM_MOUSEMOVE: {
		if (!selecting) return 0;
		const int mx = (short)LOWORD(lp);
		const int my = (short)HIWORD(lp);
		RECT rc{};
		GetClientRect(wnd, &rc);
		const int top = rc.top + sc(6);
		const int bottom = rc.bottom - sc(6);
		// 拖到框外继续自动滚：长结果一屏放不下，不滚就选不到屏幕外的行
		bool scrolled = false;
		if (my < top) { scrollY -= lineHeight(); scrolled = true; }
		else if (my > bottom) { scrollY += lineHeight(); scrolled = true; }
		if (scrolled) {
			clampScroll();
			syncScrollBar(false);
		}
		const int cy = my < top ? top : (my > bottom - 1 ? bottom - 1 : my);
		SelPos p;
		hitTest(mx, cy, p);
		selCaret = p;
		InvalidateRect(wnd, nullptr, FALSE);
		return 0;
	}

	case WM_LBUTTONUP:
		if (selecting) {
			selecting = false;
			ReleaseCapture();
			InvalidateRect(wnd, nullptr, FALSE);
		}
		return 0;

	// 捕获被系统拿走后不要再盯着鼠标，否则选区会跟着光标乱跑
	case WM_CAPTURECHANGED:
		if (selecting) {
			selecting = false;
			InvalidateRect(wnd, nullptr, FALSE);
		}
		return 0;

	case WM_SETCURSOR:
		if (LOWORD(lp) == HTCLIENT) {
			SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
			return TRUE;
		}
		break;

	case WM_KEYDOWN: {
		const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		if (ctrl && (wp == 'C' || wp == 'c')) {
			copySelection();
			return 0;
		}
		if (ctrl && (wp == 'V' || wp == 'v')) {
			if (onPaste) onPaste();
			return 0;
		}
		if (ctrl && (wp == 'A' || wp == 'a')) {
			selectAll();
			return 0;
		}
		if (wp == VK_ESCAPE) {
			clearSelection();
			return 0;
		}
		break;
	}

	case WM_PASTE:
		// 自绘控件不会自己产生 WM_PASTE，Ctrl+V 由上面的按键分支接；
		// 这里兜住右键菜单 / Shift+Insert 直接投过来的粘贴
		if (onPaste) onPaste();
		return 0;

	case WM_GETDLGCODE:
		// 不让方向键被对话框管理器吃掉（本窗口不是对话框，留着是为了以后复用）
		return DLGC_WANTARROWS | DLGC_WANTCHARS;

	case WM_NCDESTROY:
		// 摘掉 userdata 后直接返回，不能落到下面 DefWindowProcW(wnd,...)：
		// wnd 已经置空，再传空句柄下去是未定义行为
		SetWindowLongPtrW(wnd, GWLP_USERDATA, 0);
		wnd = nullptr;
		return 0;

	default:
		break;
	}
	return DefWindowProcW(wnd, msg, wp, lp);
}
