#pragma once
#include <Ui/Ui.h>
#include "ShapeBase.h"
class ShapeEllipse : public ShapeBase
{
public:
	ShapeEllipse(WinHost* win);
	~ShapeEllipse();
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
	// 当前外接矩形：工具条上切"矩形 ↔ 椭圆"时用它搬框
	bool bounds(D2D1_RECT_F& out) const override { out = rect; return true; }
public:
private:
	std::vector<D2D1_RECT_F> draggers;
	// 都给初值：mouseDown 只记按下点，rect/cx/cy/rx/ry 要等第一次 mouseDrag 才算出来，
	// 而这之间已经可能来一次 paint（刷新时机不受控），不初始化就是拿垃圾值画椭圆
	D2D1_RECT_F rect{ 0,0,0,0 };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	float strokeWidth{ 1.f }, pressX{ 0.f }, pressY{ 0.f }, cx{ 0.f }, cy{ 0.f }, rx{ 0.f }, ry{ 0.f };
	bool isFill{ false };
};

