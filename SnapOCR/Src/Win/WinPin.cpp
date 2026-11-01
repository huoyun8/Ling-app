#include "pch.h"
#include "../Shape/ShapeBase.h"
#include "../Tool/ToolMain.h"
#include "../Tool/ToolSub.h"
#include "WinPin.h"
#include "WinCap.h"
#include "History.h"
#include "../App.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Util.h"

// 说明：钉屏态不建 ToolMain / ToolSub（见构造函数注释），include 这两个头只是因为
// WinHost 的 unique_ptr 成员要求在这些类型完整的编译单元里实例化宿主的构造 / 析构。

using namespace Microsoft::WRL;
namespace {
	std::vector<std::unique_ptr<WinPin>> winPins;

	// 贴图窗口比图每边多出的那 1px：专门用来画"隔离带"。
	// 贴图是浮在别人界面上的，图的边缘和背后的窗口直接贴在一起时分不清边界，
	// 拉一条 1px 的半透明线就把两层分开了
	constexpr int kPad = 1;

	// 贴图落屏时那圈 1px 细线依次轮换的颜色。选的都是高饱和、浅底深底都看得见的色，
	// 半秒之内走完一轮，眼睛能立刻抓到"图已经贴在这儿了"。
	// 原来走两轮（0.8 秒），拖得太长，像卡住了
	const D2D1_COLOR_F kPinFlashColors[] = {
		D2D1::ColorF(0xffffff),   // 白
		D2D1::ColorF(0x00e5ff),   // 青
		D2D1::ColorF(0x1677ff),   // 蓝（和常态边框同色，过渡不突兀）
		D2D1::ColorF(0xb388ff),   // 紫
	};
	constexpr int kPinFlashColorCount = (int)(sizeof(kPinFlashColors) / sizeof(kPinFlashColors[0]));
	// 4 色 × 1 轮，每步 90ms = 0.36 秒：够看清"线在闪"，又不至于让人等它。
	// （70ms 那版偏快，用户看着像"闪了一下就没了"，定稿 90ms）
	constexpr int kPinFlashSteps = kPinFlashColorCount;
	constexpr UINT kPinFlashTimerMs = 90;
	constexpr UINT kPinFlashTimerId = 102;
}

