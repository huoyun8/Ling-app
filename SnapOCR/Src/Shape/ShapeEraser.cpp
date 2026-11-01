#include "pch.h"
#include "Win/WinHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "History.h"
#include "ShapeEraser.h"

using Microsoft::WRL::ComPtr;

ShapeEraser::ShapeEraser(WinHost* win) :ShapeBase(win), draggers{
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0)
}
{
	auto toolSub = win->toolSub.get();
	auto d2d = ui::D2D::get();
	// 橡皮没有颜色可选，这个半透明品红只在拖拽过程中当占位提示 ——
	// 松开鼠标后 isErasing 置位，改用底图画刷把这块盖回原样
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xF00FF0, 0.38f), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isRect = toolSub->isEraserMarquee;
	// 圆头圆角描边。本项目没有 App::getRoundStrokeStyle 那样的全局缓存，
	// 就每个 shape 自己持一个 —— StrokeStyle 是不可变的轻对象，代价可以忽略
	d2d->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
			D2D1_LINE_JOIN_ROUND, 8.f, D2D1_DASH_STYLE_SOLID, 0.f
		),
		nullptr, 0, roundStyle.GetAddressOf());
}

ShapeEraser::~ShapeEraser()
{

}

void ShapeEraser::paint(ID2D1DeviceContext* ctx)
{
	if (isRect) {
		if (isErasing) {
			initBackgroundBrush();
			if (bgBrush) {
				ctx->FillRectangle(rect, bgBrush.Get());
			}
		}
		else {
			ctx->FillRectangle(rect, brush.Get());
		}
		return;
	}
	// makePath 要等第一次 mouseDown 才建 path，这之前可能先来一次 paint
	if (!path) return;
	if (isErasing) {
		initBackgroundBrush();
		if (bgBrush) {
			ctx->DrawGeometry(path.Get(), bgBrush.Get(), strokeWidth, roundStyle.Get());
		}
	}
	else {
		ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, roundStyle.Get());
	}
}

void ShapeEraser::paintDragger(ID2D1DeviceContext* ctx)
{
	// 矩形模式 8 个夹点，涂抹模式只有首尾 2 个
	size_t count = isRect ? draggers.size() : 2;
	for (size_t i = 0; i < count; ++i)
	{
		ctx->DrawRectangle(draggers[i], brushDragger.Get(), 1.0f * win->dpi);
	}
}

void ShapeEraser::mouseDrag(const float x, const float y)
{
	bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (isRect) {
		// 几何一变就退回占位色，等 mouseUp 再重新进入擦除态
		resetEraser();
		const auto kind = rectDragTo(hoverDraggerIndex, pressX, pressY, x, y, rect);
		if (kind != RectDrag::Move && shiftDown) {
			auto w = rect.right - rect.left;
			auto h = rect.bottom - rect.top;
			if (w > h) {
				rect.bottom = rect.top + w;
			}
			else {
				rect.right = rect.left + h;
			}
		}
		return;
	}
	// Shift 按下时不再追加顶点，而是拖动端点 —— 效果是一条直线；松开 Shift 就是自由涂抹
	if (hoverDraggerIndex == 0) {
		if (linePoints.empty()) return;
		if (shiftDown) {
			auto& p = linePoints[0];
			p.x = x;
			p.y = y;
		}
		else if (needSample(x, y, false)) {
			linePoints.insert(linePoints.begin(), { x,y });
		}
		else {
			return;
		}
		makePath();
	}
	else if (hoverDraggerIndex == 1) {
		if (linePoints.empty()) return;
		if (shiftDown) {
			auto& p = linePoints[linePoints.size() - 1];
			p.x = x;
			p.y = y;
		}
		else if (needSample(x, y, true)) {
			linePoints.push_back({ x,y });
		}
		else {
			return;
		}
		makePath();
	}
	else if (hoverDraggerIndex == 8) {
		auto spanX{ x - pressX };
		auto spanY{ y - pressY };
		for (auto& p : linePoints)
		{
			p.x += spanX;
			p.y += spanY;
		}
		makePath();
		pressX = x;
		pressY = y;
	}
}

void ShapeEraser::mouseDown(const float x, const float y)
{
	if (isRect) {
		if (hoverDraggerIndex == -1) { //首次创建
			rect = D2D1::RectF(x, y, x, y);
			hoverDraggerIndex = 4;
		}
		rectPressAnchor(hoverDraggerIndex, x, y, rect, pressX, pressY);
		return;
	}
	if (hoverDraggerIndex == -1) { //首次创建
		linePoints.clear();
		linePoints.push_back({ x,y });
		if (GetKeyState(VK_SHIFT) & 0x8000) {
			// 直线模式需要两个点，末点跟着鼠标走
			linePoints.push_back({ x,y });
		}
		makePath();
		hoverDraggerIndex = 1;
		win->refresh();
	}
	else if (hoverDraggerIndex == 8) {
		pressX = x;
		pressY = y;
	}
}

