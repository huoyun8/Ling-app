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

D2D1_RECT_F ShapeBase::draggersBounds(const std::vector<D2D1_RECT_F>& dg) const
{
	D2D1_RECT_F box{ FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (auto& d : dg) {
		box.left = std::min(box.left, d.left);
		box.top = std::min(box.top, d.top);
		box.right = std::max(box.right, d.right);
		box.bottom = std::max(box.bottom, d.bottom);
	}
	return box;
}

void ShapeBase::drawHoverFrame(ID2D1DeviceContext* ctx, const D2D1_RECT_F& r) const
{
	if (!ctx || !brushDragger) return;
	if (r.right < r.left || r.bottom < r.top) return;
	ctx->DrawRectangle(D2D1::RectF(r.left, r.top, r.right, r.bottom), brushDragger.Get(), 2.0f * win->dpi);
}
