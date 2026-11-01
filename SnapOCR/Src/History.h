#pragma once
#include <Ui/Ui.h>
class ShapeBase;
class WinHost;
class History
{
public:
	History(WinHost* win);
	~History();
	ShapeBase* createShape(const std::wstring& state, const int& x, const int& y);
	void undo();
	void redo();
	void removeHoverShape();
	// 删掉指定 shape。ShapeText 输入为空时会异步调它把自己抹掉。
	void removeShape(ShapeBase* target);
	// 把"最后一条"矩形/椭圆原地换成另一种形状（工具条上点另一颗形状按钮时用）。
	// 框的位置尺寸、层级顺序都保留；返回换出来的新对象，没换成就返回 nullptr
	ShapeBase* convertLastRectEllipse(bool toEllipse);
public:
	std::vector<std::unique_ptr<ShapeBase>> shapes;
private:
	void removeUndoShape();
private:
	WinHost* win;
};

