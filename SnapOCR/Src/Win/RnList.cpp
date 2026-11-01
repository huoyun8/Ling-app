#include "pch.h"
#include <algorithm>
#include <cwchar>
#include <iterator>
#include "../Lang.h"
#include "RnList.h"

namespace {
	const wchar_t* kClass = L"SnapCore_RnList";

	// 配色对齐 WinQr.cpp 的基础色：表头底色取 C_FILL_GR、悬停行取 C_BG_TOP、
	// 主色取 C_BTN_BLUE、分隔线取 C_BORDER —— 和其余页的白卡片、灰框、深蓝按钮同一套
	const COLORREF L_BG = RGB(255, 255, 255);            // = C_WHITE
	const COLORREF L_HEAD = RGB(228, 234, 242);          // = C_FILL_GR
	const COLORREF L_LINE = RGB(228, 234, 242);          // = C_FILL_GR
	const COLORREF L_TEXT = RGB(45, 57, 74);             // = C_TEXT
	const COLORREF L_MUTED = RGB(120, 132, 150);         // = C_TEXT_MUTED
	const COLORREF L_PRIMARY = RGB(3, 83, 182);          // = C_BTN_BLUE
	const COLORREF L_OK = RGB(21, 128, 61);              // = C_OK_GREEN
	const COLORREF L_BAD = RGB(220, 38, 38);             // = C_WARN_RED
	const COLORREF L_HOT = RGB(247, 249, 252);           // = C_BG_TOP
	const COLORREF L_BORDER = RGB(208, 216, 228);        // = C_BORDER

	// 六列：序号(含勾选) / 文件名 / 新文件名 / 扩展名 / 大小 / 状态
	// 合计 540 = 557 的列表宽减去竖滚动条那 17 —— 正好铺满，不多出一截被裁掉。
	// 「路径」那列已去掉：目录信息在"目录"那一栏选完就固定了，每行重复一遍只会
	// 把名字挤窄；让出来的宽度全给文件名和新文件名（新文件名要就地改，越宽越好改）。
	// 序号列宽度跟着行数走（见 seqColWidth）：1 位数 / 2 位数 / 3 位数三档，
	// 省下的那点宽度当场退给文件名和新文件名 —— 行的多少说话，列就多宽
	constexpr int kNCol = 6;
	constexpr int kTableW = 540;                               // 列表可用宽
	constexpr int kColExt = 46;                                // 扩展名：".jpeg" 这点字
	constexpr int kColSize = 52;                               // 大小："1.23 MB"
	constexpr int kColStatus = 46;                             // 状态："不存在"三个字
	constexpr int kColNameMax = 240;                           // 两列名字的宽上限（逻辑单位）
	const wchar_t* const kColKeys[kNCol] = {
		L"rn.colSeq", L"rn.colName", L"rn.colNew",
		L"rn.colExt", L"rn.colSize", L"rn.colStatus"
	};
	const wchar_t* const kStatKeys[] = {
		L"rn.stReady", L"rn.stSame", L"rn.stBad", L"rn.stConflict", L"rn.stMissing", L"rn.stDone", L"rn.stFail"
	};
	const COLORREF kStatColor[] = {
		L_MUTED, L_MUTED, L_BAD, L_BAD, L_BAD, L_OK, L_BAD
	};

	std::wstring extOf(const std::wstring& name)
	{
		const size_t dot = name.find_last_of(L'.');
		if (dot == std::wstring::npos || dot == 0) return {};
		std::wstring e = name.substr(dot + 1);
		CharLowerBuffW(e.data(), static_cast<DWORD>(e.size()));
		return e;
	}

	// 扩展名原样取：不改大小写 —— 列表里那一栏显示的是"这行当前是什么格式"，
	// 刚拉进来还没动过规则时，.PNG 就该还是 .PNG，不能自己变成 .png
	std::wstring extOfRaw(const std::wstring& name)
	{
		const size_t dot = name.find_last_of(L'.');
		if (dot == std::wstring::npos || dot == 0 || dot + 1 == name.size()) return {};
		return name.substr(dot + 1);
	}

