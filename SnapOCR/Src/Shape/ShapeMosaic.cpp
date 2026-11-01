#include "pch.h"
#include "Win/WinHost.h"
#include "Tool/ToolMain.h"
#include "Tool/ToolSub.h"
#include "History.h"
#include "ShapeMosaic.h"
#include "SmartErase.h"

using Microsoft::WRL::ComPtr;

ShapeMosaic::ShapeMosaic(WinHost* win) :ShapeBase(win), draggers{
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0),
	D2D1::RectF(0,0,0,0)
}
{
	auto toolSub = win->toolSub.get();
	auto d2d = ui::D2D::get();
	// 马赛克没有颜色可选，这个半透明品红只在拖拽过程中当占位提示 ——
	// 松开鼠标后 buildMosaicBitmap 会算出真正的马赛克画刷把它替换掉
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xF00FF0, 0.38f), brush.GetAddressOf());
	strokeWidth = toolSub->getSliderVal();
	effect = toolSub->mosaicEffect();
	// 形状只有一份（选框 / 手绘），三种算法共用：切算法不改画法。
	// 例外是智能消除：它没有手绘，一律按选框处理（工具条那颗形状按钮此时是灰的）
	isRect = effect == 2 || toolSub->isMosaicMarquee;
	// 圆头圆角描边。本项目没有 App::getRoundStrokeStyle 那样的全局缓存，
	// 就每个 shape 自己持一个 —— StrokeStyle 是不可变的轻对象，代价可以忽略
	d2d->d2dFactory->CreateStrokeStyle(
		D2D1::StrokeStyleProperties(
			D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
			D2D1_LINE_JOIN_ROUND, 8.f, D2D1_DASH_STYLE_SOLID, 0.f
		),
		nullptr, 0, roundStyle.GetAddressOf());
}

ShapeMosaic::~ShapeMosaic()
{

}

void ShapeMosaic::paint(ID2D1DeviceContext* ctx)
{
	if (isRect) {
		if (mosaicBrush) {
			ctx->FillRectangle(rect, mosaicBrush.Get());
		}
		else {
			ctx->FillRectangle(rect, brush.Get());
		}
		return;
	}
	// makePath 要等第一次 mouseDown 才建 path，这之前可能先来一次 paint
	if (!path) return;
	if (mosaicBrush) {
		ctx->DrawGeometry(path.Get(), mosaicBrush.Get(), strokeWidth, roundStyle.Get());
	}
	else {
		ctx->DrawGeometry(path.Get(), brush.Get(), strokeWidth, roundStyle.Get());
	}
}

void ShapeMosaic::paintDragger(ID2D1DeviceContext* ctx)
{
	// 矩形模式 8 个夹点，涂抹模式只有首尾 2 个
	size_t count = isRect ? draggers.size() : 2;
	for (size_t i = 0; i < count; ++i)
	{
		ctx->DrawRectangle(draggers[i], brushDragger.Get(), 1.0f * win->dpi);
	}
}

// 工具条上换了模式/强度：把画布上这一条就地改过去。
//   · 算法（马赛克 / 高斯模糊 / 智能消除）只换像素算法 + 取样余量，几何一点都不动；
//   · 强度滑块同时决定色块大小与模糊半径。
// 两件事都走同一条 buildMosaicBitmap，差别只在里面按 effect 分别算 blockSize / blurRadius。
//
// 档位必须在这里重新读一遍：以前这里只读强度、不读档位，画好的那一条就永远停在
// 落笔时的算法上 —— 副条上切"高斯模糊/智能消除"只有之后新画的才生效，
// 看着像那两颗按钮没反应（原本专门写了 setVariant 做这件事，但整条链上没人调它）
void ShapeMosaic::rebuildLive()
{
	effect = win->toolSub->mosaicEffect();
	strokeWidth = win->toolSub->getSliderVal();
	buildMosaicBitmap();
	win->refresh();
}

