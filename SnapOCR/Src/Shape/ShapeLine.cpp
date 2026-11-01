#include "pch.h"
#include "App.h"
#include "Win/WinHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeLine.h"

using Microsoft::WRL::ComPtr;

ShapeLine::ShapeLine(WinHost* win) :ShapeBase(win), draggers{
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0)}
{
	auto toolSub = win->toolSub.get();
	auto color = toolSub->getSelectedColor();
	// 半透明只属于画笔。箭头副条里画的那条直线不受画笔的"半透明"影响，
	// 否则在画笔里开一次半透明，回头画箭头直线也跟着变淡（两个工具串在一起）
	useBrushTransparent = !(win->toolMain && win->toolMain->curId == L"arrow");
	isTransparent = useBrushTransparent && toolSub->isBrushSemiTransparent;
	if (isTransparent) {
		color.a = 0.5f;
	}
	auto d2d = ui::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(color, brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	// 圆头圆角描边。本项目没有 App::getRoundStrokeStyle 那样的全局缓存，
	// 就每个 shape 自己持一个 —— StrokeStyle 是不可变的轻对象，代价可以忽略
	d2d->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_ROUND,    // 起点线帽：圆角
			D2D1_CAP_STYLE_ROUND,    // 终点线帽：圆角
			D2D1_CAP_STYLE_ROUND,    // 虚线端点（如有）
			D2D1_LINE_JOIN_ROUND,    // 线段连接处：圆角
			8.f,                     // miterLimit
			D2D1_DASH_STYLE_SOLID,
			0.f
		),
		nullptr, 0, roundStyle.GetAddressOf());
}

ShapeLine::~ShapeLine()
{

}

void ShapeLine::paint(ID2D1DeviceContext* ctx)
{
	// makePath 要等第一次 mouseDown 才建 path，这之前可能先来一次 paint
	if (!path) return;
	ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, roundStyle.Get());
}

void ShapeLine::paintDragger(ID2D1DeviceContext* ctx)
{
	for (auto& dragger:draggers)
	{
		ctx->DrawRectangle(dragger, brushDragger.Get(), 1.0f * win->dpi);
	}
}

void ShapeLine::mouseDrag(const float x, const float y)
{
	// Shift 按下、或处于直线模式时不再追加顶点，而是拖动端点 —— 效果是一条直线
	const bool straight = straightMode || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (hoverDraggerIndex == 0) {
		if (linePoints.empty()) return;
		if (straight) {
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
		if (straight) {
			// 只有按下点一个顶点时先补一个终点：否则 back() 改的还是起点，线一直是零长
			if (linePoints.size() < 2) linePoints.push_back(linePoints[0]);
			linePoints.back().x = x;
			linePoints.back().y = y;
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
		for (auto& p:linePoints)
		{
			p.x += spanX;
			p.y += spanY;
		}
		makePath();
		pressX = x;
		pressY = y;
	}
}

void ShapeLine::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) { //首次创建
		linePoints.clear();
		linePoints.push_back({ x,y }); 
		if (straightMode || (GetKeyState(VK_SHIFT) & 0x8000)) {
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

void ShapeLine::mouseUp(const float x, const float y)
{
	// 没建过顶点（只有按下马上弹起、又被判成无效的那种）时下面两处下标都是越界的
	endDraggersTo(linePoints, draggers.data());
}

void ShapeLine::mouseMove(const float x, const float y)
{
    hoverDraggerIndex = draggerHit(draggers, (int)draggers.size(), x, y);
    if (hoverDraggerIndex == -1)
    {
		hitTest({ x,y });
    }
}

void ShapeLine::setCursor()
{
	// 两个端点和整条线的拖动都是"搬位置"，统一给移动光标 ——
	// 与矩形/椭圆等形状压在内部时那份光标一致，端点上也终于有了"能拖"的反馈
	if (hoverDraggerIndex >= 0) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
}

void ShapeLine::makePath()
{
	if (linePoints.empty()) return;
	auto d2d = ui::D2D::get();
	// ReleaseAndGetAddressOf 而不是 GetAddressOf：后者不放旧对象，自由画时每个鼠标事件漏一个几何体
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

bool ShapeLine::needSample(float x, float y, bool atEnd) const
{
	if (linePoints.empty()) return true;
	auto& ref = atEnd ? linePoints.back() : linePoints.front();
	auto step = minStep * win->dpi;
	auto dx = x - ref.x, dy = y - ref.y;
	return dx * dx + dy * dy >= step * step;
}

void ShapeLine::hitTest(const D2D1_POINT_2F& mousePos)
{
	// 只有笔画边缘那一圈算命中：粗笔画整片都能拖的话，就没法在已有笔画上面再画一笔了 ——
	// 鼠标一按下会变成拖动旧元素（判定在 ShapeBase 里，与马赛克/橡皮共用一份）
	if (hitStrokeEdge(linePoints, strokeWidth, mousePos.x, mousePos.y)) hoverDraggerIndex = 8;
}

// 直线：线宽 + 颜色（"半透明"开关也在这里重算，否则切了开关颜色不变）
void ShapeLine::applyStyle()
{
	auto toolSub = win->toolSub.get();
	if (!toolSub) return;
	strokeWidth = toolSub->getSliderVal();
	// 同上：只有画笔画的线跟着画笔的半透明开关走
	isTransparent = useBrushTransparent && toolSub->isBrushSemiTransparent;
	auto color = toolSub->getSelectedColor();
	if (isTransparent) color.a = 0.5f;
	if (auto d2d = ui::D2D::get(); d2d && d2d->deviceContext) {
		d2d->deviceContext->CreateSolidColorBrush(color, brush.ReleaseAndGetAddressOf());
	}
	win->refresh();
}
