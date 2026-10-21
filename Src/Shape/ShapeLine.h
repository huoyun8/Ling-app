#pragma once
#include <include/Ling.h>
#include "ShapeBase.h"
class ShapeLine : public ShapeBase
{
public:
	ShapeLine(WinHost* win);
	~ShapeLine();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void setCursor() override;
public:
private:
	void makePath();
	// 自由画只在光标挪够 minStep 时才追加顶点：否则一个像素一步地拖，点位和几何体都跟着无限涨，
	// 长笔画会越画越卡。atEnd 为 true 时和末点比，否则和首点比
	bool needSample(float x, float y, bool atEnd) const;
	float pointToSegmentDistance(const D2D1_POINT_2F& p, const D2D1_POINT_2F& a, const D2D1_POINT_2F& b);
	void hitTest(const D2D1_POINT_2F& mousePos);
private:
	// 采样步长（逻辑像素）。比一个像素大得多，肉眼仍是一条顺滑的线
	static constexpr float minStep{ 2.f };
	std::vector<D2D1_RECT_F> draggers;
	std::vector<D2D1_POINT_2F> linePoints;
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	// 圆头圆角描边：自由画的折线不用它，拐点会是尖角，端点是方头
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStyle;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	float strokeWidth{ 1.f }, pressX{0.f}, pressY{ 0.f };
	bool isTransparent{ false };
};

