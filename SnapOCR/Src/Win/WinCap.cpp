#include "pch.h"
#include <dwmapi.h>
#include <thread>
#include <Ui/Ui.h>
#include "WinCap.h"
#include "WinPin.h"
#include "WinHost.h"
#include "CutMask.h"
#include "CapVideo.h"
#include "../Tool/ToolMain.h"
#include "../Tool/ToolSub.h"
#include "../History.h"
#include "../Shape/ShapeBase.h"
#include "../Shape/ShapeText.h"
#include "../Shape/ShapeMosaic.h"
#include "../App.h"
#include "../Lang.h"
#include "../Setting.h"
#include "WinQr.h"
#include "../Util.h"
using namespace Microsoft::WRL;

namespace
{
    // 放大镜预览框：采样 50×30 屏幕像素，按 3.75 倍放大 → 图像区 187.5×112.5。
    // 原来倍率是 5（图像区 250×150），按用户要求整体缩小 1/4；采样范围不变，
    // 只是显示得小一点，取色时看到的仍是光标周围 50×30 的那一块
    constexpr float scaleNum{ 3.75f }, srcW{ 50.f }, srcH{ 30.f };
    constexpr float pixImgH{ scaleNum * srcH };
    constexpr float pixW{ srcW * scaleNum };
    // 图像区下方那条取色文字带（HEX / RGB / CMYK / POS 四行）的高度。
    // 字号 / 留白回到 10 / 5，文字带按这个内容算：
    //   最后一行顶边 = 留白*4 + 字号*3 = 20 + 30 = 50，行高 ≈ 字号 = 10，
    //   再留一个底部留白 5 → 65；取 66 兜住四舍五入，不切字也不多留空
    constexpr float pixTextH{ 66.f };
}

std::unique_ptr<WinCap> winCap;

// 窗口代次：每开一个 WinCap 就 +1。关闭时把延迟析构绑到自己的代次上，
// 下一轮消息循环里只有"当前窗口仍是这一代"才真正 reset —— 否则会把刚开的新窗口收掉
static unsigned g_capGen{ 0 };



// 纯截图工具：始终允许智能吸附（窗口/控件自动识别）。
// 唯一例外是扫码模式（grabMode）：那条路要的是"我框哪儿就是哪儿"，
// 窗口/控件/像素边缘的自动吸附会把框拉偏，反而认不出码
bool WinCap::allowSnap() const
{
    return !grabMode;
}


WinCap::WinCap() : WinHost()
{
	// 标题走品牌名：截图窗在任务栏/Alt+Tab 里露一下，写的得是当前产品名
	setTitle(SNAP_BRAND_W);
	auto [x1, y1, w1, h1] = App::get()->getScreenArea();
	// 屏幕宽高是物理像素 int，WinBase 里存 float：显式转一次（远小于 float 的精确整数上限，无损）
	this->x = x1;this->y = y1;this->w = static_cast<float>(w1);this->h = static_cast<float>(h1);
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
        this->x = x1; this->y = y1; this->w = static_cast<float>(w1); this->h = static_cast<float>(h1);
        SetWindowPos(hwnd, nullptr, x1, y1, (int)w1, (int)h1, SWP_NOZORDER | SWP_NOACTIVATE);
    });
}

WinCap::~WinCap()
{
}

bool WinCap::init()
{
    // 双击托盘图标会连着来两下，已经开着就不再建第二个。
    // 但如果拿到的是个"壳"（窗口已经没了、对象还没析构）就不能直接返回 false ——
    // 那样这个死壳会一直挂在 winCap 上，之后每次调用都返回失败，热键彻底失灵。
    // 就地回收，接着往下建新的
    if (winCap) {
        if (!winCap->isDead()) return true;
        winCap.reset();
    }
    try {
        auto ptr = new WinCap();
        winCap.reset(ptr);
        ptr->cutMask = std::make_unique<CutMask>(ptr);
        // 智能吸附的初始状态：纯截图工具始终开着（返回 true），这里保留调用是为了
        // 以后真要做"手动框选"的入口时只需改 allowSnap() 一处
        ptr->cutMask->autoSnap = ptr->allowSnap();
        // 抓像素是一次性的：窗口对象会被下一次截图复用，这里不清就会让上一次
        // 二维码识别留下的钩子把普通截图也截走
        ptr->grabMode = false;
        ptr->grabHandler = nullptr;
        // 建窗失败（CreateWindowEx 返回空，或者 onCreated 里 D2D/组合器抛异常被 catch 住）：
        // 立刻把对象收掉。留着它就是一颗定时炸弹 —— get() 一直非空、界面却不存在，
        // 用户看到的就是"按了热键没反应"，而且之后每次都会被判成 BUSY，再也截不了图
        const bool created = ptr->createNativeWindow(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, WS_POPUP);
        if (!created || !ptr->alive()) {
            winCap.reset();
            return false;
        }
        ptr->gen = ++g_capGen;
        return true;
    }
    catch (...) {
        // WinRT/组合器/D2D 的异常一律在这里拦住：这是 C++ 库的边界，
        // 让它穿出去就是 std::terminate —— 进程直接消失，比截图失败严重得多
        winCap.reset();
        return false;
    }
}

// 此刻是不是真有一个能用的截图窗口（防多开那条路用它决定要不要把主界面推出来）
bool WinCap::hasInstance()
{
	return winCap && !winCap->isDead();
}

bool WinCap::initForGrab(GrabHandler handler)
{
	// 已经有一个截图窗口开着就不能抢：那个窗口可能正在被用户调选区
	if (winCap && !winCap->isDead()) return false;
	if (!handler) return false;
	if (!init()) return false;
	winCap->grabMode = true;
	winCap->grabHandler = std::move(handler);
	// 扫码不做智能选框：autoSnap 在 init() 里已经按 allowSnap() 打开过，
	// 这里必须再关一次（grabMode 是 init 之后才置上的），否则鼠标一动就自动框到窗口上
	winCap->cutMask->autoSnap = false;
	return true;
}

// 壳还在、但其实已经不能用了：关过窗 / 句柄没了 / 窗口被系统销毁
bool WinCap::isDead() const
{
    return isClosed || hwnd == nullptr || !IsWindow(hwnd);
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
    ::prof::reset();   // 一次截图会话一份统计，别和上一次混在一起
    // 选区阶段屏蔽输入法：中文输入法开着时 'C' 会被当成拼音键吞掉，
    // WM_KEYDOWN 根本不来，"按 C 复制信息"就按不出来。摘掉上下文后
    // 所有按键都按普通字符上报；切到标注阶段再还原（见 setStage 处）
    savedImc = ImmAssociateContext(hwnd, nullptr);
    // w/h 是 float 成员但存的一直是整像素值：显式转 int，与原来的隐式截断结果一致
    App::get()->takeScreenShot(x, y, static_cast<int>(w), static_cast<int>(h), &screenImg);
	auto d2d = ui::D2D::get();
    // 图形设备没起来（硬件被驱动黑名单挡住、远程桌面会话、D2D 建失败）时 deviceContext 是空的。
    // 以前这里直接拿它建画刷 → 空指针崩在 UI 线程上，用户看到"一按热键程序就没了"。
    // 这里不能只 return：窗口已经 CreateWindowEx 成功，只 return 会让 init() 误判成功，
    // 而主界面早在 hideForCapture 里被藏起来 —— 表现就是"屏幕闪一下，程序像卡死"。
    // 必须关窗，让 createNativeWindow 之后的 !alive() 判成失败，从而触发 restoreAfterCapture 还原主界面
    if (!d2d || !d2d->deviceContext) {
        close(); return;
    }
    // 画布铺满窗口，走 swap chain（双缓冲）后端，避免调整选区时整帧闪烁
    canvas = body->makeChild<ui::Canvas>();
    canvas->enableSwapChain();
    canvas->setSizePercent(100.f, 100.f);
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushText.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.56f), brushBg.GetAddressOf());
    d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.5f, 1.f, 0.5f), crossBrush.GetAddressOf());
    POINT pos;
    GetCursorPos(&pos);
    ScreenToClient(hwnd, &pos);
    // 智能选区的像素边缘检测数据源就是这份整屏 CPU 副本本身：takeScreenShot 已经
    // 把它填好了（BGRA、stride = w*4、自上而下），直接 move 给 CutMask。
    // 以前是"再建一张 GPU 的 CPU_READ 副本 + Map 出来 memcpy 一份"，同一份桌面像素
    // 在一次会话里存三份（1080p 各 8MB / 4K 各 33MB），首帧还多一次整屏拷贝
    // 必须有 screenImg 才接：GetDIBits 失败时 takeScreenShot 提前返回，shotBuf 里
    // 只剩上一次会话的旧像素（尺寸还对得上，但内容整屏错位）—— 交给 CutMask 之后，
    // 智能选区会按一张早就过期的画面去量边界，吸到屏幕上已经不存在的框
    if (cutMask && screenImg && !App::get()->shotBuffer().empty()) {
        cutMask->adoptPixels(std::move(App::get()->shotBuffer()), (int)w, (int)h, (int)w * 4);
    }
    getPixImg(pos);
    setPixPos(pos);
    show();
    // show() 只显示不激活，而截图窗口要立刻响应键盘（Select 阶段按 Esc 退出、方向键移光标）。
    // 需要键盘焦点；用 SetForegroundWindow 抢前台会让主界面失活（WM_NCACTIVATE），
    // 标题栏颜色来回切就是"点截图主界面跳一下"的根因 —— 已改为在 WinQr 侧屏蔽失活视觉
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    // ESC 兜底轮询（104）：只认键状态，不看焦点、也不怕输入法把 WM_KEYDOWN 吃掉。
    // 文本编辑时按 ESC 会被输入法当成"取消组字"吞掉，按键根本到不了这里 ——
    // 用户看到的就是"文字编辑中要按两次 ESC"。开窗时先读一次，把进来之前
    // 残留的"按下过"标记清掉，免得刚开窗就被上一次的 ESC 关掉
    GetAsyncKeyState(VK_ESCAPE);
    escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    setTimer(25, kEscPollTimer);
}

