#include "pch.h"
#include "WinHost.h"
#include "../Tool/ToolMain.h"
#include "../Tool/ToolSub.h"
#include "../History.h"
#include "../Shape/ShapeBase.h"
#include "../Shape/ShapeRect.h"
#include "../Shape/ShapeEllipse.h"
#include "../Shape/ShapeArrow.h"
#include "../Shape/ShapeNumber.h"
#include "../Shape/ShapeLine.h"
#include "../Shape/ShapeText.h"
#include "../Shape/ShapeMosaic.h"
#include "../Shape/ShapeEraser.h"

// unique_ptr<ToolMain/ToolSub/History> 成员要求这些类型在析构处是完整的，
// 而头文件里只有前置声明，所以析构函数放到这里定义。
WinHost::~WinHost() = default;

// 当前工具"这一组"的最后一个未撤销标注。
//
// 滑条是"工具自己的"，不是"最后那个标注的"：画完矩形再切到文字工具拖字号，
// 该改的是上一个文字（如果有），绝不能顺手把那个矩形改粗 —— 用户说的
// "每个滑条只联动自己组的，不能联动别人去了"就是这个口径。
// 找不到同类就返回 nullptr，宁可不联动，也不去动别人的形状。
ShapeBase* WinHost::shapeOfCurrentTool()
{
	if (!history || !toolMain) return nullptr;
	std::wstring want = toolMain->curId;
	// 矩形和椭圆共用同一个工具按钮，具体落哪个由副编辑器上的形状单选决定
	if (want == L"rect" && toolSub && toolSub->isEllipseShape) want = L"ellipse";
	// 箭头工具二选一：选"直线"时画的是 ShapeLine，滑条/颜色要联动的就是它
	if (want == L"arrow" && toolSub && toolSub->isArrowStraight) want = L"line";
	// 画笔也二选一：选"选框"时画的是填充框（ShapeRect），剩下的手绘是 ShapeLine
	if (want == L"brush" && toolSub && toolSub->isBrushMarquee) want = L"rect";
	if (want.empty()) return nullptr;
	auto matches = [&](ShapeBase* s) {
		if (want == L"rect")    return dynamic_cast<ShapeRect*>(s) != nullptr;
		if (want == L"ellipse") return dynamic_cast<ShapeEllipse*>(s) != nullptr;
		if (want == L"arrow")   return dynamic_cast<ShapeArrow*>(s) != nullptr;
		if (want == L"line")    return dynamic_cast<ShapeLine*>(s) != nullptr;
		if (want == L"number")  return dynamic_cast<ShapeNumber*>(s) != nullptr;
		if (want == L"brush")   return dynamic_cast<ShapeLine*>(s) != nullptr;
		if (want == L"text")    return dynamic_cast<ShapeText*>(s) != nullptr;
		// 马赛克与高斯模糊共用这颗工具：两档画出来的都是 ShapeMosaic，都该跟着联动
		if (want == L"mosaic")   return dynamic_cast<ShapeMosaic*>(s) != nullptr;
		if (want == L"eraser")  return dynamic_cast<ShapeEraser*>(s) != nullptr;
		return false;
	};
	// 联动只认"最后一次动过的那一条"，而且必须正好是当前工具这一类。
	// 用户的口径：编辑了方框、又去编辑椭圆、又去编辑序号之后，联动只对序号有效；
	// 这时再切回矩形工具也不许回头把那只旧方框改了 —— 旧标注已经"过期"。
	// 以前是按类型在历史里找"最后一个同类"，切回矩形就能翻出那只旧方框，
	// 正是用户说的"不能回到方框联动又好了"
	//
	// 右键解锁的那一条算"最后动过的"（它可能压根不是历史里的最后一个）
	if (editTarget) {
		if (editTarget->isUndo) return nullptr;
		return matches(editTarget) ? editTarget : nullptr;
	}
	// 否则只看历史里最后一个未撤销的标注：类型不符就此打住，不再往前翻
	for (int i{ (int)history->shapes.size() - 1 }; i >= 0; i--) {
		auto s = history->shapes[i].get();
		if (!s || s->isUndo) continue;
		return matches(s) ? s : nullptr;
	}
	return nullptr;
}