void ShapeMosaic::mouseDrag(const float x, const float y)
{
	bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (isRect) {
		// 几何一变，之前算好的马赛克就不对了，先扔掉退回占位色
		resetMosaic();
		const auto kind = rectDragTo(hoverDraggerIndex, pressX, pressY, x, y, rect);
		if (kind != RectDrag::Move && shiftDown) {
			auto w = rect.right - rect.left;
			auto h = rect.bottom - rect.top;
			if (w > h) {
				rect.bottom = rect.top + w;
			}
			else {
				rect.right = rect.left + h;
			}
		}
		return;
	}
	// Shift 按下时不再追加顶点，而是拖动端点 —— 效果是一条直线；松开 Shift 就是自由涂抹
	if (hoverDraggerIndex == 0) {
		if (linePoints.empty()) return;
		if (shiftDown) {
			auto& p = linePoints[0];
			p.x = x;
			p.y = y;
		}
		else if (needSample(x, y, false)) {
			linePoints.insert(linePoints.begin(), { x,y });
		}
		else {
			return;
		}
		makePath();
	}
	else if (hoverDraggerIndex == 1) {
		if (linePoints.empty()) return;
		if (shiftDown) {
			auto& p = linePoints[linePoints.size() - 1];
			p.x = x;
			p.y = y;
		}
		else if (needSample(x, y, true)) {
			linePoints.push_back({ x,y });
		}
		else {
			return;
		}
		makePath();
	}
	else if (hoverDraggerIndex == 8) {
		auto spanX{ x - pressX };
		auto spanY{ y - pressY };
		for (auto& p : linePoints)
		{
			p.x += spanX;
			p.y += spanY;
		}
		makePath();
		pressX = x;
		pressY = y;
	}
}

void ShapeMosaic::mouseDown(const float x, const float y)
{
	if (isRect) {
		if (hoverDraggerIndex == -1) { //首次创建
			rect = D2D1::RectF(x, y, x, y);
			hoverDraggerIndex = 4;
		}
		rectPressAnchor(hoverDraggerIndex, x, y, rect, pressX, pressY);
		return;
	}
	if (hoverDraggerIndex == -1) { //首次创建
		linePoints.clear();
		linePoints.push_back({ x,y });
		if (GetKeyState(VK_SHIFT) & 0x8000) {
			// 直线模式需要两个点，末点跟着鼠标走
			linePoints.push_back({ x,y });
		}
		makePath();
		hoverDraggerIndex = 1;
		win->refresh();
	}
	else if (hoverDraggerIndex == 8) {
		pressX = x;
		pressY = y;
	}
}

void ShapeMosaic::mouseUp(const float x, const float y)
{
	updateDraggers();
	// 几何定下来了才算马赛克 —— 这一步要把 GPU 像素读回内存，拖拽过程中每帧做太贵
	buildMosaicBitmap();
}

void ShapeMosaic::mouseMove(const float x, const float y)
{
	if (isRect) {
		hoverDraggerIndex = draggerHit(draggers, (int)draggers.size(), x, y);
		if (hoverDraggerIndex >= 0) return;
		// 马赛克矩形是整块填充、没有可见边框，边缘命中区用固定宽度（2 逻辑像素），
		// 别跟滑块笔刷尺寸走 —— 笔刷大小跟这条边没有任何关系
		auto half{ 2.f * win->dpi };//多个一个dpi，让范围更大点
		if (x >= rect.left - half && x <= rect.right + half && y >= rect.top - half && y <= rect.bottom + half)
		{
			// 矩形整块都是实体：内部任意位置按住就能搬，不必先精确点到边框。
			// 手绘笔迹不走这里（上面 isRect 已分流），仍是只有笔画边缘能拖
			hoverDraggerIndex = 8;
		}
		return;
	}
	hoverDraggerIndex = draggerHit(draggers, 2, x, y);
	if (hoverDraggerIndex == -1)
	{
		hitTest({ x,y });
	}
}

