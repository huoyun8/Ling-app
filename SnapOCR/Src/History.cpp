#include "pch.h"
#include "History.h"
#include "Win/WinHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "Shape/ShapeBase.h"
#include "Shape/ShapeRect.h"
#include "Shape/ShapeEllipse.h"
#include "Shape/ShapeArrow.h"
#include "Shape/ShapeNumber.h"
#include "Shape/ShapeLine.h"
#include "Shape/ShapeText.h"
#include "Shape/ShapeMosaic.h"
#include "Shape/ShapeEraser.h"

History::History(WinHost* win):win{win}
{

}

History::~History()
{

}
ShapeBase* History::createShape(const std::wstring& state, const int& x, const int& y)
{
    removeUndoShape();
    auto toolMain = win->toolMain.get();
    // 工具条没建起来（建窗失败 / 资源不足）时没有"当前工具"可言，直接返回空 ——
    // 以前这里会解引用空指针，点一下选区就崩
    if (!toolMain || !win->toolSub) return nullptr;
    ShapeBase* result{nullptr};
    auto push = [&result, this](std::unique_ptr<ShapeBase> shape) {
        result = shape.get();
        shapes.push_back(std::move(shape));
    };
    const auto& id = toolMain->curId;
    if (id == L"rect") {
        // 矩形/椭圆已合并：按副编辑器的形状单选（isEllipseShape）决定画矩形还是椭圆
        if (win->toolSub->isEllipseShape) push(std::make_unique<ShapeEllipse>(win));
        else push(std::make_unique<ShapeRect>(win));
    }
    else if (id == L"arrow") {
        // 箭头副条上二选一：箭头 / 直线。直线是"按下点 → 松手点"的两点线
        if (win->toolSub->isArrowStraight) {
            auto shape = std::make_unique<ShapeLine>(win);
            shape->setStraight(true);
            push(std::move(shape));
        }
        else {
            push(std::make_unique<ShapeArrow>(win));
        }
    }
    else if (id == L"number") {
        push(std::make_unique<ShapeNumber>(win));
    }
    else if (id == L"brush") {
        // 画笔：手绘 = 自由线；选框 = 填充框（实心矩形，四角可拉）。
        // 选框强制实心，不受矩形工具那个填充开关影响
        if (win->toolSub->isBrushMarquee) {
            auto shape = std::make_unique<ShapeRect>(win);
            shape->setFillForced(true);
            push(std::move(shape));
        }
        else {
            push(std::make_unique<ShapeLine>(win));
        }
    }
    else if (id == L"text") {
        push(std::make_unique<ShapeText>(win));
    }
    else if (id == L"mosaic") {
        // 马赛克与高斯模糊是同一颗主按钮下的两档模式，构造函数自己会读副条的模式
        push(std::make_unique<ShapeMosaic>(win));
    }
    else if (id == L"eraser") {
        push(std::make_unique<ShapeEraser>(win));
    }
    // curId 落在没有对应 shape 的工具上时 result 是空的，直接返回
    if (!result) return nullptr;
    result->mouseDown((float)x, (float)y);
    return result;
}

void History::undo()
{
    // 从后往前找第一个还没被撤销的。撤销后它在 vector 里的位置不变（isUndo = true），
    // 被撤销的形状因此在 vector 里构成一段连续后缀，且越早撤的画得越早、位置越靠前；
    // 所以 redo 按相反顺序捡回来 = "从前往后找第一个被撤销的"（见 redo 注释）
    int i{ (int)(shapes.size() - 1) };
    for (; i >= 0; i--)
    {
        auto cur = shapes[i].get();
        if (!cur->isUndo) {
            cur->isUndo = true;
            if (cur == win->shapeHover) {
                win->shapeHover = nullptr;
            }
            if (cur == win->shapeShow) {
                win->shapeShow = nullptr;
            }
            if (cur == win->editTarget) {
                win->editTarget = nullptr;
            }
            // 撤销掉的正好是"这一下新建出来的那个"：newShape 也得跟着失效，
            // 否则 onUp 还会拿它去比 hasDragged（此时它已经 isUndo，后续操作全不该作用在它身上）
            if (cur == win->newShape) {
                win->newShape = nullptr;
            }
            win->refresh();
            break;
        }
    }
}

void History::redo()
{
    // 重做要按撤销的相反顺序恢复（LIFO）：后撤的先恢复。撤销永远撤"最后一笔没撤的"，
    // 所以被撤销的形状是 vector 的一段连续后缀，其中最后被撤销的那个位置最靠前 ——
    // 从前往后扫到的第一个 isUndo 正好就是它。从后往前找顺序会整个反掉：
    // 画 1~5 再全部撤销，重做会变成 5→1（先恢复最早画的），和撤销顺序一模一样而不是镜像
    for (int i{ 0 }; i < (int)shapes.size(); i++)
    {
        auto cur = shapes[i].get();
        if (cur->isUndo) {
            cur->isUndo = false;
            win->refresh();
            break;
        }
    }
}

