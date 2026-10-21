#include "pch.h"
#include "../Win/WinHost.h"
#include "ShapeBase.h"

ShapeBase::ShapeBase(WinHost* win):win{win}, draggerSize{6*win->dpi}
{
	auto d2d = Ling::D2D::get();
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000), brushDragger.GetAddressOf());
}

ShapeBase::~ShapeBase()
{}

bool ShapeBase::isInRect(const D2D1_RECT_F rect, const float x, const float y)
{
	return (x > rect.left && x<rect.right && y>rect.top && y < rect.bottom);
}

// 细线十字手柄：以控制点为中心画一个细线 "+"，宽度 1px，只标记位置、不占实心方块
void ShapeBase::paintDraggerMark(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r)
{
	if (!ctx || !brushDragger) return;
	float cx = (r.left + r.right) / 2.f;
	float cy = (r.top + r.bottom) / 2.f;
	float half = (r.right - r.left) / 2.f;
	ctx->DrawLine(D2D1::Point2F(cx - half, cy), D2D1::Point2F(cx + half, cy), brushDragger.Get(), win->dpi);
	ctx->DrawLine(D2D1::Point2F(cx, cy - half), D2D1::Point2F(cx, cy + half), brushDragger.Get(), win->dpi);
}