// 这一条算不算"可操作的那一条"：右键解锁出来的、或历史里最后一个未撤销的标注。
// 与 shapeOfCurrentTool 是两件事：那个按"当前工具的类型"挑，这个只按"是不是最后一条"挑 ——
// 悬停亮框、点击落下、画手柄全用它，不跟工具类型走（否则切到别的工具时，
// 上一条标注的框和手柄会突然全灭，看着像丢了东西）
bool WinHost::isEditableShape(const ShapeBase* s) const
{
	if (!s || !history) return false;
	if (s->isUndo) return false;
	// 右键解开的那一条算"最后动过的"（正常流程里它本来就是最后一个，
	// 这里显式认一下，免得以后解锁范围变了两边对不上）
	if (editTarget == s) return true;
	for (int i{ (int)history->shapes.size() - 1 }; i >= 0; i--) {
		auto cur = history->shapes[i].get();
		if (!cur || cur->isUndo) continue;
		return cur == s;
	}
	return false;
}

// 悬停探测。两个宿主的规则本来一模一样，重复了两份，这里合成一处。
//
// 规则只有一条：**只认最后一个未撤销的标注**。
// 用户的口径：画了第一个椭圆、又画了第二个之后，第一个就既不能被选中也不能被编辑，
// 想改它只能回退。以前这里还会把"任意一个标注"都探一遍，鼠标扫过画布时
// 早期那些图形的框、四角、旋转柄会跟着亮出来（橡皮擦那条反馈就是这个），
// 既看着乱，也会让人以为能点它。
void WinHost::probeHover(const POINT pos)
{
	if (!history) return;
	const auto p = toImgPos(pos);
	ShapeBase* last = nullptr;
	for (int i{ (int)history->shapes.size() - 1 }; i >= 0; i--) {
		auto cur = history->shapes[i].get();
		if (!cur || cur->isUndo) continue;
		last = cur;
		break;
	}
	ShapeBase* show = nullptr;
	ShapeBase* hover = nullptr;
	if (last) {
		last->mouseMove((float)p.x, (float)p.y);
		// 要显示（亮框/亮手柄）和能交互（点得住、拖得动）分开记：
		//   show  —— 本条自己说"该亮"（命中框/柄，或离得近）才算；
		//   hover —— 命中判定有落点（>= 0）就算。
		// 以前这里是 shapeShow = show ? show : hover，编辑中的文字会被 hover
		// 兜底也送进 paintDragger，把四角和旋转柄一起画出来 —— 用户要的是
		// "刚开始输入时只有一圈虚线框，四角一个都不要"（见 ShapeText::paintDragger）
		if (last->showDragger()) show = last;
		// 刚画完的那一笔：手柄常亮（用户口径"画完四周的小方框直接显示，方便快速找到拉角"）。
		// 只在它仍然是最后一条时生效 —— 又画了新的一笔、被撤销/删除、或者它正在打字，
		// 都自动回到"悬停才亮"。打字那条不走这里：文本框有自己的手柄规则（见 ShapeText::showDragger）
		if (draggerPin && (last != draggerPin || !isEditableShape(draggerPin) || draggerPin == editingText)) {
			draggerPin = nullptr;
		}
		if (!show && draggerPin) show = draggerPin;
		if (last->hoverDraggerIndex >= 0) hover = last;
	}
	const bool changed{ shapeHover != hover || shapeShow != show };
	// 能交互的那个一定也是要显示的那个
	shapeHover = hover;
	shapeShow = show;
	// 旧的那条被回退/删掉之后，editTarget 可能指向已经不在栈上的对象，
	// 这里顺手清掉，别让后面的联动拿着一根悬空指针
	if (editTarget && editTarget->isUndo) editTarget = nullptr;
	if (changed) requestPaint();
}

