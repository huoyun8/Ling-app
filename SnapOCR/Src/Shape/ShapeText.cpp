#include "pch.h"
#include "App.h"
#include "History.h"
#include "Win/WinHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "ShapeText.h"
#include <cmath>
#include <algorithm>

using Microsoft::WRL::ComPtr;

ShapeText::ShapeText(WinHost* win) :ShapeBase(win), borderPadding{ 6.f * win->dpi }
{
	setAttr();
}

ShapeText::~ShapeText()
{
	// 还在编辑就被析构（关窗 / Ctrl+Z 撤到它 / 被 removeShape 删掉）时，
	// TextBox 上还挂着两个捕获了 this 的订阅。TextBox 是宿主共用的，活得比本对象久，
	// 之后它只要再触发一次 onTextChanged（下一次编辑、下一次 setText）就会调用到已释放的 this。
	// 这里只摘订阅、不调 finishEdit：析构期间不该再去改宿主状态、也不该再 refresh。
	// 也不走 win->getTextBox() —— 那个是懒建的，析构里新建一个文本框毫无意义
	if (isEditing) {
		if (win && win->textBox) {
			win->textBox->onTextChanged.remove(textChangedTok);
			win->textBox->onFocusChanged.remove(focusTok);
		}
		textChangedTok = {};
		focusTok = {};
		isEditing = false;
	}
}

// "虚线"这个效果由 D2D 自带的 stroke style 提供（2 实 2 虚，与 2.4.25 一致），
// 不需要自己一段一段摆线段。这里只建一次，之后复用。
// 放在第一次绘制时懒建、而不是构造函数里：第一个 shape 有可能早于 D2D 设备初始化
// 被 new 出来，那时候 d2dFactory 还是空的，构造函数里建就等于永远建不出来，
// 之后这圈框会一直画成实线。建失败就一直是空的 —— DrawRectangle 收到空 strokeStyle
// 会画实线，功能还在，只是没虚线
void ShapeText::ensureDashStyle()
{
	if (dashedStrokeStyle) return;
	float dashes[] = { 2.f, 2.f };
	auto d2d = ui::D2D::get();
	if (!d2d || !d2d->d2dFactory) return;
	d2d->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_FLAT,   // 平头：整段落在像素上，端点不糊
			D2D1_CAP_STYLE_FLAT,
			D2D1_CAP_STYLE_FLAT,   // dashCap 也用平头，圆头会把每段的端头糊成半透明
			D2D1_LINE_JOIN_MITER,
			10.f,
			D2D1_DASH_STYLE_CUSTOM,
			0.f
		),
		dashes, ARRAYSIZE(dashes), dashedStrokeStyle.GetAddressOf());
}

// ———————————————— 旋转 / 缩放的坐标换算 ————————————————
//
// rect 永远存"没转过"的轴对齐盒子，angle 单独记。这样：
//   · 命中判定、拖框改位置、TextBox 落位这些老逻辑一个字都不用改，只要先把点转回来；
//   · 导出（离屏 paint）跟屏幕上完全同一条路径，转过的字进图里就是转过的样子。

D2D1_POINT_2F ShapeText::center() const
{
	return D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
}

D2D1_POINT_2F ShapeText::toWorld(const float lx, const float ly) const
{
	if (angle == 0.f) return D2D1::Point2F(lx, ly);
	const auto c = center();
	const float cs = std::cos(angle), sn = std::sin(angle);
	const float dx = lx - c.x, dy = ly - c.y;
	return D2D1::Point2F(c.x + dx * cs - dy * sn, c.y + dx * sn + dy * cs);
}

void ShapeText::toLocal(const float x, const float y, float& lx, float& ly) const
{
	if (angle == 0.f) { lx = x; ly = y; return; }
	const auto c = center();
	// 反向旋转：把世界点绕中心转 -angle。D2D 的 y 轴朝下，正角就是屏幕上的顺时针
	const float cs = std::cos(-angle), sn = std::sin(-angle);
	const float dx = x - c.x, dy = y - c.y;
	lx = c.x + dx * cs - dy * sn;
	ly = c.y + dx * sn + dy * cs;
}

// 旋转柄挂在"上边中点"往外一段：本地坐标里就是 (框中 x, 框顶 - 间隙)，再转到世界
D2D1_POINT_2F ShapeText::rotHandlePos() const
{
	const float mx = (rect.left + rect.right) * 0.5f;
	return toWorld(mx, rect.top - handleRadius() * 2.6f);
}

float ShapeText::handleRadius() const
{
	return 6.f * win->dpi;
}

D2D1_POINT_2F ShapeText::cornerPos(const int i) const
{
	switch (i) {
	case 0: return toWorld(rect.left, rect.top);
	case 1: return toWorld(rect.right, rect.top);
	case 2: return toWorld(rect.right, rect.bottom);
	default: return toWorld(rect.left, rect.bottom);
	}
}

void ShapeText::applyRotation(ID2D1DeviceContext* ctx, D2D1_MATRIX_3X2_F& saved) const
{
	ctx->GetTransform(&saved);
	// 编辑中也照样转：那阵子的字归 TextBox 那一层，它的 visual 会被推同一个角度（见
	// syncRotationToBox），所以框跟着转才和字对得上 —— 拖旋转柄的全程都是斜着的
	if (angle == 0.f) return;
	const auto c = center();
	// 合成在"当前变换之前"：先在自己的底图坐标里绕中心转，再交给宿主的缩放/平移
	// （贴图窗口那层是 Scale*Translation，所以这么乘才对得上）。
	// 这里自己乘是因为 Matrix3x2F 没有从 D2D1_MATRIX_3X2_F 转换的构造函数，
	// r * saved 直接用运算符会被卡在类型转换上
	const D2D1::Matrix3x2F r = D2D1::Matrix3x2F::Rotation(angle * 180.f / 3.14159265358979f, c);
	D2D1::Matrix3x2F m{};
	m._11 = r._11 * saved._11 + r._12 * saved._21;
	m._12 = r._11 * saved._12 + r._12 * saved._22;
	m._21 = r._21 * saved._11 + r._22 * saved._21;
	m._22 = r._21 * saved._12 + r._22 * saved._22;
	m._31 = r._31 * saved._11 + r._32 * saved._21 + saved._31;
	m._32 = r._31 * saved._12 + r._32 * saved._22 + saved._32;
	ctx->SetTransform(m);
}