	std::wstring sizeText(unsigned long long size)
	{
		wchar_t buf[64]{};
		if (size < 1024ULL) swprintf_s(buf, L"%llu B", size);
		else if (size < 1024ULL * 1024) swprintf_s(buf, L"%.1f KB", size / 1024.0);
		else if (size < 1024ULL * 1024 * 1024) swprintf_s(buf, L"%.2f MB", size / 1048576.0);
		else swprintf_s(buf, L"%.2f GB", size / 1073741824.0);
		return buf;
	}
}

const wchar_t* RnList::className()
{
	return kClass;
}

HWND RnList::create(HWND parent, int id, int dpiValue)
{
	dpi = dpiValue > 0 ? dpiValue : 96;
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	if (!GetClassInfoExW(GetModuleHandleW(nullptr), kClass, &wc)) {
		wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
		wc.lpfnWndProc = procEntry;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.lpszClassName = kClass;
		RegisterClassExW(&wc);
	}
	// WS_VSCROLL 常驻：行数够多时能滚，少的时候箭头是灰的 —— 列表宽度不变，
	// 不会因为"有时有滚动条有时没有"把列宽挤来挤去
	// WS_CLIPSIBLINGS 必须带：原地编辑框是主窗口的子窗口，靠 HWND_TOP 浮在本列表上面。
	// 双击先 selectRow（把行标成待重画）再弹编辑框，等消息循环派发 WM_PAINT 时，
	// 没有这个样式的话本列表会整行照画，正好把编辑框的全选高亮盖掉 ——
	// 看上去就是"闪一下就不选中了"。带上它，重画会绕开编辑框那一块
	wnd = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS,
		0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
		GetModuleHandleW(nullptr), nullptr);
	if (!wnd) return nullptr;
	SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
	return wnd;
}

void RnList::setDpi(int value)
{
	const int newDpi = value > 0 ? value : 96;
	if (newDpi != dpi) {
		// 手拖过的列宽是按旧 DPI 的像素记的：直接沿用会在新缩放下变得又窄又歪，
		// 按比例换算过去，用户调好的布局还能留着
		for (int c = 0; c < kNCol; c++) {
			if (userW[c] > 0) userW[c] = MulDiv(userW[c], newDpi, dpi);
		}
	}
	dpi = newDpi;
	clampTop();
	syncScrollBar();
	if (wnd) InvalidateRect(wnd, nullptr, TRUE);
}

void RnList::setFonts(HFONT normal, HFONT bold)
{
	fontNormal = normal;
	fontBold = bold;
}

void RnList::setSource(std::vector<RnRow>* value)
{
	rows = value;
	top = 0;
	refresh();
}

void RnList::setFilterList(const std::vector<std::wstring>& extsLower)
{
	filt = extsLower;
	top = 0;
	refresh();
}

void RnList::setKeyword(const std::wstring& lower)
{
	if (keyw == lower) return;
	keyw = lower;
	top = 0;
	refresh();
}

bool RnList::matched(const RnRow& row) const
{
	const std::wstring ext = extOf(row.name);
	if (!filt.empty()) {
		bool hit = false;
		for (const auto& e : filt) {
			if (e == ext) { hit = true; break; }
		}
		if (!hit) return false;
	}
	if (keyw.empty()) return true;
	// 手打关键字：扩展名或整个文件名里含它就算 —— 光打 "p" 就能把 png/jpg/pdf 一起筛出来
	if (ext.find(keyw) != std::wstring::npos) return true;
	std::wstring name = row.name;
	CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
	return name.find(keyw) != std::wstring::npos;
}

void RnList::refresh()
{
	// 行数变少（清空 / 过滤）时，选中那条可能已经不在了：越界就丢掉，
	// 否则后面每次都按一个不存在的下标去重画
	if (!rows || sel < 0 || sel >= static_cast<int>(rows->size())) sel = -1;
	clampTop();
	syncScrollBar();
	if (wnd) InvalidateRect(wnd, nullptr, TRUE);
}

