#include "pch.h"
#include "../Win/WinHost.h"
#include "ShapeBase.h"
#include <algorithm>
#include <cfloat>
#include <cmath>

ShapeBase::ShapeBase(WinHost* win):win{win}, draggerSize{6*win->dpi}
{
	auto d2d = ui::D2D::get();
	// 控制点小方块描边：醒目的半透明蓝
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0.28f, 0.62f, 1.0f, 0.95f), brushDragger.GetAddressOf());
}

ShapeBase::~ShapeBase()
{}

bool ShapeBase::isInRect(const D2D1_RECT_F rect, const float x, const float y)
{
	return (x > rect.left && x<rect.right && y>rect.top && y < rect.bottom);
}

// 矩形/椭圆最小尺寸：宽或高小于 2px 时按中心扩到 2px，避免拖出退化的小形状
void ShapeBase::enforceMinSize(D2D1_RECT_F& r)
{
	constexpr float minSize{ 2.f };
	if (r.right - r.left < minSize) {
		const float c = (r.left + r.right) / 2.f;
		r.left = c - minSize / 2.f;
		r.right = c + minSize / 2.f;
	}
	if (r.bottom - r.top < minSize) {
		const float c = (r.top + r.bottom) / 2.f;
		r.top = c - minSize / 2.f;
		r.bottom = c + minSize / 2.f;
	}
}

// ———————————————— 八夹点矩形的共用几何 ————————————————
//
// 矩形 / 椭圆 / 马赛克选框 / 橡皮选框这四个类原本各抄了一份一模一样的按下锚点、
// 拖动重算、八只夹点摆位（三份逻辑 × 四家）。这里收成一处，各家只在外面按
// rectDragTo 的返回值补自己的动作（最小尺寸 / 椭圆的中心半径 / 马赛克位图作废）。
//
// 锚点为什么这么记：拖某一个角时，**对角的那个角不动**，所以锚点抄对角；拖某一条边时，
// 对边不动，就只抄那一条边。整体平移（8）抄的是"按下点落在框内的偏移"，拖动时
// 用鼠标点减掉它，框就跟着手走且不会跳。
void ShapeBase::rectPressAnchor(const int idx, const float x, const float y,
	const D2D1_RECT_F& rect, float& pressX, float& pressY)
{
	switch (idx) {
	case 0: pressX = rect.right; pressY = rect.bottom; break;   // 左上：锚右下
	case 1: pressY = rect.bottom; break;                       // 上边：锚下边
	case 2: pressX = rect.left; pressY = rect.bottom; break;    // 右上：锚左下
	case 3: pressX = rect.left; break;                         // 右边：锚左边
	case 4: pressX = rect.left; pressY = rect.top; break;       // 右下：锚左上
	case 5: pressY = rect.top; break;                          // 下边：锚上边
	case 6: pressX = rect.right; pressY = rect.top; break;      // 左下：锚右上
	case 7: pressX = rect.right; break;                        // 左边：锚右边
	case 8: pressX = x - rect.left; pressY = y - rect.top; break; // 平移：记框内偏移
	default: pressX = x; pressY = y; break;                    // 刚落笔：锚点就是落笔点
	}
}

RectDrag ShapeBase::rectDragTo(const int idx, const float pressX, const float pressY,
	const float x, const float y, D2D1_RECT_F& rect)
{
	// 角上两条边一起动：minmax 让框可以拖过锚点翻到另一侧
	auto both = [&] {
		const auto [left, right] = std::minmax(pressX, x);
		const auto [top, bottom] = std::minmax(pressY, y);
		rect.left = left; rect.right = right;
		rect.top = top; rect.bottom = bottom;
	};
	switch (idx) {
	case 0: case 2: case 4: case 6:
		both();
		return RectDrag::Corner;
	case 1: case 5: {
		const auto [top, bottom] = std::minmax(pressY, y);
		rect.top = top; rect.bottom = bottom;
		return RectDrag::Vertical;
	}
	case 3: case 7: {
		const auto [left, right] = std::minmax(pressX, x);
		rect.left = left; rect.right = right;
		return RectDrag::Horizontal;
	}
	case 8: {
		const float w{ rect.right - rect.left }, h{ rect.bottom - rect.top };
		rect.left = x - pressX;
		rect.top = y - pressY;
		rect.right = rect.left + w;
		rect.bottom = rect.top + h;
		return RectDrag::Move;
	}
	default:
		return RectDrag::None;
	}
}

