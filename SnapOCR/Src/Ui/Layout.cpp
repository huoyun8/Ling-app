#include "Layout.h"
#include <algorithm>
#include <cmath>

namespace ui {

	namespace {
		constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

		inline bool valid(float v) { return !std::isnan(v); }

		// 百分比为 0 视同未设置
		float resolveDim(bool has, float px, float pct, float avail)
		{
			if (has) return px;
			if (!std::isnan(pct) && pct > 0.f && valid(avail)) return avail * pct / 100.f;
			return kNaN;
		}

		bool isRow(const FlNode* n)
		{
			return n->dir == FlexDirection::Row || n->dir == FlexDirection::RowReverse;
		}

		bool isReverse(const FlNode* n)
		{
			return n->dir == FlexDirection::ColumnReverse || n->dir == FlexDirection::RowReverse;
		}

		void layoutNode(FlNode* n, float availW, float availH, float forceW, float forceH);

		// 主轴方向上的外尺寸（含 margin）
		// display:none 的节点一律按 0 占位 —— 连 margin 也不许算。
		// 只把它的 w/h 清 0 是不够的：layoutNode 里那句置 0 管不到父子之间的间距，
		// 外层仍会照 outerMain 把它的 margin 累进 cursor。隐藏项一多，整排内容就被整体推右
		// （四颗隐藏按钮各带 1.5+1.5 的留白，凑起来 15px），最后一颗按钮被窗口右边框切掉一条。
		float outerMain(const FlNode* c, bool row)
		{
			if (c->displayNone) return 0.f;
			return row ? c->w + c->margin[0] + c->margin[2]
				: c->h + c->margin[1] + c->margin[3];
		}

		float outerCross(const FlNode* c, bool row)
		{
			if (c->displayNone) return 0.f;
			return row ? c->h + c->margin[1] + c->margin[3]
				: c->w + c->margin[0] + c->margin[2];
		}

