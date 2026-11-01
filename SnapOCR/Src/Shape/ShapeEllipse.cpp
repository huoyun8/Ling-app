#include "pch.h"
#include "Win/WinHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeEllipse.h"

ShapeEllipse::ShapeEllipse(WinHost* win) :ShapeBase(win), draggers{
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
	auto d2d = ui::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	isFill = toolSub->isEllSolid;   // 椭圆有自己的填充开关，与矩形的填充互不关联
}

ShapeEllipse::~ShapeEllipse()
{

}

void ShapeEllipse::paint(ID2D1DeviceContext* ctx)
{
	D2D1_ELLIPSE ellipse = D2D1::Ellipse({ cx,cy }, rx, ry);
	if (isFill) {
		ctx->FillEllipse(ellipse, brush.Get());
	}
	else {
		ctx->DrawEllipse(ellipse, brush.Get(), strokeWidth);
	}
}

void ShapeEllipse::paintDragger(ID2D1DeviceContext* ctx)
{
	for (auto& dragger : draggers)
	{
		ctx->DrawRectangle(dragger, brushDragger.Get(), 1.0f * win->dpi);
	}
}

void ShapeEllipse::mouseDrag(const float x, const float y)
{
	// 中心 / 半径全是 rect 的纯函数，改完框统一从框上重算一遍（原来四个分支各抄一遍）
	auto syncEllipse = [this] {
		cx = (rect.left + rect.right) / 2.f;
		cy = (rect.top + rect.bottom) / 2.f;
		rx = (rect.right - rect.left) / 2.f;
		ry = (rect.bottom - rect.top) / 2.f;
	};
	const auto kind = rectDragTo(hoverDraggerIndex, pressX, pressY, x, y, rect);
	// 椭圆最小尺寸 2px：只钳"四角一起拉"那一档
	if (kind == RectDrag::Corner) enforceMinSize(rect);
	syncEllipse();
	// Shift 约束成正圆。modifiers 参数已经从签名里去掉，改成和 ShapeRect 一样即时查键盘状态：
	// GetKeyState 反映的是当前正在处理的这条消息对应的状态，与 x/y 同步，不会像 GetAsyncKeyState 那样漂
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
		syncEllipse();
		ry = rx;   // 正圆：两条半径取同一个
	}
}

void ShapeEllipse::mouseDown(const float x, const float y)
{
	// 首次创建统一用 4（右下角）：与 ShapeRect 一致，也保证 mouseDrag 里那条
	// "按按下点重新算矩形"的分支能走到。用 0 的话要拿 rect.right/bottom 当初值，
	// 而这时 rect 还是空的，拖出来的框会整体偏出去一个位移
	if (hoverDraggerIndex == -1) {
		pressX = x;
		pressY = y;
		hoverDraggerIndex = 4;
		return;
	}
	rectPressAnchor(hoverDraggerIndex, x, y, rect, pressX, pressY);
}

void ShapeEllipse::mouseUp(const float x, const float y)
{
	rectDraggersTo(rect, draggers.data());
}

void ShapeEllipse::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = draggerHit(draggers, (int)draggers.size(), x, y);
	if (hoverDraggerIndex == -1)
	{
		// 填充椭圆没有可见边框，边缘命中区用固定宽度（2 逻辑像素）即可，
		// 别跟滑块线宽走 —— 填充时滑块那套值跟这条边没有任何关系
		auto half{ isFill ? 2.f * win->dpi : strokeWidth / 2.f + win->dpi };
		auto dx = x - cx;
		auto dy = y - cy;
		auto outerRx = rx + half, outerRy = ry + half;
		auto innerRx = rx - half > 0 ? rx - half : 0.f;
		auto innerRy = ry - half > 0 ? ry - half : 0.f;
		bool insideOuter = (dx / outerRx) * (dx / outerRx) + (dy / outerRy) * (dy / outerRy) <= 1.f;
		bool insideInner = innerRx > 0 && innerRy > 0 && (dx / innerRx) * (dx / innerRx) + (dy / innerRy) * (dy / innerRy) <= 1.f;
		// 填充椭圆整块都是实体：落在内部任何位置都算"要搬这一块"，
		// 不必先精确点到那圈看不见的边。空心椭圆维持原样 —— 内部是空的，按下该起一个新椭圆
		if (isFill) {
			if (insideOuter) hoverDraggerIndex = 8;
		}
		else if (insideOuter && !insideInner) {
			hoverDraggerIndex = 8;
		}
	}
}

// 同 ShapeRect::mouseWheel：光标停在边框或八个 dragger 上时滚滚轮 = 调线宽
void ShapeEllipse::mouseWheel(const float x, const float y, const short delta)
{
	if (isFill) return;
	auto next = strokeWidth + (delta < 0 ? -win->dpi : win->dpi);
	auto applied = win->toolSub->setShapeSliderVal(L"rect", next);   // 矩形/椭圆已合并，共用 rect 的滑块配置
	if (applied == strokeWidth) return;
	strokeWidth = applied;
	win->refresh();
}

void ShapeEllipse::setCursor()
{
	draggerCursor(hoverDraggerIndex);
}

// 同 ShapeRect：填充开关、线宽、颜色都跟着工具条实时走
void ShapeEllipse::applyStyle()
{
	auto toolSub = win->toolSub.get();
	if (!toolSub) return;
	strokeWidth = toolSub->getSliderVal();
	isFill = toolSub->isEllSolid;
	if (auto d2d = ui::D2D::get(); d2d && d2d->deviceContext) {
		d2d->deviceContext->CreateSolidColorBrush(toolSub->getSelectedColor(),
			brush.ReleaseAndGetAddressOf());
	}
	win->refresh();
}
