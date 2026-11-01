#include "pch.h"
#include "Win/WinHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeRect.h"

ShapeRect::ShapeRect(WinHost* win) :ShapeBase(win), draggers{ 
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0) }
{
	auto toolSub = win->toolSub.get();
	// 画笔的"选框"画的是填充框：它跟画笔自己的"半透明"开关联动（开 = 半透明填充）。
	// 矩形工具画的矩形不吃这个开关，两边互不影响
	useBrushAlpha = win->toolMain && win->toolMain->curId == L"brush";
	auto color = toolSub->getSelectedColor();
	if (useBrushAlpha && toolSub->isBrushSemiTransparent) color.a = 0.5f;
	auto d2d = ui::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(color, brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isFill = fillForced ? true : toolSub->isRectSolid;
}

ShapeRect::~ShapeRect()
{

}

void ShapeRect::paint(ID2D1DeviceContext* ctx)
{
	if (isFill) {
		ctx->FillRectangle(rect, brush.Get());
	}
	else {
		ctx->DrawRectangle(rect, brush.Get(), strokeWidth);
	}
}

void ShapeRect::paintDragger(ID2D1DeviceContext* ctx)
{
	for (auto& dragger:draggers)
	{
		ctx->DrawRectangle(dragger, brushDragger.Get(), 1.0f * win->dpi);
	}
}

void ShapeRect::mouseDrag(const float x, const float y)
{
	const auto kind = rectDragTo(hoverDraggerIndex, pressX, pressY, x, y, rect);
	// 矩形最小尺寸 2px：只钳"四角一起拉"那一档 —— 单边拖动不该把另一条边也撑开
	if (kind == RectDrag::Corner) enforceMinSize(rect);
	bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (kind != RectDrag::Move && shiftDown) {
		auto w = rect.right - rect.left;
		auto h = rect.bottom - rect.top;
		if (w > h) {
			rect.bottom = rect.top + w;
		}
		else
		{
			rect.right = rect.left + h;
		}
	}
}

void ShapeRect::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) { //首次创建：落笔点当锚，按右下角那档拖
		pressX = x;
		pressY = y;
		hoverDraggerIndex = 4;
		return;
	}
	rectPressAnchor(hoverDraggerIndex, x, y, rect, pressX, pressY);
}

void ShapeRect::mouseUp(const float x, const float y)
{
	rectDraggersTo(rect, draggers.data());
}

void ShapeRect::mouseMove(const float x, const float y)
{
    hoverDraggerIndex = draggerHit(draggers, (int)draggers.size(), x, y);
    if (hoverDraggerIndex == -1)
    {
        // 填充矩形没有可见边框，边缘命中区用固定宽度（2 逻辑像素）即可，
        // 别跟滑块线宽走 —— 填充时滑块那套值跟这条边没有任何关系
        auto half{ isFill ? 2.f * win->dpi : strokeWidth / 2.f + win->dpi };//多个一个dpi，让范围更大点
        if (x >= rect.left - half && x <= rect.right + half && y >= rect.top - half && y <= rect.bottom + half )
		{
			// 填充矩形整块都是实体：落在内部任何位置都算"要搬这一块"，不必先精确点到
			// 那圈看不见的边框。空心矩形维持原样 —— 它内部是空的，在里面按下该起一个新框
			if (isFill || x <= rect.left + half || x >= rect.right - half || y <= rect.top + half || y >= rect.bottom - half) {
				hoverDraggerIndex = 8;
			}
        }
    }
}

// 光标停在边框或八个 dragger 上（此时 WinHost 才把滚轮事件转过来）滚滚轮 = 调线宽，
// 与序号那边用滚轮调大小是一回事。填充矩形没有边框可调，直接不管。
void ShapeRect::mouseWheel(const float x, const float y, const short delta)
{
	if (isFill) return;
	// 一格一个逻辑像素。上下限交给 ToolSub 那张滑块值域表夹，用它夹完的返回值 ——
	// 线宽与工具栏滑块因此永远是同一个数，也滚不出滑块能表达的范围
	auto next = strokeWidth + (delta < 0 ? -win->dpi : win->dpi);
	// 工具名跟着当前工具走：画笔画的框要落在画笔自己的滑块配置上，不能写死 "rect"
	const std::wstring tool = (win->toolMain && !win->toolMain->curId.empty()) ? win->toolMain->curId : L"rect";
	auto applied = win->toolSub->setShapeSliderVal(tool, next);
	if (applied == strokeWidth) return;   //已经顶到值域的头了，不用重画
	strokeWidth = applied;
	win->refresh();
}

void ShapeRect::setCursor()
{
	draggerCursor(hoverDraggerIndex);
}

// 拖滑块 / 换颜色 / 切填充时按当前样式重画。原来颜色和线宽是构造时从 ToolSub 抄一份快照，
// 画完再拖滑块，画面纹丝不动 —— 用户要的是"拖的时候就看着它变"
void ShapeRect::applyStyle()
{
	auto toolSub = win->toolSub.get();
	if (!toolSub) return;
	strokeWidth = toolSub->getSliderVal();
	isFill = fillForced ? true : toolSub->isRectSolid;
	if (auto d2d = ui::D2D::get(); d2d && d2d->deviceContext) {
		// 画笔的选框：半透明开 = 填充 50% 透明，与手绘线的半透明同一个开关
		auto color = toolSub->getSelectedColor();
		if (useBrushAlpha && toolSub->isBrushSemiTransparent) color.a = 0.5f;
		d2d->deviceContext->CreateSolidColorBrush(color,
			brush.ReleaseAndGetAddressOf());
	}
	win->refresh();
}
