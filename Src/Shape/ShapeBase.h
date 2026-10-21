#pragma once
#include <vector>
#include <algorithm>
#include <include/Ling.h>
class WinHost;
class ShapeBase
{
public:
	ShapeBase(WinHost* win);
	virtual ~ShapeBase();
	virtual void paint(ID2D1DeviceContext* ctx) = 0;
	virtual void paintDragger(ID2D1DeviceContext* ctx) {};
	// 悬停时画 2px 细线框（选中编辑时还要叠加 8 个小方块，见 paintDragger）
	virtual void paintHoverFrame(ID2D1DeviceContext* ctx) {};
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
public:
	WinHost* win;
	bool isUndo;
	int hoverDraggerIndex{ -1 };
protected:
protected:
	float draggerSize;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDragger;
	// 由 8 个控制点小方块求外接包围框，用于画悬停细线框
	D2D1_RECT_F draggersBounds(const std::vector<D2D1_RECT_F>& dg) const;
	// 画 2px 细线框
	void drawHoverFrame(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r) const;
private:
};