		void layoutNode(FlNode* n, float availW, float availH, float forceW, float forceH)
		{
			if (n->displayNone) {
				n->left = n->top = n->w = n->h = 0.f;
				for (auto* c : n->children) c->left = c->top = c->w = c->h = 0.f;
				return;
			}

			float w = resolveDim(n->hasWidth, n->width, n->widthPercent, availW);
			float h = resolveDim(n->hasHeight, n->height, n->heightPercent, availH);
			if (valid(forceW)) w = forceW;
			if (valid(forceH)) h = forceH;

			const float padL = n->padding[0], padT = n->padding[1];
			const float padR = n->padding[2], padB = n->padding[3];

			// 内容测量：只在有维度未定时问（measureFunc 调用时机）
			if (n->measureFn && (!valid(w) || !valid(h))) {
				const float mw = valid(w) ? w : availW;
				const float mh = valid(h) ? h : availH;
				const FlMeasureMode mwMode = valid(w)
					? FlMeasureMode::Exactly
					: (valid(availW) ? FlMeasureMode::AtMost : FlMeasureMode::Undefined);
				const FlMeasureMode mhMode = valid(h)
					? FlMeasureMode::Exactly
					: (valid(availH) ? FlMeasureMode::AtMost : FlMeasureMode::Undefined);
				const FlSize s = n->measureFn(n, mw, mwMode, mh, mhMode);
				if (!valid(w)) w = s.width;
				if (!valid(h)) h = s.height;
			}

			float innerW = valid(w) ? std::max(0.f, w - padL - padR) : kNaN;
			float innerH = valid(h) ? std::max(0.f, h - padT - padB) : kNaN;

			if (!n->children.empty()) {
				const bool row = isRow(n);
				std::vector<FlNode*> flow, absolute;
				for (auto* c : n->children) {
					(c->positionType == Position::Absolute ? absolute : flow).push_back(c);
				}

				// 第一遍：按父容器内容区摆放，取得各自的内容尺寸
				for (auto* c : flow) layoutNode(c, innerW, innerH, kNaN, kNaN);

				// 容器自身尺寸为 auto 时，按子节点反推：主轴求和、交叉轴取最大
				const bool autoW = !valid(w), autoH = !valid(h);
				if (autoW || autoH) {
					float sumMain = 0.f, maxCross = 0.f;
					for (auto* c : flow) {
						sumMain += outerMain(c, row);
						maxCross = std::max(maxCross, outerCross(c, row));
					}
					if (autoW) w = (row ? sumMain + padL + padR : maxCross + padL + padR);
					if (autoH) h = (row ? maxCross + padT + padB : sumMain + padT + padB);
					innerW = std::max(0.f, w - padL - padR);
					innerH = std::max(0.f, h - padT - padB);
				}
				if (n->hasMinHeight && h < n->minHeight) h = n->minHeight;

				const float innerMain = row ? innerW : innerH;
				const float innerCross = row ? innerH : innerW;

				// 第二遍：grow / shrink / stretch，尺寸有变化就按新尺寸重排该子节点
				float usedMain = 0.f, totalGrow = 0.f, totalShrinkWeight = 0.f;
				for (auto* c : flow) {
					usedMain += outerMain(c, row);
					totalGrow += c->flexGrow;
					totalShrinkWeight += c->flexShrink * (row ? c->w : c->h);
				}
				const float free = valid(innerMain) ? innerMain - usedMain : kNaN;
				for (auto* c : flow) {
					float mainSize = row ? c->w : c->h;
					float crossSize = row ? c->h : c->w;
					if (valid(free)) {
						if (free > 0.f && totalGrow > 0.f && c->flexGrow > 0.f) {
							mainSize += free * c->flexGrow / totalGrow;
						}
						else if (free < 0.f && totalShrinkWeight > 0.f && c->flexShrink > 0.f) {
							const float base = row ? c->w : c->h;
							mainSize = std::max(0.f, mainSize + free * (c->flexShrink * base) / totalShrinkWeight);
						}
					}
					// 交叉轴 stretch：仅当该维未显式指定、且节点没有自己的测量函数
					const bool crossDefined = row ? c->hasHeight : c->hasWidth;
					const float crossMargins = row ? c->margin[1] + c->margin[3] : c->margin[0] + c->margin[2];
					if (n->alignItems == Align::Stretch && valid(innerCross) && !crossDefined && !c->measureFn) {
						crossSize = std::max(0.f, innerCross - crossMargins);
					}
					const float fw = row ? mainSize : crossSize;
					const float fh = row ? crossSize : mainSize;
					if (std::fabs(fw - c->w) > 0.01f || std::fabs(fh - c->h) > 0.01f) {
						layoutNode(c, innerW, innerH, fw, fh);
					}
				}

				// 主轴剩余空间的分配方式
				const float mainUsed = [&] {
					float s = 0.f;
					for (auto* c : flow) s += outerMain(c, row);
					return s;
					}();
				const float rest = valid(innerMain) ? std::max(0.f, innerMain - mainUsed) : 0.f;
				// 均分间距只按可见子项数算：display:none 的那些不该在中间留出空档
				int visibleCount = 0;
				for (auto* c : flow) if (!c->displayNone) ++visibleCount;
				float startOffset = 0.f, gap = 0.f;
				switch (n->justifyContent) {
				case Justify::Center: startOffset = rest * 0.5f; break;
				case Justify::End: startOffset = rest; break;
				case Justify::SpaceBetween:
					if (visibleCount > 1) gap = rest / (visibleCount - 1);
					break;
				case Justify::SpaceAround:
					if (visibleCount > 0) { gap = rest / visibleCount; startOffset = gap * 0.5f; }
					break;
				case Justify::SpaceEvenly:
					gap = rest / (visibleCount + 1);
					startOffset = gap;
					break;
				default: break;   // Auto / Start
				}

				float cursor = startOffset;
				for (auto* c : flow) {
					const float crossSize = row ? c->h : c->w;
					const float crossFree = valid(innerCross) ? std::max(0.f, innerCross - crossSize) : 0.f;
					float crossOffset = 0.f;
					switch (n->alignItems) {
					case Align::Center: crossOffset = crossFree * 0.5f; break;
					case Align::FlexEnd: crossOffset = crossFree; break;
					default: break;   // Auto / FlexStart / Stretch / Baseline
					}
					// 隐藏项自身也不带 margin 偏移（否则它会停在 cursor 右侧，虽然看不见，
					// 但一旦以后有人给隐藏项做坐标推算就会对不上）
					const float mL = c->displayNone ? 0.f : (row ? c->margin[0] : c->margin[1]);
					const float mC = c->displayNone ? 0.f : (row ? c->margin[1] : c->margin[0]);
					const float mainPos = cursor + mL;
					const float crossPos = crossOffset + mC;
					if (row) {
						c->left = padL + mainPos;
						c->top = padT + crossPos;
					}
					else {
						c->left = padL + crossPos;
						c->top = padT + mainPos;
					}
					cursor += outerMain(c, row) + gap;
				}

				// 绝对定位子节点：不参与流式布局，坐标相对父容器内容区
				for (auto* c : absolute) {
					layoutNode(c, innerW, innerH, kNaN, kNaN);
					const float areaW = valid(innerW) ? innerW : 0.f;
					const float areaH = valid(innerH) ? innerH : 0.f;
					// 左+右 / 上+下 同时给了偏移，且该维没有显式尺寸时，宽高由父容器剩余空间决定。
					// 两个方向必须合成一次 layoutNode 调用：分开调的话，后一次传的 kNaN 会把前一次
					// 算出来的尺寸覆盖回 auto（表现为宽度塌成内容宽度、右侧留一大片空白）。
					float fw = kNaN, fh = kNaN;
					if (c->hasPos[0] && c->hasPos[2] && !c->hasWidth && std::isnan(c->widthPercent)) {
						fw = std::max(0.f, areaW - c->pos[0] - c->pos[2] - c->margin[0] - c->margin[2]);
					}
					if (c->hasPos[1] && c->hasPos[3] && !c->hasHeight && std::isnan(c->heightPercent)) {
						fh = std::max(0.f, areaH - c->pos[1] - c->pos[3] - c->margin[1] - c->margin[3]);
					}
					if (valid(fw) || valid(fh)) layoutNode(c, innerW, innerH, fw, fh);
					float x, y;
					if (c->hasPos[0]) x = padL + c->pos[0] + c->margin[0];
					else if (c->hasPos[2]) x = padL + areaW - c->pos[2] - c->w - c->margin[2];
					else x = padL + c->margin[0];
					if (c->hasPos[1]) y = padT + c->pos[1] + c->margin[1];
					else if (c->hasPos[3]) y = padT + areaH - c->pos[3] - c->h - c->margin[3];
					else y = padT + c->margin[1];
					c->left = x;
					c->top = y;
				}
			}

			// 按像素网格取整：结果按物理像素取整。
			// 不取整的话子节点会落在半像素上，整条工具条的图标文字会比原版糊一层
			n->left = std::round(n->left);
			n->top = std::round(n->top);
			n->w = std::round(valid(w) ? w : 0.f);
			n->h = std::round(valid(h) ? h : 0.f);
		}
	}

