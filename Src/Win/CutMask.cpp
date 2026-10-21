#include "pch.h"
#include <dwmapi.h>
#include <cmath>
#include <cfloat>
#include <include/Ling.h>
#include "CutMask.h"
using namespace Microsoft::WRL;

CutMask::CutMask(Ling::WinBase* win) :win{ win }
{
	strokeWidth = 2 * win->dpi;
	paddingMargin *= win->dpi;
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushText.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.46f), brushBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x1677ff), brushBorder.GetAddressOf());
	initWinRect();
}

bool CutMask::highlight(POINT pos)
{
	// 右键进入手动十字模式后临时关闭悬停吸附，直到用户手动拖出选区
	if (!autoSnap) return false;
	// 智能识别：在识别表里找"包含光标的最小矩形"（越深层的子控件面积越小，越具体），
	// 于是光标悬停在按钮上就吸到按钮、悬停在面板上就吸到面板、悬停在窗口上就吸到窗口
	float bestArea{ FLT_MAX };
	D2D1_RECT_F target{ 0.f,0.f,0.f,0.f };
	for (auto& rect : winRect)
	{
		if (pos.x > rect.left && pos.y > rect.top && pos.x < rect.right && pos.y < rect.bottom) {
			float a = (rect.right - rect.left) * (rect.bottom - rect.top);
			if (a < bestArea) {
				bestArea = a;
				target = rect;
			}
		}
	}
	if (bestArea == FLT_MAX) return false;
	if (maskRect.left != target.left || maskRect.top != target.top ||
		maskRect.right != target.right || maskRect.bottom != target.bottom) {
		maskRect = target;
		makeLayout();
		win->refresh();
		return true;
	}
	return false;
}

void CutMask::initWinRect()
{
	winRect.clear();
	// 只认外部窗口：本程序自己的截图窗 / 工具条 / 贴屏窗都是置顶全屏或悬浮，不参与识别，
	// 否则光标一动就会先被"自己的全屏矩形"匹配到，智能识别永远吸不到桌面上别的窗口
	EnumWindows([](HWND hwnd, LPARAM lparam)
		{
			if (!hwnd) return TRUE;
			if (!IsWindowVisible(hwnd)) return TRUE;
			DWORD wpid = 0;
			GetWindowThreadProcessId(hwnd, &wpid);
			if (wpid == GetCurrentProcessId()) return TRUE;
			BOOL cloaked = FALSE;
			DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
			if (cloaked) return TRUE;
			RECT rect;
			DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(RECT));
			if (rect.right - rect.left <= 6 || rect.bottom - rect.top <= 6) return TRUE;
			auto self = (CutMask*)lparam;
			self->pushClippedRect(rect);          // 顶层窗口本身
			EnumChildWindows(hwnd, collectChild, lparam);  // 再收集它里面的子控件
			return TRUE;
		}, (LPARAM)this);
}

void CutMask::pushClippedRect(const RECT& r)
{
	auto win = this->win;
	int hostL{ (int)win->x }, hostT{ (int)win->y };
	int hostR{ (int)(win->x + win->w) }, hostB{ (int)(win->y + win->h) };
	if (r.right <= hostL || r.bottom <= hostT || r.left >= hostR || r.top >= hostB) return;
	RECT c = r;
	if (c.left < hostL) c.left = hostL;
	if (c.top < hostT) c.top = hostT;
	if (c.right > hostR) c.right = hostR;
	if (c.bottom > hostB) c.bottom = hostB;
	if (c.right - c.left <= 6 || c.bottom - c.top <= 6) return;
	winRect.push_back(D2D1::RectF(
		(float)(c.left - win->x), (float)(c.top - win->y),
		(float)(c.right - win->x), (float)(c.bottom - win->y)));
}

BOOL CALLBACK CutMask::collectChild(HWND hwnd, LPARAM lp)
{
	auto self = (CutMask*)lp;
	if (hwnd && IsWindowVisible(hwnd)) {
		RECT r;
		if (GetWindowRect(hwnd, &r) && r.right - r.left > 6 && r.bottom - r.top > 6) {
			self->pushClippedRect(r);
		}
		EnumChildWindows(hwnd, collectChild, lp);  // 递归到更深层的嵌套控件
	}
	return TRUE;
}

