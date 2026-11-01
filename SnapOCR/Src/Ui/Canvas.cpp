#include "UiPch.h"
#include "Canvas.h"
#include "WinBase.h"
#include "D2D.h"

namespace ui {

    Canvas::Canvas(WinBase* win) : Node(win)
    {
    }

    Canvas::~Canvas()
    {
        if (painting) finishPaint();
        // 位图是 ctx 的当前 target，ctx 不解绑的话 swap chain 的后台缓冲释放不干净
        if (ctx && targetBmp) ctx->SetTarget(nullptr);
    }

    void Canvas::enableSwapChain()
    {
        if (surface || swap) return;   // 已经建过后端了，改不了
        useSwapChain = true;
    }

    bool Canvas::ensureSwapChain(UINT pxW, UINT pxH)
    {
        auto d2d = D2D::get();
        // 设备没建起来（硬件与 WARP 都失败）时 d2dDevice 是空指针：
        // 以前在这里直接 CreateDeviceContext 就是空指针崩溃，且崩在 WM_PAINT 里
        if (!d2d || !d2d->d2dDevice) return false;
        // 设备换代：旧 swap chain / 旧 context / 旧后台缓冲全部作废，重建一套
        if (surfaceGen != D2D::generation()) {
            if (ctx) ctx->SetTarget(nullptr);
            targetBmp.Reset();
            swap.Reset();
            ctx.Reset();
            surface = nullptr;
            surfaceGen = D2D::generation();
        }
        if (!ctx) {
            // 自己一份 context：swap chain 路径要跨帧长期把后台缓冲设为 target，
            // 而 d2d->deviceContext 是全局共享的资源工厂（大家拿它 CreateBrush / CreateBitmap），
            // 让它长期挂着某个窗口的后台缓冲不合适，多个 Canvas 也会互相抢 target。
            // 同一个 device 上开多个 context 是允许的，画刷也能跨 context 复用。
            if (FAILED(d2d->d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, ctx.ReleaseAndGetAddressOf()))) return false;
        }
        if (swap) {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            swap->GetDesc1(&desc);
            if (desc.Width == pxW && desc.Height == pxH) return true;
            // 换尺寸：先撤掉所有对后台缓冲的引用，ResizeBuffers 才会成功
            ctx->SetTarget(nullptr);
            targetBmp.Reset();
            if (FAILED(swap->ResizeBuffers(0, pxW, pxH, DXGI_FORMAT_UNKNOWN, 0))) return false;
        }
        else {
            swap = d2d->createSwapChain(pxW, pxH);
            if (!swap) return false;
            auto s = d2d->createSurfaceForSwapChain(win->compositor, swap.Get());
            if (!s) { swap.Reset(); return false; }
            auto brush = win->compositor.CreateSurfaceBrush(s);
            brush.Stretch(winrt::Windows::UI::Composition::CompositionStretch::None);
            visual.Brush(brush);
        }
        // 把后台缓冲包成 D2D 位图当绘制目标。CANNOT_DRAW 表示它只能写不能当 source 读，
        // 这正是 swap chain 后台缓冲的语义。
        ComPtr<IDXGISurface> back;
        if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf())))) return false;
        D2D1_BITMAP_PROPERTIES1 bp{
            .pixelFormat{ D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED) },
            .dpiX{ 96.f }, .dpiY{ 96.f },
            .bitmapOptions{ D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW }
        };
        if (FAILED(ctx->CreateBitmapFromDxgiSurface(back.Get(), &bp, targetBmp.ReleaseAndGetAddressOf()))) return false;
        ctx->SetTarget(targetBmp.Get());
        return true;
    }

    ID2D1DeviceContext* Canvas::startPaint()
    {
        if (painting) return nullptr;
        const int pxW = static_cast<int>(w);
        const int pxH = static_cast<int>(h);
        if (pxW <= 0 || pxH <= 0) return nullptr;
        // 设备换代：缓存的表面绑在上一代设备上，直接丢掉重建（swap chain 那条路
        // 在 ensureSwapChain 里自己处理）
        if (surface && surfaceGen != D2D::generation()) surface = nullptr;
        if (useSwapChain) {
            if (!ensureSwapChain((UINT)pxW, (UINT)pxH)) return nullptr;
            ctx->BeginDraw();
            // 后台缓冲的原点就是画布原点，不像 DrawingSurface 那样要偏移到 tile 里
            ctx->SetTransform(D2D1::Matrix3x2F::Identity());
            painting = true;
            return ctx.Get();
        }
        if (!surface) {
            auto d2d = D2D::get();
            if (!d2d) return nullptr;
            surface = d2d->createDrawingSurface(win->compositor, (float)pxW, (float)pxH);
            // 建表面失败（设备不可用 / 显存不够）时不能往下走：surface 为空，
            // 下面一句 surface.as<>() 会抛异常，而这条路径在 WM_PAINT 里，没人接
            if (!surface) return nullptr;
            surfaceGen = D2D::generation();
            auto brush = win->compositor.CreateSurfaceBrush(surface);
            brush.Stretch(winrt::Windows::UI::Composition::CompositionStretch::None);
            visual.Brush(brush);
        }
        else {
            auto sz = surface.SizeInt32();
            if (sz.Width != pxW || sz.Height != pxH) {
                surface.Resize({ pxW, pxH });
            }
        }
        auto s = surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
        POINT offset{};
        HRESULT hr = s->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(ctx.ReleaseAndGetAddressOf()), &offset);
        if (FAILED(hr)) return nullptr;
        ctx->SetTransform(D2D1::Matrix3x2F::Translation((float)offset.x, (float)offset.y));
        painting = true;
        return ctx.Get();
    }

    void Canvas::finishPaint()
    {
        if (!painting) return;
        painting = false;
        if (useSwapChain) {
            ctx->EndDraw();
            // Present 之后合成器读的才是这一帧。
            //
            // 这里曾经是 Present(0, DXGI_PRESENT_DO_NOT_WAIT)：后台缓冲还被合成器占着时
            // 立刻返回、把这一帧丢掉。丢的不是"半帧"，是整块画布的一整帧 —— 全屏画布上
            // 画的每一样东西都停在旧内容上，直到下一次 refresh 才补。
            // 后果最明显的就是文字编辑：文字画在 TextBox 自己的小画布上（那块缓冲小、
            // 几乎不会撞车），虚线框画在这块全屏画布上。于是"打一个字，字变了、虚线框
            // 还是旧的，再打一个字才跳一下" —— 就是用户说的慢半拍/不跟手。
            // 全屏窗口还会被 DWM 提升到硬件覆盖平面，可用缓冲更紧，撞车概率比普通窗口高。
            //
            // 改成 SyncInterval=0：不锁垂直同步（Present(1,0) 才会每帧白等 16ms，
            // 那才是"手先到、框后到"的来源），但会等到有一个空闲后台缓冲再交换，
            // 于是帧不会被丢掉。缓冲数是 2（见 D2D::createSwapChain）：正常情况下
            // 这个等待是 0，只有合成器恰好占着上一帧时才会等它交还（≤ 一帧）
            if (swap->Present(0, 0) == DXGI_ERROR_WAS_STILL_DRAWING) {
                // 极端情况（合成器迟迟不交还缓冲）才会走到这里：不丢帧，改为补一帧
                win->refresh();
            }
            // 注意：ctx / targetBmp 保留不释放，下一帧继续用同一条 swap chain
            return;
        }
        auto s = surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
        s->EndDraw();
        ctx.Reset();
    }
}