	FlNodeRef FlNodeNew()
	{
		return new FlNode();
	}

	void FlNodeFree(FlNodeRef node)
	{
		delete node;
	}

	void FlNodeInsertChild(FlNodeRef parent, FlNodeRef child, size_t index)
	{
		if (!parent || !child) return;
		child->parent = parent;
		if (index >= parent->children.size()) parent->children.push_back(child);
		else parent->children.insert(parent->children.begin() + index, child);
	}

	void FlNodeRemoveChild(FlNodeRef parent, FlNodeRef child)
	{
		if (!parent || !child) return;
		auto& v = parent->children;
		auto it = std::find(v.begin(), v.end(), child);
		if (it == v.end()) return;
		v.erase(it);
		child->parent = nullptr;
	}

	size_t FlNodeGetChildCount(FlNodeConstRef node)
	{
		return node ? node->children.size() : 0;
	}

	void FlNodeSetContext(FlNodeRef node, void* ctx)
	{
		if (node) node->context = ctx;
	}

	void* FlNodeGetContext(FlNodeConstRef node)
	{
		return node ? node->context : nullptr;
	}

	void FlNodeSetMeasureFunc(FlNodeRef node, FlMeasureFn fn)
	{
		if (node) node->measureFn = fn;
	}

	void FlNodeMarkDirty(FlNodeRef)
	{
		// 每次 CalculateLayout 都会重新测量，无需缓存失效；保留空实现让上层调用不变
	}