float CutMask::snapCoord(float v, float threshold) const
{
	float best = v, bestDist = threshold;
	for (auto& r : winRect) {
		// 每条竖边界都能被吸附，横坐标 v 就近取一条
		if (std::fabs(r.left - v) <= bestDist) { bestDist = std::fabs(r.left - v); best = r.left; }
		if (std::fabs(r.right - v) <= bestDist) { bestDist = std::fabs(r.right - v); best = r.right; }
	}
	return best;
}

void CutMask::makeLayout()
{
	layout.Reset();
	auto layoutStr = std::format(L"X:{} Y:{} R:{} B:{} W:{} H:{}",
		maskRect.left, maskRect.top, maskRect.right, maskRect.bottom,
		maskRect.right - maskRect.left, maskRect.bottom - maskRect.top);
	layout = Ling::D2D::get()->makeTextLayout(layoutStr, 12 * win->dpi);
	if (!layout) return;
	DWRITE_TEXT_METRICS tm = {};
	layout->GetMetrics(&tm);
	layoutRect = D2D1::RectF(maskRect.left, maskRect.top - paddingMargin - tm.height - paddingMargin * 2, maskRect.left + tm.width + paddingMargin * 2, maskRect.top - paddingMargin);
	// 标签被顶出窗口上边时折回 maskRect 内部
	if (layoutRect.top < 0) {
		auto h = layoutRect.bottom - layoutRect.top;
		auto w = layoutRect.right - layoutRect.left;
		layoutRect.top = maskRect.top + paddingMargin / 2;
		layoutRect.bottom = layoutRect.top + h;
		layoutRect.left = maskRect.left + paddingMargin;
		layoutRect.right = layoutRect.left + w;
	}
	layout->SetMaxWidth(layoutRect.right - layoutRect.left);
	layout->SetMaxHeight(layoutRect.bottom - layoutRect.top);
}

void CutMask::startMakeRect(POINT pos)
{
	pressPos = pos;
}

void CutMask::makeRect(POINT pos)
{
	// 用普通 LONG 局部变量，不要 structured binding：std::minmax 绑定出来的是 const 引用不能赋值
	LONG left = std::min(pressPos.x, pos.x);
	LONG right = std::max(pressPos.x, pos.x);
	LONG top = std::min(pressPos.y, pos.y);
	LONG bottom = std::max(pressPos.y, pos.y);
	// 拖动框选时的智能吸附：每条边就近吸附到识别表里的窗口 / 控件边界（阈值内），
	// 这样拉出来的选区会自然贴齐窗口或按钮的边。autoSnap 关掉时（右键重进选区）纯手动，
	// 拉出来的是光标划过的那块矩形，不会被窗口边界拽走
	if (autoSnap) {
		const float th = snapThreshold * win->dpi;
		left = (LONG)snapCoord((float)left, th);
		right = (LONG)snapCoord((float)right, th);
		top = (LONG)snapCoord((float)top, th);
		bottom = (LONG)snapCoord((float)bottom, th);
	}
	if (right - left < (LONG)minSize) right = left + (LONG)minSize;
	if (bottom - top < (LONG)minSize) bottom = top + (LONG)minSize;
	maskRect.left = (float)left;
	maskRect.right = (float)right;
	maskRect.top = (float)top;
	maskRect.bottom = (float)bottom;
	makeLayout();
	win->refresh();
}

bool CutMask::hasRect() const
{
	return maskRect.right > maskRect.left && maskRect.bottom > maskRect.top;
}

void CutMask::reset()
{
	maskRect = D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	layout.Reset();   // 尺寸标签跟旧选区一起清掉，paint 里 layout 为空就直接不画
	win->refresh();
}

MaskHit CutMask::hitTest(POINT pos) const
{
	if (!hasRect()) return MaskHit::None;
	const auto& r = maskRect;
	const float px = (float)pos.x, py = (float)pos.y;
	// 选区的四条边各自延长，把整个窗口切成九块：中间那块就是选区自己，
	// 落在里面一律整体拖动；外面八块各对应一个方位，落在哪块就调哪条边/哪个角
	const int col = px < r.left ? 0 : (px > r.right ? 2 : 1);
	const int row = py < r.top ? 0 : (py > r.bottom ? 2 : 1);
	static constexpr MaskHit grid[3][3]{
		{ MaskHit::TopLeft,    MaskHit::Top,    MaskHit::TopRight },
		{ MaskHit::Left,       MaskHit::Inside, MaskHit::Right },
		{ MaskHit::BottomLeft, MaskHit::Bottom, MaskHit::BottomRight },
	};
	return grid[row][col];
}

