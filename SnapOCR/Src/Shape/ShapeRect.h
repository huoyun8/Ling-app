#pragma once
#include <Ui/Ui.h>
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
	// 拖工具条滑块 / 换颜色 / 切填充：按当前样式重画（构造时抄的那份已经过期）
	void applyStyle() override;
	// 当前外接矩形：工具条上切"矩形 ↔ 椭圆"时用它搬框
	bool bounds(D2D1_RECT_F& out) const override { out = rect; return true; }
	// 强制实心：画笔的"选框"画的是填充框，与矩形工具那边的填充开关无关
	void setFillForced(bool on) { fillForced = on; isFill = on || isFill; }
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
	// 永远实心（画笔的"选框"）：applyStyle 里不再跟随矩形工具的填充开关
	bool fillForced{ false };
	// 这条框是否跟随画笔的"半透明"开关（画笔的选框设 true，矩形工具的框是 false）
	bool useBrushAlpha{ false };
};