// 画一圈虚线框。笔宽 1 逻辑像素，描边压在矩形中线上 —— 框正好落在 TextBox 的边缘
// 内侧，既不会被它的背景吃掉，也不会跑到框外面去。
//
// 内缩半个笔宽这件事看着像"随便缩一点"，其实正是原来那两个毛病的根：
//   · 不内缩时，1px 的描边中心线正好落在两个像素的交界缝上，抗锯齿只能把它摊到
//     相邻两行、每行各覆盖 50%。实线看不出来（两行各半，灰一点也还是条直边），
//     虚线就露馅了：2 实 2 虚的小段被摊成半透明之后，比空档亮不了多少 ——
//     于是"虚线框看着是断的、发灰、一段有一段没有"；
//   · 缩半个像素之后，中心线落在像素正中，1px 描边整整盖住一行像素（覆盖率 100%），
//     实线段才是笔刷的原色，虚线才连得上。
// 实测（WIC 位图目标 + 默认抗锯齿，dash 2:2）：不内缩时线段像素值 127（一半），
// 内缩后是 0（实色）。
void ShapeText::paintDashFrame(ID2D1DeviceContext* ctx)
{
	if (!ctx || !textBrush) return;
	ensureDashStyle();
	// 转过的框跟着一起转：不叠这一层，字是斜的、框还是正的
	D2D1_MATRIX_3X2_F saved{};
	applyRotation(ctx, saved);
	// 字号再小也留 1 像素：0 宽画不出线，而 dpi 在非常规缩放下可能是 0.x
	const float w = std::max(1.f, win->dpi);
	const float h = w * 0.5f;
	// 先把四条边对到整像素，再内缩半个笔宽：这样中心线正好落在像素正中，
	// 跟 rect 本来带不带小数无关（TextBox 的 x/y 是布局算出来的，可能是 12.53 这种）
	const D2D1_RECT_F q{
		std::floor(rect.left) + h,
		std::floor(rect.top) + h,
		std::floor(rect.right) - h,
		std::floor(rect.bottom) - h
	};
	// 框比笔宽还小（刚开始输入、TextBox 还没排出宽高时）画不出东西，直接跳过。
	// 注意这里已经把变换叠上去了，必须先还原再走
	if (q.right - q.left < w || q.bottom - q.top < w) {
		ctx->SetTransform(saved);
		return;
	}
	// dashedStrokeStyle 为空时 D2D 画实线：功能不丢，只是没虚线
	ctx->DrawRectangle(q, textBrush.Get(), w, dashedStrokeStyle.Get());
	ctx->SetTransform(saved);
}