void CutMask::startAdjust(POINT pos)
{
	adjustHit = hitTest(pos);
	adjustStartRect = maskRect;
	adjustPressPos = pos;
	// 抓边或抓角：按下这一下就把对应的边挪到光标位置。
	// 内部整体拖动不能这么干 —— 那会让选区瞬间跳到光标为中心的位置。
	if (adjustHit != MaskHit::None && adjustHit != MaskHit::Inside) {
		adjust(pos);
	}
}

void CutMask::adjust(POINT pos)
{
	if (adjustHit == MaskHit::None) return;
	auto r = adjustStartRect;
	const float px = (float)pos.x, py = (float)pos.y;
	if (adjustHit == MaskHit::Inside) {
		// 整体平移，尺寸不变，夹在宿主客户区内
		const float rw = r.right - r.left, rh = r.bottom - r.top;
		const float left = std::clamp(r.left + px - adjustPressPos.x, 0.f, win->w - rw);
		const float top = std::clamp(r.top + py - adjustPressPos.y, 0.f, win->h - rh);
		r = D2D1::RectF(left, top, left + rw, top + rh);
	}
	else {
		const float cx = std::clamp(px, 0.f, win->w);
		const float cy = std::clamp(py, 0.f, win->h);
		switch (adjustHit)
		{
		case MaskHit::Left: r.left = cx; break;
		case MaskHit::Right: r.right = cx; break;
		case MaskHit::Top: r.top = cy; break;
		case MaskHit::Bottom: r.bottom = cy; break;
		case MaskHit::TopLeft: r.left = cx; r.top = cy; break;
		case MaskHit::TopRight: r.right = cx; r.top = cy; break;
		case MaskHit::BottomRight: r.right = cx; r.bottom = cy; break;
		case MaskHit::BottomLeft: r.left = cx; r.bottom = cy; break;
		default: break;
		}
		// 拖过头（比如左边越过右边）时归一化，接下来就自然变成"在拖另一条边"
		const float l = std::min(r.left, r.right), rr = std::max(r.left, r.right);
		const float t = std::min(r.top, r.bottom), b = std::max(r.top, r.bottom);
		r = D2D1::RectF(l, t, rr, b);
		// 塌到最小尺寸以下时，把动着的那条边推回来，不动锚定的那条
		const bool moveLeft = adjustHit == MaskHit::Left || adjustHit == MaskHit::TopLeft || adjustHit == MaskHit::BottomLeft;
		const bool moveTop = adjustHit == MaskHit::Top || adjustHit == MaskHit::TopLeft || adjustHit == MaskHit::TopRight;
		if (r.right - r.left < minSize) {
			if (moveLeft) r.left = r.right - minSize;
			else r.right = r.left + minSize;
		}
		if (r.bottom - r.top < minSize) {
			if (moveTop) r.top = r.bottom - minSize;
			else r.bottom = r.top + minSize;
		}
	}
	if (r.left == maskRect.left && r.top == maskRect.top &&
		r.right == maskRect.right && r.bottom == maskRect.bottom) return;
	maskRect = r;
	makeLayout();
	win->refresh();
}

void CutMask::paint(ID2D1DeviceContext* ctx)
{
	// 判断"要不要画"只看有没有选区，不看尺寸标签的 layout：
	// layout 是 makeTextLayout 失败（字体/字号异常）时的副产物，拿它当守卫会让
	// 整个压暗层和选区边框一起消失，选区还在却看不见
	if (!hasRect()) return;
	ctx->FillRectangle(D2D1::RectF(0.f, 0.f, win->w, maskRect.top), brushBg.Get());
	ctx->FillRectangle(D2D1::RectF(0.f, maskRect.bottom, win->w, win->h), brushBg.Get());
	ctx->FillRectangle(D2D1::RectF(0.f, maskRect.top, maskRect.left, maskRect.bottom), brushBg.Get());
	ctx->FillRectangle(D2D1::RectF(maskRect.right, maskRect.top, win->w, maskRect.bottom), brushBg.Get());
	auto halfStrokeWidth{ strokeWidth / 2.f };
	ctx->DrawRectangle(D2D1::RectF(maskRect.left - halfStrokeWidth, maskRect.top - halfStrokeWidth, maskRect.right + halfStrokeWidth, maskRect.bottom + halfStrokeWidth), brushBorder.Get(), strokeWidth);
	if (hideLabel || !layout) return;
	ctx->FillRectangle(layoutRect, brushBg.Get());
	ctx->DrawTextLayout({ layoutRect.left+ paddingMargin, layoutRect.top+ paddingMargin }, layout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}
