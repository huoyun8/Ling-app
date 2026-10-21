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
    ShapeBase* result{nullptr};
    auto toolMain = win->toolMain.get();
    if (toolMain->curId == L"rect") {
        // 矩形/椭圆已合并：按副编辑器的形状单选（isEllipseShape）决定画矩形还是椭圆
        if (win->toolSub->isEllipseShape) {
            auto shape = std::make_unique<ShapeEllipse>(win);
            result = shape.get();
            shapes.push_back(std::move(shape));
        }
        else {
            auto shape = std::make_unique<ShapeRect>(win);
            result = shape.get();
            shapes.push_back(std::move(shape));
        }
    }
    else if (toolMain->curId == L"arrow") {
        auto shape = std::make_unique<ShapeArrow>(win);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (toolMain->curId == L"number") {
        auto shape = std::make_unique<ShapeNumber>(win);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (toolMain->curId == L"line") {
        auto shape = std::make_unique<ShapeLine>(win);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (toolMain->curId == L"text") {
        auto shape = std::make_unique<ShapeText>(win);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (toolMain->curId == L"mosaic") {
        auto shape = std::make_unique<ShapeMosaic>(win);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    else if (toolMain->curId == L"eraser") {
        auto shape = std::make_unique<ShapeEraser>(win);
        result = shape.get();
        shapes.push_back(std::move(shape));
    }
    // curId 落在没有对应 shape 的工具上时 result 是空的，直接返回
    if (!result) return nullptr;
    result->mouseDown((float)x, (float)y);
    return result;
}

void History::undo()
{
    // 从后往前找第一个还没被撤销的。撤销后它在 vector 里的位置不变（isUndo = true），
    // 所以 redo 只要"从后往前找第一个被撤销的"就能把它按相反顺序捡回来
    int i{ (int)(shapes.size() - 1) };
    for (; i >= 0; i--)
    {
        auto cur = shapes[i].get();
        if (!cur->isUndo) {
            cur->isUndo = true;
            if (cur == win->shapeHover) {
                win->shapeHover = nullptr;
            }
            if (cur == win->editTarget) {
                win->editTarget = nullptr;
            }
            win->refresh();
            break;
        }
    }
}

void History::redo()
{
    // 必须从后往前找：撤销是从尾部开始的，所以最后被撤销的那个在 vector 里位置最靠后。
    // 从前往后找会跳过它、去恢复一个更早被撤销的元素，连按 Ctrl+Z / Ctrl+Y 的顺序就乱了
    for (int i{ (int)shapes.size() - 1 }; i >= 0; i--)
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
    for (auto it = shapes.begin(); it != shapes.end(); ++it) {
        if (it->get() == target) {
            if (win->shapeHover == target) win->shapeHover = nullptr;
            if (win->editTarget == target) win->editTarget = nullptr;
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
        shapes.erase(shapes.begin() + i);
    }
}