// ESC 的唯一收尾路径：一次按键就结束整个截图。
// 录制中先丢弃这段录制（ToolSub 会把计时器和录制条一起收掉再关窗），
// 其余状态（选区 / 标注 / 录制设置 / 文本编辑）直接关窗。
// 以前录制中只退回标注态、文本编辑中只退出编辑，用户都得再按一次 ESC
void WinCap::quitByEsc()
{
    if (escClosing || isClosed) return;
    escClosing = true;
    killTimer(kEscPollTimer);
    if (stage == CapStage::Video && toolSub) {
        toolSub->videoAbort();
        return;
    }
    if (editingText) editingText->finishEdit();
    close();
}

void WinCap::layout()
{
    PROF_SCOPE("wm.paint.frame");
    ui::WinBase::layout();
    if (!canvas) return;
    auto ctx = canvas->startPaint();
    if (!ctx) return;
    // 录制中：画面就是真桌面 —— 本窗口这一整块是透明的（交换链是预乘 alpha，Clear(0) 即全透明），
    // 只留选区外那圈压暗。底图 / 标注 / 手柄 / 放大镜一律不画：它们都是"截图的装饰"，
    // 录进视频里就是脏画面。压暗层也不画边框与尺寸标签（withFrame=false），
    // 用户口径："录的时候选区边框藏起来，画面干净"
    if (stage == CapStage::Video) {
        PROF_SCOPE("wm.paint.video");
        ctx->Clear(0);
        if (cutMask) cutMask->paint(ctx, true, false);
        canvas->finishPaint();
        return;
    }
    // 抓屏失败（显示器正在切换分辨率、会话锁定之类）时 screenImg 是空的，
    // 后面所有绘制都依赖它，只清屏
    if (!screenImg) {
        ctx->Clear(0);
        canvas->finishPaint();
        return;
    }
    // 每帧直绘：底图一次全屏 blit + 压暗层四个 FillRectangle。
    // 第 11 轮曾把「底图 + 压暗层」烘进离屏缓存（以 maskRect 为键），第 18 轮移除，
    // 理由是实测数据：压暗层只是 4 次矩形填充，GPU 上微不足道；而 requestPaint
    // 只由鼠标移动等事件触发（鼠标静止时根本不排帧），选区每变一次缓存就整体
    // 作废重烘 —— 移动时每帧反而是"离屏 blit + 4 fills + 回画布 blit"的翻倍
    // GPU 工作量，命令队列积压时就是 20~30ms 的帧尖峰（复测 max 27.8ms）。
    // 直绘每帧一次 clear + 一次 blit + 4 fills：唯一真正在画帧的场景（移动时）
    // 工作量减半，还少了 50 行缓存状态与设备丢失恢复逻辑
    D2D1_RECT_F destRect = D2D1::RectF(0, 0, (float)w, (float)h);
    ctx->Clear(0);
    ctx->DrawBitmap(screenImg.Get(), destRect);
    // 标注：shape 一律用窗口坐标（本窗口铺满桌面、scale 恒为 1），直接画在选区之上
    if (history) {
        for (auto& shape : history->shapes)
        {
            if (!shape->isUndo) shape->paint(ctx);
        }
        // 手柄画在 shapeShow 身上：它是"鼠标移上去就亮框"的那个，可能并不是能拖的那个
        // （能拖的仍然只有最后一个 / 右键解开的），所以两个指针分开用。
        // 但最后必须过 isEditableShape 这一关：悬停指针可能在工具条收走焦点的空档里
        // 过期（工具条是独立窗口，光标在它上面时本窗收不到 WM_MOUSEMOVE，不刷新悬停），
        // 不在这里兜住，更早的标注就会把手柄一直亮着 —— 用户的口径是"只认最后一条"，
        // 对所有形状一视同仁（橡皮擦连画三下，第一下还带八个方块就是这么来的）
        if (!isMouseDown) {
            auto show = shapeShow ? shapeShow : shapeHover;
            if (show && isEditableShape(show)) show->paintDragger(ctx);
        }
    }
    // 压暗层 + 选区边框 + 尺寸标签画在标注之上：橡皮擦恢复底图时只会擦掉标注，
    // 不会把框外的压暗边和选取边框一起擦掉
    if (cutMask) { PROF_SCOPE("wm.paint.mask"); cutMask->paint(ctx, true, true); }
    { PROF_SCOPE("wm.paint.pix"); paintPix(ctx); }
    canvas->finishPaint();
}

BOOL WinCap::setCursor()
{
    // 录制中整窗让出鼠标（见 onHitTest），光标也一律普通箭头：桌面上的那个应用才是主体
    if (stage == CapStage::Video) {
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;
    }
    if (stage == CapStage::Select) {
        SetCursor(LoadCursor(nullptr, IDC_CROSS));
        return TRUE;
    }
    if (stage == CapStage::Adjust) {
        POINT pos{};
        GetCursorPos(&pos);
        ScreenToClient(hwnd, &pos);
        // 编辑文本时光标形状交给 TextBox 决定，但四角缩放柄压在文本框的四个角上：
        // 那几处要让标注先说话（斜双箭头），否则光标是"移进去能拉"、手柄却亮不出来
        if (editingText && textBox && textBox->isPosIn(pos)) {
            if (editingText->hoverDraggerIndex >= ShapeText::kHitCornerTL
                && editingText->hoverDraggerIndex <= ShapeText::kHitCornerBL) {
                editingText->setCursor();
                return TRUE;
            }
            bool handled{ false };
            onCursor(&handled);
            if (handled) return TRUE;
        }
        const bool toolActive = toolMain && !toolMain->curId.empty();
        // 选中工具时只有边/角那一圈归拖拽（见 onDown），光标必须跟这个口径一致：
        // 否则框外远处显示斜拉箭头、点下去却在画标注
        const auto hit = toolActive ? cutMask->hitEdge(pos, edgeGrabBand()) : cutMask->hitTest(pos);
        switch (hit)
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
            if (toolActive) {
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
            // 选中工具、又离边框很远：这一点是画标注用的
            if (toolActive) {
                SetCursor(LoadCursor(nullptr, toolMain->curId == L"text" ? IDC_IBEAM : IDC_CROSS));
                return TRUE;
            }
            break;
        }
    }
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    return TRUE;
}


// 合帧：截图窗的重绘就是整屏重绘，而鼠标可以按 125~1000Hz 送 WM_MOUSEMOVE。
// 每个消息都同步重绘一次的话，重绘（10ms 级）比消息间隔还长，输入队列必然积压，
// 而 WM_PAINT 是队列里优先级最低的，于是"手已停、框还在追"。
// 这里按 kFrameMs 封顶：到点了立刻画（RDW_UPDATENOW 顺便保证它不被输入消息饿死），
// 没到点就排一个一次性定时器，把这一帧合并到最近的那次重绘上
void WinCap::requestPaint()
{
	PROF_SCOPE("wm.paint.request");
    if (!alive()) return;
    const ULONGLONG now = GetTickCount64();
    if (now - lastFrameTick >= kFrameMs) {
        lastFrameTick = now;
        // 上一帧排下的补帧定时器要一起撤掉：这一帧已经当场画了，留着它到点还会把
        // 一模一样的画面再整屏重绘一遍（多一次 clear + 整屏 blit）
        if (framePending) {
            framePending = false;
            killTimer(102);
        }
        refresh();   // refresh 会置脏并 InvalidateRect
        // WM_PAINT 排在输入消息后面：手一直在动时它可能被饿很久。
        // RDW_UPDATENOW 把这一帧立刻画掉，重绘节奏就由上面的时间闸门说了算
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        return;
    }
    if (!framePending) {
        framePending = true;
        setTimer((UINT)kFrameMs, 102);
    }
}

void WinCap::setPixPos(POINT pos)
{
    auto span{ 10 * dpi };
    pixPos.x = int(pos.x + span + dpi);
    if (pixPos.x + pixW > w) {
        pixPos.x = int(pos.x - span - pixW + dpi);
    }
    pixPos.y = int(pos.y + span + dpi);
    auto pixH{ pixImgH + pixTextH * dpi };
    if (pixPos.y + pixH > h) {
        pixPos.y = int(pos.y - span - pixH + dpi);
    }
}