void ShapeText::paint(ID2D1DeviceContext* ctx)
{
	if (isEditing) {
		// 编辑中文字由 TextBox 自己那层画，这里不重复画（重复的话会和它错位、还会糊在一起）。
		// 它的宽高跟着文本内容长，顺手把框的矩形同步过来 —— 本函数在 flex 排布之后才跑，取到的是本帧的值。
		syncFrameFromBox();
		// 虚线框编辑期间常显。原来只靠 paintDragger 在"悬停且未按下"时画：
		// 输入时鼠标一移开框就没了、移回来要等一次 hover 刷新才出现，时有时无还慢半拍。
		// 导出不受影响：导出必先点到工具条 → TextBox 失焦 → finishEdit，等真走离屏 paint 时已不在编辑态
		paintDashFrame(ctx);
		return;
	}
	// makeTextLayout 要等编辑结束才跑，这之前可能先来一次 paint
	if (!textLayout) return;
	// 文字按 angle 绕框中心转：叠一层变换画完再还原，导出走的是同一条路径，
	// 所以存出来的图里字也是转过的
	D2D1_MATRIX_3X2_F saved{};
	applyRotation(ctx, saved);
	// 文字在框里居中。以前原点写死成 rect.left/top + borderPadding：编辑那一会儿框刚好是
	// "文本 + 一圈内边距"，看着对得上；一旦用角柄放大，内边距不跟着放大，字就滑到框的
	// 左上角去了 —— 放得越大越明显（用户说的"放大后就很明显没在虚线框中心"），转过角度
	// 之后同一条偏移还会变成整体平移。按文本自身的度量算偏移，框多大、转多少度都居中
	DWRITE_TEXT_METRICS tm{};
	float tx = rect.left + borderPadding, ty = rect.top + borderPadding;
	if (SUCCEEDED(textLayout->GetMetrics(&tm))) {
		// 框被拖得比文字还小时 max(0,...) 兜住：宁可贴左上角，也不把字甩到框外
		tx = rect.left + std::max(0.f, (rect.right - rect.left - tm.widthIncludingTrailingWhitespace) * 0.5f);
		ty = rect.top + std::max(0.f, (rect.bottom - rect.top - tm.height) * 0.5f);
	}
	ctx->DrawTextLayout({ tx, ty }, textLayout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
	ctx->SetTransform(saved);
	// 拖旋转柄 / 角柄的过程中要看得见框在跟着走。宿主只在"悬停且没按下"时调 paintDragger，
	// 按下这段时间没有人画框 —— 在这里补一次，否则拖动全程没有任何反馈
	if (interacting) paintDashFrame(ctx);
}

// 把 TextBox 的实际盒子抄进 rect。TextBox 的 x/y/w/h 是窗口坐标（物理像素），
// rect 存的是底图坐标，差一个缩放倍数。
//
// 两个时机都要调：paint() 里调是为了保证每帧都跟手；onTextChanged 里也调一次是为了
// 让 rect 在"文本刚变"的那一刻就是新宽度 —— 这样虚线框不必等下一帧的布局结果，
// 也顺手让紧随其后的鼠标命中判定（mouseMove 用的就是 rect）不会拿到旧框
void ShapeText::syncFrameFromBox()
{
	auto tb = win->getTextBox();
	if (!tb) return;
	// 拖四角期间不让 TextBox 的自动尺寸改写 rect：这时 rect 是唯一的数据源
	// （scaleTo 按 rectStart 绕中心算出新框），而 TextBox 每换一次字号就会按文本
	// 重新自动排版、连带改自己的 w/h。两边都写同一件事，结果就是框边抖、中心漂
	// —— 表现是"拖角时尺寸像被谁抢回去"。松手后本条不再生效，恢复按 TextBox 同步
	if (interacting && hoverDraggerIndex >= kHitCornerTL && hoverDraggerIndex <= kHitCornerBL) return;
	// 只在 TextBox 真的排出尺寸之后才同步：startEdit 那一帧它还没布局，
	// w/h 还是 0，照着同步会把 rect 压成一条线 —— 表现就是"刚点下去没有虚线框"，
	// 要等下一次布局刷新才出现。保留上一次的矩形，框至少还在原地
	if (tb->w <= 0.f || tb->h <= 0.f) return;
	const float s = win->scale;
	rect = D2D1::RectF(tb->x / s, tb->y / s, (tb->x + tb->w) / s, (tb->y + tb->h) / s);
}

// 手柄画不画。
//   非编辑态：命中任意一档（框边 8 / 框里 1 / 四角 / 旋转柄）就给全套手柄 —— 老规矩；
//   编辑中  ：只有落在虚线框本身（框边 8、四角）或顶上旋转柄上才给。
//              刚点下去开始输入时鼠标本来就在框里（1），那阵子只该有一圈虚线框；
//              鼠标一移到框上，手柄立刻挂出来；再走远（0）就收起来
bool ShapeText::handlesVisible() const
{
	if (handleTrail) return true;
	if (isEditing) {
		return hoverDraggerIndex == 8
			|| (hoverDraggerIndex >= kHitCornerTL && hoverDraggerIndex <= kHitCornerBL)
			|| hoverDraggerIndex == kHitClose
			|| hoverDraggerIndex == kHitRotate;
	}
	return hoverDraggerIndex >= 0;
}

// history 里从后往前第一个不是 undo 的，就是画面最上层；自己是不是它
bool ShapeText::isLastShape() const
{
	if (!win->history) return false;
	for (int i{ (int)win->history->shapes.size() - 1 }; i >= 0; --i) {
		auto s = win->history->shapes[i].get();
		if (!s || s->isUndo) continue;
		return s == static_cast<const ShapeBase*>(this);
	}
	return false;
}

// 编辑中的字由 TextBox 那一层画，它是个轴对齐的真控件：要它跟着倾斜，
// 只能把角度写进它的 Composition visual（Node::setRotationDeg），旋转中心就是它自己的中心
void ShapeText::syncRotationToBox()
{
	auto tb = win->getTextBox();
	if (!tb) return;
	tb->setRotationDeg(angle * 180.f / 3.14159265358979f);
}

// 拖角缩放时把 rect / 字号推给正在编辑的 TextBox。
// TextBox 是自动尺寸的：字号一变它自己按新字号重排，但位置得由这里管住 ——
// 只改字号的话它会从左上角往右下长，中心越拉越偏（用户要的是绕中心缩放）。
// 内边距按同一比例放大：不跟着长，框就比字大一圈，中心同样会偏
void ShapeText::applyFrameToBox()
{
	auto tb = win->getTextBox();
	if (!tb) return;
	const float s = win->scale, d = win->dpi;
	const float kk = scaleStartFont > 0.f ? fontSize / scaleStartFont : 1.f;
	// 内边距先改：setFontSize 内部会按新字号重排并写回自动尺寸，那时就该带上新的内边距
	tb->setPadding(borderPadding * kk * s / d);
	tb->setFontSize(fontSize * s / d);
	tb->setPosition(ui::Edge::Left, rect.left * s / d);
	tb->setPosition(ui::Edge::Top, rect.top * s / d);
	// 顺手把布局值也写成新盒子（下一帧布局会用同样的值覆盖回来）：
	// 紧随其后的命中判定和画框都靠它，不能等下一帧还拿旧尺寸
	tb->x = rect.left * s;
	tb->y = rect.top * s;
	tb->w = (rect.right - rect.left) * s;
	tb->h = (rect.bottom - rect.top) * s;
}

// 旋转柄悬停态：不套圆盘，直接把"上下两个弯箭头"画出来（顺时针旋转符号）。
// 几何：左右各一段 150° 圆弧，缺的两道口子（正上方、正下方）各插一个实心箭头 ——
// 上面那个朝右、下面那个朝左，两头一凑就是"绕中心转"。
// 画在 rp 周围，尺寸和静息那个圆点同一个量级，鼠标压上去不会觉得柄"跳"了一下
namespace {
	// 往 sink 里塞一个指向 dir 的实心三角（tip 在 p 往前 hs 处）
	void addArrowHead(ID2D1GeometrySink* s, const D2D1_POINT_2F p,
		const D2D1_POINT_2F dir, const float hs)
	{
		const float len = std::hypot(dir.x, dir.y);
		if (!s || len <= 0.f) return;
		const float ux = dir.x / len, uy = dir.y / len;   // 箭头朝向
		const float px = -uy, py = ux;                    // 垂直方向
		const float bw = hs * 0.75f;                      // 底半宽
		const D2D1_POINT_2F tip{ p.x + ux * hs, p.y + uy * hs };
		const D2D1_POINT_2F b1{ p.x + px * bw, p.y + py * bw };
		const D2D1_POINT_2F b2{ p.x - px * bw, p.y - py * bw };
		s->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
		s->AddLine(b1);
		s->AddLine(b2);
		s->EndFigure(D2D1_FIGURE_END_CLOSED);
	}
}

void ShapeText::paintRotateArrows(ID2D1DeviceContext* ctx, const D2D1_POINT_2F rp)
{
	auto d2d = ui::D2D::get();
	if (!ctx || !d2d || !d2d->d2dFactory) return;
	const float r = handleRadius() * 1.05f;        // 弧半径
	const float lw = std::max(1.f, win->dpi) * 1.3f;
	const float hs = handleRadius() * 0.62f;       // 箭头长
	// 和四角同一套做法：蓝弧压在蓝色调的图上会糊成一片，先在底下垫一层白晕。
	// 白晕只是"描粗一圈再盖上蓝的"，不改形状 —— 用户明确不要"圆盘里塞箭头"那种
	const float halo = std::max(1.f, win->dpi);
	// 角度按屏幕坐标算（y 往下为正）：-90° 正上，90° 正下
	auto pt = [&](const float deg) {
		const float a = deg * 3.14159265358979f / 180.f;
		return D2D1::Point2F(rp.x + r * std::cos(a), rp.y + r * std::sin(a));
	};

	// 两段圆弧：右侧 -75°→75°（从上到下走外圈），左侧 105°→255°（从下到上）
	// 都留 30° 缺口，缺口正中就是两个箭头要插的位置
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> arcs;
	Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(d2d->d2dFactory->CreatePathGeometry(arcs.GetAddressOf()))
		|| FAILED(arcs->Open(sink.GetAddressOf()))) return;
	sink->BeginFigure(pt(-75.f), D2D1_FIGURE_BEGIN_HOLLOW);
	sink->AddArc(D2D1::ArcSegment(pt(75.f), D2D1::SizeF(r, r), 0.f,
		D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->BeginFigure(pt(105.f), D2D1_FIGURE_BEGIN_HOLLOW);
	sink->AddArc(D2D1::ArcSegment(pt(255.f), D2D1::SizeF(r, r), 0.f,
		D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->Close();
	// 先白后蓝：白的那遍描宽 2×halo，露在蓝弧外就成了一圈白晕
	if (brushHandleFill) ctx->DrawGeometry(arcs.Get(), brushHandleFill.Get(), lw + halo * 2.f);
	ctx->DrawGeometry(arcs.Get(), brushDragger.Get(), lw);

	// 两个实心箭头：上边朝右、下边朝左（顺时针）。不描边，纯填充，
	// 小尺寸下描边的三角会糊成一团
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> heads;
	Microsoft::WRL::ComPtr<ID2D1GeometrySink> hSink;
	if (FAILED(d2d->d2dFactory->CreatePathGeometry(heads.GetAddressOf()))
		|| FAILED(heads->Open(hSink.GetAddressOf()))) return;
	addArrowHead(hSink.Get(), pt(-90.f), D2D1::Point2F(1.f, 0.f), hs);
	addArrowHead(hSink.Get(), pt(90.f), D2D1::Point2F(-1.f, 0.f), hs);
	hSink->Close();
	// 箭头也垫白：白的那遍只"描边"，内圈随后被蓝色实心填掉，留在外面的就是白晕
	if (brushHandleFill) ctx->DrawGeometry(heads.Get(), brushHandleFill.Get(), halo * 2.f);
	ctx->FillGeometry(heads.Get(), brushDragger.Get());
}

// 角柄里那一笔。柄本身现在填成实色（半透明蓝），里面的图形得用白色画，
// 蓝底压蓝线什么也看不出来 —— 所以要 brushHandleFill（白）
void ShapeText::paintCornerGlyph(ID2D1DeviceContext* ctx, const D2D1_POINT_2F cp,
	const float side, const bool closeButton, const float slope)
{
	if (!ctx || !brushHandleFill) return;
	auto d2d = ui::D2D::get();
	const float lw = std::max(1.f, win->dpi) * 1.1f;
	if (closeButton) {
		// ×：两道对角线，留一点边距，撑满圆底但不顶到边
		const float o = side * 0.18f;
		ctx->DrawLine(D2D1::Point2F(cp.x - o, cp.y - o), D2D1::Point2F(cp.x + o, cp.y + o),
			brushHandleFill.Get(), lw);
		ctx->DrawLine(D2D1::Point2F(cp.x - o, cp.y + o), D2D1::Point2F(cp.x + o, cp.y - o),
			brushHandleFill.Get(), lw);
		return;
	}
	if (!d2d || !d2d->d2dFactory) return;
	// 45° 双头箭头：一根斜杆 + 两头各一个实心三角。
	// slope 跟这个角本身的对角线走向一致（左上/右下是 "\"，左下是 "/"），
	// 箭头顺着那条线指出去，看着就是"沿这个方向拉大小"
	constexpr float inv = 0.70710678f;
	const D2D1_POINT_2F dir{ inv, inv * slope };
	const float L = side * 0.26f;
	const D2D1_POINT_2F a{ cp.x - dir.x * L, cp.y - dir.y * L };
	const D2D1_POINT_2F b{ cp.x + dir.x * L, cp.y + dir.y * L };
	ctx->DrawLine(a, b, brushHandleFill.Get(), lw);
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> heads;
	Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(d2d->d2dFactory->CreatePathGeometry(heads.GetAddressOf()))
		|| FAILED(heads->Open(sink.GetAddressOf()))) return;
	const float hs = side * 0.22f;
	addArrowHead(sink.Get(), b, dir, hs);
	addArrowHead(sink.Get(), a, D2D1::Point2F(-dir.x, -dir.y), hs);
	sink->Close();
	ctx->FillGeometry(heads.Get(), brushHandleFill.Get());
}

void ShapeText::paintDragger(ID2D1DeviceContext* ctx)
{
	// 虚线框画在这里而不是 paint 里：导出图片走的是离屏 paint(ctx)，画在那边会被存进图里。
	// 本函数只在悬停/被点过 且没按下鼠标时调，正好是该显示提示框的时候。
	// 编辑中那圈框由 paint() 每帧画，这里不重复（重了也只是同一条线画两遍）
	if (!isEditing) paintDashFrame(ctx);
	// 手柄出来只看悬停：鼠标压在框上/框边/某个手柄上（或正走在通往旋转柄的走廊上）。
	// 编辑中另有一套更窄的规则 —— 鼠标得落到虚线框上才出，落在文字区域里不出（见 handlesVisible）。
	// 离开框就地收起 —— 用户的口径"离开虚线框这些按钮就会收起来"，
	// 所以没有"点过一下就钉住不落"那一档了
	if (!handlesVisible()) return;
	// 只有最上层那个才画手柄（被压住时画了也点不到）
	if (!isLastShape()) return;

	// 四角的手柄 + 顶上那只旋转柄。位置都按 angle 转过，和框保持同一个姿态。
	// 白底画刷只给静息的旋转圆点用（圆点压在图上，白底才看得清它是个"点"）。
	// 从"正在画的这个 ctx"建：之前是懒建在 ui::D2D::get()->deviceContext 上，
	// 那个 DC 到绘制时可能是空的（实测日志里 fill 一直是 0），于是画出来只剩一圈描边
	if (!brushHandleFill && ctx) {
		ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
			brushHandleFill.GetAddressOf());
	}
	const float lw = std::max(1.f, win->dpi);
	const float hr = handleRadius();

	// 旋转柄：静息态是一个白底圆点（保持原样）；
	// 鼠标压到它上面（kHitRotate）时不套圆盘了，直接把"旋转箭头"画出来：
	// 左右两段圆弧 + 上下两个箭头，一眼就是"绕中心转"。
	// 以前是"白圆盘里塞一个 ↻ 字形"，用户的口径是不要那种"圆内加个箭头"
	if (kShowRotateHandle) {
		const auto rp = rotHandlePos();
		const bool hot = (hoverDraggerIndex == kHitRotate);
		// 不画"框顶中点 → 柄"那根连线：用户认可的样子是柄自己悬在框上方，
		// 一根线拉着反而像多了条尾巴（他给的参考图里也没有线）
		if (!hot) {
			// 静息态：白底圆点 + 一圈描边（最初那版的样式）
			const float rdot = hr * 0.85f;
			if (brushHandleFill) {
				ctx->FillEllipse(D2D1::Ellipse(rp, rdot, rdot), brushHandleFill.Get());
			}
			ctx->DrawEllipse(D2D1::Ellipse(rp, rdot, rdot), brushDragger.Get(), lw * 1.4f);
		}
		else {
			paintRotateArrows(ctx, rp);
		}
	}

	// 四角：圆底 + 里面一笔 45° 双箭头 / ×。直径取 10*dpi（100% 缩放下 10px）：
	// 12*dpi 用户看着太大，9*dpi 又偏小，取中间
	const float side = 10.f * win->dpi;
	const float half = side * 0.5f;
	// 外面再垫一圈白：蓝圆压在蓝色调的图上（或任何同色背景）几乎看不见，
	// 白环把圆和背景隔开，不管底下是什么颜色都分得清。
	// 只加在视觉上，命中半径（handleRadius + 6*dpi）本来就比这圈大，不用改判定
	const float ring = std::max(1.f, win->dpi);
	for (int i = 0; i < 4; i++) {
		const auto cp = cornerPos(i);
		if (brushHandleFill) {
			ctx->FillEllipse(D2D1::Ellipse(cp, half + ring, half + ring), brushHandleFill.Get());
		}
		// 实心圆：用户觉得小方块改圆更顺眼（试一下效果）
		ctx->FillEllipse(D2D1::Ellipse(cp, half, half), brushDragger.Get());
		// 右上角是关闭（×），其余三个是缩放（斜 45° 的双箭头）
		paintCornerGlyph(ctx, cp, side, i == 1, i == 3 ? -1.f : 1.f);
	}
}


void ShapeText::mouseDrag(const float x, const float y)
{
	if (hoverDraggerIndex == 8) {
		// 平移用屏幕坐标的增量：转过的框也是"跟着手走"，不能先转回本地再算
		auto spanX{ x - pressX };
		auto spanY{ y - pressY };
		rect.left += spanX;
		rect.right += spanX;
		rect.top += spanY;
		rect.bottom += spanY;
		pressX = x;
		pressY = y;
		return;
	}
	if (hoverDraggerIndex == kHitRotate) {
		const auto c = center();
		const float a = std::atan2(y - c.y, x - c.x);
		angle = rotStartAngle + (a - rotGrabAngle);
		// 转到接近正方向时吸回 0：手抖差个零点几度，看着是正的、存出来却是斜的，
		// 那种"怎么也摆不正"的别扭劲就是这么来的
		constexpr float d2r = 3.14159265358979f / 180.f;
		const float deg = angle / d2r;
		const float snapped = std::round(deg / 90.f) * 90.f;
		if (std::abs(deg - snapped) < 3.f) angle = snapped * d2r;
		// 编辑中的字归 TextBox 那一层，角度得同时推给它，不然框转了、字还是正的
		if (isEditing) syncRotationToBox();
		win->refresh();
		return;
	}
	if (hoverDraggerIndex >= kHitCornerTL && hoverDraggerIndex <= kHitCornerBL) {
		// 对角拉大小 = 等比缩放：按下点离中心多远，就按那个比例放大缩小
		const auto c = center();
		const float fontBefore = fontSize;
		scaleTo(std::hypot(x - c.x, y - c.y) / scaleStartDist);
		// 字或框真的变了才重画：鼠标在一个像素里抖、或已经缩到头时，
		// 每来一次 move 都 RedrawWindow(RDW_UPDATENOW) 会跟 TextBox 那一层抢着刷，
		// 看着就是"拖角时一闪一闪"。没变就不刷，帧数立刻稳下来
		if (std::abs(fontSize - fontBefore) > 1e-3f) win->refresh();
	}
}

// 等比缩放：字号和框一起按 k 变。以 rectStart 为基准算，不累积误差；
// 字号钳在 10~180 物理像素之间，太小画不动、太大直接撑爆整张图
void ShapeText::scaleTo(const float k)
{
	constexpr float minFont = 10.f, maxFont = 180.f;
	const float lo = minFont * win->dpi / scaleStartFont;
	const float hi = maxFont * win->dpi / scaleStartFont;
	const float kk = std::clamp(k, lo, hi);
	fontSize = scaleStartFont * kk;
	// 跟下方那条字号滑条联动，但只在滑条值域内（逻辑像素 10~72）才回填：
	// 拖角放大到 72 以上时字号不夹，滑条定格在最大值 72 —— 之后再动滑条，
	// 字号才会被滑条的值接管（72、71、70 …）
	if (win->toolSub) {
		const float maxLogical = win->toolSub->sliderMaxOf(L"text");
		const float logical = fontSize / win->dpi;
		const bool over = logical > maxLogical;
		win->toolSub->setShapeSliderVal(L"text", over ? maxLogical * win->dpi : fontSize);
		// 记住滑条此刻显示的值（超上限时就是最大那一格），供 applyStyle 判断
		// "是滑条动了要接管字号"还是"只是换了颜色、字号别动"
		lastSliderPhysical = (over ? maxLogical : logical) * win->dpi;
	}
	// 字号不再被滑条夹回：超过 60 照旧生效，框按它重算（缩放仍绕中心，中心不动）
	const float scale = scaleStartFont > 0.f ? fontSize / scaleStartFont : 1.f;
	const float cx = (rectStart.left + rectStart.right) * 0.5f;
	const float cy = (rectStart.top + rectStart.bottom) * 0.5f;
	const float hw = (rectStart.right - rectStart.left) * 0.5f * scale;
	const float hh = (rectStart.bottom - rectStart.top) * 0.5f * scale;
	rect = D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh);
	// 布局要按新字号重排，否则屏幕上还是原来那么大
	makeTextLayout();
	// 编辑中屏幕上那行字是 TextBox 画的，字号/位置得推过去它才跟着变
	if (isEditing) applyFrameToBox();
}

void ShapeText::mouseUp(const float x, const float y)
{
	if (!interacting) return;
	interacting = false;
	win->refresh();
}

void ShapeText::mouseDown(const float x, const float y)
{
	if (hoverDraggerIndex == -1) { //首次创建
		// 文字的起笔点就是按下点，虚线框比它外扩一个 borderPadding
		rect = D2D1::RectF(x - borderPadding, y - borderPadding, x + borderPadding, y + borderPadding);
		hoverDraggerIndex = 1;
		// 记下落笔点：鼠标还赖在这儿的时候先不认四角（见 freshBox 的注释）
		freshBox = true;
		freshX = x;
		freshY = y;
		startEdit();
	}
	else if (hoverDraggerIndex == kHitRotate) {
		// 编辑中也接：旋转不打断输入 —— 角度推给 TextBox，字当场跟着斜
		// （以前这里先 finishEdit，理由是"TextBox 是轴对齐的控件，转不了"；
		//   现在 TextBox 自己会转，收笔那一步就去掉了）
		if (isEditing) {
			// 这一下是从 TextBox 手里要过来的：不聚焦、不挪光标、不拖选
			if (auto tb = win->getTextBox()) tb->swallowNextDown();
		}
		if (!isEditing && text.empty()) return;
		const auto c = center();
		rotStartAngle = angle;
		rotGrabAngle = std::atan2(y - c.y, x - c.x);
		interacting = true;
		win->refresh();
	}
	else if (hoverDraggerIndex >= kHitCornerTL && hoverDraggerIndex <= kHitCornerBL) {
		// 编辑中也接：只改字号（TextBox 换字号重排，输入不中断），不必先收笔
		if (isEditing) {
			if (auto tb = win->getTextBox()) tb->swallowNextDown();
		}
		if (!isEditing && text.empty()) return;
		rectStart = rect;
		scaleStartFont = fontSize > 0.f ? fontSize : 20.f * win->dpi;
		const auto c = center();
		scaleStartDist = std::max(1.f, std::hypot(x - c.x, y - c.y));
		interacting = true;
		win->refresh();
	}
	else if (hoverDraggerIndex == kHitClose) {
		// 右上角那个 ×：点一下直接关掉这个文本框。编辑中的话先把这一下从编辑层
		// 手里要过来（不聚焦、不挪光标）
		if (isEditing) {
			if (auto tb = win->getTextBox()) tb->swallowNextDown();
			finishEdit(); // 空文本在这一步已经排进"丢弃"队列（见 finishEdit）
		}
		if (text.empty()) return;
		// 有字：走撤销栈（标 isUndo），不要 erase —— 标记式撤销看不见被 erase 的东西，
		// 误点 × 之后按 Ctrl+Z 会退掉再往前那个标注，而不是刚删的这段字
		if (isLastShape()) win->history->undo();
		else removeLater();
	}
	else if (hoverDraggerIndex == 1) { //点在框里：继续编辑
		pressX = x;
		pressY = y;
		startEdit();
	}
	else if (hoverDraggerIndex == 8) { //点在边框上：结束编辑，准备拖动
		pressX = x;
		pressY = y;
		finishEdit();
	}
	else if (hoverDraggerIndex == 0) { //点在框外：结束编辑
		finishEdit();
	}
}

// 点到线段的距离。旋转柄那根短线在判定里当成一条"有宽度的走廊"用：
// 线上本来没有任何可命中的东西，鼠标顺着它往上走，走到半路就断了命中
static float distToSegment(const float px, const float py,
	const D2D1_POINT_2F a, const D2D1_POINT_2F b)
{
	const float vx = b.x - a.x, vy = b.y - a.y;
	const float wx = px - a.x, wy = py - a.y;
	const float len2 = vx * vx + vy * vy;
	float t = len2 > 0.f ? (wx * vx + wy * vy) / len2 : 0.f;
	t = std::clamp(t, 0.f, 1.f);
	const float dx = px - (a.x + t * vx), dy = py - (a.y + t * vy);
	return std::sqrt(dx * dx + dy * dy);
}

void ShapeText::mouseMove(const float x, const float y)
{
	hoverDraggerIndex = -1;
	nearHover = false;
	handleTrail = false;
	const float dpi = win->dpi;
	const float hr = handleRadius();
	// 鼠标离开落笔点超过几个像素，就不再是"刚点下去那一刻"了。
	// 之后哪怕框还是空的、只有一个光标，四角也照常判
	if (freshBox && std::hypot(x - freshX, y - freshY) > 4.f * dpi) freshBox = false;
	// 手柄的判定半径给得比画出来的大一圈。柄本来就小，旋转柄还挂在框顶外面：
	// 判定一紧，鼠标从框上往柄走的中途就会丢命中，框和手柄一起消失，
	// 于是只好"再点一下"把框重新叫出来 —— 这就是看着"要点一下才显示"的由来。
	// 现在框边（half）、框外靠近区（near）、手柄（hit）三段是连着盖住的，中途不断
	// 编辑中要收窄到"画出来那个方块"那么大：那阵子鼠标大半时间在框里打字，
	// 判定一宽，整片文字区都能算成角，刚点下去框一出来就挂满手柄
	const float hit = isEditing ? hr * 0.8f + 2.f * dpi : hr + 6.f * dpi;
	// 框那三档（边上 / 框里 / 编辑中移到框外）还是老规矩，只是先要把点转回未旋转的坐标系
	float lx = x, ly = y;
	toLocal(x, y, lx, ly);
	auto half{ borderPadding / 2.f + dpi };//多给一个 dpi，让判定范围宽松点
	// 注意别叫 near：windef.h 里 near/far 是空宏，会把这里的声明整行吃掉
	const float nearPad{ half + 8.f * dpi };
	const bool inBox{ lx >= rect.left - half && lx <= rect.right + half
		&& ly >= rect.top - half && ly <= rect.bottom + half };
	const bool inNear{ lx >= rect.left - nearPad && lx <= rect.right + nearPad
		&& ly >= rect.top - nearPad && ly <= rect.bottom + nearPad };
	// 四角柄。右上角（i==1）现在是关闭，命中码另给一档（kHitClose），
	// 其余三个照旧映射成 kHitCornerTL+i。
	// 编辑中鼠标是停在框里不动的（就是当初点下去那个位置），而框一直在往右长。
	// 刚开始那个空框只有一个字大，整块都落在角的判定范围里 —— 只有"鼠标还赖在落笔点上"
	// 那一刻要挡一挡，否则一按下去四角就全亮（用户不要这个）。
	// 鼠标一挪开（freshBox 清掉）就恢复正常：框再小、哪怕只有一个光标，四角也能拉
	const bool cornerTooSmall = isEditing && freshBox
		&& (rect.right - rect.left <= hit * 4.f || rect.bottom - rect.top <= hit * 4.f);
	// 手柄只在"自己是最上层"时才认命中 —— 被压住时手柄不画，
	// 认了就等于让一圈看不见的按钮把下面的点击吃掉
	const bool topmost = isLastShape();
	if (!cornerTooSmall && topmost) {
		for (int i = 0; i < 4; i++) {
			const auto cp = cornerPos(i);
			if (std::abs(x - cp.x) <= hit && std::abs(y - cp.y) <= hit) {
				hoverDraggerIndex = (i == 1) ? kHitClose : kHitCornerTL + i;
				return;
			}
		}
	}
	// 旋转柄
	const auto rp = rotHandlePos();
	if (kShowRotateHandle && topmost && std::hypot(x - rp.x, y - rp.y) <= hit) {
		hoverDraggerIndex = kHitRotate;
		return;
	}
	// 框顶中点 → 旋转柄那根连线，按一条走廊判（宽 = 柄半径）：
	// 鼠标沿这根线往上移，手感是连续的，不会走到一半框就没了。
	// 走廊上只亮框和手柄、不接点击（hoverDraggerIndex 仍留 -1），
	// 但 handleTrail 要置真 —— 否则那几像素里四角会闪掉一次。
	// 框里那一截不算：编辑中框可能小到整块都在走廊宽度以内，一算就把手柄叫出来了
	if (kShowRotateHandle && !inBox) {
		const auto topMid = toWorld((rect.left + rect.right) * 0.5f, rect.top);
		if (distToSegment(x, y, topMid, rp) <= hr) { nearHover = true; handleTrail = true; }
	}
	if (inBox)
	{
		if (lx <= rect.left + half || lx >= rect.right - half || ly <= rect.top + half || ly >= rect.bottom - half) {
			hoverDraggerIndex = 8;
		}
		else {
			hoverDraggerIndex = 1;
		}
	}
	else if (inNear) {
		// 框外一小圈：只把框亮出来，不挂手柄。
		// 编辑中还要把它当成"点在框外"（0）—— 不然这一下点击既不收笔、也不新建，
		// 会跑到"上一个还在编辑、又新起一个"那条乱路上
		nearHover = true;
		if (isEditing) hoverDraggerIndex = 0;
	}
	else if (isEditing) {
		// 编辑中即使鼠标移出去了也要让自己保持 shapeHover，
		// 这样下一次点击才会走到自己的 mouseDown（结束编辑），而不是新建一个文本
		hoverDraggerIndex = 0;
	}
}


void ShapeText::setCursor()
{
	if (hoverDraggerIndex == kHitRotate || hoverDraggerIndex == kHitClose) {
		// 旋转柄 / 关闭柄上只给系统手型。以前这里自绘过一支 32×32 的"旋转圆"，看着比图标还打眼，
		// 用户直接说丑 —— 图标本身已经把"能转 / 能关"说清楚了，光标不必再喊一遍
		SetCursor(LoadCursor(nullptr, IDC_HAND));
	}
	else if (hoverDraggerIndex >= kHitCornerTL && hoverDraggerIndex <= kHitCornerBL) {
		// 光标要跟框的实际姿态走，不能按"最初横平竖直"那一套固定分配：
		// 框转了 45°，原来看起来是 "\" 的那个角已经变成一条竖线，再给斜双箭头就很怪。
		// 做法：取这个角在本地的对角线方向（指向框外），按 angle 转过来，
		// 再按它在屏幕上的实际朝向挑最接近的那款双箭头
		const float dirX = (hoverDraggerIndex == kHitCornerTL
			|| hoverDraggerIndex == kHitCornerBL) ? -1.f : 1.f;
		const float dirY = (hoverDraggerIndex == kHitCornerTL
			|| hoverDraggerIndex == kHitCornerTR) ? -1.f : 1.f;
		const float cs = std::cos(angle), sn = std::sin(angle);
		const float wx = dirX * cs - dirY * sn;
		const float wy = dirX * sn + dirY * cs;
		// 0=横 1="\" 2=竖 3="/"，四个一循环（+4 再取模是为了吃下负角度）
		const int sector = (((int)std::lround(std::atan2(wy, wx) * 57.29577951f / 45.f)) % 4 + 4) % 4;
		switch (sector) {
		case 1:  SetCursor(LoadCursor(nullptr, IDC_SIZENWSE)); break;
		case 2:  SetCursor(LoadCursor(nullptr, IDC_SIZENS));   break;
		case 3:  SetCursor(LoadCursor(nullptr, IDC_SIZENESW)); break;
		default: SetCursor(LoadCursor(nullptr, IDC_SIZEWE));   break;
		}
	}
	else if (hoverDraggerIndex == 8) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
	else if (hoverDraggerIndex == 1) {
		SetCursor(LoadCursor(nullptr, IDC_IBEAM));
	}
	else if (hoverDraggerIndex == 0) {
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
	}
}

void ShapeText::startEdit()
{
	if (isEditing) return;
	isEditing = true;
	// 每次进入编辑都跟当前工具栏走：改了颜色/字号再点已有文本，就是要按新样式改
	setAttr();
	auto tb = win->getTextBox();
	auto d = win->dpi;
	// rect 是底图坐标，TextBox 是挂在窗口上的真控件、收的是逻辑像素，
	// 所以要先乘上缩放倍数（窗口坐标）再除以 dpi（逻辑像素）
	auto s = win->scale;
	tb->setPosition(ui::Edge::Left, rect.left * s / d);
	tb->setPosition(ui::Edge::Top, rect.top * s / d);
	// 顺手把布局坐标也写成本 shape 的矩形：TextBox 刚显示出来时 x/y/w/h 还是上一次的旧值，
	// 而紧跟着到来的鼠标事件要靠 isPosIn 判断该不该失焦，拿旧值会把刚拿到的焦点当场弹掉。
	// 下一帧 flex 引擎会用同样的位置把它们覆盖回来。
	tb->x = rect.left * s;
	tb->y = rect.top * s;
	tb->w = (rect.right - rect.left) * s;
	tb->h = (rect.bottom - rect.top) * s;
	tb->setColor(ui::Color(colorValue));
	tb->setCaretColor(ui::Color(colorValue));
	// 内边距也得跟着倍数走：borderPadding 是底图空间的，而 TextBox 的 padding 是窗口空间的
	// （Ui 层默认 6.f 逻辑像素），不乘倍数的话提交后文字会比编辑时偏出去 borderPadding*(scale-1)
	tb->setPadding(borderPadding * s / d);
	// fontSize 是底图上的物理像素，setFontSize 收逻辑像素（内部再乘 dpi），中间还差一个缩放倍数
	tb->setFontSize(fontSize * s / d);
	tb->setBold(isBold);
	tb->setItalic(isItalic);
	tb->setText(text);
	// 这段文字本来就是斜的（之前转过、收笔后再点进来编辑）：TextBox 得摆成同一个姿态，
	// 否则一进编辑它自己先"正"一下，字会跳一下。
	// 放在 setText 之后：旋转中心是它自己盒子的中心，尺寸得先按新文本定下来
	tb->setRotationDeg(angle * 180.f / 3.14159265358979f);
	tb->show();
	// 订阅放在 setText 之后：setText 自己也会触发 onTextChanged，不用理那一次
	textChangedTok = tb->onTextChanged.add([this](ui::TextBox*, const std::wstring& val) {
		text = val;
		// 文本一改，TextBox 已经在 setText 内部按新文本重排完宽高了（applyAutoSize），
		// 所以这里立刻就能拿到新尺寸，不必等下一帧的布局 —— 虚线框"打一个字长大一格"
		syncFrameFromBox();
		// 走 requestPaint 而不是 refresh：截图窗覆写了它，会当场 RedrawWindow(RDW_UPDATENOW)
		// 把这一帧画掉（不覆写的宿主就是普通 refresh）。这样框和文字落在同一次合成里，
		// 不会出现"字变了、框还是上一格"的半拍差
		win->requestPaint();
	});
	focusTok = tb->onFocusChanged.add([this](ui::TextBox*, bool focused) {
		// 点到别处、按 ESC、窗口失焦都会走到这儿
		if (!focused) finishEdit();
	});
	win->setEditingText(this);
	tb->focus();
	win->refresh();
}

void ShapeText::finishEdit()
{
	if (!isEditing) return;
	isEditing = false;
	auto tb = win->getTextBox();
	// 先摘订阅：下面的 blur 会再触发一次 onFocusChanged，不摘就会重入
	tb->onTextChanged.remove(textChangedTok);
	tb->onFocusChanged.remove(focusTok);
	textChangedTok = {};
	focusTok = {};
	text = tb->getText();
	tb->blur();
	tb->hide();
	// 角度复位：这个 TextBox 是共用的，下一个 ShapeText 进来编辑时不该继承上一段文字的姿态
	// （它要转的话 startEdit 会自己再推一次）
	tb->setRotationDeg(0.f);
	win->setEditingText(nullptr);
	makeTextLayout();
	win->refresh();
	if (text.empty()) {
		// 空文本不留痕：点一下没输入就走开，不该在 history 里攒一堆看不见的 shape。
		// 不能在这儿直接删 —— 本函数是从 shape 自己的事件回调里调进来的，删了后面还要用 this
		removeLater();
	}
}

// 延迟到下一轮消息循环再把自己从 history 摘掉：两个调用点（空文本收笔、点关闭 ×）
// 都发生在 shape 自己的事件回调里，当场 erase 会把 this 一起释放。
// 延迟期间窗口可能已经被关掉（history 随宿主一起析构），
// 所以先拿宿主在 App 窗口表里的位置做存活判断 —— 宿主析构时会把自己从表里摘掉，
// 那时候再解引用 win / history 就是 use-after-free
void ShapeText::removeLater()
{
	auto w = win;
	auto app = ui::App::get();
	if (!app) return;
	app->dq.TryEnqueue([w, self = this]() {
		auto app = ui::App::get();
		if (!app) return;
		if (std::find(app->windows.begin(), app->windows.end(), w) == app->windows.end()) return;
		if (!w->history) return;
		w->history->removeShape(self);
	});
}

void ShapeText::applyStyle()
{
	// 滑条值没变（换颜色 / 粗体 / 斜体）时不要用滑条重置字号：
	// 拖角放大到上限以上的字，不能因为调个颜色就被打回 72
	float sliderNow = -1.f;
	if (win->toolSub) sliderNow = win->toolSub->getSliderVal();
	const float fontBefore = fontSize;
	setAttr();
	if (sliderNow >= 0.f && sliderNow == lastSliderPhysical) fontSize = fontBefore;
	else lastSliderPhysical = sliderNow;
	if (isEditing) {
		// 正在输入：样式同时推给 TextBox，字由它画，不推过去就看不到变化
		if (auto tb = win->getTextBox()) {
			tb->setColor(ui::Color(colorValue));
			tb->setCaretColor(ui::Color(colorValue));
			tb->setFontSize(fontSize * win->scale / win->dpi);
			tb->setBold(isBold);
			tb->setItalic(isItalic);
		}
	}
	else {
		// 已经收笔：字归自己画。按新字号重排一遍，拖滑块时画面就跟着变
		makeTextLayout();
		// 字重排了，外面那个虚线框必须跟着变 —— 不然字变大变小、框还是原来那圈，
		// 松手后再点它想拉大小，框跟字对不上。尺寸 = 新排版尺寸 + 两侧内边距，
		// 中心保持不动（文字就是按"左上角 + 内边距"画在框里的，中心和框心重合，
		// 这样转过的文字调字号也不会自己飘走）
		if (textLayout) {
			DWRITE_TEXT_METRICS m{};
			if (SUCCEEDED(textLayout->GetMetrics(&m)) && m.width > 0.f && m.height > 0.f) {
				const float nw{ m.width + borderPadding * 2.f };
				const float nh{ m.height + borderPadding * 2.f };
				const auto c = center();
				rect = D2D1::RectF(c.x - nw * 0.5f, c.y - nh * 0.5f,
					c.x + nw * 0.5f, c.y + nh * 0.5f);
			}
		}
	}
	win->refresh();
}

void ShapeText::makeTextLayout()
{
	// 不折行，宽高都放开，与 TextBox 的 autoSize 一致
	textLayout = ui::D2D::get()->makeTextLayout(text, fontSize);
	if (!textLayout) return;
	textLayout->SetFontWeight(isBold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, { 0, INT_MAX });
	textLayout->SetFontStyle(isItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, { 0, INT_MAX });
}

void ShapeText::setAttr()
{
	auto toolSub = win->toolSub.get();
	// 工具条没建起来时（建窗失败 / ensureAnnotation 兜底路径）没有"当前样式"可取，
	// 用一份能画出来的默认值顶上 —— 直接解引用空指针就是一次崩溃
	if (!toolSub) {
		colorValue = 0xCF1322FF;
		color = ui::Color(colorValue).getD2DColor();
		fontSize = 12.f * win->dpi;
		isBold = false;
		isItalic = false;
		if (auto d2d = ui::D2D::get(); d2d && d2d->deviceContext) {
			d2d->deviceContext->CreateSolidColorBrush(color, textBrush.ReleaseAndGetAddressOf());
		}
		return;
	}
	colorValue = toolSub->getSelectedColorValue();
	color = toolSub->getSelectedColor();
	// getSliderVal 返回的已经是物理像素
	fontSize = toolSub->getSliderVal();
	isBold = toolSub->isTextBold;
	isItalic = toolSub->isTextItalic;
	// 设备不可用时 deviceContext 是空的：没有画刷就退化成"不画字"，
	// 比在新建文本这一步直接空指针崩溃好
	if (auto d2d = ui::D2D::get(); d2d && d2d->deviceContext) {
		d2d->deviceContext->CreateSolidColorBrush(color, textBrush.ReleaseAndGetAddressOf());
	}
}