void RnList::selectRow(int rowIndex)
{
	// 选了同一行就不再重画；换行时旧行也要刷掉
	if (rowIndex == sel) return;
	const int old = sel;
	sel = rowIndex;
	if (old >= 0) invalidateRow(old);
	if (sel >= 0) invalidateRow(sel);
}

void RnList::invalidateRow(int rowIndex)
{
	if (!wnd) return;
	RECT r{};
	if (!cellRect(rowIndex, 0, r)) return;
	RECT rc{};
	GetClientRect(wnd, &rc);
	RECT band{ 0, r.top, rc.right, r.bottom };
	InvalidateRect(wnd, &band, FALSE);
}

int RnList::visibleCount() const
{
	if (!rows) return 0;
	int n = 0;
	for (const auto& r : *rows) if (matched(r)) n++;
	return n;
}

int RnList::visibleIndex(int rowIndex) const
{
	if (!rows || rowIndex < 0 || rowIndex >= static_cast<int>(rows->size())) return -1;
	if (!matched((*rows)[rowIndex])) return -1;
	int k = 0;
	for (int i = 0; i < rowIndex; i++) {
		if (matched((*rows)[i])) k++;
	}
	return k;
}

bool RnList::cellRect(int rowIndex, int col, RECT& out) const
{
	if (col < 0 || col >= kNCol) return false;
	const int vi = visibleIndex(rowIndex);
	if (vi < 0) return false;
	const int slot = vi - top;
	if (slot < 0 || slot >= rowsPerPage()) return false;
	const int hd = headerHeight();
	const int rh = rowHeight();
	out = RECT{ colLeft(col), hd + slot * rh, colLeft(col) + colWidth(col), hd + (slot + 1) * rh };
	return true;
}

int RnList::seqColWidth() const
{
	// 行数只有个位数就 1 位数，两位数就 2 位，上百了才给 3 位（再往上也按 3 位算）
	const int n = rows ? static_cast<int>(rows->size()) : 0;
	const int digits = n >= 100 ? 3 : (n >= 10 ? 2 : 1);
	// 数字从第 22 起（勾选框占 6..20），右对齐，右边再留 4；一个数字约 7 宽
	return scPx(22) + scPx(7) * digits + scPx(4);
}

int RnList::colLeft(int col) const
{
	int x = 0;
	for (int i = 0; i < col && i < kNCol; i++) x += colWidth(i);
	return x;
}

int RnList::colMin(int col) const
{
	switch (col) {
	case 0: return std::max(scPx(22) + scPx(4), seqColWidth());  // 序号：永远装得下当前位数
	case 1:
	case 2: return scPx(50);
	case 3: return scPx(38);
	case 4: return scPx(44);
	default: return scPx(40);
	}
}

int RnList::colMax(int col) const
{
	// 两列名字给个上限：界面就这么宽，名字列拖到半屏以上，剩下的列全被压成一条缝，
	// 名字本身也变不成"能一眼看完"的样子 —— 过长的文件名照旧靠省略号收尾，
	// 想看全名双击进编辑框就是了
	switch (col) {
	case 1:
	case 2:
		return scPx(kColNameMax);
	default:
		return tableW > 0 ? tableW : scPx(kTableW);
	}
}

