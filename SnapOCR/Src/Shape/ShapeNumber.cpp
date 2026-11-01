#include "pch.h"
#include "Win/WinHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "History.h"
#include "ShapeNumber.h"

using Microsoft::WRL::ComPtr;

ShapeNumber::ShapeNumber(WinHost* win) :ShapeBase(win), draggers{
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0) },
	// 半径就是工具栏滑块的值（物理像素），跟别的工具的"线宽"是同一个滑块。
	// 拖拽/滚轮改过之后会回写给滑块（见 ToolSub::setShapeSliderVal），所以后面新建的序号沿用同一大小，
	// 关掉应用再打开也还是这个大小 —— 值存在 config.json 的 toolPin.number.radius 里
	r{ win->toolSub->getSliderVal() },
	val{getNextVal(win)}
{
	auto toolSub = win->toolSub.get();
	auto d2d = ui::D2D::get();
	// 颜色按"第几个标号"取：双色时奇数号用一号色、偶数号用二号色（1红2蓝3红…）
	d2d->deviceContext->CreateSolidColorBrush(toolSub->getNumberColor(val), brush.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0XFFFFFF), brushText.GetAddressOf());
	isSquare = toolSub->isNumberSquare;
	isFill = toolSub->isNumberSquare ? toolSub->isNumSquareSolid : toolSub->isNumCircleSolid;
}

int ShapeNumber::getNextVal(WinHost* win)
{
	int maxVal{ 0 };
	for (auto& shape : win->history->shapes) {
		auto number = dynamic_cast<ShapeNumber*>(shape.get());
		if (number && !number->isUndo && number->val > maxVal) {
			maxVal = number->val;
		}
	}
	return maxVal + 1;
}

ShapeNumber::~ShapeNumber()
{

}

// 把序号排到 2r × 2r 的方框里居中，字号取 r（直径的一半），刚好填满圆
void ShapeNumber::makeTextLayout()
{
	auto d2d = ui::D2D::get();
	auto text = std::to_wstring(val);
	d2d->dwriteFactory->CreateTextLayout(text.data(), (UINT32)text.length(),
		d2d->baseTextFormat.Get(), r * 2, r * 2, layoutText.ReleaseAndGetAddressOf());
	if (!layoutText) return;
	layoutText->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
	layoutText->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	layoutText->SetFontSize(r, { 0, (UINT32)text.length() });
}

void ShapeNumber::paint(ID2D1DeviceContext* ctx)
{
	// makePath / makeTextLayout 要等第一次 mouseDown 才跑，这之前可能先来一次 paint
	if (!path || !layoutText) return;
	if (isFill) {
		ctx->FillGeometry(path.Get(), brush.Get());
		ctx->DrawTextLayout({ cx - r,cy - r }, layoutText.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	}
	else {
		ctx->DrawGeometry(path.Get(), brush.Get(), win->dpi);
		ctx->DrawTextLayout({ cx - r,cy - r }, layoutText.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	}
}

void ShapeNumber::paintDragger(ID2D1DeviceContext* ctx)
{
	if (isWheel) return;
	for (auto& dragger:draggers)
	{
		ctx->DrawRectangle(dragger, brushDragger.Get(), 1.0f * win->dpi);
	}
}

void ShapeNumber::mouseDrag(const float x, const float y)
{
	if (hoverDraggerIndex == 0) {
		auto spanX{ x - pressX };
		auto spanY{ y - pressY };
		cx += spanX;
		cy += spanY;
		makePath();
		pressX = x;
		pressY = y;
	}
	else if (hoverDraggerIndex == 1) {
		angle = -atan2f(y - cy, x - cx) * 180.f / 3.14159265358979323846f;
		makePath();
	}
	else if (hoverDraggerIndex == 2) {
		auto dx{ x - cx };
		auto dy{ y - cy };
		r = sqrtf(dx * dx + dy * dy);
		auto minR{ 8.f * win->dpi };
		if (r < minR) r = minR;
		// 用它夹好的返回值：半径不能超出工具栏滑块的值域，否则滑块显示的就不是真实大小了
		r = win->toolSub->setShapeSliderVal(L"number", r);
		makePath();
		makeTextLayout();
	}
}

void ShapeNumber::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) { //首次创建
		cx = x;
		cy = y;
		pressX = cx;
		pressY = cy;
		hoverDraggerIndex = 0;
		makePath();
		makeTextLayout();
		win->refresh();
	}
	else if (hoverDraggerIndex >= 0) {
		pressX = x;
		pressY = y;
	}
}