	void FlNodeCalculateLayout(FlNodeRef root, float w, float h)
	{
		if (!root) return;
		// 根节点的尺寸就是窗口的边框盒，直接强制
		layoutNode(root, w, h, w, h);
		root->left = 0.f;
		root->top = 0.f;
	}

	float FlLayoutGetLeft(FlNodeConstRef node) { return node ? node->left : 0.f; }
	float FlLayoutGetTop(FlNodeConstRef node) { return node ? node->top : 0.f; }
	float FlLayoutGetWidth(FlNodeConstRef node) { return node ? node->w : 0.f; }
	float FlLayoutGetHeight(FlNodeConstRef node) { return node ? node->h : 0.f; }

	namespace {
		// Edge -> margin/padding/position 数组下标；返回 -1 表示未命中单边
		int edgeIndex(Edge e)
		{
			switch (e) {
			case Edge::Left: case Edge::Start: return 0;
			case Edge::Top: return 1;
			case Edge::Right: case Edge::End: return 2;
			case Edge::Bottom: return 3;
			default: return -1;
			}
		}

		void setEdge(float (&arr)[4], Edge e, float val)
		{
			const int i = edgeIndex(e);
			if (i >= 0) { arr[i] = val; return; }
			switch (e) {
			case Edge::Horizontal: arr[0] = arr[2] = val; break;
			case Edge::Vertical: arr[1] = arr[3] = val; break;
			case Edge::All: arr[0] = arr[1] = arr[2] = arr[3] = val; break;
			default: break;
			}
		}

		void setEdgeFlag(bool (&arr)[4], Edge e, bool val)
		{
			const int i = edgeIndex(e);
			if (i >= 0) { arr[i] = val; return; }
			switch (e) {
			case Edge::Horizontal: arr[0] = arr[2] = val; break;
			case Edge::Vertical: arr[1] = arr[3] = val; break;
			case Edge::All: arr[0] = arr[1] = arr[2] = arr[3] = val; break;
			default: break;
			}
		}
	}

	void FlStyleSetWidth(FlNodeRef n, float val) { if (n) { n->hasWidth = true; n->width = val; } }
	void FlStyleSetHeight(FlNodeRef n, float val) { if (n) { n->hasHeight = true; n->height = val; } }
	void FlStyleSetWidthPercent(FlNodeRef n, float percent) { if (n) n->widthPercent = percent; }
	void FlStyleSetHeightPercent(FlNodeRef n, float percent) { if (n) n->heightPercent = percent; }
	void FlStyleSetMinHeight(FlNodeRef n, float val) { if (n) { n->hasMinHeight = true; n->minHeight = val; } }
	void FlStyleSetMargin(FlNodeRef n, Edge e, float val) { if (n) setEdge(n->margin, e, val); }
	void FlStyleSetPadding(FlNodeRef n, Edge e, float val) { if (n) setEdge(n->padding, e, val); }

	void FlStyleSetPosition(FlNodeRef n, Edge e, float val)
	{
		if (!n) return;
		setEdge(n->pos, e, val);
		setEdgeFlag(n->hasPos, e, true);
	}

	void FlStyleSetPositionType(FlNodeRef n, Position val) { if (n) n->positionType = val; }
	void FlStyleSetDisplay(FlNodeRef n, bool none) { if (n) n->displayNone = none; }
	void FlStyleSetOverflowScroll(FlNodeRef n) { if (n) n->overflowScroll = true; }
	void FlStyleSetFlexGrow(FlNodeRef n, float val) { if (n) n->flexGrow = val; }
	void FlStyleSetFlexShrink(FlNodeRef n, float val) { if (n) n->flexShrink = val; }
	void FlStyleSetFlexDirection(FlNodeRef n, FlexDirection val) { if (n) n->dir = val; }
	void FlStyleSetFlexWrap(FlNodeRef n, Wrap val) { if (n) n->wrap = val; }
	void FlStyleSetJustifyContent(FlNodeRef n, Justify val) { if (n) n->justifyContent = val; }
	void FlStyleSetAlignItems(FlNodeRef n, Align val) { if (n) n->alignItems = val; }
}