void WinCap::getPixImg(POINT pos)
{
	PROF_SCOPE("wm.getpix");
    if (isPress || stage != CapStage::Select) return;
    if (!screenImg) return;   // 抓屏失败，放大镜没有源可取样
    const long sw = static_cast<long>(srcW), sh = static_cast<long>(srcH);
    const long iw = static_cast<long>(w), ih = static_cast<long>(h);
    // 期望的源矩形：以光标为正中心，可以越出屏幕。
    const long wantL = pos.x - sw / 2, wantT = pos.y - sh / 2;
    long vl = std::max(wantL, 0L), vt = std::max(wantT, 0L);
    long vr = std::min(wantL + sw, iw), vb = std::min(wantT + sh, ih);
    // 交集在 pixImg 内的落点：期望矩形左上角为原点，所以减 wantL/wantT（越界时为正偏移）
    const long dl = vl - wantL, dt = vt - wantT;
    if (!pixImg) {
        auto d2d = ui::D2D::get();
        // D2D 设备丢失 / 显存不足时 deviceContext 可能为空，直接放弃这一帧，
        // 否则下面 CreateBitmap 的空结果会一路带着 nullptr 走到 CopyFromBitmap 崩掉
        if (!d2d || !d2d->deviceContext) return;
        D2D1_BITMAP_PROPERTIES1 prop{};
        prop.pixelFormat = screenImg->GetPixelFormat();
        prop.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
        screenImg->GetDpi(&prop.dpiX, &prop.dpiY);
        auto hr = d2d->deviceContext->CreateBitmap(D2D1::SizeU(sw, sh), nullptr, 0, &prop, &pixImg);
        if (FAILED(hr) || !pixImg) return;
    }
    auto srcRect = D2D1::RectU(vl, vt, vr, vb);
    auto destPoint = D2D1::Point2U(static_cast<UINT32>(dl), static_cast<UINT32>(dt));
    auto hr = pixImg->CopyFromBitmap(&destPoint, screenImg.Get(), &srcRect);
    // 拷贝失败（源矩形退化、设备丢失）：把有效区清空，这一帧只画取景框底色。
    // 照旧往下设 pixSrcRect 的话，画出来的是 pixImg 里上一帧的残留像素，看着像"放大镜错帧"
    if (FAILED(hr) || vr <= vl || vb <= vt) {
        pixSrcRect = D2D1::RectF(0, 0, 0, 0);
        return;
    }
    // pixImg 里这一帧真正有内容的区域。越界部分留着上一帧的残留，
    // 绘制时靠 DrawBitmap 的 srcRect 把它排除掉，露出取景框自己的底色。
    pixSrcRect = D2D1::RectF((float)dl, (float)dt, float(dl + (vr - vl)), float(dt + (vb - vt)));
}

