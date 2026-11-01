#include "UiPch.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <typeinfo>
#include "WinBase.h"
#include "Node.h"

namespace ui {

	Node::Node(WinBase* win) :win{ win }, node(FlNodeNew()), visual{ win->compositor.CreateSpriteVisual() }
	{
	}

	Node::~Node()
	{
		children.clear();
		FlNodeFree(node);
	}

	void Node::setId(const std::wstring& id)
	{
		this->id = id;
	}

	std::unique_ptr<Node> Node::detachChild(Node* child) {
		auto it = std::find_if(children.begin(), children.end(), [child](const std::unique_ptr<Node>& p) { return p.get() == child; });
		if (it == children.end()) return nullptr;
		FlNodeRemoveChild(node, child->node);
		visual.Children().Remove(child->visual);
		child->parent = nullptr;
		std::unique_ptr<Node> owned = std::move(*it);
		children.erase(it);
		return owned;
	}

	void Node::removeChild(Node* child) {
		// 返回值丢弃 == 立即销毁该子节点（unique_ptr 临时对象析构）。
		if (detachChild(child)) win->refresh();
	}
	void Node::removeAllChildren()
	{
		// 不能写成 for (auto& c : children) detachChild(c.get()); ——
		// detachChild 内部会 children.erase()，vector 的 erase 使被删位置及其之后
		// 的迭代器全部失效，range-for 的 ++ 于是跳过下一个元素（每两个只删掉一个），
		// 且最后一次 erase 后可能越过 end()，是 UB。
		// 改成：先把 visual 的挂接逐个摘掉，最后一次性清空容器。
		for (auto& child : children) {
			FlNodeRemoveChild(node, child->node);
			visual.Children().Remove(child->visual);
			child->parent = nullptr;
		}
		// unique_ptr 析构即销毁子节点；clear 不触碰迭代器，安全。
		children.clear();
		win->refresh();
	}
	void Node::setChild(Node* child)
	{
		child->parent = this;
		visual.Children().InsertAtTop(child->visual);
		FlNodeInsertChild(this->node, child->node, FlNodeGetChildCount(this->node));
		// 保证边框始终盖在所有子节点之上（如果启用了边框）
		if (borderVisual) {
			visual.Children().Remove(borderVisual);
			visual.Children().InsertAtTop(borderVisual);
		}
	}
	bool Node::isPosIn(POINT pos)
	{
		// 转过的节点：先把点绕中心反向转回来，再按轴对齐的盒子判（见 setRotationDeg）
		float lx = (float)pos.x, ly = (float)pos.y;
		inverseRotate((float)pos.x, (float)pos.y, lx, ly);
		if (lx > x && lx<x + w && ly>y && ly < y + h) {
			return true;
		}
		return false;
	}

	void Node::setRotationDeg(float deg)
	{
		if (deg == rotationDeg) return;
		rotationDeg = deg;
		visual.RotationAngleInDegrees(deg);
		// 旋转中心固定在自己盒子的中心（局部坐标）。盒子的宽高在 layout() 里还会变，
		// 那里会再同步一次，所以这里写 0 尺寸也算数不出来 —— 只是过渡一帧
		visual.CenterPoint({ w * 0.5f, h * 0.5f, 0.0f });
		win->refresh();
	}

	void Node::inverseRotate(float px, float py, float& ox, float& oy) const
	{
		if (rotationDeg == 0.f) { ox = px; oy = py; return; }
		const float cx = x + w * 0.5f, cy = y + h * 0.5f;
		const float a = -rotationDeg * 3.14159265358979f / 180.f;
		const float cs = std::cos(a), sn = std::sin(a);
		const float dx = px - cx, dy = py - cy;
		ox = cx + dx * cs - dy * sn;
		oy = cy + dx * sn + dy * cs;
	}

	void Node::rotatePoint(float px, float py, float& ox, float& oy) const
	{
		if (rotationDeg == 0.f) { ox = px; oy = py; return; }
		const float cx = x + w * 0.5f, cy = y + h * 0.5f;
		const float a = rotationDeg * 3.14159265358979f / 180.f;
		const float cs = std::cos(a), sn = std::sin(a);
		const float dx = px - cx, dy = py - cy;
		ox = cx + dx * cs - dy * sn;
		oy = cy + dx * sn + dy * cs;
	}
	void Node::hide()
	{
		visual.IsVisible(false);
		FlStyleSetDisplay(node, true);
		win->refresh();
	}

	void Node::show()
	{
		visual.IsVisible(true);
		FlStyleSetDisplay(node, false);
		win->refresh();
	}