void ShapeMosaic::setCursor()
{
	if (isRect) {
		draggerCursor(hoverDraggerIndex);
		return;
	}
	if (hoverDraggerIndex == 8) {
		SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	}
}

void ShapeMosaic::makePath()
{
	if (linePoints.empty()) return;
	resetMosaic();
	auto d2d = ui::D2D::get();
	// ReleaseAndGetAddressOf 而不是 GetAddressOf：后者不放旧对象，自由涂抹时每个鼠标事件漏一个几何体
	d2d->d2dFactory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
	if (!path) return;
	ComPtr<ID2D1GeometrySink> sink;
	path->Open(sink.GetAddressOf());
	sink->BeginFigure(linePoints[0], D2D1_FIGURE_BEGIN_HOLLOW);
	if (linePoints.size() > 1) {
		sink->AddLines(&linePoints[1], (UINT32)(linePoints.size() - 1));
	}
	sink->EndFigure(D2D1_FIGURE_END_OPEN);
	sink->Close();
}

bool ShapeMosaic::needSample(float x, float y, bool atEnd) const
{
	if (linePoints.empty()) return true;
	auto& ref = atEnd ? linePoints.back() : linePoints.front();
	auto step = minStep * win->dpi;
	auto dx = x - ref.x, dy = y - ref.y;
	return dx * dx + dy * dy >= step * step;
}

void ShapeMosaic::resetMosaic()
{
	mosaicBitmap.Reset();
	mosaicBrush.Reset();
	mosaicOrigin = { 0.f, 0.f };
}

void ShapeMosaic::updateDraggers()
{
	if (isRect) {
		rectDraggersTo(rect, draggers.data());
		return;
	}
	endDraggersTo(linePoints, draggers.data());
}