void WinCap::paintPix(ID2D1DeviceContext* ctx)
{
	if (isPress || stage != CapStage::Select) return;
    D2D1_RECT_F pixRect{ (float)pixPos.x, (float)pixPos.y, pixPos.x + pixW, pixPos.y + pixImgH + pixTextH * dpi };
    ctx->FillRectangle(pixRect, brushBg.Get());
    if (pixImg && pixSrcRect.right > pixSrcRect.left && pixSrcRect.bottom > pixSrcRect.top) {
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
    auto crossRect0 = D2D1::RectF(static_cast<float>(pixPos.x), pixPos.y+pixImgH / 2 - crossWHalf, pixPos.x+pixW / 2 - crossWHalf, pixPos.y + pixImgH / 2 + crossWHalf);
    auto crossRect1 = D2D1::RectF(pixPos.x + pixW / 2 + crossWHalf, pixPos.y+pixImgH / 2 - crossWHalf, pixPos.x + pixW, pixPos.y + pixImgH / 2 + crossWHalf);
    auto crossRect2 = D2D1::RectF(pixPos.x + pixW / 2 - crossWHalf, static_cast<float>(pixPos.y), pixPos.x + pixW / 2 + crossWHalf, pixPos.y + pixImgH / 2 - crossWHalf);
    auto crossRect3 = D2D1::RectF(pixPos.x + pixW / 2 - crossWHalf, pixPos.y+pixImgH / 2 + crossWHalf, pixPos.x + pixW / 2 + crossWHalf, pixPos.y + pixImgH);

    ctx->FillRectangle(crossRect0, crossBrush.Get());
    ctx->FillRectangle(crossRect1, crossBrush.Get());
    ctx->FillRectangle(crossRect2, crossBrush.Get());
    ctx->FillRectangle(crossRect3, crossBrush.Get());

    auto d2d = ui::D2D::get();
    // DWrite 工厂是独立于 D3D 的初始化链：D3D 建成了它也可能没起来（罕见）。
    // 这里不判空，第一帧画放大镜文字就访问违例 —— 与 CutMask 构造的 deviceContext 同理
    if (!d2d || !d2d->dwriteFactory || !d2d->baseTextFormat) return;
    // 字号 / 留白 10 / 5：文字带高 66 就是按这四行内容配出来的（见上 pixTextH）
    float padding{ 5.f * dpi },fontSize{ 10.f*dpi };
    // 四行内容与"按 C 复制"共用 pixInfoLines()，两边格式不会走散
    auto lines = pixInfoLines();
    for (size_t i = 0; i < lines.size() && i < 4; i++)
    {
        const auto& str = lines[i];
        // 文本没变就复用上一帧的 layout（前三行颜色不动时命中缓存；
        // POS 行每帧都变，等于只缓存前三行）
        if (!pixTextLayout[i] || pixTextLast[i] != str) {
            pixTextLayout[i].Reset();
            d2d->dwriteFactory->CreateTextLayout(str.data(), (UINT32)str.length(), d2d->baseTextFormat.Get(), FLT_MAX, FLT_MAX, pixTextLayout[i].GetAddressOf());
            if (!pixTextLayout[i]) continue;   // 建失败（字体异常）：这一行不画，别把空指针递给 D2D
            pixTextLayout[i]->SetFontSize(fontSize, { 0,INT_MAX });
            pixTextLast[i] = str;
        }
        ctx->DrawTextLayout({ pixPos.x + padding, pixPos.y + pixImgH + padding*(i+1) + fontSize*i }, pixTextLayout[i].Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    // 字带右下角"色彩信息 / 按C复制"：只是提示，真正复制走 onKey 的 'C'。
    // 右对齐贴边，静态文本只建一次。
    // 字号 12：与四行色值文字的 10 拉开一档，提示更显眼（用户指定）
    const float hintX = pixPos.x + padding, hintW = pixW - padding * 2;
    const float hintFont{ 12.f * dpi };
    // 提示两行换个族：四行色值是等宽感的"数据"，提示是"人话"，同族同号挤在一格里
    // 显得笨重。Microsoft YaHei UI 是雅黑的 UI 变体（字形一致、左右边距更紧），
    // 小字号下不发胖，也不引入任何外部字体文件，系统自带、缺了会自动回退。
    // 想要更秀气可以换成 L"等线"（DengXian，Win10 起自带）
    const wchar_t* hintFamily{ L"Microsoft YaHei UI" };
    if (!pixHintLayout[0] || !pixHintLayout[1]) {
        pixHintLayout[0].Reset();
        pixHintLayout[1].Reset();
        d2d->dwriteFactory->CreateTextLayout(L"复制信息", 4, d2d->baseTextFormat.Get(), hintW, FLT_MAX, pixHintLayout[0].GetAddressOf());
        d2d->dwriteFactory->CreateTextLayout(L"按C", 2, d2d->baseTextFormat.Get(), hintW, FLT_MAX, pixHintLayout[1].GetAddressOf());
        for (auto& lay : pixHintLayout) {
            if (!lay) continue;
            lay->SetFontSize(hintFont, { 0,INT_MAX });
            lay->SetFontFamilyName(hintFamily, { 0,INT_MAX });
            lay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }
    // 两行提示在整条字带里整体垂直居中：用 DWrite 的实测行高算中心，
    // 不按 fontSize 估 —— 换字族、换字号都还停在正中。水平仍旧右对齐贴边，
    // 右边那块是空的，左边四行色值最长的也就到一半宽，互不压字
    if (pixHintLayout[0] && pixHintLayout[1]) {
        DWRITE_TEXT_METRICS m0{}, m1{};
        pixHintLayout[0]->GetMetrics(&m0);
        pixHintLayout[1]->GetMetrics(&m1);
        const float total = m0.height + m1.height;
        const float top = pixPos.y + pixImgH + (pixTextH * dpi - total) * 0.5f;
        ctx->DrawTextLayout(D2D1::Point2F(hintX, top),
            pixHintLayout[0].Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        ctx->DrawTextLayout(D2D1::Point2F(hintX, top + m0.height),
            pixHintLayout[1].Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
}

// 放大镜底下的四行取色信息。绘制和"按 C 复制"共用：格式只写一处
std::vector<std::wstring> WinCap::pixInfoLines()
{
    POINT pos;
    GetCursorPos(&pos);
    // 取色优先走内存里的整屏像素副本（CutMask 的吸附数据源，截图时就拷好了）：
    // O(1) 数组读。GetPixel 每帧走 GDI 实时取屏，个别驱动/远程桌面上一次几十毫秒，
    // 放大镜跟着鼠标每帧都取，拖动时整屏像卡死。副本没就绪（抓屏失败等）才退回 GetPixel
    COLORREF cr;
    if (cutMask && cutMask->pixelReady()) {
        // 窗口铺满虚拟桌面，客户区坐标 = 屏幕坐标 - 虚拟屏原点；pixAt 自带越界保护
        auto p = cutMask->pixelAt((int)(pos.x - x), (int)(pos.y - y));
        cr = RGB((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
    }
    else {
        HDC hScreen = GetDC(NULL);
        cr = GetPixel(hScreen, pos.x, pos.y);
        ReleaseDC(NULL, hScreen);
    }
    const auto [c, m, yy, k] = getCMYK(GetRValue(cr), GetGValue(cr), GetBValue(cr));
    return {
        std::format(L"HEX : #{:02X}{:02X}{:02X}", GetRValue(cr), GetGValue(cr), GetBValue(cr)),
        std::format(L"RGB : {},{},{}", GetRValue(cr), GetGValue(cr), GetBValue(cr)),
        std::format(L"CMYK : {},{},{},{}", c, m, yy, k),
        std::format(L"POS : X:{} Y:{}", pos.x, pos.y),
    };
}

// 选区阶段按 C：四行一起进剪切板（纯文本，一行一条）。
// 用户要的是"按住能真复制"，所以这里不进工具条、也不依赖任何焦点，
// 直接在 WinCap 的按键链上接
void WinCap::copyPixInfo()
{
    auto lines = pixInfoLines();
    std::wstring text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) text += L"\r\n";
        text += lines[i];
    }
    Util::copyText(text);
}

void WinCap::onKey(UINT key)
{
    // 编辑文本时按键归 TextBox（Ctrl+C 复制的是框里的字，回车不保存），
    // 唯独 ESC 一律按"结束整个截图"处理：先把这段文字收尾（空文本会被丢弃），
    // 再直接关窗。以前这里只 finishEdit，用户得再按一次 ESC 才退得出去
    if (editingText) {
        // ESC 仍然交给统一入口：收尾这段文字 + 直接关窗（见 quitByEsc），
        // 不再"只退出编辑"等用户按第二次
        if (key == VK_ESCAPE) quitByEsc();
        return;
    }
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    // 录制中：Ctrl+S 存文件、Ctrl+C 存剪切板（等价于录制条上那两颗按钮），
    // Esc 直接结束整个截图（丢弃这段录制）。走 ToolSub 的同一个入口，
    // 这样计时器 / 停录 / 恢复界面只有一条路径 —— 现在键盘和按钮不会各做一套
    if (stage == CapStage::Video) {
        if (ctrl && (key == 'S' || key == 'C')) {
            if (toolSub) toolSub->videoSave(key == 'C');
        }
        else if (key == VK_ESCAPE) {
            // 一次 ESC 就退出：以前只退到 Adjust（标注/选区），用户还得再按一次才关得掉
            quitByEsc();
        }
        return;
    }
    // Adjust 阶段：标注快捷键优先。Enter = 确认（把带标注的选区复制进剪切板并关窗），
    // Ctrl+C 复制、Ctrl+S 存盘、Ctrl+Z/Y 撤销重做、Del 删 hover、Esc 关闭
    if (stage == CapStage::Adjust && toolMain && history) {
        if (ctrl && key == 'Z') { history->undo(); return; }
        else if (ctrl && key == 'Y') { history->redo(); return; }
        else if (ctrl && key == 'C') { copyToClipboard(); return; }
        else if (ctrl && key == 'S') { saveToFile(); return; }
        else if (key == VK_DELETE) { history->removeHoverShape(); return; }
        else if (key == VK_RETURN) { copyToClipboard(); return; }
        else if (key == VK_ESCAPE) { quitByEsc(); return; }
    }
    // 不在 Select 阶段，标注快捷键之外只认关闭
    if (stage != CapStage::Select) {
        if (key == VK_ESCAPE) quitByEsc();
        return;
    }
    // Select 阶段：移动光标 / 退出。
    // 原来这里还有 Ctrl+H / Ctrl+R / Ctrl+K / Ctrl+P 四个"取色并复制"的快捷键，
    // 连同放大镜底下的提示文字一起删了：选区阶段最常按的是 Esc 和方向键，
    // 那四个键既没人记、又和标注阶段的 Ctrl+C/S 混在一起，只添乱
    // C = 把放大镜底下那四行（HEX / RGB / CMYK / POS）复制进剪切板，
    // 对应字带右侧那两行提示"复制信息 / 按 C"。Ctrl+C 不占这个键，
    // 免得和标注阶段的"复制截图"混在一起
    if (!ctrl && key == 'C') {
        copyPixInfo();
    }
    else if (key == VK_ESCAPE) {
        quitByEsc();
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
    if (!screenImg) return cutImg;
    auto& maskRect = cutMask->maskRect;
    const UINT32 cw = (UINT32)(maskRect.right - maskRect.left);
    const UINT32 ch = (UINT32)(maskRect.bottom - maskRect.top);
    if (cw == 0 || ch == 0) return cutImg;
    D2D1_BITMAP_PROPERTIES1 prop{};
    prop.pixelFormat = screenImg->GetPixelFormat();
    prop.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
    screenImg->GetDpi(&prop.dpiX, &prop.dpiY);
    // 设备起不来时没有 deviceContext（screenImg 也可能是空的），跟其它失败路径一样返回空图
    auto d2d = ui::D2D::get();
    if (!d2d || !d2d->deviceContext) return nullptr;
    auto hr = d2d->deviceContext->CreateBitmap(D2D1::SizeU(cw, ch), nullptr, 0, &prop, cutImg.GetAddressOf());
    if (FAILED(hr) || !cutImg) return nullptr;
    auto start = D2D1::Point2U(0, 0);
    auto rect = D2D1::RectU((UINT32)maskRect.left, (UINT32)maskRect.top, (UINT32)maskRect.right, (UINT32)maskRect.bottom);
    hr = cutImg->CopyFromBitmap(&start, screenImg.Get(), &rect);
    if (FAILED(hr)) return nullptr;
    return cutImg;
}

LRESULT WinCap::onHitTest(const POINT pos)
{
    (void)pos;
    // 录制阶段整窗让出鼠标：用户要能直接操作被录的那个应用，
    // 压暗的透明区也一样能点下去（HTTRANSPARENT 会把消息转给下面那个窗口）
    if (stage == CapStage::Video) return HTTRANSPARENT;
    // 截图阶段：整窗吞掉，点击不许穿到桌面
    return HTCLIENT;
}

void WinCap::onDown(POINT pos, bool isRight)
{
    // 按下这一下就不再"把手柄钉着"了：要么是去拉角/搬框，要么是在别处落新的一笔，
    // 之后要不要亮手柄由悬停判定说话（见 probeHover）
    draggerPin = nullptr;
    if (isRight) {
        // Select 阶段点右键 = 重新进入屏幕选区界面（和刚按完快捷键那一帧一样），
        // 连吸附出来的虚线框一起清掉，等用户重新拖框
        if (stage == CapStage::Select) {
            restartSelect();
            return;
        }
        if (stage == CapStage::Adjust) {
            // 编辑状态右键 = 选中编辑：点中某个已有标注就解锁它（越过"只编辑最后一个"），
            // 让它可被直接拖动 / 修改
            if (!history) return;   // 标注没建起来（ensureAnnotation 失败）：右键无处可选
            auto imgPos = toImgPos(pos);
            // 只有最后一个未撤销的标注能被"选中编辑"。更早的那些不接操作 ——
            // 想改它们只能一步步回退（用户定的规则），所以这里找到它就直接返回
            for (int i{ (int)history->shapes.size() - 1 }; i >= 0; i--) {
                auto cur = history->shapes[i].get();
                if (!cur || cur->isUndo) continue;
                cur->mouseMove((float)imgPos.x, (float)imgPos.y);
                if (cur->hoverDraggerIndex >= 0) {
                    editTarget = cur;
                    shapeHover = cur;
                    refresh();
                }
                return;
            }
            // 右键点在空白处 = 重新进入屏幕选区界面。
            // 一旦画了圆 / 矩形等标注，选框就已结束，后续都是编辑器的活，禁止二次选框
            if (!history->shapes.empty()) return;
            restartSelect();
            return;
        }
        close();
        return;
    }
    // 编辑文本时，落在文本框里的点击整个交给 TextBox —— 只有四角缩放柄例外：
    // 那几处正好压在文本框的角上，让出去就等于"角上按不动"。
    // 交给标注时会先 finishEdit 再开始缩放（见 ShapeText::mouseDown）
    if (editingText && textBox && textBox->isPosIn(pos)) {
        const auto ip = toImgPos(pos);
        editingText->mouseMove((float)ip.x, (float)ip.y);
        if (!editingText->onScaleHandle()) return;
    }
    // 点在框外：先把这一笔收掉，但**这一下不能白按** —— 收完继续走下面的正常流程，
    // "离开旧框"和"在新位置落笔"合成同一次点击。以前是 finishEdit 后直接 return，
    // 那一下被吃掉，用户在别处要再点一次才开始输入（两次点击才换到新位置）。
    // 唯一例外是压在这一笔自己的边框上：那是"收笔 + 抓手挪框"的手势，照旧交给它处理
    if (editingText && editingText->hoverDraggerIndex == 0) {
        editingText->finishEdit();
        shapeHover = nullptr;
        shapeShow = nullptr;    // 显示用的那个同理，否则下一帧还会给它画框
    }
    // 双击判定得自己做：Ui 的窗口类没带 CS_DBLCLKS，WM_LBUTTONDBLCLK 根本不会来
    auto now = GetTickCount64();
    bool isDblClick = (now - lastDownTime <= GetDoubleClickTime())
        && std::abs(pos.x - lastDownPos.x) <= GetSystemMetrics(SM_CXDOUBLECLK)
        && std::abs(pos.y - lastDownPos.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
    lastDownTime = now;
    lastDownPos = pos;
    if (stage == CapStage::Adjust) {
        // 工具条 / 历史没建起来（ensureAnnotation 失败，系统资源紧张时可能发生）：
        // 标注这条路整段跳过，但不能再往下走 —— 下面每一处都要解引用 history
        if (!history) return;
        if (isDblClick && (!toolMain || toolMain->curId.empty())) {
            copyToClipboard();
            return;
        }
        // 有副编辑器（选中了工具）= 进入编辑，四周区域锁定：点哪都用对应工具画，不再改选框
        if (toolMain && !toolMain->curId.empty()) {
            // 例外：贴着选区边/角那一圈仍然归"拖选区"。
            // 以前是彻底锁死 —— 选了矩形工具之后框就拖不动、也改不了大小，
            // 想挪一下框再画必须先取消工具选中，用户对这件事的反馈就是
            // "编辑器不跟随选区框"。现在按边/角优先，带外（框内外远处）照旧画标注
            const auto edgeHit = cutMask->hitEdge(pos, edgeGrabBand());
            if (edgeHit != MaskHit::None && edgeHit != MaskHit::Inside) {
                isPress = true;
                // 方向直接用 hitEdge 判出来的：这一点可能落在框内但贴着边，
                // 交给 hitTest 会判成 Inside（＝整体平移），手感就错了
                cutMask->startAdjust(pos, edgeHit);
                return;
            }
            // 落笔之前先把悬停刷到当前位置：工具条是独立窗口，光标在它上面时
            // 本窗收不到 WM_MOUSEMOVE，等移回来直接按下，shapeHover 还是上一个
            // 位置的值 —— 拿着它往下走就会改到一条早就不是"最后一个"的标注
            // （或是该新建一笔时反而拖动了旧元素）。探一次只要一次 mouseMove，不亏
            probeHover(pos);
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
        // 没有副编辑器 = 可以再次选择/拖动区域（内部拖=移动，边/角=改大小）
        isPress = true;
        // 抓鼠标：拖选区时手很可能划过工具条（它是独立顶层窗口）。不抓的话
        // 松开那一下被工具条吃掉，本窗收不到 WM_LBUTTONUP，isPress 一直为真 ——
        // 之后鼠标一动，选区就跟着光标跑，只能再点一下才停
        SetCapture(hwnd);
        cutMask->startAdjust(pos);
        return;
    }
    if (stage == CapStage::Select) {
        // 手动拖动框选区。从这一下起就是纯手画：CutMask::makeRect 不做任何吸附 ——
        // 用户已经下笔了，就别再改他的数（想吸准用悬停智选，那才是它的分工）。
        // autoSnap 在这里恢复成开，管的是"松开鼠标以后"的悬停智选：右键重进选区时
        // 它是关的（先到 +，动鼠标才识别），按下拖框也算"动过"了，顺手恢复
        if (allowSnap() && !cutMask->autoSnap) cutMask->autoSnap = true;
        snapArmed = false;   // 已经主动按下拖框，待命状态结束
        isPress = true;
        cutMask->startMakeRect(pos);
    }
}

void WinCap::onMove(POINT pos)
{
	PROF_SCOPE("wm.move.total");
    if (stage == CapStage::Select) {
        if (isPress) {
            cutMask->makeRect(pos);
        }
        else {
            // 右键重进选区后的"第一次移动"：智能吸附到这一下才恢复（先到+，移动才识别），
            // 与按快捷键首次进入的节奏一致。
            // 注意必须真的位移了才算：右键松手时系统会补发一次光标没动的 WM_MOUSEMOVE，
            // 若那一次就恢复，就会"点右键立刻又自动框到窗口上"
            if (snapArmed) {
                bool moved = std::abs(pos.x - snapArmPos.x) > 2 || std::abs(pos.y - snapArmPos.y) > 2;
                if (moved) {
                    snapArmed = false;
                    if (allowSnap()) cutMask->autoSnap = true;
                }
            }
            // 坐标根本没动的一帧（系统在右键松手、焦点切换后会补发一次相同的
            // WM_MOUSEMOVE）：选区不会变，放大镜也不用挪，整屏重画一遍纯属浪费
            if (pos.x == lastMovePos.x && pos.y == lastMovePos.y) return;
            lastMovePos = pos;
            // 选区（智能吸附）先算，放大镜位置随后更新，最后只排一帧 ——
            // highlight 自己不再重绘，就是为了让这一帧里三件事都到位
            cutMask->highlight(pos);
            getPixImg(pos);
            setPixPos(pos);
            requestPaint();
        }
    }
    else if (stage == CapStage::Adjust) {
        if (!history) return;   // 同 onDown：没有历史就没有可悬停的元素
        // 框内的鼠标原本整个让给 TextBox。但四角缩放柄就压在文本框的角上，
        // 让出去就成了"输入时只有虚线框、移到角上也不出四角"。这里补一次命中探测：
        // probeHover 内部只在结果变了才重绘，TextBox 自己的活一点不碰。
        // 按着角柄/旋转柄拖动时这一条要跳过（isMouseDown 为真），交给下面的 mouseDrag ——
        // 不然拖动中鼠标划回框里那几像素，缩放/旋转就停住了
        if (editingText && textBox && textBox->isPosIn(pos) && !isMouseDown) {
            probeHover(pos);
            return;
        }
        if (isMouseDown) {
            // 正在画 shape（选中了工具）：拖拽交给 shape，坐标直接用窗口坐标
            auto imgPos = toImgPos(pos);
            if (pos.x != pressPos.x || pos.y != pressPos.y) hasDragged = true;
            if (shapeHover) shapeHover->mouseDrag((float)imgPos.x, (float)imgPos.y);
            requestPaint();
        }
        else if (isPress) {
            // 正在调选区：四边/角改大小，没选工具时内部拖动 = 移动选区。
            // 选区一变，编辑器也要跟着走，所以重排工具条。
            // adjust 内部的 requestPaint 与本行合并成一帧（同一个时间闸门）
            cutMask->adjust(pos);
            layoutTools();
            requestPaint();
        }
        else if (editingText || (toolMain && !toolMain->curId.empty())) {
            // 没按下：做悬停判定（规则在 WinHost::probeHover 里，和贴图窗共用）。
            // 可交互的仍然是"只认最后一个 / 右键解开的那个"；显示这一路放宽到任意标注，
            // 鼠标移上去就把虚线框和手柄亮出来，不必先点一下。
            // 编辑中也照做：手柄（四角/旋转柄）就挂在框上，鼠标移过去要立刻亮、立刻能拉
            probeHover(pos);
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
        // 抓像素模式（二维码识别）：选区一定就用选区里的原始像素去回调，
        // 完全不进标注阶段 —— 工具条、历史、离屏画布一概不用
        if (grabMode) {
            GrabHandler handler = std::move(grabHandler);
            grabHandler = nullptr;
            grabMode = false;
            std::vector<BYTE> px;
            D2D1_SIZE_U size{};
            bool ok = grabPixels(px, size);
            close();
            // handler 放在 close 之后调：回调里常常要立刻拉起主窗口，
            // 先 close 可以让全屏的截图窗先让位
            if (ok && handler) handler(std::move(px), (int)size.width, (int)size.height);
            return;
        }
        if (enterByArg()) return;
        // 按住 Ctrl 框选：跳过调整，直接钉到桌面上。
        if ((GetKeyState(VK_CONTROL) & 0x8000)) {
            startPin();
            return;
        }
        stage = CapStage::Adjust;
        // 标注阶段要能打字（文字工具输中文），把选区阶段摘掉的输入法还回去
        if (savedImc) { ImmAssociateContext(hwnd, savedImc); savedImc = nullptr; }
        ensureAnnotation();
        layoutTools();
        refresh();  // 收掉放大镜
        // 刚建的 ToolMain/ToolSub 是顶层窗口，可能抢走焦点；立刻把焦点夺回 WinCap，
        // 否则框选完马上按 Esc 会被吞掉（焦点落在别的窗口上）。移动一下鼠标时系统可能
        // 把焦点还回来，所以表现为"动一下就又能退出"
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        // 进入 Adjust 后：选区四周（四边 + 四角）可拖改大小；选区内选中工具 = 画标注，
        // 没选工具 = 拖动移动选区；Enter / 双击 / 工具条按钮确认收尾
    }
    else if (stage == CapStage::Adjust) {
        isPress = false;
        // 拖选区那一路（onDown 里没选工具的分支）抓了鼠标，抬手时在这里放开
        if (!isMouseDown && GetCapture() == hwnd) ReleaseCapture();
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
                // 这一笔画完了：把四角小方框钉住亮着，用户不用先把鼠标挪到角上才看得见它们。
                // 下一次按下 / 撤销 / 删除都会解除（见 probeHover 与 History::removeShape）
                draggerPin = shapeHover;
                refresh();
                setTimer(800, 100);
            }
        }
        // 抬手之后再探一次悬停：文字刚收笔 / 刚画完一笔时鼠标通常是不动的，
        // 此刻不重算，虚线框和手柄就要等下一次鼠标移动才出现 —— 看起来像"点了才有"
        if (toolMain && !toolMain->curId.empty()) probeHover(pos);
    }
}

// close() 里 DestroyWindow 之后同步触发 onDestroy，而这条路径通常是从某个工具条的按钮
// 回调里一路进来的（工具条是 WinPin 的成员）。在这里直接 winCap.reset() 就是 use-after-free，
// 所以窗口句柄立即销毁，C++ 对象的释放推迟到下一轮消息循环。
void WinCap::onClosed()
{
    ::prof::dump();   // 会话结束：把累积的埋点快照写进 %TEMP%\shotcore_prof.txt
    if (isClosed) return;
    isClosed = true;
    killTimer(kEscPollTimer);
    // 正在编辑的文字先收尾。不收的后果有两个：
    //   1) TextBox 上还挂着 ShapeText 的 onTextChanged / onFocusChanged 订阅，
    //      而那个 ShapeText 会随 history 一起析构 —— 之后 TextBox 再触发一次就是野指针；
    //   2) 编辑中的文字只活在 TextBox 那一层，直接关窗等于这段文字凭空消失
    if (editingText) editingText->finishEdit();
    // 录屏还在跑（用户直接关窗 / 进程退出路径）：先把编码线程停掉并丢掉临时文件，
    // 线程没停就走析构会和采集设备卡死。
    // 但这件活必须挪出 UI 线程：MP4 收尾（SinkWriter Finalize）要几百毫秒到几秒，
    // 关窗之后用户紧接着就会按截图热键，堵在这里就是"按了没反应，过好久才生效"。
    // unique_ptr 直接搬进后台线程，WinCap 这边立刻放手，之后不再碰它
    if (capVideo) {
        std::thread([v = std::move(capVideo)]() mutable {
            v->discard();
            v.reset();
            }).detach();
    }
    // 收起附属工具条（它们各自是独立顶层窗口，WinCap 关闭时一并销毁）
    if (toolSub) toolSub->close();
    if (toolMain) toolMain->close();
    // 截图前被 hideForCapture 收起来的主界面，到这里放回去。没藏过就是个空操作，
    // 所以这条路径上可以无条件调 —— 否则主界面会一直停在隐藏状态，用户以为程序没了
    WinQr::restoreAfterCapture();
    // 绑定代次：延迟析构只允许收掉"自己这一代"的窗口。
    // 关窗后托盘 / 热键可能在同一轮消息循环里就发起下一次截图（会先把死壳回收再新建），
    // 那时 winCap 已经指向新对象了，这里若无条件 reset() 就会把刚建好的新窗口干掉，
    // 表现为"截一次之后再也截不了"甚至闪退
    const unsigned myGen = gen;
    auto app = ui::App::get();
    // App 可能已经在退出流程里被拆掉了（先收窗口再拆单例）：那时排不进队列，
    // 也没必要再排 —— 进程就要结束了
    if (!app) return;
    app->dq.TryEnqueue([myGen]() {
        auto cur = WinCap::get();
        if (cur && cur->gen != myGen) return;   // 已经有新一代窗口了，别碰
        winCap.reset();
        // 会话结束把整屏 CPU 副本还给系统：shotBuf 是给"下一次截图"复用的，容量一直占着
        // （1080p 8.3MB / 4K 33MB）。截完一次就回托盘待命是这个工具最常见的状态，
        // 为省下下次那十几毫秒的首次 touch 让它常年占着不划算 —— 下次截图时 resize 会重新要
        if (auto host = App::get()) host->releaseShotBuffer();
        // 会话真的收干净了，才把工作集还给系统：整屏位图、压暗层、录制条都在这之前释放，
        // 这里再放否则只是把还要用的页换出去（预览/连拍时反而拖慢下一帧）
        if (auto host = App::get()) host->trimMemory();
        // 队列是异步执行的，这一轮里 App 可能已经被拆掉（宿主退出）—— 再解引用就是崩溃
        auto app = ui::App::get();
        if (!app) return;
        // 用完即走模式下截图结束就退出进程，与 App 构造里的判断对称。
        // 但标注这条条路是先把图钉到桌面上再关自己的，那种情况下活还没干完，
        // 退出的活交给最后一个关掉的贴图窗口
        if (!WinPin::hasWindow()) {
            if (app->args[L"--auto-quit"] == L"true") {
                app->quit(0);
            }
        }
    });
}


bool WinCap::enterByArg()
{
    auto& args = ui::App::get()->args;
    auto it = args.find(L"--enter");
    if (it == args.end()) return false;
    auto& val = it->second;
    if (val == L"pin") {
        startPin();   //等于替用户按住了 Ctrl 框选
        return true;
    }
    return false;
}

// 把整个窗口退回"按快捷键首次进入"的屏幕选区界面：底图铺满、没有选区、没有工具条，
// 十字光标 + 放大镜跟着鼠标，等用户重新拖框。
// 与首次进入完全同一条节奏：先到"+"（autoSnap 先关掉、光标立刻切十字，不出框），
// 鼠标第一次移动 / 第一次按下拖框时恢复智能吸附 —— "先到+，移动才会智能选区"。
void WinCap::restartSelect()
{
    // 1. 归零所有按下态：右键可能落在拖框 / 拖形状的中途
    // 正在编辑的文字先收尾：否则 TextBox 会留在屏上继续吃键盘，而它对应的 ShapeText
    // 在下面被当成普通标注继续参与绘制，出现"编辑框和图文两份"的错位
    if (editingText) editingText->finishEdit();
    isPress = false;
    if (isMouseDown) {
        isMouseDown = false;
        ReleaseCapture();
    }
    newShape = nullptr;
    shapeHover = nullptr;
    shapeShow = nullptr;
    editTarget = nullptr;
    draggerPin = nullptr;   // 重开选区：上一轮画完钉住的手柄一并解除
    hasDragged = false;
    prevPressCreatedShape = false;
    lastDownTime = 0;          // 别把上一轮的按下时间算进双击判定
    killTimer(100);            // 悬停刷新的定时器也跟着停
    // 2. 选区先清掉：后面取消工具选中时 layoutTools 会因为"没有选区"直接返回，不会乱摆
    cutMask->reset();
    // 与首次进入同一条节奏：先到"+"—— 智能吸附先关掉，右键这一刻不出框；
    // 鼠标第一次移动（onMove）或第一次按下拖框（onDown）时再打开，一动才识别。
    // 光标也立刻切成十字：Adjust 阶段它可能是 SIZEALL 等形状，而 WinCap 本就是前台窗口，
    // SetForegroundWindow 不会重发 WM_SETCURSOR，不主动切就得等鼠标动一下才变
    cutMask->autoSnap = false;
    SetCursor(LoadCursor(nullptr, IDC_CROSS));
    // 3. 工具条收起，工具选中态也复位（新一轮框选从"没选工具"开始）
    if (toolMain) {
        toolMain->cancelSelect();
        toolMain->hide();
    }
    if (toolSub) toolSub->hideTools();
    // 4. 回到 Select 阶段，放大镜摆到光标处 —— 就是 onCreated 里那一帧的画面
    stage = CapStage::Select;
    // 又回到选区阶段：输入法重新摘掉，否则在这儿按 C 会被拼音组字吞掉
    if (!savedImc) savedImc = ImmAssociateContext(hwnd, nullptr);
    POINT cur{};
    GetCursorPos(&cur);
    ScreenToClient(hwnd, &cur);
    // 记下"回到+这一刻"的光标位置：之后必须真的移动了才恢复智能识别，
    // 否则右键松手补发的那次未移动的 WM_MOUSEMOVE 会立刻把框吸出来
    snapArmPos = cur;
    snapArmed = true;
    setPixPos(cur);
    getPixImg(cur);
    // 5. 焦点抢回来：工具条消失后焦点可能还留在它上面，否则接下来 Esc / 方向键会失灵
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    refresh();
}

// —— 主编辑器定位 ——

bool WinCap::layoutTool(ui::WinBase* tool, int* outX, int* outY)
{
    if (!tool) return false;
    if (!cutMask || !cutMask->hasRect()) return false;
    const int toolW = (int)(tool->w + 0.5f);
    const int toolH = (int)(tool->h + 0.5f);
    // 工具条对着选区摆，跟主编辑器在不在无关
    // maskRect 是本窗口的客户区坐标，换算到屏幕坐标
    const int maskLeftScr = x + (int)cutMask->maskRect.left;
    const int maskTopScr = y + (int)cutMask->maskRect.top;
    const int maskRightScr = x + (int)cutMask->maskRect.right;
    const int maskBottomScr = y + (int)cutMask->maskRect.bottom;

    // 用框选区域所在显示器的工作区判断上/下方是否有足够空间
    RECT maskScrRect{ maskLeftScr, maskTopScr, maskRightScr, maskBottomScr };
    HMONITOR hMon = MonitorFromRect(&maskScrRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    // 取不到显示器信息时退回整个虚拟桌面：mi.rcWork 不初始化就是垃圾值，
    // 工具条会摆到屏幕外去
    if (!hMon || !GetMonitorInfo(hMon, &mi)) {
        mi.rcWork.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        mi.rcWork.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        mi.rcWork.right = mi.rcWork.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        mi.rcWork.bottom = mi.rcWork.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }

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

    // 兜底：不越出所在显示器工作区（横向纵向都得夹：选区压在屏幕最上/最下边、
    // 或者叠放分支算出来落在工作区外时，只夹横向会把工具条摆到屏幕外，用户点不到）
    if (toolX < mi.rcWork.left) toolX = mi.rcWork.left;
    if (toolX + toolW > mi.rcWork.right) toolX = mi.rcWork.right - toolW;
    if (toolY < mi.rcWork.top) toolY = mi.rcWork.top;
    if (toolY + toolH > mi.rcWork.bottom) toolY = mi.rcWork.bottom - toolH;
    tool->setPosition(toolX, toolY);
    if (outX) *outX = toolX;
    if (outY) *outY = toolY;
    return true;
}
void WinCap::startPin()
{
    if (!cutMask->hasRect() || !screenImg) return;
    auto& maskRect = cutMask->maskRect;
    // WinPin 构造里会回头来取 getCutImg()，所以得先把它建起来再关自己
    WinPin::init(int(maskRect.left) + x, int(maskRect.top) + y,
        int(maskRect.right - maskRect.left), int(maskRect.bottom - maskRect.top));
    close();
}


void WinCap::saveToFile()
{
    // 保存直接落到设置页选定的目录（默认桌面，不弹另存为对话框），路径带毫秒时间戳。
    // 存完给一条软件内的轻提示（半透明小条贴在屏幕下方，1.8 秒自己消失），然后关窗
    std::vector<BYTE> pixels;
    D2D1_SIZE_U size{};
    if (!getImagePixels(pixels, size)) return;
    auto path = Util::savePathIn(Setting::get()->getShotDir(), L"png");
    // 连桌面目录都拿不到（极少数策略禁用账户）：同样要给一句提示，不然就是"点了没反应"
    if (path.empty()) {
        if (auto app = ui::App::get()) app->showToast(Lang::get(L"cap.saveFail"));
        return;
    }
    if (Util::saveToFile(path, (int)size.width, (int)size.height, pixels.data())) {
        if (auto app = ui::App::get()) app->showToast(Lang::get(L"cap.savedTip"));
        close();
        return;
    }
    // 写盘失败（目录被设成只读、U 盘拔了、磁盘满、同名文件被占用）以前是无声无息：
    // 用户点了保存、界面还停在原地、以为是自己没点着。这里必须给出原因
    if (auto app = ui::App::get()) app->showToast(Lang::get(L"cap.saveFail"));
}


void WinCap::copyToClipboard()
{
    std::vector<BYTE> pixels;
    D2D1_SIZE_U size{};
    if (!getImagePixels(pixels, size)) return;
    // 剪切板被别的程序占着（OpenClipboard 失败）时不能关窗：用户按了 Ctrl+C / Enter，
    // 图和这一整段标注会话会一起没了，而他还以为已经复制好了
    if (!Util::saveToClipboard((int)size.width, (int)size.height, pixels.data())) {
        if (auto app = ui::App::get()) app->showToast(Lang::get(L"cap.copyFail"));
        return;
    }
    close();
}

// 首次进入 Adjust 阶段时建出标注用的工具条与历史（懒建，避免截图窗口一开始就带着工具条）
void WinCap::ensureAnnotation()
{
    // history 一定要有：出图（getImagePixels）和"没有标注"这条路径都依赖它，
    // 工具条建不出来也不能把它一起漏掉
    if (!history) history = std::make_unique<History>(this);
    if (toolMain && toolSub) return;
    // 两个工具条都是独立顶层窗口，构造里就要过一遍 WinRT 组合器 —— 系统资源紧张、
    // 会话正在切换时可能抛异常。这里必须拦住：本函数是从 WM_LBUTTONUP 的处理链里调进来的，
    // 而 winrt::delegate 的 Invoke 是 noexcept 的，异常穿出去就是 terminate（程序直接消失）
    try {
        toolMain = std::make_unique<ToolMain>(this);
        toolSub = std::make_unique<ToolSub>(this);
    }
    catch (...) {
        // 建不出来的那条就地收掉；剩下的空指针由 layoutTools / onDown 等处的判空兜住，
        // 用户仍然能拖框、出图，只是没有标注工具条
        toolMain.reset();
        toolSub.reset();
        return;
    }
    // 建窗失败（CreateWindowEx 空 / 组合器失败）同样是"不可用"，按上面一半处理
    if (!toolMain->alive() || !toolSub->alive()) {
        toolMain.reset();
        toolSub.reset();
        return;
    }
    // 工具条挂成截图窗的"从属窗口"（owner）。不挂的话它们是两条独立的置顶窗口，
    // z 序只按"谁最后被提升"排：系统把别的程序提到前台时，它们仍留在最顶层，
    // 看上去就是标注器比选区高一级、飘在别人窗口上面。
    // 有了 owner，z 序永远跟着截图窗走 —— 截图窗下去，它们就跟着下去；
    // 选区边框是画在截图窗里的，两者从此在同一层，不会再浮起来
    if (this->hwnd) {
        SetWindowLongPtrW(toolMain->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(this->hwnd));
        SetWindowLongPtrW(toolSub->hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(this->hwnd));
    }
}

// 把 ToolMain / ToolSub 摆到选区周围。ToolMain 用 WinCap::layoutTool 对着选区右下/上对齐，
// ToolSub 永远紧贴 ToolMain 下方（见 ToolSub::updatePosition）。
void WinCap::layoutTools()
{
    if (!toolMain || !toolSub) return;
    if (!cutMask || !cutMask->hasRect()) return;
    // 录制中：主条藏着，录制条不再挂在它下面（那是"副条"的摆法），而是自己顶到选区右下角 ——
    // 用户口径"像截图那样贴边到窗口右下角"。layoutTool 就是主条用的那套"右边缘对齐选区右边缘、
    // 优先摆在选区下方"的算法，直接拿来用，录完才回到主/副条那一套
    if (stage == CapStage::Video) {
        int toolX{ 0 }, toolY{ 0 };
        layoutTool(toolSub.get(), &toolX, &toolY);
        // 录 gif 时录制条不能压在选区里：mp4 走的是桌面复制，WDA_EXCLUDEFROMCAPTURE
        // 能让录制条不进画面；gif 走的是 GDI BitBlt，那个属性对它完全无效 ——
        // 而 layoutTool 在选区上下都没余量时会把条叠进选区右下角内部，
        // 于是每一帧都带着这条工具条。这里再试一次左右外侧，能整条挪出去就挪
        if (videoIsGif && toolSub->hwnd) {
            const int toolW = (int)(toolSub->w + 0.5f);
            const int toolH = (int)(toolSub->h + 0.5f);
            const RECT mask{ x + (int)cutMask->maskRect.left, y + (int)cutMask->maskRect.top,
                             x + (int)cutMask->maskRect.right, y + (int)cutMask->maskRect.bottom };
            const bool inside = toolX < mask.right && toolX + toolW > mask.left
                             && toolY < mask.bottom && toolY + toolH > mask.top;
            if (inside) {
                HMONITOR hMon = MonitorFromRect(&mask, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{ sizeof(MONITORINFO) };
                RECT wa;
                if (hMon && GetMonitorInfo(hMon, &mi)) wa = mi.rcWork;
                else {
                    wa.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
                    wa.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
                    wa.right = wa.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
                    wa.bottom = wa.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
                }
                const int gap = (int)(cutMask->strokeWidth + 2.f * toolSub->dpi + 0.5f);
                // 竖直居中更贴近"贴着选区"的观感，再夹回工作区
                int cy = mask.top + ((mask.bottom - mask.top) - toolH) / 2;
                if (cy < wa.top) cy = wa.top;
                if (cy + toolH > wa.bottom) cy = wa.bottom - toolH;
                const int leftX = mask.left - gap - toolW;
                const int rightX = mask.right + gap;
                if (leftX >= wa.left) {
                    toolSub->setPosition(leftX, cy);
                    toolX = leftX; toolY = cy;
                }
                else if (rightX + toolW <= wa.right) {
                    toolSub->setPosition(rightX, cy);
                    toolX = rightX; toolY = cy;
                }
                // 左右也放不下（选区几乎铺满整屏）就维持原样：这种情形没有任何
                // 屏幕内的位置能避开，硬挪只会把条挤到屏幕外、用户点不到停止
            }
        }
        // 录制条是录制中唯一的入口（主条已经藏了）：窗口要是被谁藏了，这里无条件显示回来。
        // 以前这个分支只摆位置不 show，一旦被藏就再也没有显示的入口 ——
        // 用户看到的就是"运行态偶发丢失，只能杀进程"
        toolSub->ensureShown();
        // 录制条要压在宿主之上。宿主现在是 WS_EX_LAYERED 的分层窗口，它的压暗层会被
        // 合成到普通 topmost 窗口上面 —— 录制条落在它下面就会被压暗（用户："运行态的菜单被压暗了"）。
        // 这里必须是 HWND_TOPMOST（提到 topmost 带最顶、也就是压在宿主之上）；
        // 副条本来就带 WS_EX_TOPMOST，与"常显的主条"是两码事，别一起降级
        if (toolSub->hwnd && IsWindowVisible(toolSub->hwnd))
            SetWindowPos(toolSub->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
    layoutTool(toolMain.get());
    // 位置摆好之后再恢复显示：工具条可能是被 Select 阶段（右键重选区）hide 掉的，
    // 先 show 再摆位置会在旧位置闪一下
    if (toolMain->hwnd && !IsWindowVisible(toolMain->hwnd)) toolMain->show();
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
    // 主编辑器（ToolMain）始终可见；副编辑器（ToolSub）只在选中了对应工具、或
    // hover 预览（previewId 非空）时才出现，否则收起。选区移动 / 缩放时这里被
    // 重新调用来让整组编辑器跟着选区走
    if ((!toolMain->curId.empty() || !toolMain->previewId.empty()) && toolSub->hasContent()) {
        toolSub->updatePosition(wa);
    }
    else {
        toolSub->hideTools();
    }
}

void WinCap::onToolStyleChanged()
{
    // 只联动"当前工具这一组"的最后一个标注（规则在 WinHost::shapeOfCurrentTool）：
    // 拖字号只认文字、拖线宽只认矩形/椭圆/箭头那一类，不会顺手把别的形状改了。
    // 编辑中的文字同理 —— 工具切到矩形之后，拖矩形滑条不该跑来改它的字号
    // 正在编辑的那一段文字优先：右键解锁的可能是更早的那一段，
    // 拖字号当然要改手上这一段，而不是画布上最后那一段
    ShapeBase* s = (editingText && toolMain && toolMain->curId == L"text")
        ? static_cast<ShapeBase*>(editingText) : shapeOfCurrentTool();
    if (!s) return;
    // 马赛克/高斯模糊没有颜色线宽，它的"强度"就是滑块值，按新强度重建
    if (auto* m = dynamic_cast<ShapeMosaic*>(s)) m->rebuildLive();
    else s->applyStyle();
}

ui::TextBox* WinCap::getTextBox()
{
    if (textBox) return textBox;
    // 建在 canvas 之后：子 visual 按插入顺序叠放，文本框要盖在截图和标注上面。
    // 绝对定位，位置由 ShapeText 按自己的矩形指定。
    textBox = body->makeChild<ui::TextBox>();
    textBox->setPositionType(ui::Position::Absolute);
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
    if (stage == CapStage::Adjust && toolMain) {
        if (toolMain->curId.empty() || !shapeHover) return;
        auto imgPos = toImgPos(pos);
        shapeHover->mouseWheel((float)imgPos.x, (float)imgPos.y, space > 0 ? (short)WHEEL_DELTA : (short)-WHEEL_DELTA);
        return;
    }
    // Select 阶段滚轮 = 循环切换光标下的候选元素（同一位置叠了按钮/输入框/面板时逐个选）。
    // 悬停吸附开着（首次进入）才给循环，右键重进的手动模式不干预
    if (stage == CapStage::Select) {
        cutMask->cycleHover(pos, space > 0 ? 1 : -1);
    }
}

void WinCap::onTimerCB(UINT id)
{
    // 104：ESC 兜底轮询。输入法组字时按 ESC 只会被输入法吃掉（WM_KEYDOWN 不来），
    // 文本编辑中焦点也可能不在宿主上 —— 用户的口径是"什么状态都按一下就退出"，
    // 所以这里直接看键状态，与焦点 / 输入法无关。低两位都认：
    // 2600Hz 的键盘在 25ms 轮询下也可能"按下又抬起"都落在两次采样之间
    if (id == kEscPollTimer) {
        const SHORT st = GetAsyncKeyState(VK_ESCAPE);
        const bool down = (st & 0x8000) != 0;
        const bool pressed = (st & 0x0001) != 0 || (down && !escDown);
        escDown = down;
        if (pressed) quitByEsc();
        return;
    }
    // 101：智能选区的停定维护（鼠标停住 30ms 后做节流重枚举）。
    // 独立成一个 id，跟下面的形状悬停刷新互不干扰
    if (id == 101) {
        if (cutMask) cutMask->onSettle();
        return;
    }
    // 103：悬停断开后的挂住到期。压暗层多留 kHoldMs 再收，桌面图标格间穿行时不闪
    if (id == 103) {
        if (cutMask) cutMask->onHoldExpire();
        return;
    }
    // 102：合帧补帧。上一个 requestPaint 因为离上一帧不足 kFrameMs 被推后到这里，
    // 只补一帧 —— 期间又来了多少次移动都已经体现在最新的光标/选区状态里
    if (id == 102) {
        killTimer(102);
        framePending = false;
        lastFrameTick = GetTickCount64();
        refresh();
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        return;
    }
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
    // 标注完点"贴图"：把这张图钉在桌面上，工具条收起，只剩图本身
    std::vector<BYTE> pixels;
    D2D1_SIZE_U size{};
    if (!getImagePixels(pixels, size)) return;
    if (!cutMask) return;
    auto& m = cutMask->maskRect;
    WinPin::initFromData((int)(x + m.left), (int)(y + m.top), (int)size.width, (int)size.height, pixels);
    close();
}

// —— 录屏 ——
// 起录：只从标注阶段进（选区已经定死），把窗口掰成"看得见真桌面、鼠标归别人"的样子，
// 再建编码器。返回 false 表示这一步没干成，副编辑器留在设置态，什么都不用回滚
bool WinCap::beginVideo(bool mp4, bool speaker, bool mic)
{
    if (stage != CapStage::Adjust) return false;
    if (!cutMask || !cutMask->hasRect()) return false;
    // 正在编辑的文字先收笔：录制开始后鼠标不再归本窗口，编辑框挂在那里就收不了尾
    if (editingText) editingText->finishEdit();
    // 鼠标捕获先放开：调整选区时可能按着，带进录制就把整个桌面锁死了
    if (GetCapture() == hwnd) ReleaseCapture();
    // 先把编码器真的拉起来，再动窗口样式。线程起不来就直接返回 false，
    // 副编辑器留在设置态 —— 不需要任何回滚。
    // 原来的顺序是先切 stage / 切成穿透 / 藏起主条，最后才建编码器：一旦线程
    // 起不来（线程池耗尽、内存告急），窗口已经成了"看起来正在录"的样子，
    // 录制条上的计时一秒一秒走，实际一帧都没采，用户点停止才发现什么都没有
    capVideo = std::make_unique<CapVideo>(this, mp4, speaker, mic);
    if (!capVideo->start()) {
        capVideo.reset();
        return false;
    }
    stage = CapStage::Video;
    videoIsGif = !mp4;
    // 让鼠标整块穿过去 —— 录制中桌面上的应用才是主体，用户要能点图标、拖窗口进画面。
    // 只靠 WM_NCHITTEST 返回 HTTRANSPARENT 是不够的：那条规则只把消息转给**同一线程**里
    // 底下的窗口，桌面上的其它进程根本收不到，点击等于被本窗吞掉（用户口径："还是不可以点击"）。
    // 只加 WS_EX_TRANSPARENT 也不够：实测点击仍然落在本窗上（它只对同进程窗口生效）。
    // 跨进程真正生效的是"分层窗口 + TRANSPARENT"这套组合，与原版 setMouseTransparent 一致
    const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT | WS_EX_LAYERED);
    // 整窗不额外压一层透明度：压暗是 D2D 自己在画，这里只把"分层"这条件补上
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    // 两条工具条都别进画面。它们就叠在被录的选区旁边，靠 WDA_EXCLUDEFROMCAPTURE
    // 让桌面复制拿到的画面里根本不出现这两个窗口（录制条留在屏幕上给用户点）
    if (toolMain && toolMain->hwnd) App::setCaptureAffinity(toolMain->hwnd, true);
    if (toolSub && toolSub->hwnd) App::setCaptureAffinity(toolSub->hwnd, true);
    // 主条收起来：录制中桌面上只留副条上那条录制条
    if (toolMain) toolMain->hide();
    // 反复被别的窗口抢到前面时，录制条会跟着沉下去；补一次置顶（本窗本来就是 topmost）。
    // FRAMECHANGED 让刚加的扩展样式（分层）立刻生效，而不是等到下一次改窗口尺寸
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    refresh();   // 切到 Video 分支的绘制：清透明 + 只画压暗层
    return true;
}

std::wstring WinCap::stopVideo(bool toClipboard)
{
	// 空路径有两个来源，提示必须分开：一帧都没录到，和"录到了但没拷到目标目录"
	// （磁盘满 / 目录只读 / 被占用，内容还在数据目录里）。以前两种情况都说"没有录到画面"，
	// 用户录了几分钟却读到这句，只会以为白录、也不会去数据目录里找那份临时文件
	bool copyFailed = false;
	auto path = capVideo ? capVideo->stopAndSave(toClipboard, &copyFailed) : L"";
	// 录完马上给一条软件内的轻提示：录制条这一下就要跟着 exitVideo 收掉了，
	// 落盘结果只留在那条上是看不到的
	if (!path.empty()) {
		if (auto app = ui::App::get()) app->showToast(Lang::get(L"cap.savedTip"));
	}
	else if (copyFailed) {
		if (auto app = ui::App::get()) app->showToast(Lang::get(L"cap.saveFail"));
	}
	else {
		if (auto app = ui::App::get()) app->showToast(Lang::get(L"video.noFrames"));
	}
	return path;
}

void WinCap::discardVideo()
{
    if (capVideo) capVideo->discard();
}

// 退出录屏：把宿主恢复到录屏之前（底图/边框回来、鼠标归自己、工具条回来、副条收起）
void WinCap::exitVideo()
{
    if (stage != CapStage::Video) return;
    // 编码线程在 videoSave / videoDiscard 里已经停过了，这里只是把壳收掉
    capVideo.reset();
    stage = CapStage::Adjust;
    videoIsGif = false;
    // 鼠标收回来：穿透那套（TRANSPARENT + 分层）只属于录制那一段
    const LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE,
        exStyle & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT | WS_EX_LAYERED));
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (toolMain && toolMain->hwnd) App::setCaptureAffinity(toolMain->hwnd, false);
    if (toolSub && toolSub->hwnd) App::setCaptureAffinity(toolSub->hwnd, false);
    // 清掉录屏按钮的选中态：curId 空了，副条那条录制条也跟着收起（cancelSelect 内部会重排）
    if (toolMain) toolMain->cancelSelect();
    layoutTools();
    refresh();
}

// 离屏把"选区底图 + 未撤销 shape"合成到一张新位图上再读回像素。
// shape 存的是窗口坐标，导出时整体平移到选区左上角。
// 不直接画到 screenImg 上：它是 ShapeEraser 的"原样"来源、ShapeMosaic 的取样来源。
bool WinCap::grabPixels(std::vector<BYTE>& pixels, D2D1_SIZE_U& size)
{
	if (!screenImg || !cutMask || !cutMask->hasRect()) return false;
	if (!ui::D2D::ready()) return false;
	auto& m = cutMask->maskRect;
	const UINT32 cutW = (UINT32)(m.right - m.left);
	const UINT32 cutH = (UINT32)(m.bottom - m.top);
	if (cutW == 0 || cutH == 0) return false;
	if (editingText) editingText->finishEdit();
	size = D2D1::SizeU(cutW, cutH);
	auto d2d = ui::D2D::get();
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
	hr = S_OK;
	try {
		ctx->Clear(D2D1::ColorF(0, 0.0f));
		auto srcRect = D2D1::RectF((float)m.left, (float)m.top, (float)m.right, (float)m.bottom);
		ctx->DrawBitmap(screenImg.Get(), D2D1::RectF(0, 0, (float)cutW, (float)cutH), 1.0f,
			D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &srcRect);
		hr = ctx->EndDraw();
	}
	catch (...) {
		// 异常穿出 C ABI 就是 terminate；宁可返回失败也不让它跑出去
		hr = E_FAIL;
	}
	ctx->SetTransform(D2D1::Matrix3x2F::Identity());
	ctx->SetTarget(nullptr);
	if (FAILED(hr)) return false;

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
	struct MapGuard {
		ID2D1Bitmap1* bmp;
		~MapGuard() { if (bmp) bmp->Unmap(); }
	} mapGuard{ cpuBmp.Get() };
	const UINT32 rowBytes = size.width * 4;
	pixels.resize((size_t)rowBytes * size.height);
	for (UINT32 row = 0; row < size.height; ++row)
	{
		auto dst = pixels.data() + (size_t)row * rowBytes;
		CopyMemory(dst, mapped.bits + (size_t)row * mapped.pitch, rowBytes);
		for (UINT32 i = 3; i < rowBytes; i += 4) dst[i] = 255;
	}
	return true;
}

bool WinCap::getImagePixels(std::vector<BYTE>& pixels, D2D1_SIZE_U& size)
{
    if (!cutMask || !cutMask->hasRect()) return false;
    auto& m = cutMask->maskRect;
    const UINT32 cutW = (UINT32)(m.right - m.left);
    const UINT32 cutH = (UINT32)(m.bottom - m.top);
    if (cutW == 0 || cutH == 0) return false;
    // 取选区那一块底图、shape 平移到选区左上角。离屏合成本身在 WinHost::compositePixels，
    // 与贴图窗共用一套（target 位图的收尾、Map/Unmap、行紧缩、alpha 补 255 都只写一遍）
    size = D2D1::SizeU(cutW, cutH);
    return compositePixels(size,
        D2D1::RectF((float)m.left, (float)m.top, (float)m.right, (float)m.bottom),
        -(float)m.left, -(float)m.top, pixels);
}