void ShapeNumber::updateDraggers()
{
	auto half{ draggerSize / 2 };
	draggers[0].left = cx - half;
	draggers[0].top = cy - half;
	draggers[0].right = cx + half;
	draggers[0].bottom = cy + half;

	draggers[1].left = tip.x - half;
	draggers[1].top = tip.y - half;
	draggers[1].right = tip.x + half;
	draggers[1].bottom = tip.y + half;


	draggers[2].left = mid.x - half;
	draggers[2].top = mid.y - half;
	draggers[2].right = mid.x + half;
	draggers[2].bottom = mid.y + half;
}

void ShapeNumber::mouseUp(const float x, const float y)
{
	updateDraggers();
}

void ShapeNumber::mouseMove(const float x, const float y)
{
    hoverDraggerIndex = draggerHit(draggers, (int)draggers.size(), x, y);
	// 填充序号：整个徽章盘都是实体，落在里面任何位置按住就能整体搬，
	// 不必先精确点到圆心那只夹点。空心序号维持原样 —— 盘内是空的，按下该起一个新序号。
	// r > 0 才认：新建出来还没落笔时 r 是 0、cx/cy 也是 0，那个徽章根本不存在
	if (hoverDraggerIndex == -1 && isFill && r > 0) {
		const float dx{ x - cx }, dy{ y - cy };
		const float hitR{ r + 2.f * win->dpi };
		if (isSquare) {
			// 方徽章：先按当前角度把点转回本地坐标系再判 —— 徽章转到多少度，
			// 命中区就跟着转多少度（与圆徽章按半径判定的粗精度是同一条标准）
			const float radians = -angle * 3.14159265358979323846f / 180.f;
			const float lx = dx * cosf(radians) + dy * sinf(radians);
			const float ly = -dx * sinf(radians) + dy * cosf(radians);
			if (fabsf(lx) <= hitR && fabsf(ly) <= hitR) hoverDraggerIndex = 0;
		}
		else {
			const bool inside = dx * dx + dy * dy <= hitR * hitR;
			if (inside) hoverDraggerIndex = 0;
		}
	}
	if (isWheel) {
		isWheel = false;
		mouseUp(x, y);
		win->refresh();
	}
}

void ShapeNumber::mouseWheel(const float x, const float y, const short delta)
{
	isWheel = true;
	// 下限交给 setShapeSliderVal 按滑块值域表夹（表中的 min/max 是逻辑像素，内部乘 dpi
	// 之后才是物理像素）。这里不要再写一个 "6.f * win->dpi" 的阈值 —— 那是把值域表里
	// 那一份复制了第二遍，以后改表就漏了这一处；而且 6 逻辑像素在 150% 缩放下是 9 物理
	// 像素，会先被这个阈值卡住，缩不到值域真正允许的最小半径
	auto before{ r };
	if (delta < 0) r--;
	else r++;
	r = win->toolSub->setShapeSliderVal(L"number", r);
	// 已经顶到值域的头了（夹完还是原值）：几何体和文字都不必重建，省下一格一次的
	// path 分配和整窗重绘。isWheel 保持 true —— 滚轮确实刚动过，下次 mousemove 会走
	// mouseMove 里的重排分支把夹点摆回正确位置，那里会自己把它复位
	if (r == before) return;
	makePath();
	makeTextLayout();
	win->refresh();
}

void ShapeNumber::setCursor()
{
	if (hoverDraggerIndex >=0) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
}

D2D1_POINT_2F ShapeNumber::localPoint(const float degrees)
{
	float radians = degrees * 3.14159265358979323846f / 180.f;
	return D2D1::Point2F(r * cosf(radians), -r * sinf(radians));
}

D2D1_POINT_2F ShapeNumber::transformPoint(const D2D1_POINT_2F& point)
{
	float radians = -angle * 3.14159265358979323846f / 180.f;
	float cosValue = cosf(radians);
	float sinValue = sinf(radians);
	return D2D1::Point2F(
		cx + point.x * cosValue - point.y * sinValue,
		cy + point.x * sinValue + point.y * cosValue
	);
}