WinPin::WinPin(int x, int y, int w, int h, const std::vector<BYTE>* data) : WinHost()
{
	// 窗口比图每边大 1px（kPad）：多出来的那一圈留给自己画的隔离带，
	// 图本身不缩水、坐标也不用重算 —— 只是整体往里挪 1px
	this->x = x - kPad;
	this->y = y - kPad;
	history = std::make_unique<History>(this);
	// 宽高原样是物理像素的 int，WinBase 里存的是 float：显式转一次，
	// 免得每次编译都冒一条"可能丢失数据"（这两个值远小于 float 的整数上限）
	this->w = static_cast<float>(w + kPad * 2);
	this->h = static_cast<float>(h + kPad * 2);
	// 设备起不来时（D3D 与 WARP 都失败）deviceContext 是空的：跳过建底图，
	// 后面按"没底图"的路径处理（窗口尺寸归零、绘制走空分支），不能直接解引用
	auto d2d = ui::D2D::get();
	if (data && d2d && d2d->deviceContext) {
		// 外部像素建底图。ShapeMosaic / ShapeEraser 会把它当画刷源，属性与 getCutImg() 出来的保持一致
		D2D1_BITMAP_PROPERTIES1 props{};
		props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
		props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
		props.dpiX = 96.0f;
		props.dpiY = 96.0f;
		d2d->deviceContext->CreateBitmap(D2D1::SizeU(w, h), data->data(), w * 4, &props, screenImg.GetAddressOf());
	}
	else if (!data) {
		// 从 WinCap 手里拿选区那一块。WinCap 可能已经先一步关掉了（异常路径），
		// 那就当"没有底图"处理，下面会把窗口尺寸归零、绘制走空分支
		if (auto cap = WinCap::get()) screenImg = cap->getCutImg();
	}
	// 底图没建起来（抓屏失败 / 选区退化）时把窗口尺寸归零，后面所有"按底图尺寸"的运算都得到 0，
	// 绘制和导出会直接走空分支，而不是拿一块不存在的位图往下画
	if (!screenImg) {
		this->w = 0.f;
		this->h = 0.f;
	}
	// 钉屏态：WinPin 只由 WinCap 贴屏创建，出来的是"成品图"，不带编辑器 ——
	// 不建 ToolMain / ToolSub（原版虽建了但 pinned 恒为 true，工具条从建起到销毁一次都没显示过），
	// 也就没有工具选中态，左键从头到尾只有"拖窗口"一个含义。
	// history 仍要建：导出像素走 WinHost::compositePixels，那里要遍历 history->shapes
	onMouseDown.add([this](POINT pos, BOOL isRight) {this->onDown(pos, isRight);});
	onMouseMove.add([this](POINT pos) {this->onMove(pos);});
	onMouseUp.add([this](POINT pos, BOOL isRight) {this->onUp(pos, isRight);});
	// Ui 传进来的是已经换算成滚动距离的 space（一格 = 60 逻辑像素 × dpi），
	// shape 只关心方向，这里按符号还原成 ±WHEEL_DELTA
	onMouseWheel.add([this](POINT pos, float space) {
		// Ui 的滚轮事件不带修饰键状态，自己查：按住 Ctrl 是缩放窗口。
		// 不带 Ctrl 的滚轮没有别的用途（没有 shape 可滚），忽略
		if (GetKeyState(VK_CONTROL) & 0x8000) {
			// 一格 10%，按当前倍数等比走，放大和缩小的手感才对称
			applyScale(scale * (space > 0 ? 1.1f : 1.f / 1.1f), pos);
		}
	});
	onTimer.add([this](UINT id) {this->onTimerCB(id);});
	onKeyDown.add([this](UINT key) {this->onKey(key);});
	onDestroy.add([this]() { this->onClosed(); });
	// DPI 变了（用户改了缩放比例，或者窗口被拖到缩放比例不同的显示器上）：系统会按新旧缩放比
	// 把窗口整体放大一圈，但贴图窗口的尺寸是钉死在底图像素上的 —— 底图按原始像素画，
	// 跟着缩放只会让窗口比图大一圈：右边、下边多出一条空白，边框看着比左上两边粗，
	// 位置也不能在 onDpiChanged 里改 —— 那个事件在系统建议矩形生效之前触发，改了马上被覆盖。
	// 所以等建议矩形应用完（紧随而来的 WM_SIZE）再把尺寸掰回底图大小
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		applyWinSize();
	});
}

// WinBase::close() 里 DestroyWindow 之后同步触发 onDestroy。此时可能正处在按键处理、
// 复制 / 保存的收尾栈上：窗口句柄立即销毁（用户马上看到界面消失），
// C++ 对象的释放推迟到下一轮消息循环，栈上访问 this 仍是安全的
void WinPin::onClosed()
{
	// 防止 close() 被走两遍（比如快捷键和复制收尾先后触发）时排两次销毁
	if (isClosed) return;
	isClosed = true;
	// 先收起悬停指针 —— 它们指向 history 里的元素，history 随 WinPin 一起析构，留着悬空指针没意义
	shapeHover = nullptr;
	shapeShow = nullptr;
	editingText = nullptr;
	// screenImg / canvas / history 都是成员（canvas 挂在 body 的子节点上），随下面这次 erase 一并释放
	auto app = ui::App::get();
	// App 已经在退出流程里被拆掉（先收窗口再拆单例）：排不进队列，也没必要排
	if (!app) return;
	app->dq.TryEnqueue([this]() {
		std::erase_if(winPins, [this](const std::unique_ptr<WinPin>& p) { return p.get() == this; });
		// 队列是异步的，这一轮里 App 可能已经没了 —— 再解引用就是崩溃
		auto app = ui::App::get();
		if (!app) return;
		// 用完即走模式下，最后一个贴图窗口关掉就退出进程，不驻留在系统里。
		// 贴图可以同时开好几个（标注、扫码各来一张），所以得等它们都没了才退
		if (winPins.empty()) {
			if (app->args[L"--auto-quit"] == L"true") {
				app->quit(0);
			}
		}
	});
}

