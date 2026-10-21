#include "pch.h"
#include <include/Ling.h>
#include "WinCap.h"
#include "WinPin.h"
#include "WinHost.h"
#include "CutMask.h"
#include "../Tool/ToolMain.h"
#include "../Tool/ToolSub.h"
#include "../History.h"
#include "../Shape/ShapeBase.h"
#include "../Shape/ShapeText.h"
#include "../Shape/ShapeMosaic.h"
#include "../App.h"
#include "../Util.h"
#include "../Update.h"
using namespace Microsoft::WRL;

namespace
{
    constexpr float scaleNum{ 5.f }, srcW{ 50.f }, srcH{ 30.f };
    constexpr float pixImgH{ scaleNum * srcH };
    constexpr float pixW{ srcW * scaleNum };

    // 把坐标夹在 [min, max-size] 里，用于把工具条保持在显示器工作区范围内
    int clampPos(float val, float size, int min, int max)
    {
        auto upper = max - static_cast<int>(size);
        if (upper < min) upper = min;
        auto result = static_cast<int>(val);
        if (result < min) result = min;
        if (result > upper) result = upper;
        return result;
    }
}

std::unique_ptr<WinCap> winCap;

WinCap::WinCap() : WinHost()
{
	setTitle(L"Screen Capture");
    auto [x1, y1, w1, h1] = App::get()->getScreenArea();
	this->x = x1;this->y = y1;this->w = w1;this->h = h1;
    onMouseDown.add([this](POINT pos, bool isRight) { this->onDown(pos, isRight); });
    onMouseMove.add([this](POINT pos) { this->onMove(pos); });
    onMouseUp.add([this](POINT pos, bool isRight) { this->onUp(pos, isRight); });
    onKeyDown.add([this](UINT key) { this->onKey(key); });
    onMouseWheel.add([this](POINT pos, float space) { this->onWheel(pos, space); });
    onTimer.add([this](UINT id) { this->onTimerCB(id); });
    onDestroy.add([this]() { this->onClosed(); });
    // WinCap 是铺满桌面的顶层窗口，工具条叠在选区内。一旦 WinCap 被点击获焦，
    // 它会被顶到所有 TOPMOST 窗口之上、盖住工具条，所以获焦时把工具条重新置顶
    onFocus.add([this]() { this->raiseTools(); });
    // DPI 变了（用户改了缩放比例）：系统会按新旧缩放比把窗口整体缩放一圈，但本窗口是铺满整个
    // 虚拟桌面的，缩放之后就盖不住桌面了，而且底图、选区（cutMask->maskRect）用的都是物理像素，
    // 窗口一变形它们全部错位，挂在选区上的工具条自然也跟着偏。改缩放不会改分辨率，
    // 桌面还是那么多像素，所以等系统把建议矩形应用完（紧随而来的 WM_SIZE）再把窗口掰回桌面大小。
    // 位置不能在 onDpiChanged 里改 —— 那个事件在建议矩形生效之前触发，改了马上被覆盖
    onDpiChanged.add([this]() { dpiChanged = true; });
    onSizeChanged.add([this]() {
        if (!dpiChanged) return;
        dpiChanged = false;
        auto [x1, y1, w1, h1] = App::get()->getScreenArea();
        this->x = x1; this->y = y1; this->w = w1; this->h = h1;
        SetWindowPos(hwnd, nullptr, x1, y1, (int)w1, (int)h1, SWP_NOZORDER | SWP_NOACTIVATE);
    });
}

WinCap::~WinCap()
{
}

void WinCap::init()
{
    // 双击托盘图标会连着来两下，已经开着就不再建第二个
    if (winCap) return;
    auto ptr = new WinCap();
    winCap.reset(ptr);
	ptr->cutMask = std::make_unique<CutMask>(ptr);
    ptr->createNativeWindow(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, WS_POPUP);//WS_EX_TOPMOST
}

WinCap* WinCap::get()
{
    return winCap.get();
}

void WinCap::dispose()
{
    winCap.reset();
}

void WinCap::onCreated()
{
    App::get()->takeScreenShot(x, y, w, h, &screenImg);
	auto d2d = Ling::D2D::get();
    // 画布铺满窗口，走 swap chain（双缓冲）后端，避免调整选区时整帧闪烁
    canvas = body->makeChild<Ling::Canvas>();
    canvas->enableSwapChain();
    canvas->setSizePercent(100.f, 100.f);
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushText.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.56f), brushBg.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.5f, 1.f, 0.5f), crossBrush.GetAddressOf());
    POINT pos;
    GetCursorPos(&pos);
    ScreenToClient(hwnd, &pos);
    getPixImg(pos);
    setPixPos(pos);
    show();
    // show() 只显示不激活，而截图窗口要立刻响应键盘（Select 阶段按 Esc 退出、方向键移光标）。
    // 全局热键触发时本进程有权抢前台，主动拿到焦点，否则没点过窗口时 Esc 会失效
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
}