int RnList::colWidth(int col) const
{
	if (col < 0 || col >= kNCol) return 0;
	// 一直用"实际的客户区宽度"算列宽，不写死 540：列表里常驻竖滚动条，
	// 滚动条在 125% / 150% 缩放下不一定是 17 的整数倍 —— 写死的话最后一列
	// 和右框线之间会剩下一条空白，表头分隔线也会跟着偏，"拖动"看起来就不准
	const int total = tableW > 0 ? tableW : scPx(kTableW);
	const int minName = scPx(50);

	int w[kNCol] = {};
	w[0] = userW[0] > 0 ? std::max(userW[0], seqColWidth()) : seqColWidth();
	w[3] = userW[3] > 0 ? userW[3] : scPx(kColExt);
	w[4] = userW[4] > 0 ? userW[4] : scPx(kColSize);
	w[5] = userW[5] > 0 ? userW[5] : scPx(kColStatus);

	const int freeW = total - w[0] - w[3] - w[4] - w[5];
	const bool f1 = userW[1] > 0, f2 = userW[2] > 0;
	if (f1 && f2) {
		w[1] = userW[1];
		w[2] = userW[2];
	}
	else if (f1) {
		w[1] = userW[1];
		w[2] = std::max(minName, freeW - w[1]);
	}
	else if (f2) {
		w[2] = userW[2];
		w[1] = std::max(minName, freeW - w[2]);
	}
	else {
		const int half = freeW / 2;
		w[1] = half - scPx(6);
		w[2] = freeW - w[1];
	}

	// 账面校正：各列合计必须正好等于列表可用宽，否则右边缘会空一截、或者
	// 多出来的部分被裁掉。差额先记在新文件名那列头上（它本来就是"吃剩余"的），
	// 它压到最小还差，就依次往后借 —— 借的顺序就是"谁最能屈能伸"
	int sum = 0;
	for (int c = 0; c < kNCol; c++) sum += w[c];
	int delta = total - sum;
	for (int c : { 2, 1, 0, 3, 4, 5 }) {
		if (delta == 0) break;
		const int lo = colMin(c);
		const int hi = std::max(lo, colMax(c));
		const int want = std::clamp(w[c] + delta, lo, hi);
		delta += (w[c] - want);   // 差额记到下一列头上
		w[c] = want;
	}
	return w[col];
}

int RnList::rowsPerPage() const
{
	if (!wnd) return 1;
	RECT rc{};
	GetClientRect(wnd, &rc);
	const int body = rc.bottom - headerHeight();
	const int rh = rowHeight();
	if (body <= 0 || rh <= 0) return 1;
	return std::max(1, body / rh);
}

void RnList::clampTop()
{
	const int n = visibleCount();
	const int per = rowsPerPage();
	const int maxTop = std::max(0, n - per);
	if (top > maxTop) top = maxTop;
	if (top < 0) top = 0;
}

int RnList::dividerAt(POINT pt) const
{
	if (!wnd || pt.y >= headerHeight()) return -1;
	const int tol = scPx(3);
	for (int c = 0; c < kNCol - 1; c++) {
		const int x = colLeft(c) + colWidth(c);
		if (pt.x >= x - tol && pt.x <= x + tol) return c;
	}
	return -1;
}

void RnList::beginDrag(int div, int x)
{
	// 第一次拖：把当前这一套宽度整个"冻"下来。之后只改被拖那一对相邻列，
	// 两列之和不变 —— 各列合计还是正好铺满，别的列也不会跟着乱跳。
	// 只冻一对是不够的：没冻的那些列按"剩余宽度"重新算，拖这个边界会连带
	// 把远处的列也挤动，看起来就像"拉这里，那里跑了"
	if (userW[0] <= 0) {
		for (int c = 0; c < kNCol; c++) userW[c] = colWidth(c);
	}
	dragDiv = div;
	dragStartX = x;
	dragStartL = colWidth(div);
	dragStartR = colWidth(div + 1);
	if (onResizeColumns) onResizeColumns();
}

void RnList::dragTo(int x)
{
	if (dragDiv < 0) return;
	const int dx = x - dragStartX;
	const int pair = dragStartL + dragStartR;
	const int minL = colMin(dragDiv);
	const int minR = colMin(dragDiv + 1);
	// 两头都受"最小 / 最大宽"夹住：名字列拖不到上限以外，别的列也就不会被挤没
	const int maxL = std::min({ pair - minR, colMax(dragDiv) });
	const int left = std::clamp(dragStartL + dx, minL, std::max(minL, maxL));
	userW[dragDiv] = left;
	userW[dragDiv + 1] = std::max(minR, pair - left);
}

void RnList::resetDragCols(int div)
{
	// 双击分隔线：整表的列宽回到自动分配（序号列也重新跟着位数走）
	if (div < 0 || div >= kNCol - 1) return;
	for (int c = 0; c < kNCol; c++) userW[c] = 0;
}

void RnList::endDrag()
{
	dragDiv = -1;
}

