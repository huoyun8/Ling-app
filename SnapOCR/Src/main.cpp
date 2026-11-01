#include "pch.h"
#include "App.h"
#include "Win/WinQr.h"

namespace {
    // 进程级 DPI 感知：必须在任何窗口创建之前设，晚一步系统就已经按虚拟化缩放画过了。
    // 参考的做法是 Per-Monitor V2（Win10 1703+），失败退回 System Aware。
    // 不设的后果不是"稍微模糊"：125%/150% 缩放下整个界面会被位图拉伸，
    // 我们那套按 DPI 换算的像素坐标全都白算
    void enableDpiAwareness()
    {
        auto user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
            auto setCtx = reinterpret_cast<SetCtxFn>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
            if (setCtx && setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
        }
        SetProcessDPIAware();
    }
}

// 入口不需要命令行参数（App 自己从 GetCommandLineW 解析），也不碰 nCmdShow ——
// 主界面是否显示由 App 按 --enter / --auto-start 决定
int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPTSTR, _In_ int)
{
    enableDpiAwareness();
    App::init();
    // GetMessage 返回 -1 表示出错（比如消息钩子拒收），把它当成功会拿着未初始化的
    // MSG 继续 TranslateMessage/DispatchMessage
    MSG msg{};
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        // 设置页的快捷键显示框是只读 EDIT，按键回不到主窗口，只能在这一层拦
        // （焦点在显示框上按 Delete 就清空那一行）。已处理的不要再派发
        if (WinQr::handleAppKey(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    App::dispose(); //趁 COM 还活着把单例拆掉，别留给 CoUninitialize 之后的静态析构
    ui::dispose();
    return 0;
}