/// <summary>
/// 进删除除 hover 状态的 shape
/// </summary>
void History::removeHoverShape()
{
    if (!win->shapeHover) return;
    auto target = win->shapeHover;
    // 正在编辑的话先收尾：TextBox 是 WinHost 上共用的一个，
    // 删了 shape 却留着它显示，下一次编辑就会带着上一次的文字。
    if (auto txt = dynamic_cast<ShapeText*>(target)) {
        txt->finishEdit();
    }
    removeShape(target);
}

void History::removeShape(ShapeBase* target)
{
    if (!target) return;
    for (auto it = shapes.begin(); it != shapes.end(); ++it) {
        if (it->get() == target) {
            // 宿主上所有指向它的裸指针都要一起断掉。漏一个就是悬空指针：
            //   shapeHover  / editTarget —— onMove / onDown 会解引用它
            //   newShape                  —— onUp 里判"是不是这次新建的那个"
            //   editingText               —— getImagePixels 会调它的 finishEdit()
            if (win->shapeHover == target) win->shapeHover = nullptr;
            if (win->shapeShow == target) win->shapeShow = nullptr;
            if (win->draggerPin == target) win->draggerPin = nullptr;
            if (win->editTarget == target) win->editTarget = nullptr;
            if (win->newShape == target) win->newShape = nullptr;
            if (win->editingText == target) win->editingText = nullptr;
            shapes.erase(it);
            win->refresh();
            return;
        }
    }
}

void History::removeUndoShape()
{
    int i{ (int)(shapes.size() - 1) };
    for (; i >= 0; i--)
    {
        auto cur = shapes[i].get();
        if (!cur->isUndo) {
            break;
        }
        if (win->shapeHover == cur) win->shapeHover = nullptr;
        if (win->shapeShow == cur) win->shapeShow = nullptr;
        if (win->draggerPin == cur) win->draggerPin = nullptr;
        if (win->editTarget == cur) win->editTarget = nullptr;
        if (win->newShape == cur) win->newShape = nullptr;
        if (win->editingText == cur) win->editingText = nullptr;
        shapes.erase(shapes.begin() + i);
    }
}

// 工具条上点"椭圆"时，把画布上最后那只矩形就地变成椭圆；反过来也一样。
// 用户的口径："画了方框，点椭圆的时候能变成椭圆" —— 联动不止改样式，连形状一起换。
// 只认最后一条未撤销的标注（与滑条联动同一口径），别去动更早的那些
ShapeBase* History::convertLastRectEllipse(const bool toEllipse)
{
	ShapeBase* src = nullptr;
	int idx{ -1 };
	for (int i{ (int)shapes.size() - 1 }; i >= 0; i--) {
		auto s = shapes[i].get();
		if (!s || s->isUndo) continue;
		src = s;
		idx = i;
		break;
	}
	if (!src) return nullptr;
	const bool srcEllipse = dynamic_cast<ShapeEllipse*>(src) != nullptr;
	const bool srcRect = dynamic_cast<ShapeRect*>(src) != nullptr;
	if (!srcEllipse && !srcRect) return nullptr;   // 最后一条不是矩形/椭圆，不动
	if (srcEllipse == toEllipse) return nullptr;   // 已经是这只形状
	D2D1_RECT_F r{ 0.f, 0.f, 0.f, 0.f };
	if (!src->bounds(r)) return nullptr;
	if (r.right - r.left <= 0.f || r.bottom - r.top <= 0.f) return nullptr;

	std::unique_ptr<ShapeBase> fresh;
	if (toEllipse) fresh = std::make_unique<ShapeEllipse>(win);
	else           fresh = std::make_unique<ShapeRect>(win);
	ShapeBase* ns = fresh.get();
	// 用"按下 → 拖 → 抬"把同一块几何走一遍：rect、中心点、一圈手柄都由它自己算，
	// 不用在这里另抄一份算法（抄了迟早和 mouseDrag 里那份走散）
	ns->mouseDown(r.left, r.top);
	ns->mouseDrag(r.right, r.bottom);
	ns->mouseUp(r.right, r.bottom);
	// 原位替换：图层顺序不变；指向旧对象的那几个指针一起挪到新对象上，免得悬空。
	// draggerPin 也要搬 —— 画完一笔它就钉在"最后那条"上，此刻换成椭圆，它指的正是被换掉的那个。
	// 现在只是因为 probeHover 里先比 last != draggerPin 短路了才没踩到野指针，靠这个顺序太脆
	if (win->shapeHover == src) win->shapeHover = ns;
	if (win->shapeShow == src) win->shapeShow = ns;
	if (win->editTarget == src) win->editTarget = ns;
	if (win->draggerPin == src) win->draggerPin = ns;
	shapes[idx] = std::move(fresh);
	win->refresh();
	return ns;
}