	void Node::setBg(const Color& color)
	{
		bgColor = color;
		visual.Brush(win->compositor.CreateColorBrush(color.getUIColor()));
	}

	void Node::layout()
	{
		// 物理像素贴整（ink snapping）—— 整棵树在这里统一处理，只此一处。
		// 根因：合成器把 surface brush 贴到 visual 时，按 visual 的**绝对**设备坐标
		// 对 surface 做双线性重采样。布局引擎给的是浮点坐标：pad=1.5 的格子居中就有
		// 1.5px 偏移，flex 均分（setFlexGrow(1)）更是 166.67 这种小数。只要祖先链上
		// 任意一环的局部 offset 是小数，绝对位置就是小数，子树里所有 surface
		// （图标、画布）都会被重采样抹糊 —— 这才是"图标永远发虚"的根子。
		// 以前只在 Text 层圆整过自己那一层，父节点（Button/工具栏行）的小数偏移没圆，
		// 15px 的图标照样被采样，所以看着"修了还是糊"。
		// 每个 visual 的 offset 都是相对父节点的局部值：逐层贴整，绝对位置（逐层累加）
		// 恒为整数，重采样从此不会发生。w/h 一并贴整，是为了让 surface 的整数尺寸
		// （Text/Canvas 用 (int)w 建）与 visual.Size 严格相等，Stretch::None 下 1:1，
		// 不出现亚像素裁切或露边。
		x = std::round(FlLayoutGetLeft(node));
		y = std::round(FlLayoutGetTop(node));
		w = std::round(FlLayoutGetWidth(node));
		h = std::round(FlLayoutGetHeight(node));
		visual.Offset({ x, y, 0.0f });
		visual.Size({ w, h });
		// 转过的话，旋转中心跟着盒子走：TextBox 编辑中宽高随输入变，中心不跟着更新就会越转越偏
		if (rotationDeg != 0.f) visual.CenterPoint({ w * 0.5f, h * 0.5f, 0.0f });
		if (parent) {
			x = parent->x + x;
			y = parent->y + y;
		}
		syncChrome();
		for (auto& child : children) {
			child->layout();
		}
	}

	void Node::applyDpiChange()
	{
		const float d = win->dpi;
		if (width)  FlStyleSetWidth (node, *width  * d);
		if (height) FlStyleSetHeight(node, *height * d);
		if (margin[0])  FlStyleSetMargin(node, Edge::Left,   *margin[0]  * d);
		if (margin[1])  FlStyleSetMargin(node, Edge::Top,    *margin[1]  * d);
		if (margin[2])  FlStyleSetMargin(node, Edge::Right,  *margin[2]  * d);
		if (margin[3])  FlStyleSetMargin(node, Edge::Bottom, *margin[3]  * d);
		if (padding[0]) FlStyleSetPadding(node, Edge::Left,   *padding[0] * d);
		if (padding[1]) FlStyleSetPadding(node, Edge::Top,    *padding[1] * d);
		if (padding[2]) FlStyleSetPadding(node, Edge::Right,  *padding[2] * d);
		if (padding[3]) FlStyleSetPadding(node, Edge::Bottom, *padding[3] * d);
		if (edges[0]) FlStyleSetPosition(node, Edge::Left, *edges[0] * win->dpi);
		if (edges[1]) FlStyleSetPosition(node, Edge::Top, *edges[1] * win->dpi);
		if (edges[2]) FlStyleSetPosition(node, Edge::Right, *edges[2] * win->dpi);
		if (edges[3]) FlStyleSetPosition(node, Edge::Bottom, *edges[3] * win->dpi);
		onDpiChanged();
		for (auto& child : children) {
			child->applyDpiChange();
		}
	}

	void Node::setFlexGrow(float val)
	{
		FlStyleSetFlexGrow(node, val);
	}

	void Node::setFlexShrink(float val)
	{
		FlStyleSetFlexShrink(node, val);
	}

	void Node::setWidth(float w)
	{
		width = w;
		FlStyleSetWidth(node, w * win->dpi);
	}
	void Node::setHeight(float h)
	{
		height = h;
		FlStyleSetHeight(node, h * win->dpi);
	}

	void Node::setSize(float w, float h)
	{
		setWidth(w);
		setHeight(h);
	}

	void Node::setWidthPercent(float percent)
	{
		// 百分比必须能盖掉之前 setWidth 定下的固定值：Layout.cpp 的 resolveDim 里
		// hasWidth 优先于 widthPercent，不清这个标记，百分比就是空操作。
		// 实际踩到的就是 Slider —— 构造函数里 setHeight(20)，ToolSub 再 setHeightPercent(100)，
		// 结果滑块一直是 20 高，轨道中心比同排按钮高 5px（实测 13.5 vs 18.5）
		if (node) node->hasWidth = false;
		// 百分比不随 dpi 变化，直接透传给布局引擎
		FlStyleSetWidthPercent(node, percent);
	}