void RnList::syncScrollBar()
{
	if (!wnd) return;
	const int n = visibleCount();
	SCROLLINFO si{};
	si.cbSize = sizeof(si);
	si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	si.nMin = 0;
	si.nMax = std::max(0, n - 1);
	si.nPage = rowsPerPage();
	si.nPos = top;
	SetScrollInfo(wnd, SB_VERT, &si, TRUE);
}

int RnList::hitTest(POINT pt, int* colOut) const
{
	if (colOut) *colOut = -1;
	if (!rows) return -1;
	if (pt.y < headerHeight()) return -1;
	const int rh = rowHeight();
	if (rh <= 0) return -1;
	const int slot = (pt.y - headerHeight()) / rh;
	if (slot < 0) return -1;
	const int visIdx = top + slot;
	int col = -1;
	for (int c = 0; c < kNCol; c++) {
		if (pt.x >= colLeft(c) && pt.x < colLeft(c) + colWidth(c)) { col = c; break; }
	}
	if (colOut) *colOut = col;
	int k = 0;
	for (int i = 0; i < static_cast<int>(rows->size()); i++) {
		if (!matched((*rows)[i])) continue;
		if (k == visIdx) return i;
		k++;
	}
	return -1;
}

void RnList::onSize()
{
	if (!wnd) return;
	RECT rc{};
	GetClientRect(wnd, &rc);
	tableW = rc.right;
}

LRESULT CALLBACK RnList::procEntry(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	auto self = reinterpret_cast<RnList*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
	return self->proc(msg, wp, lp);
}

