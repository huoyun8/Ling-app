#pragma once
// 布局相关的枚举与极简 flex 引擎（窗口/控件的对齐、位置、换行等样式项集中在下面）。
#include <limits>

namespace ui {
	constexpr float SizeNaN = std::numeric_limits<float>::quiet_NaN();

	enum class Align
	{
		Auto,
		FlexStart,
		Center,
		FlexEnd,
		Stretch,
		Baseline,
		SpaceBetween,
		SpaceAround,
		SpaceEvenly
	};

	enum class Edge
	{
		Left,
		Top,
		Right,
		Bottom,
		Start,
		End,
		Horizontal,
		Vertical,
		All
	};

	enum class FlexDirection
	{
		Column,
		ColumnReverse,
		Row,
		RowReverse
	};

	enum class Justify
	{
		Auto,
		Start,
		Center,
		End,
		SpaceBetween,
		SpaceAround,
		SpaceEvenly
	};

	enum class Position
	{
		Static,
		Relative,
		Absolute
	};

	enum class Wrap
	{
		NoWrap,
		Wrap,
		WrapReverse
	};
}

// 只覆盖本项目实际用到的子集：主轴 row/column、justify（起止/居中/两侧分散）、
// align（起止/居中/stretch）、grow/shrink、margin/padding、百分比尺寸、绝对定位、
// overflow:scroll 的"允许子节点溢出"语义。wrap 未实现（按 NoWrap 处理）。
//
// 每个节点持有一份 style，CalculateLayout 一次性算出
// left/top/width/height（相对父节点边框盒）。这样上层 Node::layout 的写法
// 与原先逐行对应，日后与上游合并时 diff 很小。

#include <vector>
#include <limits>

namespace ui {
	struct FlNode;
	using FlNodeRef = FlNode*;
	using FlNodeConstRef = const FlNode*;

	struct FlSize { float width{ 0.f }, height{ 0.f }; };
	// 取值口径与旧实现一致，便于对照
	enum class FlMeasureMode { Undefined = 0, Exactly = 1, AtMost = 2 };
	using FlMeasureFn = FlSize(*)(FlNodeConstRef node, float width, FlMeasureMode widthMode, float height, FlMeasureMode heightMode);

	struct FlNode
	{
		// ---- style ----
		bool hasWidth{ false }, hasHeight{ false };
		float width{ 0.f }, height{ 0.f };
		// 百分比尺寸。NaN = 未设置；0 视同未设置
		float widthPercent{ std::numeric_limits<float>::quiet_NaN() };
		float heightPercent{ std::numeric_limits<float>::quiet_NaN() };
		bool hasMinHeight{ false };
		float minHeight{ 0.f };
		float margin[4]{ 0.f, 0.f, 0.f, 0.f };   // left, top, right, bottom
		float padding[4]{ 0.f, 0.f, 0.f, 0.f };
		float pos[4]{ 0.f, 0.f, 0.f, 0.f };
		bool hasPos[4]{ false, false, false, false };
		FlexDirection dir{ FlexDirection::Column };
		Align alignItems{ Align::Stretch };
		Justify justifyContent{ Justify::Auto };
		Wrap wrap{ Wrap::NoWrap };
		Position positionType{ Position::Relative };
		float flexGrow{ 0.f };
		float flexShrink{ 0.f };   // 默认 0：不主动压缩
		bool displayNone{ false };
		bool overflowScroll{ false };

		// ---- 树 ----
		FlNode* parent{ nullptr };
		std::vector<FlNode*> children;

		// ---- 测量 ----
		void* context{ nullptr };
		FlMeasureFn measureFn{ nullptr };

		// ---- 布局结果（相对父节点左上角，物理像素）----
		float left{ 0.f }, top{ 0.f }, w{ 0.f }, h{ 0.f };
	};

	FlNodeRef FlNodeNew();
	void FlNodeFree(FlNodeRef node);
	void FlNodeInsertChild(FlNodeRef parent, FlNodeRef child, size_t index);
	void FlNodeRemoveChild(FlNodeRef parent, FlNodeRef child);
	size_t FlNodeGetChildCount(FlNodeConstRef node);

	void FlNodeSetContext(FlNodeRef node, void* ctx);
	void* FlNodeGetContext(FlNodeConstRef node);
	void FlNodeSetMeasureFunc(FlNodeRef node, FlMeasureFn fn);
	// 标脏即"下一轮布局重新问测量函数"。
	// 本实现每次布局都会重新测量，因此这里只需要触发窗口刷新（调用方已做），
	// 留一个空实现是为了让上层代码逐行不变。
	void FlNodeMarkDirty(FlNodeRef node);

	// 计算整棵树。w/h 是根节点的边框盒尺寸（物理像素）。
	void FlNodeCalculateLayout(FlNodeRef root, float w, float h);

	float FlLayoutGetLeft(FlNodeConstRef node);
	float FlLayoutGetTop(FlNodeConstRef node);
	float FlLayoutGetWidth(FlNodeConstRef node);
	float FlLayoutGetHeight(FlNodeConstRef node);

	// ---- style 设置 ----
	void FlStyleSetWidth(FlNodeRef node, float val);
	void FlStyleSetHeight(FlNodeRef node, float val);
	void FlStyleSetWidthPercent(FlNodeRef node, float percent);
	void FlStyleSetHeightPercent(FlNodeRef node, float percent);
	void FlStyleSetMinHeight(FlNodeRef node, float val);
	void FlStyleSetMargin(FlNodeRef node, Edge edge, float val);
	void FlStyleSetPadding(FlNodeRef node, Edge edge, float val);
	void FlStyleSetPosition(FlNodeRef node, Edge edge, float val);
	void FlStyleSetPositionType(FlNodeRef node, Position val);
	void FlStyleSetDisplay(FlNodeRef node, bool none);
	void FlStyleSetOverflowScroll(FlNodeRef node);
	void FlStyleSetFlexGrow(FlNodeRef node, float val);
	void FlStyleSetFlexShrink(FlNodeRef node, float val);
	void FlStyleSetFlexDirection(FlNodeRef node, FlexDirection val);
	void FlStyleSetFlexWrap(FlNodeRef node, Wrap val);
	void FlStyleSetJustifyContent(FlNodeRef node, Justify val);
	void FlStyleSetAlignItems(FlNodeRef node, Align val);
}
