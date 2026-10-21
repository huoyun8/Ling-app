#pragma once
#include <include/Ling.h>
class WinHost;
class ShapeBase
{
public:
	ShapeBase(WinHost* win);
	virtual ~ShapeBase();
	virtual void paint(ID2D1DeviceContext* ctx) = 0;
	virtual void paintDragger(ID2D1DeviceContext* ctx) {};
	virtual void mouseMove(const float x, const float y) { };
	virtual void mouseDrag(const float x, const float y) {};
	virtual void mouseDown(const float x, const float y) { };
	virtual void mouseUp(const float x, const float y) { };
	virtual void mouseWheel(const float x, const float y, const short delta) {};
	virtual void setCursor() {};
	// 新建这一笔如果只是按下马上弹起（没有拖动），默认当成什么也没画，元素直接丢掉。
	// 单击本身就是正常用法的元素（number 落徽章、text 进编辑）覆盖它返回 true
	virtual bool isValidWithoutDrag() { return false; };
	bool isInRect(const D2D1_RECT_F rect, const float x, const float y);
	// 选中形状的控制点手柄：用细线十字标记（替代原先的实心小方块）
	void paintDraggerMark(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r);
public:
	WinHost* win;
	bool isUndo;
	int hoverDraggerIndex{ -1 };
protected:
protected:
	float draggerSize;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDragger;
private:
};