LRESULT RnList::proc(UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_PAINT:
		paint();
		return 0;

	case WM_ERASEBKGND:
		return 1;

	case WM_SIZE:
		onSize();
		clampTop();
		syncScrollBar();
		InvalidateRect(wnd, nullptr, TRUE);
		return 0;

	case WM_DROPFILES:
		// 列表自己不接拖放（不调 DragAcceptFiles）：系统会顺着父链找那个开着
		// WS_EX_ACCEPTFILES 的窗口，也就是主窗口，由它统一处理。
		// 早先在这里也开了接受、再把 WM_DROPFILES 转发给父窗口 —— HDROP 被
		// 父窗口 DragFinish 之后，系统在消息返回时又释放一次，拖文件/文件夹进来
		// 就是双重释放，进程直接崩。这条现在只是兜底：真落到列表上就还给系统
		return DefWindowProcW(wnd, msg, wp, lp);

	case WM_VSCROLL: {
		SCROLLINFO si{};
		si.cbSize = sizeof(si);
		si.fMask = SIF_ALL;
		GetScrollInfo(wnd, SB_VERT, &si);
		int t = top;
		switch (LOWORD(wp)) {
		case SB_LINEUP: t -= 1; break;
		case SB_LINEDOWN: t += 1; break;
		case SB_PAGEUP: t -= rowsPerPage(); break;
		case SB_PAGEDOWN: t += rowsPerPage(); break;
		case SB_THUMBPOSITION:
		case SB_THUMBTRACK: t = si.nTrackPos; break;
		case SB_TOP: t = 0; break;
		case SB_BOTTOM: t = si.nMax; break;
		default: return 0;
		}
		clampTop();
		if (t == top) { syncScrollBar(); return 0; }
		top = std::max(0, t);
		clampTop();
		SetScrollPos(wnd, SB_VERT, top, TRUE);
		if (onScroll) onScroll();
		InvalidateRect(wnd, nullptr, TRUE);
		return 0;
	}

	case WM_MOUSEWHEEL: {
		const int delta = GET_WHEEL_DELTA_WPARAM(wp);
		if (delta == 0) return 0;
		const int before = top;
		// 一格滚 3 行：列表只有六七行高，滚 1 行太黏、滚一页又跳得太远
		top += (delta > 0 ? -3 : 3);
		clampTop();
		if (top == before) return 0;
		SetScrollPos(wnd, SB_VERT, top, TRUE);
		if (onScroll) onScroll();
		InvalidateRect(wnd, nullptr, TRUE);
		return 0;
	}

	case WM_SETCURSOR:
		// 悬停在表头分隔线上就换成左右箭头，不然用户根本不知道这儿能拖
		if (LOWORD(lp) == HTCLIENT) {
			POINT pt{};
			GetCursorPos(&pt);
			ScreenToClient(wnd, &pt);
			if (dragDiv >= 0 || dividerAt(pt) >= 0) {
				SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
				return TRUE;
			}
		}
		break;

	case WM_MOUSEMOVE: {
		POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
		if (dragDiv >= 0) {
			dragTo(pt.x);
			InvalidateRect(wnd, nullptr, TRUE);
			return 0;
		}
		const int div = dividerAt(pt);
		if (div != hoverDiv) {
			hoverDiv = div;
			RECT hr{};
			GetClientRect(wnd, &hr);
			hr.bottom = headerHeight();
			InvalidateRect(wnd, &hr, FALSE);
		}
		int col = -1;
		const int idx = hitTest(pt, &col);
		if (idx != hot) {
			const int old = hot;
			hot = idx;
			if (old >= 0) invalidateRow(old);
			if (idx >= 0) invalidateRow(idx);
		}
		if (!tracking) {
			TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, wnd, 0 };
			TrackMouseEvent(&tme);
			tracking = true;
		}
		return 0;
	}

	case WM_LBUTTONUP:
		if (dragDiv >= 0) {
			ReleaseCapture();
			endDrag();
			InvalidateRect(wnd, nullptr, TRUE);
			return 0;
		}
		break;

	case WM_CAPTURECHANGED:
		// 拖到一半被别处抢了捕获（弹出菜单之类）：收尾，别让列宽跟着乱跑
		if (dragDiv >= 0) {
			endDrag();
			InvalidateRect(wnd, nullptr, TRUE);
		}
		return 0;

	case WM_MOUSELEAVE: {
		tracking = false;
		hoverDiv = -1;
		if (hot >= 0) {
			const int old = hot;
			hot = -1;
			invalidateRow(old);
		}
		RECT hr{};
		GetClientRect(wnd, &hr);
		hr.bottom = headerHeight();
		InvalidateRect(wnd, &hr, FALSE);
		return 0;
	}

	case WM_LBUTTONDOWN: {
		SetFocus(wnd);
		POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
		// 落在表头分隔线上：开始拖列宽，不算点行
		const int div = dividerAt(pt);
		if (div >= 0) {
			SetCapture(wnd);
			hoverDiv = div;
			beginDrag(div, pt.x);
			InvalidateRect(wnd, nullptr, TRUE);
			return 0;
		}
		int col = -1;
		const int idx = hitTest(pt, &col);
		if (idx >= 0 && col >= 0) {
			selectRow(idx);   // 点哪儿都先把整行选中
			// 序号列里只有勾选框本身该翻转；点在行号上只是想选这一行，
			// 顺手把勾选也改了会让人觉得"点一下就少选了一个文件"
			const bool onCheck = col != 0 || pt.x < colLeft(0) + scPx(22);
			if (onCheck && onClick) onClick(idx, col);
		}
		return 0;
	}

	case WM_LBUTTONDBLCLK: {
		POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
		// 双击表头分隔线 = 这两列回到自动分配，等于"撤销这次调宽"
		const int div = dividerAt(pt);
		if (div >= 0) {
			resetDragCols(div);
			if (onResizeColumns) onResizeColumns();
			InvalidateRect(wnd, nullptr, TRUE);
			return 0;
		}
		int col = -1;
		const int idx = hitTest(pt, &col);
		if (idx >= 0 && col >= 0) {
			// 双击的第二下（系统合成的那种）也要落到"选中这一行"上，
			// 否则快点的两次里第一下选中、第二下进编辑，视觉上却像没选中
			selectRow(idx);
			if (onDblClick) onDblClick(idx, col);
		}
		return 0;
	}

	default:
		break;
	}
	return DefWindowProcW(wnd, msg, wp, lp);
}