// 离屏合成。两个宿主以前各有一份几乎逐行相同的实现（唯一差别是"取底图哪一块、
// shape 要不要平移"），踩过的坑全一样：target 位图必须无论成败都 EndDraw + 解绑、
// GPU 位图不能直接 Map、mapped.pitch 要逐行紧缩、底图 alpha 全 0 必须补成不透明。
// 合成这一处后，上面每一条都只写一遍。
// 为什么要离屏而不是直接画到 screenImg 上：screenImg 是 ShapeEraser 的"原样"来源、
// ShapeMosaic 的取样来源，一旦被 shape 覆写，之后再擦除/打码就会拿到已经画过的画面
bool WinHost::compositePixels(const D2D1_SIZE_U size, const D2D1_RECT_F& src,
	const float shiftX, const float shiftY, std::vector<BYTE>& pixels)
{
	// 下面每一样都是硬依赖：设备不可用（远程桌面 / 驱动重置）就没有 DeviceContext，
	// history 为空就没有 shape 可画，screenImg 为空连底图都没有
	if (!screenImg || !history || size.width == 0 || size.height == 0) return false;
	if (!ui::D2D::ready()) return false;
	// 编辑中的文字是 TextBox 自己那层画的，进不了下面这个离屏 target。
	// 先收尾，把文字交回 ShapeText 自己画，保存/复制出去的图才有它
	if (editingText) editingText->finishEdit();
	auto ctx = ui::D2D::get()->deviceContext.Get();

	D2D1_BITMAP_PROPERTIES1 targetProps{
		.pixelFormat{ D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED) },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
	};
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBmp;
	auto hr = ctx->CreateBitmap(size, nullptr, 0, &targetProps, targetBmp.GetAddressOf());
	if (FAILED(hr)) return false;

	ctx->SetTarget(targetBmp.Get());
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->BeginDraw();
	// 这一段必须保证"无论如何都解绑 target 并 EndDraw"：
	// 共享的 deviceContext 一旦留在 BeginDraw 状态，之后所有窗口的绘制都会错乱
	hr = S_OK;
	try {
		ctx->Clear(D2D1::ColorF(0, 0.0f));
		ctx->DrawBitmap(screenImg.Get(), D2D1::RectF(0, 0, (float)size.width, (float)size.height), 1.0f,
			D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &src);
		ctx->SetTransform(D2D1::Matrix3x2F::Translation(shiftX, shiftY));
		for (auto& shape : history->shapes)
		{
			if (!shape->isUndo) shape->paint(ctx);
		}
		hr = ctx->EndDraw();
	}
	catch (...) {
		// 任何一个 shape 的 paint 抛出（D2D 资源被回收、几何体异常）都要落到这里：
		// 异常穿出 C ABI 就是 terminate，进程直接消失
		hr = E_FAIL;
	}
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->SetTarget(nullptr);
	if (FAILED(hr)) return false;

	// GPU 上的 target 位图不能直接 Map，先拷到一块带 CPU_READ 的位图上
	D2D1_BITMAP_PROPERTIES1 cpuProps{
		.pixelFormat{ targetBmp->GetPixelFormat() },
		.dpiX{ 96.0f }, .dpiY{ 96.0f },
		.bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
	};
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> cpuBmp;
	hr = ctx->CreateBitmap(size, nullptr, 0, &cpuProps, cpuBmp.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = cpuBmp->CopyFromBitmap(nullptr, targetBmp.Get(), nullptr);
	if (FAILED(hr)) return false;
	D2D1_MAPPED_RECT mapped{};
	hr = cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped);
	if (FAILED(hr)) return false;
	// 作用域守卫：下面任何一步提前 return（含 vector 分配抛 bad_alloc）都要把 Map 解开，
	// 否则这块位图会一直处于"已映射"状态，再次 Map 直接失败
	struct MapGuard {
		ID2D1Bitmap1* bmp;
		~MapGuard() { if (bmp) bmp->Unmap(); }
	} mapGuard{ cpuBmp.Get() };
	// mapped.pitch 按 GPU 行对齐，可能大于 w*4；剪切板和 WIC 都要求紧凑步长，逐行紧缩
	const UINT32 rowBytes = size.width * 4;
	pixels.resize((size_t)rowBytes * size.height);
	for (UINT32 row = 0; row < size.height; ++row)
	{
		auto dst = pixels.data() + (size_t)row * rowBytes;
		CopyMemory(dst, mapped.bits + (size_t)row * mapped.pitch, rowBytes);
		// 底图是 GDI 抓来的，alpha 全 0；target 位图是 PREMULTIPLIED，画进去之后整张图
		// 都是半透明，PNG 和剪切板会当真。导出的是成品图，统一按不透明补上
		for (UINT32 i = 3; i < rowBytes; i += 4) dst[i] = 255;
	}
	return true;
}