void ShapeBase::rectDraggersTo(const D2D1_RECT_F& rect, D2D1_RECT_F* out) const
{
	if (!out) return;
	const float half{ draggerSize / 2 };
	const float w{ rect.right - rect.left }, h{ rect.bottom - rect.top };
	// 顺序是"左上 → 上 → 右上 → 右 → 右下 → 下 → 左下 → 左"，与 mouseDrag 里的档位一一对应
	const D2D1_POINT_2F p[8]{
		{ rect.left,           rect.top },
		{ rect.left + w / 2,   rect.top },
		{ rect.right,          rect.top },
		{ rect.right,          rect.top + h / 2 },
		{ rect.right,          rect.bottom },
		{ rect.left + w / 2,   rect.bottom },
		{ rect.left,           rect.bottom },
		{ rect.left,           rect.top + h / 2 },
	};
	for (int i = 0; i < 8; ++i) {
		out[i] = D2D1::RectF(p[i].x - half, p[i].y - half, p[i].x + half, p[i].y + half);
	}
}

int ShapeBase::draggerHit(const std::vector<D2D1_RECT_F>& draggers, const int count,
	const float x, const float y)
{
	for (int i = 0; i < count; ++i) {
		if (isInRect(draggers[i], x, y)) return i;
	}
	return -1;
}

void ShapeBase::draggerCursor(const int idx) const
{
	if (idx == 0 || idx == 4) {
		SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
	}
	else if (idx == 1 || idx == 5) {
		SetCursor(LoadCursor(nullptr, IDC_SIZENS));
	}
	else if (idx == 2 || idx == 6) {
		SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
	}
	else if (idx == 3 || idx == 7) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
	}
	else if (idx == 8) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
}

void ShapeBase::endDraggersTo(const std::vector<D2D1_POINT_2F>& pts, D2D1_RECT_F* out) const
{
	if (pts.empty() || !out) return;
	const float half{ draggerSize / 2 };
	const D2D1_POINT_2F ends[2]{ pts.front(), pts.back() };
	for (int i = 0; i < 2; ++i) {
		out[i] = D2D1::RectF(ends[i].x - half, ends[i].y - half, ends[i].x + half, ends[i].y + half);
	}
}

// 点到线段的距离
static float distToSegment(const D2D1_POINT_2F& p, const D2D1_POINT_2F& a, const D2D1_POINT_2F& b)
{
	const float abx = b.x - a.x, aby = b.y - a.y;
	const float apx = p.x - a.x, apy = p.y - a.y;
	const float ab2 = abx * abx + aby * aby;
	float t = (ab2 == 0.f) ? 0.f : (apx * abx + apy * aby) / ab2;
	t = std::clamp(t, 0.f, 1.f);   // 限制在线段范围内
	const float dx = p.x - (a.x + t * abx), dy = p.y - (a.y + t * aby);
	return std::sqrt(dx * dx + dy * dy);
}

bool ShapeBase::hitStrokeEdge(const std::vector<D2D1_POINT_2F>& pts,
	const float strokeWidth, const float x, const float y) const
{
	const float outer{ strokeWidth * 0.5f + win->dpi };   // 外沿保持原来的判定范围
	// 往里让出这么宽算内部。细笔画算出来是负数，等于整条线都能拖
	const float inner{ outer - draggerSize / 2 };
	// 得先在所有线段里取最小距离再判断：自交的笔画里，某一段的边缘可能正好压在另一段的
	// 内部，那种位置视觉上是在笔画里面，逐段判断会误判成边缘
	float minDist{ FLT_MAX };
	const D2D1_POINT_2F p{ x, y };
	for (size_t i = 0; i + 1 < pts.size(); ++i) {
		minDist = std::min(minDist, distToSegment(p, pts[i], pts[i + 1]));
	}
	return minDist <= outer && minDist >= inner;
}