void RnList::paint()
{
	PAINTSTRUCT ps{};
	HDC dc = BeginPaint(wnd, &ps);
	if (!dc) return;

	RECT rc{};
	GetClientRect(wnd, &rc);
	const int w = rc.right;
	const int h = rc.bottom;
	if (w <= 0 || h <= 0) {
		EndPaint(wnd, &ps);
		return;
	}
	tableW = w;   // 每帧都对一遍：列宽全从它算，右边缘才不会留缝

	// 双缓冲：表格每帧要刷几十个格子，直接画在窗口 DC 上会闪
	HDC mem = CreateCompatibleDC(dc);
	HBITMAP bmp = mem ? CreateCompatibleBitmap(dc, w, h) : nullptr;
	HGDIOBJ oldBmp = bmp ? SelectObject(mem, bmp) : nullptr;

	HDC out = mem ? mem : dc;
	auto fill = [out](const RECT& r, COLORREF c) {
		HBRUSH b = CreateSolidBrush(c);
		FillRect(out, &r, b);
		DeleteObject(b);
	};
	auto line = [out](int x0, int y0, int x1, int y1, COLORREF c) {
		HPEN pen = CreatePen(PS_SOLID, 1, c);
		HGDIOBJ old = SelectObject(out, pen);
		MoveToEx(out, x0, y0, nullptr);
		LineTo(out, x1, y1);
		SelectObject(out, old);
		DeleteObject(pen);
	};

	RECT all{ 0, 0, w, h };
	fill(all, L_BG);

	const int hd = headerHeight();
	RECT head{ 0, 0, w, hd };
	fill(head, L_HEAD);

	SetBkMode(out, TRANSPARENT);
	SetTextColor(out, L_MUTED);
	HGDIOBJ oldFont = SelectObject(out, fontBold ? fontBold : fontNormal);
	for (int c = 0; c < kNCol; c++) {
		const std::wstring title = Lang::get(kColKeys[c]);
		RECT cr{ colLeft(c) + scPx(5), 0, colLeft(c) + colWidth(c) - scPx(4), hd };
		if (c > 0) line(colLeft(c), scPx(5), colLeft(c), hd - scPx(5), L_LINE);
		DrawTextW(out, title.c_str(), -1, &cr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}
	// 表头分隔线可拖：悬停 / 正在拖的那条画一道亮线，告诉用户"就这儿"
	const int showDiv = (dragDiv >= 0) ? dragDiv : hoverDiv;
	if (showDiv >= 0 && showDiv < kNCol - 1) {
		const int x = colLeft(showDiv) + colWidth(showDiv);
		RECT hl{ x - scPx(1), scPx(2), x + scPx(2), hd - scPx(2) };
		HBRUSH hb = CreateSolidBrush(L_PRIMARY);
		FillRect(out, &hl, hb);
		DeleteObject(hb);
	}
	line(0, hd - 1, w, hd - 1, L_BORDER);

	const int n = visibleCount();
	if (n == 0) {
		const std::wstring tip = Lang::get(L"rn.empty");
		RECT tr{ 0, hd, w, h };
		SetTextColor(out, L_MUTED);
		SelectObject(out, fontNormal);
		DrawTextW(out, tip.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}
	else {
		const int rh = rowHeight();
		const int per = rowsPerPage();
		int drawn = 0;
		int k = 0;
		for (int i = 0; i < static_cast<int>(rows->size()) && drawn < per; i++) {
			if (!matched((*rows)[i])) continue;
			if (k < top) { k++; continue; }
			RECT rr{ 0, hd + drawn * rh, w, hd + (drawn + 1) * rh };
			drawRow(out, rr, i, k + 1);
			line(0, rr.bottom - 1, w, rr.bottom - 1, L_LINE);
			drawn++;
			k++;
		}
	}

	// 外框：列表自己一条 1px 边，跟页面上的输入框、按钮一个调子
	line(0, 0, w - 1, 0, L_BORDER);
	line(0, 0, 0, h - 1, L_BORDER);
	line(w - 1, 0, w - 1, h - 1, L_BORDER);
	line(0, h - 1, w - 1, h - 1, L_BORDER);

	if (mem) {
		BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
		SelectObject(mem, oldBmp);
		DeleteObject(bmp);
		DeleteDC(mem);
	}
	SelectObject(out, oldFont);
	EndPaint(wnd, &ps);
}

void RnList::drawRow(HDC dc, const RECT& rr, int rowIndex, int displayIndex)
{
	const RnRow& r = (*rows)[rowIndex];
	SetBkMode(dc, TRANSPARENT);

	if (rowIndex == sel) {
		// 选中行：整行淡蓝底，比悬停那层更实一点，一眼能看出"这行被选中了"
		HBRUSH b = CreateSolidBrush(RGB(226, 236, 253));
		FillRect(dc, &rr, b);
		DeleteObject(b);
	}
	else if (rowIndex == hot) {
		HBRUSH b = CreateSolidBrush(L_HOT);
		FillRect(dc, &rr, b);
		DeleteObject(b);
	}

	// —— 序号列：勾选框 + 行号 ——
	const int box = scPx(14);
	const int bx = colLeft(0) + scPx(6);
	const int by = rr.top + ((rr.bottom - rr.top) - box) / 2;
	RECT cb{ bx, by, bx + box, by + box };
	HBRUSH bFill = CreateSolidBrush(r.checked ? L_PRIMARY : L_BG);
	FillRect(dc, &cb, bFill);
	DeleteObject(bFill);
	FrameRect(dc, &cb, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
	if (r.checked) {
		HPEN pen = CreatePen(PS_SOLID, scPx(2), L_BG);
		HGDIOBJ oldPen = SelectObject(dc, pen);
		const POINT pts[3] = {
			{ cb.left + box * 3 / 10, by + box * 52 / 100 },
			{ cb.left + box * 44 / 100, by + box * 74 / 100 },
			{ cb.left + box * 76 / 100, by + box * 28 / 100 },
		};
		Polyline(dc, pts, 3);
		SelectObject(dc, oldPen);
		DeleteObject(pen);
	}

	auto cellText = [&](int col, int left, const std::wstring& text, COLORREF color, UINT flags) {
		RECT cr{ left, rr.top, colLeft(col) + colWidth(col) - scPx(4), rr.bottom };
		SetTextColor(dc, color);
		DrawTextW(dc, text.c_str(), -1, &cr,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | flags);
	};

	HGDIOBJ oldFont = SelectObject(dc, fontNormal);
	wchar_t num[16]{};
	swprintf_s(num, L"%d", displayIndex);
	cellText(0, colLeft(0) + scPx(22), num, L_MUTED, DT_RIGHT);

	// 文件名：直接贴左边起，前面不再挂格式角标 —— 扩展名本来就单列一栏，
	// 名称前再叠一个小方块既挤字又和「扩展名」列重复
	const std::wstring ext = extOfRaw(r.newName);
	cellText(1, colLeft(1) + scPx(5), r.name, L_TEXT, 0);

	// 新文件名：默认一律常规正文色；刚改过名的改红 —— 手工改过（manualName 有值）和执行成功（done）
	// 都算"这行是我刚动过的"，一眼能挑出来
	const COLORREF newColor = (r.done || !r.manualName.empty()) ? L_BAD : L_TEXT;
	cellText(2, colLeft(2) + scPx(5), r.newName, newColor, 0);

	// 扩展名带前导点，和参考图一致
	cellText(3, colLeft(3) + scPx(5), ext.empty() ? ext : L"." + ext, L_MUTED, 0);

	cellText(4, colLeft(4) + scPx(4), sizeText(r.size), L_MUTED, DT_RIGHT);

	// 状态列只说一件事：这行改名成了没有 —— 成功（绿）/ 失败（红）。
	// 就绪 / 同名 / 无效 / 冲突 / 不存在这些都不写，改名前留白：
	// 它们要么是常态（等于没信息），要么等执行完自然变成成功/失败
	const int st = (r.status >= 0 && r.status < static_cast<int>(std::size(kStatKeys))) ? r.status : RnReady;
	if (st == RnDone || st == RnFail) {
		cellText(5, colLeft(5) + scPx(5), Lang::get(kStatKeys[st]), kStatColor[st], 0);
	}

	SelectObject(dc, oldFont);
}