bool WinPin::hasWindow()
{
	return !winPins.empty();
}

void WinPin::dispose()
{
	winPins.clear();
}

D2D1_SIZE_U WinPin::getImgSize() const
{
	if (!screenImg) return D2D1::SizeU(0, 0);
	// 要的是像素数，所以问 GetPixelSize 而不是 GetSize（后者返回的是按位图自身 dpi 折算的 DIP）
	return screenImg->GetPixelSize();
}

void WinPin::applyWinSize()
{
	auto sz = getImgSize();
	if (!hwnd || sz.width == 0 || sz.height == 0) return;
	auto newW = std::max(1, static_cast<int>(std::lround(sz.width * scale))) + kPad * 2;
	auto newH = std::max(1, static_cast<int>(std::lround(sz.height * scale))) + kPad * 2;
	w = static_cast<float>(newW);
	h = static_cast<float>(newH);
	// 不走 setSize：它收的是逻辑像素、内部还要乘一遍 dpi，而这里的宽高本来就是物理像素
	SetWindowPos(hwnd, nullptr, 0, 0, newW, newH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
}

POINT WinPin::toImgPos(const POINT& pos) const
{
	// 先减掉那 1px 隔离带：窗口客户区原点在图的左上角左边 1px
	const POINT p{ pos.x - kPad, pos.y - kPad };
	if (scale == 1.f) return p;
	return POINT{ static_cast<LONG>(std::lround(p.x / scale)), static_cast<LONG>(std::lround(p.y / scale)) };
}

void WinPin::applyScale(float newScale, POINT anchor)
{
	auto sz = getImgSize();
	if (sz.width == 0 || sz.height == 0) return;
	// 上限跟着底图大小走：窗口边长再大，swap chain 那块显存也吃不消，人也看不过来；
	// 但至少要能回到 1 倍，所以下面用 max 兜一下
	auto maxScale = std::max(1.f, std::min(8.f, 16000.f / std::max(sz.width, sz.height)));
	newScale = std::clamp(newScale, 0.1f, maxScale);
	if (std::abs(newScale - scale) < 0.0001f) return;
	// anchor 底下那个底图上的点，缩放前后都要停在光标下：屏幕坐标 = 窗口原点 + 底图点 × 倍数
	auto imgX = (anchor.x - kPad) / scale;
	auto imgY = (anchor.y - kPad) / scale;
	scale = newScale;
	applyWinSize();
	// setPosition 内部会 SetWindowPos，随后的 WM_MOVE 会带出 onMoved。
	// 减 kPad：图起点在窗口客户区的 (1,1)，光标要压住的那个图上点在新位置也要在 (kPad + img×新倍数)
	setPosition(x + anchor.x - kPad - static_cast<int>(std::lround(imgX * scale)),
		y + anchor.y - kPad - static_cast<int>(std::lround(imgY * scale)));
	scaleTip = ui::D2D::get()->makeTextLayout(std::format(L"{}%", static_cast<int>(std::lround(scale * 100.f))), 11.f * dpi);
	// 停手 800 毫秒后由定时器把倍数提示收掉。同一个 id 再调一次 SetTimer 就是重新计时，
	// 所以连续滚动期间它一直不会触发
	setTimer(800, 101);
	refresh();
}

// 画在窗口右上角，半透明底 + 白字，与 CutMask 上那个坐标标签一个路子。
// 调用方要先把缩放变换收回去：这是窗口装饰，不跟着图一起放大
void WinPin::paintScaleTip(ID2D1DeviceContext* ctx)
{
	if (!scaleTip || !brushTipBg) return;
	DWRITE_TEXT_METRICS tm{};
	if (FAILED(scaleTip->GetMetrics(&tm))) return;
	auto pad = 3.f * dpi;
	auto margin = 5.f * dpi;
	D2D1_RECT_F bgRect{ w - margin - tm.width - pad * 2, margin, w - margin, margin + tm.height + pad * 2 };
	// 图小到装不下提示时，贴着左边画，别画到窗口外面去
	if (bgRect.left < margin) bgRect.left = margin;
	ctx->FillRectangle(bgRect, brushTipBg.Get());
	ctx->DrawTextLayout({ bgRect.left + pad, bgRect.top + pad }, scaleTip.Get(), brushTipText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

WinPin::~WinPin()
{
}

// 建窗并入表。两条入口只差底图来源（WinCap 的截屏位图 / 外部像素），失败收尾完全一样
void WinPin::create(int x, int y, int w, int h, const std::vector<BYTE>* data)
{
    // 整个构造过程要建窗口、过 WinRT 组合器与 D2D：任何一步抛异常都必须在这里拦住。
    // 本函数是从 WinCap 工具条的"贴图"按钮回调一路调进来的，而 winrt::delegate 的 Invoke 是 noexcept，
    // 异常穿出去就是 std::terminate —— 用户点一下"贴图"，程序直接消失
    try {
        auto ptr = new WinPin(x, y, w, h, data);
        std::unique_ptr<WinPin> winPin{ ptr };
        if (!ptr->createNativeWindow(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_POPUP) || !ptr->alive()) {
            // 建窗失败：对象在这里析构，绝不入表（否则 hasWindow() 永远为真，还永远关不掉）
            return;
        }
        winPins.push_back(std::move(winPin));
    }
    catch (...) {
        // 底图没建起来，就当这次贴图没发生：调用方（WinCap）随后会关掉自己，
        // 行为与"贴图失败"一致，不会有半死不活的窗口留在屏幕上
    }
}

void WinPin::init(int x, int y, int w, int h)
{
    create(x, y, w, h, nullptr);
}

void WinPin::initFromData(int x, int y, int w, int h, std::vector<BYTE>& data)
{
    create(x, y, w, h, &data);
}

void WinPin::onCreated()
{
    disableBorderRadius();
    auto d2d = ui::D2D::get();
    // 图形设备没起来时 deviceContext 是空的（见 D2D::initDevice）：这里就地判空，
    // 只把窗口显示出来，画布与画刷都不建 —— 绘制路径上每一处都对空画刷做了保护，
    // 顶多看到一张空图，不会一按"贴图"就把进程带走
    if (!d2d || !d2d->deviceContext) {
        show();
        return;
    }
    // 画布铺满窗口，走 swap chain（双缓冲）后端，避免拖动窗口时整帧闪烁
    canvas = body->makeChild<ui::Canvas>();
    canvas->enableSwapChain();
    canvas->setSizePercent(100.f, 100.f);
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x1677ff), borderBrush.GetAddressOf());
    // 隔离带：外圈一条 1px 半透明黑，贴在别人的窗口上时把两层压开。
    // 半透明所以图边缘的颜色还透得出来，不会像描了一圈实心黑框
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.30f), outerEdgeBrush.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.46f), brushTipBg.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushTipText.GetAddressOf());
    // 到场提示：贴上的瞬间，四周那圈 1px 细线按 白→青→蓝→紫 走完一轮就收。
    // 只动这一根细线的颜色，不改粗细、不遮画面
    d2d->deviceContext->CreateSolidColorBrush(kPinFlashColors[0], flashBrush.GetAddressOf());
    show();
    // 贴图没有"完成"的过场动画，界面上一点动静都没有，用户常常不确定到底贴上没
    flashTicks = kPinFlashSteps;
    flashIdx = 0;
    setTimer(kPinFlashTimerMs, kPinFlashTimerId);
}

