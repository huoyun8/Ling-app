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
public:
	WinHost* win;
	// 必须给初值：除了 History 的撤销/重做，没人给它赋过值，而 WinCap/WinPin 的 paint 循环
	// 一上来就读 !isUndo。不给初值的话这块内存是垃圾，新建的标注可能第一帧就画不出来
	bool isUndo{ false };
	int hoverDraggerIndex{ -1 };
protected:
	float draggerSize{ 6.f };
	// 矩形/椭圆等形状的最小尺寸钳制：宽高小于 2px 时按中心扩到 2px
	void enforceMinSize(D2D1_RECT_F& r);
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDragger;
private:
};