void WinCap::layout()
{
    Ling::WinBase::layout();
    if (!canvas) return;
    auto ctx = canvas->startPaint();
    if (!ctx) return;
    ctx->Clear(0);
    D2D1_RECT_F destRect = D2D1::RectF(0, 0, (float)w, (float)h);
    ctx->DrawBitmap(screenImg.Get(), destRect);
    // 标注：shape 一律用窗口坐标（本窗口铺满桌面、scale 恒为 1），直接画在选区之上
    if (history) {
        for (auto& shape : history->shapes)
        {
            if (!shape->isUndo) shape->paint(ctx);
        }
        if (!isMouseDown && shapeHover) shapeHover->paintDragger(ctx);
    }
    // 压暗层 + 选区边框 + 尺寸标签画在标注之上：橡皮擦恢复底图时只会擦掉标注，
    // 不会把框外的压暗边和选取边框一起擦掉
    cutMask->paint(ctx);
    paintPix(ctx);
    canvas->finishPaint();
}

BOOL WinCap::setCursor()
{
    if (stage == CapStage::Select) {
        SetCursor(LoadCursor(nullptr, IDC_CROSS));
        return TRUE;
    }
    if (stage == CapStage::Adjust) {
        POINT pos{};
        GetCursorPos(&pos);
        ScreenToClient(hwnd, &pos);
        // 编辑文本时光标形状交给 TextBox 决定
        if (editingText && textBox && textBox->isPosIn(pos)) {
            bool handled{ false };
            onCursor(&handled);
            if (handled) return TRUE;
        }
        switch (cutMask->hitTest(pos))
        {
        case MaskHit::TopLeft:
        case MaskHit::BottomRight:
            SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
            return TRUE;
        case MaskHit::TopRight:
        case MaskHit::BottomLeft:
            SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
            return TRUE;
        case MaskHit::Top:
        case MaskHit::Bottom:
            SetCursor(LoadCursor(nullptr, IDC_SIZENS));
            return TRUE;
        case MaskHit::Left:
        case MaskHit::Right:
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return TRUE;
        case MaskHit::Inside:
            // 选中了工具：落在已有标注上按标注的形状光标，否则按该工具的光标
            if (toolMain && !toolMain->curId.empty()) {
                if (shapeHover) {
                    shapeHover->setCursor();
                }
                else {
                    SetCursor(LoadCursor(nullptr, toolMain->curId == L"text" ? IDC_IBEAM : IDC_CROSS));
                }
                return TRUE;
            }
            // 没选工具：选区内部拖动 = 移动选区
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            return TRUE;
        default:
            break;
        }
    }
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    return TRUE;
}

void WinCap::setPixPos(POINT pos)
{
    auto span{ 10 * dpi };
    pixPos.x = int(pos.x + span + dpi);
    if (pixPos.x + pixW > w) {
        pixPos.x = int(pos.x - span - pixW + dpi);
    }
    pixPos.y = int(pos.y + span + dpi);
    auto pixH{ pixImgH + 76.f * dpi };
    if (pixPos.y + pixH > h) {
        pixPos.y = int(pos.y - span - pixH + dpi);
    }
}

void WinCap::getPixImg(POINT pos)
{
    if (isPress || stage != CapStage::Select) return;
    const long sw = static_cast<long>(srcW), sh = static_cast<long>(srcH);
    const long iw = static_cast<long>(w), ih = static_cast<long>(h);
    // 期望的源矩形：以光标为正中心，可以越出屏幕。
    const long wantL = pos.x - sw / 2, wantT = pos.y - sh / 2;
    long vl = std::max(wantL, 0L), vt = std::max(wantT, 0L);
    long vr = std::min(wantL + sw, iw), vb = std::min(wantT + sh, ih);
    // 交集在 pixImg 内的落点：期望矩形左上角为原点，所以减 wantL/wantT（越界时为正偏移）
    const long dl = vl - wantL, dt = vt - wantT;
    if (!pixImg) {
        auto d2d = Ling::D2D::get();
        D2D1_BITMAP_PROPERTIES1 prop{};
        prop.pixelFormat = screenImg->GetPixelFormat();
        prop.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
        screenImg->GetDpi(&prop.dpiX, &prop.dpiY);
        auto hr = d2d->deviceContext->CreateBitmap(D2D1::SizeU(sw, sh), nullptr, 0, &prop, &pixImg);
    }
    auto srcRect = D2D1::RectU(vl, vt, vr, vb);
    auto destPoint = D2D1::Point2U(static_cast<UINT32>(dl), static_cast<UINT32>(dt));
    auto hr = pixImg->CopyFromBitmap(&destPoint, screenImg.Get(), &srcRect);
    // pixImg 里这一帧真正有内容的区域。越界部分留着上一帧的残留，
    // 绘制时靠 DrawBitmap 的 srcRect 把它排除掉，露出取景框自己的底色。
    pixSrcRect = D2D1::RectF((float)dl, (float)dt, float(dl + (vr - vl)), float(dt + (vb - vt)));
}

