#pragma once
#include <Ui/Ui.h>
#include "ShapeBase.h"
class ShapeNumber : public ShapeBase
{
public:
	ShapeNumber(WinHost* win);
	~ShapeNumber();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
	// 工具条滑块 / 颜色 / 填充变了：按当前样式重画（见 .cpp）
	void applyStyle() override;
	// 单击就是它的正常用法：落一个序号徽章，不需要拖动
	bool isValidWithoutDrag() override { return true; };
	// 切换形状/填充（副条上圆、方两颗形状按钮，共 4 种样式）：换形状要重建几何体，
	// 颜色也按当前单色/双色重取
	void setStyle(bool square, bool fill);
public:
private:
	static int getNextVal(WinHost* win);
	D2D1_POINT_2F localPoint(const float degrees);
	D2D1_POINT_2F transformPoint(const D2D1_POINT_2F& point);
	void makePath();
	// 半径变化后要重排文字，两处调用点合到一起
	void makeTextLayout();
	// 三只夹点（圆心 / 箭头尖 / 弧中点）跟着 r 和 angle 重摆。原来只在 mouseUp 里算，
	// 拖滑块改半径那条路上就没人更新 —— 图标变大了，夹点还停在旧位置
	void updateDraggers();
private:
	std::vector<D2D1_RECT_F> draggers;
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layoutText;
	// cx/cy 在 mouseDown 里才落定，之间可能先 paint 一次，给初值免得读到垃圾值
	float pressX{ 0.f }, pressY{ 0.f }, cx{ 0.f }, cy{ 0.f }, r{ 0.f }, angle{ 270.f };
	D2D1_POINT_2F tip{ 0,0 }, mid{ 0,0 };
	bool isFill{ false }, isSquare{ false }, isWheel{ false };
	int val{ 1 };
};