void WinPin::layout()
{
    ui::WinBase::layout();
    // 底图 / 画布没有（D2D 设备不可用、建窗失败）就没有可画的东西，直接收工 ——
    // 下面 screenImg->GetSize() 与 history->shapes 都是硬依赖
    if (!screenImg || !canvas || !history) return;
    auto ctx = canvas->startPaint();
    if (!ctx) return;
    ctx->Clear(0);
    auto sz = screenImg->GetSize();
    D2D1_RECT_F destRect = D2D1::RectF(0, 0, sz.width, sz.height);
    // 底图按底图像素画，缩放整个交给这个变换。
    // 末尾再平移 kPad：窗口比图每边大 1px，图要从那圈隔离带里面开始画
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale)
        * D2D1::Matrix3x2F::Translation((float)kPad, (float)kPad));
    ctx->DrawBitmap(screenImg.Get(), destRect);
	// 钉屏态画不出任何 shape（没有编辑器，历史永远为空），这个循环实际上一笔都不画；
	// 保留它与 WinCap 同一结构，compositePixels 导出时也是同一套遍历
	for (auto& shape : history->shapes)
	{
		if (!shape->isUndo) {
			shape->paint(ctx);
		}
	}
	// 蓝边框和倍数提示属于窗口装饰，不跟着图缩放：变换收回来，按窗口坐标画。
	// 边界做一条隔离线就够：最外侧 1px 半透明黑，贴在别人的窗口上时把两层压开。
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	const float px = 1.f;   // 物理 1px：这条线按屏幕像素算，不跟 dpi 放大
	ctx->DrawRectangle(D2D1::RectF(px * .5f, px * .5f, w - px * .5f, h - px * .5f), outerEdgeBrush.Get(), px);
	ctx->DrawRectangle(D2D1::RectF(px * 1.5f, px * 1.5f, w - px * 1.5f, h - px * 1.5f), borderBrush.Get(), px);
	// 贴图到场提示：同一位置盖一根 1px 细线，颜色由定时器逐步换（白→青→蓝→紫），
	// 于是看到的是"细线在变色"而不是边框变粗
	if (flashTicks > 0 && flashBrush) {
		const float fw = 1.f * dpi;
		ctx->DrawRectangle(D2D1::RectF(fw * .5f, fw * .5f, w - fw * .5f, h - fw * .5f), flashBrush.Get(), fw);
	}
	paintScaleTip(ctx);
    canvas->finishPaint();
}