void ShapeMosaic::buildMosaicBitmap()
{
	if (!isRect && !path) return;
	if (isRect && (rect.right <= rect.left || rect.bottom <= rect.top)) return;
	// 滑条的原始定义就是"笔画宽度"（配置键名一直是 width），马赛克块大小、
	// 高斯模糊半径都是从它派生出来的，不是两个独立参数：
	//   · 色块 = 宽度/3（粗笔配大块，下限 6px 保证肉眼看得见）
	//   · 模糊半径 = (宽度-18)*0.4（见 createMosaicBitmap 里那段）
	// 联动也按这个口径来：滑动条一变，就按新宽度把这一个标注重建一遍
	int blockSize = std::max(6, (int)std::round(strokeWidth / 3.0f));
	mosaicBitmap = createMosaicBitmap(blockSize);
	mosaicBrush.Reset();
	if (!mosaicBitmap) return;
	// NEAREST_NEIGHBOR 是关键：线性插值会把色块边界糊成渐变，就不像马赛克了
	auto bitmapBrushProps = D2D1::BitmapBrushProperties(
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_EXTEND_MODE_CLAMP,
		D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
	auto brushProps = D2D1::BrushProperties();
	auto d2d = ui::D2D::get();
	d2d->deviceContext->CreateBitmapBrush(mosaicBitmap.Get(), &bitmapBrushProps, &brushProps, mosaicBrush.GetAddressOf());
	if (mosaicBrush) {
		// 位图只覆盖包围盒那一小块，画刷默认从原点铺，要平移到包围盒左上角才对得上
		mosaicBrush->SetTransform(D2D1::Matrix3x2F::Translation(mosaicOrigin.x, mosaicOrigin.y));
	}
}

// 把"这个 shape 之前"的画面在包围盒范围内重现一遍，读回像素打成马赛克，再包成画刷。
// 用 d2d->deviceContext 做离屏绘制是安全的：它是全项目共享的资源工厂，
// 没有任何地方给它 SetTarget / BeginDraw（窗口绘制走的是各自 surface 或 swap chain 的 context）。
// 这里 SetTarget → BeginDraw → EndDraw → SetTarget(nullptr) 在函数内闭环，不跨帧持有。
ComPtr<ID2D1Bitmap> ShapeMosaic::createMosaicBitmap(int blockSize)
{
	ComPtr<ID2D1Bitmap> result;
	if ((!isRect && !path) || win->w <= 0 || win->h <= 0) return result;
	if (!win->screenImg) return result;
	if (!win->history) return result;         // 没有历史就拼不出"自己之前"的画面
	if (!ui::D2D::ready()) return result;   // 设备不可用：下面全是 deviceContext 调用

	auto d2d = ui::D2D::get();
	auto ctx = d2d->deviceContext.Get();

	D2D1_RECT_F bounds{};
	HRESULT hr{ S_OK };
	if (isRect) {
		bounds = rect;
	}
	else {
		// 描边后的实际覆盖范围，比 path 本身宽 strokeWidth
		hr = path->GetWidenedBounds(strokeWidth, roundStyle.Get(), nullptr, &bounds);
		if (FAILED(hr)) return result;
	}

	// 取样范围必须按"底图像素尺寸"算，不能用窗口尺寸：
	//   · WinCap：窗口铺满桌面，两者恰好相等；
	//   · WinPin：Ctrl+滚轮缩放后窗口是底图的 scale 倍，用窗口尺寸会把底图拉伸着画进
	//     这块离屏位图，马赛克取到的位置和强度全是错的。
	// 底图就是所有 shape 的坐标基准，按它算才是唯一自洽的口径
	auto imgSize = win->screenImg->GetPixelSize();
	const int winW = (int)imgSize.width, winH = (int)imgSize.height;
	if (winW <= 0 || winH <= 0) return result;
	// 模糊强度由"强度条"（马赛克滑块）连续调节：滑块最低档(18)→半径0=几乎透明可看字，
	// 最高档(68)→约20px。斜率调缓(0.4)、上限压低到20，让强度跨度平缓，
	// 避免低档几下就饱和成一坨、后半段滑块没变化；同时用模糊半径做区域外扩余量
	int blurRadius = effect == 1 ? (int)std::clamp((strokeWidth - 18.f) * 0.4f, 0.f, 20.f) : 0;
	// 取样余量：马赛克按块大小，模糊按半径；智能消除要留出足够的 donor 区域给算法取内容
	const int donorPad = std::clamp((int)std::round(std::max(bounds.right - bounds.left, bounds.bottom - bounds.top) * 0.6f), 24, 128);
	int pad = effect == 1 ? blurRadius : (effect == 2 ? donorPad : blockSize);
	int left = std::max(0, std::min((int)std::floor(bounds.left) - pad, winW));
	int top = std::max(0, std::min((int)std::floor(bounds.top) - pad, winH));
	int right = std::max(0, std::min((int)std::ceil(bounds.right) + pad + 1, winW));
	int bottom = std::max(0, std::min((int)std::ceil(bounds.bottom) + pad + 1, winH));
	if (left >= right || top >= bottom) return result;

	mosaicOrigin = { (float)left, (float)top };
	auto localSize = D2D1::SizeU((UINT32)(right - left), (UINT32)(bottom - top));

	D2D1_BITMAP_PROPERTIES1 targetProps{
		.pixelFormat{ D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED) },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
	};
	ComPtr<ID2D1Bitmap1> targetBitmap;
	hr = ctx->CreateBitmap(localSize, nullptr, 0, &targetProps, targetBitmap.GetAddressOf());
	if (FAILED(hr)) return result;

	// 底图 + 排在自己前面且没被撤销的 shape。平移变换让窗口坐标直接落到这块小位图里，
	// 各 shape 的 paint 不用知道自己被画到了别处
	ctx->SetTarget(targetBitmap.Get());
	ctx->SetTransform(D2D1::Matrix3x2F::Translation(-mosaicOrigin.x, -mosaicOrigin.y));
	ctx->BeginDraw();
	// 共享 deviceContext 上的 SetTarget/BeginDraw 必须成对收尾：中途抛异常（GPU 资源被回收、
	// 某个 shape 的 paint 失败）时若直接返回，这个 context 会永久停在绘制态，之后整窗都画不出来
	hr = S_OK;
	try {
		ctx->Clear(D2D1::ColorF(0, 0.0f));
		ctx->DrawBitmap(win->screenImg.Get(), D2D1::RectF(0, 0, (float)winW, (float)winH));
		for (auto& shape : win->history->shapes)
		{
			auto cur = shape.get();
			if (cur == this) break;
			if (!cur->isUndo) {
				cur->paint(ctx);
			}
		}
		hr = ctx->EndDraw();
	}
	catch (...) {
		hr = E_FAIL;
	}
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	// 解绑，下面 CopyFromBitmap 才能把它当源读
	ctx->SetTarget(nullptr);
	if (FAILED(hr)) return result;

	// GPU 上的 target 位图不能直接 Map，得先拷到一块带 CPU_READ 的位图上
	D2D1_BITMAP_PROPERTIES1 cpuProps{
		.pixelFormat{ targetBitmap->GetPixelFormat() },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	ComPtr<ID2D1Bitmap1> cpuBitmap;
	hr = ctx->CreateBitmap(localSize, nullptr, 0, &cpuProps, cpuBitmap.GetAddressOf());
	if (FAILED(hr)) return result;

	hr = cpuBitmap->CopyFromBitmap(nullptr, targetBitmap.Get(), nullptr);
	if (FAILED(hr)) return result;

	D2D1_MAPPED_RECT mapped{};
	hr = cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped);
	if (FAILED(hr)) return result;
	// 作用域守卫：后面 blurPixels/mosaicPixels 里的 vector 分配若抛 bad_alloc，
	// 也要保证这块位图被 Unmap 掉
	struct MapGuard {
		ID2D1Bitmap1* bmp;
		~MapGuard() { if (bmp) bmp->Unmap(); }
	} mapGuard{ cpuBitmap.Get() };

	std::vector<BYTE> pixels((size_t)mapped.pitch * localSize.height);
	CopyMemory(pixels.data(), mapped.bits, pixels.size());

	// 高斯模糊模式：对读回的整块区域像素做三次盒式模糊（近似高斯），
	// 不再打像素色块；模糊半径随滑块走（粗笔 = 更糊）
	if (effect == 2) {
		// 智能消除：掩膜标出要抹掉的范围，算法用洞外的原图内容重建。
		// 补不全就返回空位图 —— 画面上保持品红占位色，不把原图露出来
		std::vector<BYTE> mask;
		if (!makeMask(left, top, localSize.width, localSize.height, mask)) return result;
		if (!smarterase::fill(pixels.data(), mapped.pitch, (int)localSize.width, (int)localSize.height, mask.data())) return result;
	}
	else if (effect == 1) {
		blurPixels(pixels.data(), mapped.pitch, localSize.width, localSize.height, blurRadius);
	}
	else {
		mosaicPixels(pixels.data(), mapped.pitch, localSize.width, localSize.height, blockSize);
	}

	auto bitmapProps = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	hr = ctx->CreateBitmap(localSize, pixels.data(), mapped.pitch, &bitmapProps, result.GetAddressOf());
	return result;
}