void ShapeEraser::mouseUp(const float x, const float y)
{
	updateDraggers();
	// 退化的几何（零面积矩形、只有一个点的笔画）盖回底图什么也看不出来，
	// 保持占位色让用户看到自己画了个无效的东西
	if (isRect) {
		isErasing = rect.right > rect.left && rect.bottom > rect.top;
	}
	else {
		isErasing = path && linePoints.size() > 1;
	}
}

void ShapeEraser::mouseMove(const float x, const float y)
{
	if (isRect) {
		hoverDraggerIndex = draggerHit(draggers, (int)draggers.size(), x, y);
		if (hoverDraggerIndex >= 0) return;
		// 橡皮擦矩形是整块擦除、没有可见边框，边缘命中区用固定宽度（2 逻辑像素），
		// 别跟滑块笔刷尺寸走 —— 笔刷大小跟这条边没有任何关系
		auto half{ 2.f * win->dpi };//多个一个dpi，让范围更大点
		if (x >= rect.left - half && x <= rect.right + half && y >= rect.top - half && y <= rect.bottom + half)
		{
			// 矩形整块都是实体：内部任意位置按住就能搬，不必先精确点到边框。
			// 手绘擦除不走这里（上面 isRect 已分流），仍是只有笔画边缘能拖
			hoverDraggerIndex = 8;
		}
		return;
	}
	hoverDraggerIndex = draggerHit(draggers, 2, x, y);
	if (hoverDraggerIndex == -1)
	{
		hitTest({ x,y });
	}
}

void ShapeEraser::setCursor()
{
	if (isRect) {
		draggerCursor(hoverDraggerIndex);
		return;
	}
	if (hoverDraggerIndex == 8) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
}

void ShapeEraser::makePath()
{
	if (linePoints.empty()) return;
	resetEraser();
	auto d2d = ui::D2D::get();
	// ReleaseAndGetAddressOf 而不是 GetAddressOf：后者不放旧对象，自由涂抹时每个鼠标事件漏一个几何体
	d2d->d2dFactory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
	if (!path) return;
	ComPtr<ID2D1GeometrySink> sink;
	path->Open(sink.GetAddressOf());
	sink->BeginFigure(linePoints[0], D2D1_FIGURE_BEGIN_HOLLOW);
	if (linePoints.size() > 1) {
		sink->AddLines(&linePoints[1], (UINT32)(linePoints.size() - 1));
	}
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->Close();
}

bool ShapeEraser::needSample(float x, float y, bool atEnd) const
{
	if (linePoints.empty()) return true;
	auto& ref = atEnd ? linePoints.back() : linePoints.front();
	auto step = minStep * win->dpi;
	auto dx = x - ref.x, dy = y - ref.y;
	return dx * dx + dy * dy >= step * step;
}

void ShapeEraser::resetEraser()
{
	isErasing = false;
	bgBrush.Reset();
}

// 底图画刷懒创建。底图与窗口同尺寸同坐标，所以不需要像 ShapeMosaic 那样平移画刷
void ShapeEraser::initBackgroundBrush()
{
	if (bgBrush || !win->screenImg) return;
	auto bitmapBrushProps = D2D1::BitmapBrushProperties(
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
	auto brushProps = D2D1::BrushProperties();
	auto d2d = ui::D2D::get();
	d2d->deviceContext->CreateBitmapBrush(win->screenImg.Get(), &bitmapBrushProps, &brushProps, bgBrush.GetAddressOf());
}

void ShapeEraser::updateDraggers()
{
	if (isRect) {
		rectDraggersTo(rect, draggers.data());
		return;
	}
	endDraggersTo(linePoints, draggers.data());
}

void ShapeEraser::hitTest(const D2D1_POINT_2F& mousePos)
{
	// 只有笔画边缘那一圈算命中：粗笔画（马赛克、橡皮擦尤其明显）整片都能拖的话，
	// 就没法在已有笔画上面再画一笔了 —— 鼠标一按下会变成拖动旧元素。
	// 矩形模式本来就是只有边框附近才响应，这里跟它对齐（判定在 ShapeBase 里）
	if (hitStrokeEdge(linePoints, strokeWidth, mousePos.x, mousePos.y)) hoverDraggerIndex = 8;
}


// 工具条滑块 = 手绘时的粗细，拖一下就立刻看到笔迹变粗变细（收笔之后也算，
// 因为 paint 里描边宽度读的就是 strokeWidth）。矩形模式是"整块一次擦干净"，
// 没有粗细这个概念，滑块对它不生效 —— 不动它的 rect
void ShapeEraser::applyStyle()
{
	if (isRect) return;
	auto toolSub = win->toolSub.get();
	if (!toolSub) return;
	strokeWidth = toolSub->getSliderVal();
	win->refresh();
}