void WinPin::onMinMaxInfo(MINMAXINFO* mmi)
{
	auto [x, y, w, h] = App::get()->getScreenArea();
	mmi->ptMaxPosition.x = x;
	mmi->ptMaxPosition.y = y;
	mmi->ptMaxSize.x = w;
	mmi->ptMaxSize.y = h;
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
	// Ctrl+滚轮放大后窗口可以比屏幕大（超出的部分自然被裁掉）。默认的最大跟踪尺寸只有
	// 主显示器那么大，不放开的话 SetWindowPos 会被系统按住，放大就到此为止了
	mmi->ptMaxTrackSize.x = 20000;
	mmi->ptMaxTrackSize.y = 20000;
}

void WinPin::onDown(POINT pos, BOOL isRight)
{
	// 钉屏态没有编辑器：右键没有"收起 / 展开工具条"可做，什么都不发生
	if (isRight) return;
	// 双击判定得自己做，做法同 WinCap::onDown：Ui 的窗口类没带 CS_DBLCLKS，
	// WM_LBUTTONDBLCLK 根本不会来，只能拿系统的双击间隔和双击判定框自己认。
	// 与 WinCap 唯一的不同是这里比屏幕坐标而不是客户区坐标：拖动贴图窗口时窗口跟着光标走，
	// 抓住的那一点始终停在光标下，光标的客户区坐标几乎不变 —— 用客户区坐标会把
	// "拖一下松手再拖一下"当成双击，图就这么被复制走关掉了。
	// 而拖动必然意味着光标在屏幕上真的移动过，屏幕坐标能把这种情况分开
	POINT screenPos{};
	GetCursorPos(&screenPos);
	auto now = GetTickCount64();
	bool isDblClick = (now - lastDownTime <= GetDoubleClickTime())
		&& std::abs(screenPos.x - lastDownPos.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
		&& std::abs(screenPos.y - lastDownPos.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
	lastDownTime = now;
	lastDownPos = screenPos;
	// 双击 = Ctrl+C：把图送进剪切板并关窗。要在下面所有分支之前处理：
	// 这一下不是拖窗，不该留下 capture
	if (isDblClick) {
		copyToClipboard();
		return;
	}
	// 记的是按下点在窗口内的偏移（客户区坐标），拖动时用它把抓住的那一点保持在光标下。
	// 钉屏态左键只有拖窗口一个用途（没有工具条可选画笔）
	pressPos.x = pos.x;
	pressPos.y = pos.y;
	isMouseDown = true;
	SetCapture(hwnd);
}

void WinPin::onMove(POINT pos)
{
	// 钉屏态没有编辑器与悬停判定：移动只有在按住左键时才有意义 —— 拖窗口
	if (!isMouseDown) return;
	setPosition(x + pos.x - pressPos.x, y + pos.y - pressPos.y);
}

void WinPin::onUp(POINT pos, BOOL isRight)
{
	// 右键按下时什么都没抓（见 onDown），抬手也就没什么要收的
	if (isRight) return;
	isMouseDown = false;
	ReleaseCapture();
}

void WinPin::onTimerCB(UINT id)
{
	if (id == kPinFlashTimerId) { //贴图到场提示：细线换下一种颜色，换完就收
		// 画刷没建起来（D2D 初始化失败）就直接收摊，别再空指针崩一次
		if (flashTicks <= 0 || !flashBrush) {
			flashTicks = 0;
			killTimer(kPinFlashTimerId);
			return;
		}
		flashIdx = (flashIdx + 1) % kPinFlashColorCount;
		flashBrush->SetColor(kPinFlashColors[flashIdx]);
		if (--flashTicks == 0) killTimer(kPinFlashTimerId);
		refresh();
		return;
	}
	if (id == 101) { //缩放停手了，收掉右上角的倍数提示
		killTimer(101);
		scaleTip = nullptr;
		refresh();
		return;
	}
	if (id != 100) return;
	if (!shapeHover) {
		refresh();
		killTimer(100);
	}
}

ui::TextBox* WinPin::getTextBox()
{
	// WinHost 的纯虚接口。钉屏态没有文字编辑（没有 text 工具可点），
	// 这个框永远用不上，也就永远不必建 —— 返回空，调用方（ShapeText）在贴图窗里不会来
	return textBox;
}

void WinPin::setEditingText(ShapeText* shape)
{
	// WinHost 的纯虚接口。钉屏态没有文字编辑，ShapeText 不会在贴图窗里进入编辑态
	editingText = shape;
}

void WinPin::onToolStyleChanged()
{
	// WinHost 的纯虚接口。钉屏态没有副编辑器（ToolSub），没有任何回调会走到这里
}

void WinPin::onKey(UINT key)
{
	if (!history) return;   // 历史没建起来：撤销 / 删除 / 复制都没有意义
	bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	if (ctrl && key == 'Z') {
		history->undo();
	}
	else if (ctrl && key == 'Y') {
		history->redo();
	}
	else if (ctrl && key == 'C') {
		copyToClipboard();
	}
	else if (ctrl && key == 'S') {
		saveToFile();
	}
	else if (key == VK_RETURN) {
		copyToClipboard();
	}
	else if (key == VK_DELETE) {
		history->removeHoverShape();
	}
	else if (key == VK_ESCAPE) {
		close();
	}
}

void WinPin::pinToScreen()
{
	// 建窗时已带 WS_EX_TOPMOST，这里把置顶再钉一次（WinHost 的纯虚接口，兜底用途）。
	// 钉屏态没有工具条可收 —— WinPin 从构造起就不带编辑器
	SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void WinPin::copyToClipboard()
{
	std::vector<BYTE> pixels;
	D2D1_SIZE_U size{};
	if (!getImagePixels(pixels, size)) return;
	// 与 WinCap 同一口径：剪切板拿不到时别关窗，否则贴着的这张图就白白丢了
	if (!Util::saveToClipboard((int)size.width, (int)size.height, pixels.data())) {
		if (auto app = ui::App::get()) app->showToast(Lang::get(L"cap.copyFail"));
		return;
	}
	close();
}

void WinPin::saveToFile()
{
	// 保存直接落到设置页选定的截图目录（默认桌面），与截图主窗口一致
	std::vector<BYTE> pixels;
	D2D1_SIZE_U size{};
	if (!getImagePixels(pixels, size)) return;
	auto path = Util::savePathIn(Setting::get()->getShotDir(), L"png");
	// 连桌面目录都拿不到（极少数策略禁用账户）：必须给一句提示。
	// 以前这里直接 return，用户按了 Ctrl+S、界面毫无动静，只能一遍遍重按
	if (path.empty()) {
		if (auto app = ui::App::get()) app->showToast(Lang::get(L"cap.saveFail"));
		return;
	}
	if (Util::saveToFile(path, (int)size.width, (int)size.height, pixels.data())) {
		// 与截图主窗口同一条收尾：软件内的半透明轻提示，不再弹托盘气泡
		if (auto app = ui::App::get()) app->showToast(Lang::get(L"cap.savedTip"));
		close();
		return;
	}
	// 写盘失败（目录只读、U 盘拔了、磁盘满、同名文件被占用）同样要说出原因，
	// 否则用户看到的就是"点了没反应"，还以为是贴图窗口不响应键盘
	if (auto app = ui::App::get()) app->showToast(Lang::get(L"cap.saveFail"));
}

// 导出像素：整张贴图，始终按原始像素尺寸（不跟窗口的缩放倍数）。
// 离屏合成本身在 WinHost::compositePixels，与截图窗共用一套 —— 取整张底图、
// shape 不平移（WinPin 的 shape 坐标本来就是底图坐标）
bool WinPin::getImagePixels(std::vector<BYTE>& pixels, D2D1_SIZE_U& size)
{
	// 尺寸一律取底图的像素尺寸，不用窗口的 w/h —— Ctrl+滚轮缩放改的是窗口，
	// 导出的图该始终是原始大小。调用方也得按这个尺寸解释 pixels，所以用出参交出去
	auto imgSize = getImgSize();
	if (imgSize.width == 0 || imgSize.height == 0) return false;
	size = imgSize;
	// 取样矩形按位图自己的 DIP 尺寸给（底图恒为 96dpi，于是它就等于像素尺寸）；
	// 输出尺寸仍是像素尺寸，两者一致，画出来就是 1:1 的原图
	auto srcSize = screenImg->GetSize();
	return compositePixels(imgSize, D2D1::RectF(0.f, 0.f, srcSize.width, srcSize.height), 0.f, 0.f, pixels);
}

BOOL WinPin::setCursor()
{
	// 钉屏态没有工具条 / 编辑器 / 悬停形状：整个窗口就是一个可拖动的图，永远给移动光标
	SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
	return TRUE;
}

// WinHost 的纯虚接口。钉屏态不建工具条（见构造函数注释），没有东西可摆，
// 也就没有"按窗口 / 选区摆工具条"这回事 —— 空实现，完整版在 WinCap 那边
void WinPin::layoutTools()
{
}