	void Node::setHeightPercent(float percent)
	{
		if (node) node->hasHeight = false;
		FlStyleSetHeightPercent(node, percent);
	}

	void Node::setSizePercent(float w, float h)
	{
		if (node) { node->hasWidth = false; node->hasHeight = false; }
		FlStyleSetWidthPercent(node, w);
		FlStyleSetHeightPercent(node, h);
	}

	void Node::setMargin(float val)
	{
		margin[0] = val;
		margin[1] = val;
		margin[2] = val;
		margin[3] = val;
		FlStyleSetMargin(node, Edge::All, val * win->dpi);
	}

	void Node::setMargin(float left, float top, float right, float bottom)
	{
		setMarginLeft(left);
		setMarginTop(top);
		setMarginRight(right);
		setMarginBottom(bottom);
	}

	float Node::getMarginLeft()   { return margin[0].value_or(0.f); }
	float Node::getMarginTop()    { return margin[1].value_or(0.f); }
	float Node::getMarginRight()  { return margin[2].value_or(0.f); }
	float Node::getMarginBottom() { return margin[3].value_or(0.f); }

	void Node::setMarginLeft(float val)
	{
		margin[0] = val;
		FlStyleSetMargin(node, Edge::Left, val * win->dpi);
	}

	void Node::setMarginTop(float val)
	{
		margin[1] = val;
		FlStyleSetMargin(node, Edge::Top, val * win->dpi);
	}

	void Node::setMarginRight(float val)
	{
		margin[2] = val;
		FlStyleSetMargin(node, Edge::Right, val * win->dpi);
	}

	void Node::setMarginBottom(float val)
	{
		margin[3] = val;
		FlStyleSetMargin(node, Edge::Bottom, val * win->dpi);
	}

	void Node::setPadding(float val)
	{
		padding[0] = val;
		padding[1] = val;
		padding[2] = val;
		padding[3] = val;
		FlStyleSetPadding(node, Edge::All, val * win->dpi);
	}

	void Node::setPadding(float left, float top, float right, float bottom)
	{
		setPaddingLeft(left);
		setPaddingTop(top);
		setPaddingRight(right);
		setPaddingBottom(bottom);
	}

	void Node::setPaddingLeft(float val)
	{
		padding[0] = val;
		FlStyleSetPadding(node, Edge::Left, val * win->dpi);
	}

	void Node::setPaddingTop(float val)
	{
		padding[1] = val;
		FlStyleSetPadding(node, Edge::Top, val * win->dpi);
	}

	void Node::setPaddingRight(float val)
	{
		padding[2] = val;
		FlStyleSetPadding(node, Edge::Right, val * win->dpi);
	}

	void Node::setPaddingBottom(float val)
	{
		padding[3] = val;
		FlStyleSetPadding(node, Edge::Bottom, val * win->dpi);
	}

	float Node::getPaddingLeft()   { return padding[0].value_or(0.f); }
	float Node::getPaddingTop()    { return padding[1].value_or(0.f); }
	float Node::getPaddingRight()  { return padding[2].value_or(0.f); }
	float Node::getPaddingBottom() { return padding[3].value_or(0.f); }

	std::tuple<float, float, float, float>  Node::getPadding() {
		return std::make_tuple(
			padding[0].value_or(0.f),
			padding[1].value_or(0.f),
			padding[2].value_or(0.f),
			padding[3].value_or(0.f)
		);
	}
	void Node::setPositionType(const Position val)
	{
		FlStyleSetPositionType(node, (Position)val);
	}

	void Node::setPosition(Edge edge, float val)
	{
		if (edge == Edge::Left) {
			edges[0] = val;
		}
		else if (edge == Edge::Top) {
			edges[1] = val;
		}
		else if (edge == Edge::Right) {
			edges[2] = val;
		}
		else if (edge == Edge::Bottom) {
			edges[3] = val;
		}
		FlStyleSetPosition(node, (Edge)edge, val*win->dpi);
	}

	void Node::setAlignItems(const Align val)
	{
		FlStyleSetAlignItems(node, (Align)val);
	}

	void Node::setFlexWrap(const Wrap val)
	{
		FlStyleSetFlexWrap(node, (Wrap)val);
	}
	void Node::setJustifyContent(const Justify val)
	{
		FlStyleSetJustifyContent(node, (Justify)val);
	}
	void Node::setFlexDirection(const FlexDirection flexDirection)
	{
		FlStyleSetFlexDirection(node, (FlexDirection)flexDirection);
	}

