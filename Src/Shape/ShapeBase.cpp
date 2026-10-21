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

// 矩形/椭圆最小尺寸：宽或高小于 2px 时按中心扩到 2px，避免拖出退化的小形状
void ShapeBase::enforceMinSize(D2D1_RECT_F& r)
{
	constexpr float minSize{ 2.f };
	if (r.right - r.left < minSize) {
		const float c = (r.left + r.right) / 2.f;
		r.left = c - minSize / 2.f;
		r.right = c + minSize / 2.f;
	}
	if (r.bottom - r.top < minSize) {
		const float c = (r.top + r.bottom) / 2.f;
		r.top = c - minSize / 2.f;
		r.bottom = c + minSize / 2.f;
	}
}
