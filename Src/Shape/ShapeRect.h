#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"
class ShapeRect : public ShapeBase
{
public:
	ShapeRect(WinHost* win);
	~ShapeRect();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseWheel(const float x, const float y, const short delta) override;
	void setCursor() override;
public:
private:
	std::vector<D2D1_RECT_F> draggers;
	// 给初值：mouseDown 只记按下点，rect 要等第一次 mouseDrag 才算出来，
	// 而这之间已经可能来一次 paint（刷新时机不受控），不初始化就是拿垃圾值画矩形
	D2D1_RECT_F rect{ 0,0,0,0 };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	// pressX/pressY 与 ShapeEllipse 一样给初值：rect 已经是 0,0,0,0，
	// 万一在第一次 mouseDown 之前就被 mouseDrag / mouseWheel 碰到（刷新时机不受控），
	// 拿未初始化的浮点数算 minmax 会画出一块飞到天外的矩形
	float strokeWidth{ 1.f }, pressX{ 0.f }, pressY{ 0.f };
	bool isFill{ false };
};