	// ---- 圆角 & 边框 ----------------------------------------------------------
	// 设计：
	//  - 圆角用 GeometricClip(RoundedRectangleGeometry) 挂到 visual 上 —— 背景
	//    Brush（setBg 设的 ColorBrush）和子节点会一起被裁剪。
	//  - 边框用 ShapeVisual + SpriteShape 描一圈 stroke。Composition 的 stroke
	//    以几何路径为中线两侧扩散 —— 要让 stroke 外沿贴 (0,0)-(w,h)，几何本身
	//    需要向内 inset borderW/2；对应地，几何的圆角半径也要减去 borderW/2。
	//  - 真正把物理像素写进 Composition 的动作集中在 syncChrome()，每次 layout
	//    以及 setter 触发时调用；DPI 变化后 WinBase 会 relayout，无需额外挂钩。
	void Node::setBorderRadius(float r)
	{
		cornerRadius = r;
		if (r > 0.f && !clipGeo) {
			clipGeo = win->compositor.CreateRoundedRectangleGeometry();
			visual.Clip(win->compositor.CreateGeometricClip(clipGeo));
		}
		else if (r <= 0.f && clipGeo) {
			// 关掉圆角：把 clip 清掉，几何对象也丢，syncChrome 才不会再更新它
			visual.Clip(winrt::Windows::UI::Composition::CompositionClip{ nullptr });
			clipGeo = nullptr;
		}
		syncChrome();
		win->refresh();
	}

	void Node::setBorder(float width, const Color& color)
	{
		// 走 virtual：子类（如 Button）可以在此缓存"正常态"颜色。
		// 顺序：先 setBorderColor（此时 borderShape 可能还没创建，Node 版本只更新成员），
		//      再 setBorderWidth（创建 shape 时会读 borderColor 做首刷）。
		setBorderColor(color);
		setBorderWidth(width);   // 内部会走 syncChrome
	}

	void Node::setBorderWidth(float width)
	{
		borderWidth = width;
		if (width > 0.f && !borderVisual) {
			borderVisual = win->compositor.CreateShapeVisual();
			borderGeo = win->compositor.CreateRoundedRectangleGeometry();
			borderShape = win->compositor.CreateSpriteShape(borderGeo);
			borderShape.StrokeBrush(win->compositor.CreateColorBrush(borderColor.getUIColor()));
			borderVisual.Shapes().Append(borderShape);
			// 顶层：保证边框盖在所有子节点之上
			visual.Children().InsertAtTop(borderVisual);
		}
		else if (width <= 0.f && borderVisual) {
			visual.Children().Remove(borderVisual);
			borderVisual = nullptr;
			borderGeo = nullptr;
			borderShape = nullptr;
		}
		syncChrome();
		win->refresh();
	}

	void Node::setBorderColor(const Color& color)
	{
		borderColor = color;
		if (borderShape) {
			borderShape.StrokeBrush(win->compositor.CreateColorBrush(color.getUIColor()));
			win->refresh();
		}
	}



	void Node::syncChrome()
	{
		if (w <= 0.f || h <= 0.f) return;
		const float d = win->dpi;
		if (clipGeo) {
			// 圆角剪切：clip 相对 visual 自身坐标系 (0,0)-(w,h)
			const float r = std::min({ cornerRadius * d, w * 0.5f, h * 0.5f });
			clipGeo.Size({ w, h });
			clipGeo.CornerRadius({ r, r });
		}

		if (borderVisual) {
			const float bw = borderWidth * d;
			const float inset = bw * 0.5f;
			// stroke 以路径为中线双向扩散，路径内缩 bw/2 才能让外沿贴 (0,0)-(w,h)
			borderVisual.Size({ w, h });
			borderVisual.Offset({ 0.f, 0.f, 0.f });
			// Geometry 无 Offset 属性，把位移写在 Shape 上（CompositionShape.Offset）
			borderShape.Offset({ inset, inset });
			borderGeo.Size({ std::max(0.f, w - bw), std::max(0.f, h - bw) });
			// 圆角边框：外圆角 = cornerRadius，描边中线半径 = outerR - bw/2
			const float outerR = std::min({ cornerRadius * d, w * 0.5f, h * 0.5f });
			const float midR = std::max(0.f, outerR - inset);
			borderGeo.CornerRadius({ midR, midR });
			borderShape.StrokeThickness(bw);
		}
	}
	std::weak_ptr<bool> Node::getWeakThis()
	{
		return std::weak_ptr<bool>(alive);
	}
}