// 智能消除的洞掩膜：把当前几何（矩形整块 / 描边覆盖范围）画成白色，
// 再读回 alpha 当 0/1 掩膜。用 D2D 画是为了跟 reshape 后的几何完全一致 ——
// 手绘笔画有圆头、拐角和线宽，自己算距离场容易在拐点处对不上
// left/top 是包围盒左上角在底图里的位置，画画时平移到局部位图坐标系
bool ShapeMosaic::makeMask(int left, int top, UINT32 maskW, UINT32 maskH, std::vector<BYTE>& mask)
{
	if ((!isRect && !path) || maskW == 0 || maskH == 0) return false;
	if (!ui::D2D::ready()) return false;
	auto d2d = ui::D2D::get();
	auto ctx = d2d->deviceContext.Get();

	ComPtr<ID2D1Bitmap1> target;
	D2D1_BITMAP_PROPERTIES1 targetProps{
		.pixelFormat{ D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED) },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
	};
	if (FAILED(ctx->CreateBitmap(D2D1::SizeU(maskW, maskH), nullptr, 0, &targetProps, target.GetAddressOf()))) return false;

	ComPtr<ID2D1SolidColorBrush> white;
	if (FAILED(ctx->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 1.f), white.GetAddressOf())) || !white) return false;

	ctx->SetTarget(target.Get());
	ctx->SetTransform(D2D1::Matrix3x2F::Translation((float)-left, (float)-top));
	ctx->BeginDraw();
	ctx->Clear(D2D1::ColorF(0u, 0.0f));   // 全透明黑：掩膜里"没画到"的地方 alpha 就是 0
	HRESULT hr{ S_OK };
	try {
		if (isRect) {
			ctx->FillRectangle(rect, white.Get());
		}
		else {
			ctx->DrawGeometry(path.Get(), white.Get(), strokeWidth, roundStyle.Get());
		}
		hr = ctx->EndDraw();
	}
	catch (...) {
		hr = E_FAIL;
	}
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->SetTarget(nullptr);
	if (FAILED(hr)) return false;

	// GPU 位图不能直接 Map：先拷到 CPU 可读的位图（与 createMosaicBitmap 同样的两步）
	D2D1_BITMAP_PROPERTIES1 cpuProps{
		.pixelFormat{ target->GetPixelFormat() },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	ComPtr<ID2D1Bitmap1> cpuBitmap;
	if (FAILED(ctx->CreateBitmap(D2D1::SizeU(maskW, maskH), nullptr, 0, &cpuProps, cpuBitmap.GetAddressOf()))) return false;
	if (FAILED(cpuBitmap->CopyFromBitmap(nullptr, target.Get(), nullptr))) return false;

	D2D1_MAPPED_RECT mapped{};
	if (FAILED(cpuBitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
	struct MapGuard {
		ID2D1Bitmap1* bmp;
		~MapGuard() { if (bmp) bmp->Unmap(); }
	} mapGuard{ cpuBitmap.Get() };

	mask.assign((size_t)maskW * maskH, 0);
	size_t marked = 0;
	for (UINT32 y = 0; y < maskH; ++y) {
		auto row = mapped.bits + (size_t)y * mapped.pitch;
		for (UINT32 x = 0; x < maskW; ++x) {
			if (row[(size_t)x * 4 + 3] < 128) continue;
			mask[(size_t)y * maskW + x] = 1;
			++marked;
		}
	}
	return marked > 0;
}

// 按 blockSize 分块，每块取平均色再整块填回去 —— 就是马赛克
void ShapeMosaic::mosaicPixels(BYTE* bits, UINT32 pitch, UINT32 width, UINT32 height, int blockSize)
{
	if (!bits || blockSize <= 0 || width == 0 || height == 0) return;

	for (UINT32 y = 0; y < height; y += blockSize) {
		UINT32 yEnd = std::min(y + (UINT32)blockSize, height);
		for (UINT32 x = 0; x < width; x += blockSize) {
			UINT32 xEnd = std::min(x + (UINT32)blockSize, width);
			unsigned long long bSum{ 0 }, gSum{ 0 }, rSum{ 0 }, aSum{ 0 }, count{ 0 };
			for (UINT32 yy = y; yy < yEnd; ++yy) {
				auto row = bits + yy * pitch;
				for (UINT32 xx = x; xx < xEnd; ++xx) {
					auto pixel = row + xx * 4;
					bSum += pixel[0];
					gSum += pixel[1];
					rSum += pixel[2];
					aSum += pixel[3];
					++count;
				}
			}
			if (count == 0) continue;
			BYTE b = (BYTE)(bSum / count);
			BYTE g = (BYTE)(gSum / count);
			BYTE r = (BYTE)(rSum / count);
			BYTE a = (BYTE)(aSum / count);
			for (UINT32 yy = y; yy < yEnd; ++yy) {
				auto row = bits + yy * pitch;
				for (UINT32 xx = x; xx < xEnd; ++xx) {
					auto pixel = row + xx * 4;
					pixel[0] = b;
					pixel[1] = g;
					pixel[2] = r;
					pixel[3] = a;
				}
			}
		}
	}
}

// 三次盒式模糊近似高斯：每遍先水平后垂直，读旧缓冲写新缓冲，半径越大越糊。
// 关键：对每个像素直接对 [x-r, x+r] 的"实际夹取范围"求平均、窗口计数按真实像素数来，
// 绝不把夹取到边界的重复像素重复计数 —— 否则起点那个颜色会被重复加进窗口、
// 把整块区域刷成同一种颜色（例如起点是蓝、后面是白，结果全蓝）。
void ShapeMosaic::blurPixels(BYTE* bits, UINT32 pitch, UINT32 width, UINT32 height, int radius)
{
	if (!bits || radius <= 0 || width == 0 || height == 0) return;
	std::vector<BYTE> tmp((size_t)pitch * height);
	constexpr UINT32 stride{ 4 };
	for (int pass = 0; pass < 3; ++pass) {
		// 水平：bits → tmp
		for (UINT32 y = 0; y < height; ++y) {
			auto src = bits + (size_t)y * pitch;
			auto dst = tmp.data() + (size_t)y * pitch;
			for (UINT32 x = 0; x < width; ++x) {
				int x0 = (int)x - radius;
				if (x0 < 0) x0 = 0;
				int x1 = (int)x + radius;
				if (x1 >= (int)width) x1 = (int)width - 1;
				unsigned long long b{ 0 }, g{ 0 }, r{ 0 }, a{ 0 };
				for (int xx = x0; xx <= x1; ++xx) {
					auto px = src + (size_t)xx * stride;
					b += px[0]; g += px[1]; r += px[2]; a += px[3];
				}
				int cnt = x1 - x0 + 1;
				auto d = dst + (size_t)x * stride;
				d[0] = (BYTE)(b / cnt); d[1] = (BYTE)(g / cnt);
				d[2] = (BYTE)(r / cnt); d[3] = (BYTE)(a / cnt);
			}
		}
		// 垂直：tmp → bits
		for (UINT32 x = 0; x < width; ++x) {
			for (UINT32 y = 0; y < height; ++y) {
				int y0 = (int)y - radius;
				if (y0 < 0) y0 = 0;
				int y1 = (int)y + radius;
				if (y1 >= (int)height) y1 = (int)height - 1;
				unsigned long long b{ 0 }, g{ 0 }, r{ 0 }, a{ 0 };
				for (int yy = y0; yy <= y1; ++yy) {
					auto px = tmp.data() + (size_t)yy * pitch + (size_t)x * stride;
					b += px[0]; g += px[1]; r += px[2]; a += px[3];
				}
				int cnt = y1 - y0 + 1;
				auto d = bits + (size_t)y * pitch + (size_t)x * stride;
				d[0] = (BYTE)(b / cnt); d[1] = (BYTE)(g / cnt);
				d[2] = (BYTE)(r / cnt); d[3] = (BYTE)(a / cnt);
			}
		}
	}
}

void ShapeMosaic::hitTest(const D2D1_POINT_2F& mousePos)
{
	// 只有笔画边缘那一圈算命中：粗笔画（马赛克、橡皮擦尤其明显）整片都能拖的话，
	// 就没法在已有笔画上面再画一笔了 —— 鼠标一按下会变成拖动旧元素。
	// 矩形模式本来就是只有边框附近才响应，这里跟它对齐（判定在 ShapeBase 里）
	if (hitStrokeEdge(linePoints, strokeWidth, mousePos.x, mousePos.y)) hoverDraggerIndex = 8;
}