void ShapeNumber::makePath()
{
	auto d2d = ui::D2D::get();
	if (!d2d || !d2d->d2dFactory) return;
	// ReleaseAndGetAddressOf 而不是 GetAddressOf：后者不放旧对象，拖动时每个鼠标事件漏一个几何体
	d2d->d2dFactory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
	// 创建失败时 path 为空，下面 Open 就是空指针（paint 里已经判过 !path，只是不画）
	if (!path) return;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(path->Open(sink.GetAddressOf())) || !sink) return;
	// 尖角都伸到 r + r/3，两种形状同一条规格；mid 是远端那只夹点
	// （圆 = 弧中点，方 = 对边中点），拖拽手感保持一致
	tip = transformPoint(D2D1::Point2F(r + r / 3.f, 0.f));
	if (isSquare) {
		// 方徽章：半边长 r 的方框，右缘中段开一个口（开口张角与圆徽章的 ±10° 相当，
		// tan(10°) ≈ 0.17633），尖角从口里伸出去。全直线，没有弧段。
		// 顶点必须按"沿边走"的顺序排：从上唇沿右缘走到右上角，再跨顶边……
		// 漏掉角上的点，那条边就会斜着切进方框里（画出来是缺了一角的旗子）
		constexpr float lipY{ 0.17633f };
		auto start = transformPoint(D2D1::Point2F(r, -r * lipY));
		mid = transformPoint(D2D1::Point2F(-r, 0.f));
		sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
		sink->AddLine(transformPoint(D2D1::Point2F(r, -r)));
		sink->AddLine(transformPoint(D2D1::Point2F(-r, -r)));
		sink->AddLine(transformPoint(D2D1::Point2F(-r, r)));
		sink->AddLine(transformPoint(D2D1::Point2F(r, r)));
		sink->AddLine(transformPoint(D2D1::Point2F(r, r * lipY)));
		sink->AddLine(tip);
		sink->AddLine(start);
		sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	}
	else {
		auto start = transformPoint(localPoint(10.f));
		mid = transformPoint(localPoint(180.f));
		auto end = transformPoint(localPoint(350.f));
		sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
		sink->AddArc(D2D1::ArcSegment( mid, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
		sink->AddArc(D2D1::ArcSegment( end, D2D1::SizeF(r, r), 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL ));
		sink->AddLine(tip);
		sink->AddLine(start);
		sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	}
	sink->Close();
}

void ShapeNumber::setStyle(bool square, bool fill)
{
	// 换形状（圆 ↔ 方）要重建几何体、重摆夹点；实心/空心只影响绘制方式。
	// 颜色要按当前单色/双色重取 —— 双色下同一条序号用哪个色由它的序号（奇偶）决定，
	// 所以必须问 getNumberColor(val)，不能用工具条上选中的那一个颜色
	const bool reshaped = isSquare != square;
	isSquare = square;
	isFill = fill;
	if (reshaped) {
		makePath();
		updateDraggers();  // tip/mid 都挪了位置，夹点得跟上
	}
	auto toolSub = win->toolSub.get();
	if (auto d2d = ui::D2D::get(); d2d && d2d->deviceContext && toolSub) {
		d2d->deviceContext->CreateSolidColorBrush(toolSub->getNumberColor(val),
			brush.ReleaseAndGetAddressOf());
	}
	win->refresh();
}

// 序号：滑块一动，整个图标一起变 —— 圆环的半径、箭头、里面那个数字，以及三只夹点。
// 只改 r 不重建 path 的话，画面里就是"数字挪了个位置、圈还是原来那么大"，
// 看着像只有数字在联动（用户反馈的正是这个）
void ShapeNumber::applyStyle()
{
	auto toolSub = win->toolSub.get();
	if (!toolSub) return;
	const float newR = toolSub->getSliderVal();
	const bool wasSquare = isSquare;
	isSquare = toolSub->isNumberSquare;
	isFill = toolSub->isNumberSquare ? toolSub->isNumSquareSolid : toolSub->isNumCircleSolid;
	if (auto d2d = ui::D2D::get(); d2d && d2d->deviceContext) {
		d2d->deviceContext->CreateSolidColorBrush(toolSub->getNumberColor(val),
			brush.ReleaseAndGetAddressOf());
	}
	// path 为空 = 这个序号还没落笔（构造函数里不建几何体），没什么可缩的
	if (path && (newR != r || isSquare != wasSquare)) {
		r = newR;
		makePath();        // 圆环/方框 + 箭头按新半径重建
		makeTextLayout();  // 数字字号取 r，跟着一起变
		updateDraggers();  // 夹点也得到新位置上，否则鼠标还得去旧地方找
	}
	win->refresh();
}
