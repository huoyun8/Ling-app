#include "pch.h"
#include "../Win/WinHost.h"
#include "ShapeBase.h"

ShapeBase::ShapeBase(WinHost* win):win{win}, draggerSize{6*win->dpi}
{
	auto d2d = Ling::D2D::get();
	// 控制点小方块描边：醒目的半透明蓝
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0.28f, 0.62f, 1.0f, 0.95f), brushDragger.GetAddressOf());
}

ShapeBase::~ShapeBase()
{}

bool ShapeBase::isInRect(const D2D1_RECT_F rect, const float x, const float y)
{
	return (x > rect.left && x<rect.right && y>rect.top && y < rect.bottom);
}
