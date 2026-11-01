#pragma once
#include <Ui/Ui.h>
#include "ShapeBase.h"
class ShapeMosaic : public ShapeBase
{
public:
	ShapeMosaic(WinHost* win);
	~ShapeMosaic();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	// 滑动条变化时按当前强度重建（实时显现），供 WinCap / WinPin::onToolStyleChanged 调用。
	// 副条上换算法（马赛克 / 高斯模糊 / 智能消除）也走这里 —— 画好的那一条跟着一起换
	void rebuildLive();
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void setCursor() override;
public:
private:
	void makePath();
	// 自由涂抹的采样节流：光标挪够 minStep 才记一个顶点，避免长笔画把点位和几何体撑爆
	bool needSample(float x, float y, bool atEnd) const;
	void resetMosaic();
	void updateDraggers();
	void buildMosaicBitmap();
	Microsoft::WRL::ComPtr<ID2D1Bitmap> createMosaicBitmap(int blockSize);
	// 智能消除的洞掩膜：把当前几何（矩形整块 / 描边覆盖范围）渲染成 0/1 掩膜，
	// 局部位图坐标系（left/top 是包围盒左上角在底图里的位置）
	bool makeMask(int left, int top, UINT32 maskW, UINT32 maskH, std::vector<BYTE>& mask);
	void mosaicPixels(BYTE* bits, UINT32 pitch, UINT32 width, UINT32 height, int blockSize);
	// 三次盒式模糊近似高斯（水平/垂直各3遍），半径随滑块
	void blurPixels(BYTE* bits, UINT32 pitch, UINT32 width, UINT32 height, int radius);

	void hitTest(const D2D1_POINT_2F& mousePos);
private:
	// 采样步长（逻辑像素）
	static constexpr float minStep{ 2.f };
	D2D1_RECT_F rect{ 0.f,0.f,0.f,0.f };
	std::vector<D2D1_RECT_F> draggers;
	std::vector<D2D1_POINT_2F> linePoints;
	Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
	// 圆头圆角描边，涂抹模式下让笔画首尾和拐点是圆的
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStyle;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
	Microsoft::WRL::ComPtr<ID2D1Bitmap> mosaicBitmap;
	Microsoft::WRL::ComPtr<ID2D1BitmapBrush> mosaicBrush;
	// 马赛克位图只覆盖笔画包围盒那一小块，这是它左上角在窗口里的位置。
	// mosaicBrush 要按它平移，否则贴图会跑到窗口原点去。
	D2D1_POINT_2F mosaicOrigin{ 0.f, 0.f };
	float strokeWidth{ 1.f }, pressX{ 0.f }, pressY{ 0.f };
	bool isRect{ false };
	// 高斯模糊模式：整块矩形区域内做高斯模糊，而不是像素色块马赛克
	// 效应档位：0 = 马赛克 / 1 = 高斯模糊 / 2 = 智能消除（内容感知填充）
	int effect{ 0 };
};