void WinCap::paintPix(ID2D1DeviceContext* ctx)
{
	if (isPress || stage != CapStage::Select) return;
    D2D1_RECT_F pixRect{ (float)pixPos.x, (float)pixPos.y, pixPos.x + pixW, pixPos.y + pixImgH + 76.f * dpi };
    ctx->FillRectangle(pixRect, brushBg.Get());
    if (pixSrcRect.right > pixSrcRect.left && pixSrcRect.bottom > pixSrcRect.top) {
        D2D1_RECT_F imgRect{
            pixPos.x + pixSrcRect.left * scaleNum,
            pixPos.y + pixSrcRect.top * scaleNum,
            pixPos.x + pixSrcRect.right * scaleNum,
            pixPos.y + pixSrcRect.bottom * scaleNum
        };
        ctx->DrawBitmap(pixImg.Get(), imgRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &pixSrcRect);
    }
    ctx->DrawRectangle(pixRect, brushBg.Get(),dpi);

    float crossWHalf{ 4.f*dpi };
    auto crossRect0 = D2D1::RectF(pixPos.x, pixPos.y+pixImgH / 2 - crossWHalf, pixPos.x+pixW / 2 - crossWHalf, pixPos.y + pixImgH / 2 + crossWHalf);
    auto crossRect1 = D2D1::RectF(pixPos.x + pixW / 2 + crossWHalf, pixPos.y+pixImgH / 2 - crossWHalf, pixPos.x + pixW, pixPos.y + pixImgH / 2 + crossWHalf);
    auto crossRect2 = D2D1::RectF(pixPos.x + pixW / 2 - crossWHalf, pixPos.y, pixPos.x + pixW / 2 + crossWHalf, pixPos.y + pixImgH / 2 - crossWHalf);
    auto crossRect3 = D2D1::RectF(pixPos.x + pixW / 2 - crossWHalf, pixPos.y+pixImgH / 2 + crossWHalf, pixPos.x + pixW / 2 + crossWHalf, pixPos.y + pixImgH);

    ctx->FillRectangle(crossRect0, crossBrush.Get());
    ctx->FillRectangle(crossRect1, crossBrush.Get());
    ctx->FillRectangle(crossRect2, crossBrush.Get());
    ctx->FillRectangle(crossRect3, crossBrush.Get());

    POINT pos;
	GetCursorPos(&pos);
    HDC hScreen = GetDC(NULL);
    COLORREF cr = GetPixel(hScreen, pos.x, pos.y);
    ReleaseDC(NULL, hScreen);
    auto d2d = Ling::D2D::get();
    float padding{ 7.f * dpi },fontSize{ 10.f*dpi };
    Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
    for (size_t i = 0; i < 4; i++)
    {
        std::wstring str;
        if (i == 0) {
            str = std::format(L"HEX (Ctrl+H) : #{:02X}{:02X}{:02X}", GetRValue(cr), GetGValue(cr), GetBValue(cr));
		}
		else if (i == 1) {
			str = std::format(L"RGB (Ctrl+R) : {},{},{}", GetRValue(cr), GetGValue(cr), GetBValue(cr));
		}
        else if (i == 2) {
            auto [c, m, y1, k] = getCMYK(GetRValue(cr), GetGValue(cr), GetBValue(cr));
            str = std::format(L"CMYK (Ctrl+K) : {},{},{},{}", c, m, y1,k);
        }
        else if (i == 3) {
            str = std::format(L"POS (Ctrl+P) : X:{} Y:{}", pos.x, pos.y);
        }
        d2d->dwriteFactory->CreateTextLayout(str.data(), (UINT32)str.length(), d2d->baseTextFormat.Get(), FLT_MAX, FLT_MAX, &textLayout);
        textLayout->SetFontSize(fontSize, { 0,INT_MAX });
        ctx->DrawTextLayout({ pixPos.x + padding, pixPos.y + pixImgH + padding*(i+1) + fontSize*i }, textLayout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
}

void WinCap::onKey(UINT key)
{
    // 编辑文本时所有按键都归 TextBox：否则 Ctrl+C 复制的是截图、回车会保存并关窗
    if (editingText) return;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    // Adjust 阶段：标注快捷键优先。Enter = 确认（把带标注的选区复制进剪切板并关窗），
    // Ctrl+C 复制、Ctrl+S 存盘、Ctrl+Z/Y 撤销重做、Del 删 hover、Esc 关闭
    if (stage == CapStage::Adjust && toolMain) {
        if (ctrl && key == 'Z') { history->undo(); return; }
        else if (ctrl && key == 'Y') { history->redo(); return; }
        else if (ctrl && key == 'C') { copyToClipboard(); return; }
        else if (ctrl && key == 'S') { saveToFile(); return; }
        else if (key == VK_DELETE) { history->removeHoverShape(); return; }
        else if (key == VK_RETURN) { copyToClipboard(); return; }
        else if (key == VK_ESCAPE) { close(); return; }
    }
    // 不在 Select 阶段，标注快捷键之外只认关闭
    if (stage != CapStage::Select) {
        if (key == VK_ESCAPE) close();
        return;
    }
    // Select 阶段：取色 / 移动光标 / 退出
    auto func = []() {
        POINT pos;
        GetCursorPos(&pos);
        HDC hScreen = GetDC(NULL);
        COLORREF cr = GetPixel(hScreen, pos.x, pos.y);
        ReleaseDC(NULL, hScreen);
        return cr;
    };
    if (key == VK_ESCAPE) {
        close();
    }
    else if (key == 'H' && ctrl) {
		auto cr = func();
        BYTE r = GetRValue(cr), g = GetGValue(cr), b = GetBValue(cr);
        wchar_t hex[7];
        swprintf_s(hex, L"%02X%02X%02X", r, g, b);
        Ling::Util::setTextToClipboard(hex);
        close();
    }
    else if (key == 'R' && ctrl) {
        auto cr = func();
        BYTE r = GetRValue(cr), g = GetGValue(cr), b = GetBValue(cr);
        Ling::Util::setTextToClipboard(std::format(L"rgb({},{},{})", r, g, b));
        close();
    }
    else if (key == 'K' && ctrl) {
        auto cr = func();
        BYTE r = GetRValue(cr), g = GetGValue(cr), b = GetBValue(cr);
        auto [c, m, y1, k] = getCMYK(r, g, b);
        Ling::Util::setTextToClipboard(std::format(L"cmyk({},{},{},{})", c, m, y1, k));
        close();
    }
    else if (key == 'P' && ctrl) {
        POINT pos;
        GetCursorPos(&pos);
        Ling::Util::setTextToClipboard(std::format(L"{},{}", pos.x, pos.y));
        close();
    }
    else if (key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT) {
        POINT pos;
        GetCursorPos(&pos);
        if (key == VK_UP) pos.y -= 1;
        else if (key == VK_DOWN) pos.y += 1;
        else if (key == VK_LEFT) pos.x -= 1;
        else pos.x += 1;
        SetCursorPos(pos.x, pos.y); // 后面的 WM_MOUSEMOVE 会让 onMove 跟着刷新
    }
}

std::tuple<int, int, int, int> WinCap::getCMYK(const BYTE& r, const BYTE& g, const BYTE& b)
{
    double R = r / 255.0, G = g / 255.0, B = b / 255.0;
    double K = 1.0 - (std::max)(R, (std::max)(G, B));
    double C = (K == 1.0) ? 0.0 : (1.0 - R - K) / (1.0 - K);
    double M = (K == 1.0) ? 0.0 : (1.0 - G - K) / (1.0 - K);
    double Y = (K == 1.0) ? 0.0 : (1.0 - B - K) / (1.0 - K);
    return std::make_tuple(static_cast<int>(std::round(C * 100)),
        static_cast<int>(std::round(M * 100)),
        static_cast<int>(std::round(Y * 100)),
        static_cast<int>(std::round(K * 100))
    );
}
ComPtr<ID2D1Bitmap1> WinCap::getCutImg()
{
    ComPtr<ID2D1Bitmap1> cutImg;
    auto& maskRect = cutMask->maskRect;
    const UINT32 cw = (UINT32)(maskRect.right - maskRect.left);
    const UINT32 ch = (UINT32)(maskRect.bottom - maskRect.top);
    if (cw == 0 || ch == 0) return cutImg;
    D2D1_BITMAP_PROPERTIES1 prop{};
    prop.pixelFormat = screenImg->GetPixelFormat();
    prop.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
    screenImg->GetDpi(&prop.dpiX, &prop.dpiY);
    Ling::D2D::get()->deviceContext->CreateBitmap(D2D1::SizeU(cw, ch), nullptr, 0, &prop, cutImg.GetAddressOf());
    auto start = D2D1::Point2U(0, 0);
    auto rect = D2D1::RectU((UINT32)maskRect.left, (UINT32)maskRect.top, (UINT32)maskRect.right, (UINT32)maskRect.bottom);
    cutImg->CopyFromBitmap(&start, screenImg.Get(), &rect);
    return cutImg;
}

LRESULT WinCap::onHitTest(const POINT pos)
{
    return HTCLIENT;
}

void WinCap::onDown(POINT pos, bool isRight)
{
    if (isRight) {
        // Select 阶段点右键 = 等同快捷键重进选区：清空选区并恢复智能吸附，
        // 回到"按快捷键首次进入"的初始十字状态（悬停重新正常智能选取）
        if (stage == CapStage::Select) {
            isPress = false;
            cutMask->reset();
            cutMask->autoSnap = true;
            return;
        }
        if (stage == CapStage::Adjust) {
            // 编辑状态右键 = 选中编辑：点中某个已有标注就解锁它（越过"只编辑最后一个"），
            // 让它可被直接拖动 / 修改
            auto imgPos = toImgPos(pos);
            for (int i{ (int)history->shapes.size() - 1 }; i >= 0; i--) {
                auto cur = history->shapes[i].get();
                if (cur->isUndo) continue;
                cur->mouseMove((float)imgPos.x, (float)imgPos.y);
                if (cur->hoverDraggerIndex >= 0) {
                    editTarget = cur;
                    shapeHover = cur;
                    refresh();
                    return;
                }
            }
            // 右键点空白：只有还没画过任何标注时才允许回到重新选区；
            // 一旦画了圆 / 矩形等标注，选框就已结束，后续都是编辑器的活，禁止二次选框
            if (!history->shapes.empty()) return;
            // 空白处且没画过标注：收掉正在进行的标注（画到一半的形状 / 编辑中的文字 / 工具条），
            // 重置选区回到 Select 重新框选，并清掉右键解锁目标
            editTarget = nullptr;
            if (editingText && textBox) {
                textBox->hide();
                editingText = nullptr;
            }
            if (isMouseDown) {
                isMouseDown = false;
                ReleaseCapture();
                if (newShape) {
                    history->removeShape(newShape);  // 丢掉这次按下画到一半的新形状
                }
            }
            newShape = nullptr;
            shapeHover = nullptr;
            hasDragged = false;
            if (toolMain) toolMain->hide();
            if (toolSub) toolSub->hideTools();
            cutMask->reset();
            stage = CapStage::Select;
            refresh();
            return;
        }
        close();
        return;
    }
    // 编辑文本时，落在文本框里的点击整个交给 TextBox
    if (editingText && textBox && textBox->isPosIn(pos)) return;
    // 双击判定得自己做：Ling 的窗口类没带 CS_DBLCLKS，WM_LBUTTONDBLCLK 根本不会来
    auto now = GetTickCount64();
    bool isDblClick = (now - lastDownTime <= GetDoubleClickTime())
        && std::abs(pos.x - lastDownPos.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
        && std::abs(pos.y - lastDownPos.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
    lastDownTime = now;
    lastDownPos = pos;
    if (stage == CapStage::Adjust) {
        // 没选工具时双击 = 复制（等效 Ctrl+C）。选了工具就交给标注，两次按下都是画图
        if (isDblClick && (!toolMain || toolMain->curId.empty())) {
            copyToClipboard();
            return;
        }
        // 选区内部且选中了工具 → 在该位置开始画对应标注；
        // 否则（内部没选工具 = 拖动移动选区；按在边/角 = 改选区大小）→ 交给 CutMask
        auto hit = cutMask->hitTest(pos);
        if (hit == MaskHit::Inside && toolMain && !toolMain->curId.empty()) {
            isMouseDown = true;
            hasDragged = false;
            SetCapture(hwnd);
            pressPos = pos;
            auto imgPos = toImgPos(pos);
            if (shapeHover) {
                newShape = nullptr; //改的是已有元素，不参与空元素判定
                shapeHover->mouseDown((float)imgPos.x, (float)imgPos.y);
            }
            else {
                editTarget = nullptr;  // 新画一笔，回到"只可编辑最后一个"
                shapeHover = history->createShape(toolMain->curId, imgPos.x, imgPos.y);
                newShape = shapeHover;
            }
            return;
        }
        isPress = true;
        cutMask->startAdjust(pos);
        return;
    }
    if (stage == CapStage::Select) {
        // 手动拖动框选区：沿用当前吸附开关 —— 右键进入手动十字后保持不吸附，
        // 只有再次按快捷键首次进入时才恢复智能吸附
        isPress = true;
        cutMask->startMakeRect(pos);
    }
}

void WinCap::onMove(POINT pos)
{
    if (stage == CapStage::Select) {
        if (isPress) {
            cutMask->makeRect(pos);
        }
        else {
            cutMask->highlight(pos);
            getPixImg(pos);
            setPixPos(pos);
            refresh();
        }
    }
    else if (stage == CapStage::Adjust) {
        if (editingText && textBox && textBox->isPosIn(pos)) return;
        if (isMouseDown) {
            // 正在画 shape（选中了工具）：拖拽交给 shape，坐标直接用窗口坐标
            auto imgPos = toImgPos(pos);
            if (pos.x != pressPos.x || pos.y != pressPos.y) hasDragged = true;
            if (shapeHover) shapeHover->mouseDrag((float)imgPos.x, (float)imgPos.y);
            refresh();
        }
        else if (isPress) {
            // 正在调选区：四边/角改大小，没选工具时内部拖动 = 移动选区。
            // 选区一变，编辑器也要跟着走，所以重排工具条
            cutMask->adjust(pos);
            refresh();
            layoutTools();
        }
        else if (toolMain && !toolMain->curId.empty()) {
            // 没按下：选中了工具就做悬停判定。默认只可编辑最后一个（只有最新那个 shape 的
            // 夹点能悬停/编辑，之前的全部锁定；想改前面的必须先回退 Ctrl+Z 撤销到它）；
            // 若右键解锁了某个更早的标注（editTarget），则它也能被悬停 / 编辑
            auto imgPos = toImgPos(pos);
            ShapeBase* hover = nullptr;
            if (editTarget && !editTarget->isUndo) {
                editTarget->mouseMove((float)imgPos.x, (float)imgPos.y);
                if (editTarget->hoverDraggerIndex >= 0) hover = editTarget;
            }
            if (!hover) {
                ShapeBase* last = nullptr;
                for (int i{ (int)(history->shapes.size() - 1) }; i >= 0; i--) {
                    auto cur = history->shapes[i].get();
                    if (cur->isUndo) continue;
                    last = cur;
                    break;
                }
                if (last) {
                    last->mouseMove((float)imgPos.x, (float)imgPos.y);
                    if (last->hoverDraggerIndex >= 0) hover = last;
                }
            }
            if (hover) {
                if (shapeHover != hover) {
                    shapeHover = hover;
                    setTimer(800, 100);
                    refresh();
                }
                return;
            }
            if (shapeHover) shapeHover = nullptr;
        }
    }
}

void WinCap::onUp(POINT pos, bool isRight)
{
    if (stage == CapStage::Select) {
        isPress = false;
        // 只是点了一下，又没吸附到任何窗口，那就接着让用户框
        if (!cutMask->hasRect()) return;
        // 命令行指定了直奔某个阶段：它比下面 Ctrl 那条钉图的快捷路径更优先
        if (enterByArg()) return;
        // 按住 Ctrl 框选：跳过调整，直接钉到桌面上
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            startPin();
            return;
        }
        stage = CapStage::Adjust;
        ensureAnnotation();
        layoutTools();
        refresh();  // 收掉放大镜
        // 刚建的 ToolMain/ToolSub 是顶层窗口，可能抢走焦点；立刻把焦点夺回 WinCap，
        // 否则框选完马上按 Esc 会被吞掉（焦点落在别的窗口上）。移动一下鼠标时系统可能
        // 把焦点还回来，所以表现为"动一下就又能退出了"
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        // 进入 Adjust 后：选区四周（四边 + 四角）可拖改大小；选区内选中工具 = 画标注，
        // 没选工具 = 拖动移动选区；Enter / 双击 / 工具条按钮确认收尾
    }
    else if (stage == CapStage::Adjust) {
        isPress = false;
        if (isMouseDown) {
            isMouseDown = false;
            ReleaseCapture();
            auto justCreated = newShape;
            newShape = nullptr;
            prevPressCreatedShape = false;
            if (toolMain && !toolMain->curId.empty() && shapeHover) {
                // 新建的这一笔按下马上弹起，什么也没画出来：直接丢掉
                if (shapeHover == justCreated && !hasDragged && !shapeHover->isValidWithoutDrag()) {
                    history->removeShape(shapeHover);
                    return;
                }
                prevPressCreatedShape = (shapeHover == justCreated);
                auto imgPos = toImgPos(pos);
                shapeHover->mouseUp((float)imgPos.x, (float)imgPos.y);
                refresh();
                setTimer(800, 100);
            }
        }
    }
}

// close() 里 DestroyWindow 之后同步触发 onDestroy，而这条路径通常是从某个工具条的按钮
// 回调里一路进来的（工具条是 WinPin 的成员）。在这里直接 winCap.reset() 就是 use-after-free，
// 所以窗口句柄立即销毁，C++ 对象的释放推迟到下一轮消息循环。
void WinCap::onClosed()
{
    if (isClosed) return;
    isClosed = true;
    // 收起附属工具条（它们各自是独立顶层窗口，WinCap 关闭时一并销毁）
    if (toolSub) toolSub->close();
    if (toolMain) toolMain->close();
    Ling::App::get()->dq.TryEnqueue([]() {
        winCap.reset();
        // 用完即走模式下截图结束就退出进程，与 App 构造里的判断对称。
        // 但标注这条条路是先把图钉到桌面上再关自己的，那种情况下活还没干完，
        // 退出的活交给最后一个关掉的贴图窗口
        if (!WinPin::hasWindow()) {
            if (Ling::App::get()->args[L"--auto-quit"] == L"true") {
                Ling::App::get()->quit(0);
            }
            else {
                Update::checkLater(); //只剩托盘图标了，顺便查一下更新
            }
        }
    });
}


bool WinCap::enterByArg()
{
    auto& args = Ling::App::get()->args;
    auto it = args.find(L"--enter");
    if (it == args.end()) return false;
    auto& val = it->second;
    if (val == L"pin") {
        startPin();   //等于替用户按住了 Ctrl 框选
        return true;
    }
    return false;
}

void WinCap::layoutTool(Ling::WinBase* tool)
{
    if (!tool) return;
    const int toolW = (int)(tool->w + 0.5f);
    const int toolH = (int)(tool->h + 0.5f);
    // maskRect 是本窗口的客户区坐标，换算到屏幕坐标
    const int maskLeftScr = x + (int)cutMask->maskRect.left;
    const int maskTopScr = y + (int)cutMask->maskRect.top;
    const int maskRightScr = x + (int)cutMask->maskRect.right;
    const int maskBottomScr = y + (int)cutMask->maskRect.bottom;

    // 用框选区域所在显示器的工作区判断上/下方是否有足够空间
    RECT maskScrRect{ maskLeftScr, maskTopScr, maskRightScr, maskBottomScr };
    HMONITOR hMon = MonitorFromRect(&maskScrRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    GetMonitorInfo(hMon, &mi);

    // 间距按工具条自己所在显示器的缩放算：本窗口铺满整个虚拟桌面，dpi 是系统缩放，
    // 混合缩放的多屏下和选区所在的那块屏不一定是一回事
    const int gap = (int)(cutMask->strokeWidth + 2.f * tool->dpi + 0.5f); // 与框选边框的间距
    const bool fitBelow = (maskBottomScr + gap + toolH) <= mi.rcWork.bottom;
    const bool fitAbove = (maskTopScr - gap - toolH) >= mi.rcWork.top;

    // 工具条右侧与框选区域右侧对齐
    int toolX = maskRightScr - toolW;
    int toolY = 0;
    if (fitBelow) {
        // 右下方
        toolY = maskBottomScr + gap;
    }
    else if (fitAbove) {
        // 右上方
        toolY = maskTopScr - gap - toolH;
    }
    else {
        // 叠加在框选区域右下方内部，与右/底各留 3*dpi
        const int overlapPad = (int)(3.f * tool->dpi + 0.5f);
        toolX = maskRightScr - toolW - overlapPad;
        toolY = maskBottomScr - toolH - overlapPad;
    }

    // 兜底：不越出所在显示器工作区
    if (toolX < mi.rcWork.left) toolX = mi.rcWork.left;
    if (toolX + toolW > mi.rcWork.right) toolX = mi.rcWork.right - toolW;
    tool->setPosition(toolX, toolY);
}


void WinCap::startPin()
{
    if (!cutMask->hasRect()) return;
    auto& maskRect = cutMask->maskRect;
    // WinPin 构造里会回头来取 getCutImg()，所以得先把它建起来再关自己
    WinPin::init(int(maskRect.left) + x, int(maskRect.top) + y,
        int(maskRect.right - maskRect.left), int(maskRect.bottom - maskRect.top));
    close();
}

void WinCap::saveToFile()
{
    std::vector<BYTE> pixels;
    D2D1_SIZE_U size{};
    if (!getImagePixels(pixels, size)) return;
    auto path = Util::getSaveFilePath(hwnd);
    if (path.empty()) return; //用户取消了
    if (Util::saveToFile(path, (int)size.width, (int)size.height, pixels.data())) {
        close();
    }
}

void WinCap::copyToClipboard()
{
    std::vector<BYTE> pixels;
    D2D1_SIZE_U size{};
    if (!getImagePixels(pixels, size)) return;
    Util::saveToClipboard((int)size.width, (int)size.height, pixels.data());
    close();
}

// 首次进入 Adjust 阶段时建出标注用的工具条与历史（懒建，避免截图窗口一开始就带着工具条）
void WinCap::ensureAnnotation()
{
    if (toolMain) return;
    history = std::make_unique<History>(this);
    toolMain = std::make_unique<ToolMain>(this);
    toolSub = std::make_unique<ToolSub>(this);
}

// 把 ToolMain / ToolSub 摆到选区周围。ToolMain 用 WinCap::layoutTool 对着选区右下/上对齐，
// ToolSub 永远紧贴 ToolMain 下方（见 ToolSub::updatePosition）。
void WinCap::layoutTools()
{
    if (!toolMain || !toolSub) return;
    if (!cutMask->hasRect()) return;
    layoutTool(toolMain.get());
    RECT maskScr{ (int)(x + cutMask->maskRect.left), (int)(y + cutMask->maskRect.top),
                  (int)(x + cutMask->maskRect.right), (int)(y + cutMask->maskRect.bottom) };
    HMONITOR hMon = MonitorFromRect(&maskScr, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ .cbSize = sizeof(MONITORINFO) };
    RECT wa{};
    if (hMon && GetMonitorInfo(hMon, &mi)) wa = mi.rcWork;
    else {
        auto [sx, sy, sw, sh] = App::get()->getScreenArea();
        wa = RECT{ sx, sy, sx + sw, sy + sh };
    }
    // 主编辑器（ToolMain）始终可见；副编辑器（ToolSub）只在选中了对应工具时才出现，
    // 没选中就收起。选区移动 / 缩放时这里被重新调用来让整组编辑器跟着选区走
    if (!toolMain->curId.empty() && toolSub->hasContent()) {
        toolSub->updatePosition(wa);
    }
    else {
        toolSub->hideTools();
    }
}

void WinCap::onToolStyleChanged()
{
    if (editingText) editingText->applyStyle();
    // 马赛克高斯模糊：滑动条一动，最后编辑的那个模糊形状立刻按新强度重建显现
    if (history && !history->shapes.empty()) {
        for (int i = (int)history->shapes.size() - 1; i >= 0; --i) {
            auto s = history->shapes[i].get();
            if (!s || s->isUndo) continue;
            if (auto* m = dynamic_cast<ShapeMosaic*>(s)) m->rebuildLive();
            return;
        }
    }
}

Ling::TextBox* WinCap::getTextBox()
{
    if (textBox) return textBox;
    // 建在 canvas 之后：子 visual 按插入顺序叠放，文本框要盖在截图和标注上面。
    // 绝对定位，位置由 ShapeText 按自己的矩形指定。
    textBox = body->makeChild<Ling::TextBox>();
    textBox->setPositionType(Ling::Position::Absolute);
    textBox->setAutoSize(true);
    textBox->hide();
    return textBox;
}

void WinCap::setEditingText(ShapeText* shape)
{
    editingText = shape;
}

// 本窗口铺满桌面、scale 恒为 1，shape 直接用窗口坐标（也就是底图/屏幕坐标）
POINT WinCap::toImgPos(const POINT& pos) const
{
    return pos;
}

void WinCap::onWheel(POINT pos, float space)
{
    if (stage != CapStage::Adjust || !toolMain) return;
    if (toolMain->curId.empty() || !shapeHover) return;
    auto imgPos = toImgPos(pos);
    shapeHover->mouseWheel((float)imgPos.x, (float)imgPos.y, space > 0 ? (short)WHEEL_DELTA : (short)-WHEEL_DELTA);
}

void WinCap::onTimerCB(UINT id)
{
    if (id != 100) return;
    if (!shapeHover) {
        refresh();
        killTimer(100);
    }
}

// WinCap 获焦时被顶到最前、盖住工具条；这里把它们再拎回 TOPMOST 最上面。
// 用 SWP_NOACTIVATE：只调层叠不抢焦点，键盘焦点仍留在 WinCap 上
void WinCap::raiseTools()
{
    if (toolMain && toolMain->hwnd && IsWindowVisible(toolMain->hwnd))
        SetWindowPos(toolMain->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (toolSub && toolSub->hwnd && IsWindowVisible(toolSub->hwnd))
        SetWindowPos(toolSub->hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// 把带标注的选区钉到桌面上：合成像素交给一个贴图窗口展示，自己关窗
void WinCap::pinToScreen()
{
    std::vector<BYTE> pixels;
    D2D1_SIZE_U size{};
    if (!getImagePixels(pixels, size)) return;
    auto& m = cutMask->maskRect;
    WinPin::initFromData((int)(x + m.left), (int)(y + m.top), (int)size.width, (int)size.height, pixels);
    close();
}

// 离屏把"选区底图 + 未撤销 shape"合成到一张新位图上再读回像素。
// shape 存的是窗口坐标，导出时整体平移到选区左上角。
// 不直接画到 screenImg 上：它是 ShapeEraser 的"原样"来源、ShapeMosaic 的取样来源。
bool WinCap::getImagePixels(std::vector<BYTE>& pixels, D2D1_SIZE_U& size)
{
    if (!screenImg || !cutMask->hasRect() || !history) return false;
    auto& m = cutMask->maskRect;
    const UINT32 cutW = (UINT32)(m.right - m.left);
    const UINT32 cutH = (UINT32)(m.bottom - m.top);
    if (cutW == 0 || cutH == 0) return false;
    // 编辑中的文字是 TextBox 自己那层画的，进不了离屏 target，先收尾交回 ShapeText 自己画
    if (editingText) editingText->finishEdit();
    size = D2D1::SizeU(cutW, cutH);
    auto d2d = Ling::D2D::get();
    auto ctx = d2d->deviceContext.Get();

    D2D1_BITMAP_PROPERTIES1 targetProps{
        .pixelFormat{ D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED) },
        .dpiX{ 96.0f }, .dpiY{ 96.0f },
        .bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET }
    };
    ComPtr<ID2D1Bitmap1> targetBmp;
    auto hr = ctx->CreateBitmap(size, nullptr, 0, &targetProps, targetBmp.GetAddressOf());
    if (FAILED(hr)) return false;

    ctx->SetTarget(targetBmp.Get());
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx->BeginDraw();
    ctx->Clear(D2D1::ColorF(0, 0.0f));
    // 底图：只取选区那块（srcRect），画满整张导出图
    auto srcRect = D2D1::RectF((float)m.left, (float)m.top, (float)m.right, (float)m.bottom);
    ctx->DrawBitmap(screenImg.Get(), D2D1::RectF(0, 0, (float)cutW, (float)cutH), 1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &srcRect);
    // shape 用窗口坐标，平移到选区左上角后画进导出图
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(-m.left, -m.top));
    for (auto& shape : history->shapes)
    {
        if (!shape->isUndo) shape->paint(ctx);
    }
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    hr = ctx->EndDraw();
    ctx->SetTarget(nullptr);
    if (FAILED(hr)) return false;

    // GPU 上的 target 位图不能直接 Map，先拷到一块带 CPU_READ 的位图上
    D2D1_BITMAP_PROPERTIES1 cpuProps{
        .pixelFormat{ targetBmp->GetPixelFormat() },
        .dpiX{ 96.0f }, .dpiY{ 96.0f },
        .bitmapOptions{ D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
    };
    ComPtr<ID2D1Bitmap1> cpuBmp;
    hr = ctx->CreateBitmap(size, nullptr, 0, &cpuProps, cpuBmp.GetAddressOf());
    if (FAILED(hr)) return false;
    hr = cpuBmp->CopyFromBitmap(nullptr, targetBmp.Get(), nullptr);
    if (FAILED(hr)) return false;
    D2D1_MAPPED_RECT mapped{};
    hr = cpuBmp->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (FAILED(hr)) return false;
    // mapped.pitch 按 GPU 行对齐，可能大于 cutW*4；剪切板和 WIC 都要求紧凑步长，逐行紧缩
    const UINT32 rowBytes = size.width * 4;
    pixels.resize((size_t)rowBytes * size.height);
    for (UINT32 row = 0; row < size.height; ++row)
    {
        auto dst = pixels.data() + (size_t)row * rowBytes;
        CopyMemory(dst, mapped.bits + (size_t)row * mapped.pitch, rowBytes);
        // 底图是 GDI 抓来的，alpha 全 0，PNG 和剪切板会当真，统一按不透明补上
        for (UINT32 i = 3; i < rowBytes; i += 4) dst[i] = 255;
    }
    cpuBmp->Unmap();
    return true;
}
