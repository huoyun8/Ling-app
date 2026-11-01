#include "pch.h"
#include "App.h"
#include "Setting.h"
#include "Tray.h"
#include "Lang.h"
#include "./Win/WinCap.h"
#include "./Win/WinPin.h"
#include "./Win/WinQr.h"
#include "./Win/ZipKill.h"


std::unique_ptr<App> app;
// 构造期间就可用的"活单例"指针。
// 为什么不能只靠 app（unique_ptr）：App 的构造函数里就会开窗口（--enter / --auto-quit
// 直接进截图窗），而截图窗要回调 App::get()->takeScreenShot() —— 那时 app 还没轮到
// reset(ptr)，get() 拿到空指针，第一块成员（shotBuf）一写就是访问冲突。
// 所以构造一开始就把 this 挂到这里，App::get() 认它；析构时摘掉。
static App* g_liveApp{ nullptr };



App::~App()
{
    if (g_liveApp == this) g_liveApp = nullptr;
}

void App::init()
{
    app.reset(new App());
}

void App::dispose()
{
    // 先停后台看护线程（会 join），再拆窗口：否则线程可能在窗口析构后还去 PostMessage
    ZipKill::stopGuard();
    // 窗口对象是文件级静态变量，交给静态析构就晚了（那时 CoUninitialize 已经跑完），
    // 所以趁这里把还开着的窗口先放掉
    WinPin::dispose();
    WinCap::dispose();
    WinQr::dispose();
    Lang::dispose();
    Setting::dispose();
    app.reset();
}

App* App::get()
{
    // g_liveApp 在构造一开始就挂上，构造期间（开窗口、抓屏）同样有效
    return g_liveApp;
}

void App::takeScreenShot(int x, int y, int w, int h, ID2D1Bitmap1** img)
{
    if (!img || w <= 0 || h <= 0) return;
    *img = nullptr;
    HDC hScreen = GetDC(NULL);
    if (!hScreen) return;
    HDC hDC = CreateCompatibleDC(hScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, w, h);
    if (!hDC || !hBitmap) {
        // 显示器正在切换分辨率 / 会话锁定等情况下这几个 GDI 对象会拿不到，
        // 不拦一下下面的 SelectObject / GetDIBits 会一路带着 NULL 往下走
        if (hBitmap) DeleteObject(hBitmap);
        if (hDC) DeleteDC(hDC);
        ReleaseDC(NULL, hScreen);
        return;
    }
    auto oldObj = SelectObject(hDC, hBitmap);
    // BitBlt 失败时 hBitmap 里还是 CreateCompatibleBitmap 给的未初始化像素：
    // 不拦的话用户截到的是一整片花屏/黑块，还以为是自己屏幕的问题
    if (!BitBlt(hDC, 0, 0, w, h, hScreen, x, y, SRCCOPY)) {
        SelectObject(hDC, oldObj);
        DeleteDC(hDC);
        DeleteObject(hBitmap);
        ReleaseDC(NULL, hScreen);
        return;
    }
    ReleaseDC(NULL, hScreen);
    // GetDIBits 的前提是位图没有被选进任何 DC（MSDN 明说，违者在部分驱动上
    // 会返回错乱的行）。BitBlt 已经完成，先把位图摘下来再读像素
    SelectObject(hDC, oldObj);
    // 复用缓冲区：容量够时 resize 不分配、不重新页错误。第一次之后这条路径
    // 只剩 BitBlt + GetDIBits 的纯拷贝，砍掉每次十几毫秒的分配开销
    shotBuf.resize((size_t)w * 4 * h);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    auto lines = GetDIBits(hDC, hBitmap, 0, h, shotBuf.data(), &bmi, DIB_RGB_COLORS);
    DeleteDC(hDC);
    DeleteObject(hBitmap);
    if (lines == 0) return;
    D2D1_BITMAP_PROPERTIES1 props = {
       .pixelFormat{D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)},
       .dpiX{96.0f}, .dpiY{96.0f}, .bitmapOptions{D2D1_BITMAP_OPTIONS_NONE}
    };
    auto d2d = ui::D2D::get();
    // 建失败时 *img 保持为 nullptr，调用方（WinCap::onCreated）会因此跳过整帧的绘制。
    // 设备本身不可用时 deviceContext 是空的，这里必须先判 —— 否则一按热键就崩
    if (!ui::D2D::ready()) return;
    d2d->deviceContext->CreateBitmap(D2D1::SizeU(w, h), shotBuf.data(), w * 4, props, img);
}

std::tuple<int, int, int, int> App::getScreenArea()
{
	return std::make_tuple(GetSystemMetrics(SM_XVIRTUALSCREEN), 
        GetSystemMetrics(SM_YVIRTUALSCREEN), 
        GetSystemMetrics(SM_CXVIRTUALSCREEN), 
        GetSystemMetrics(SM_CYVIRTUALSCREEN));
}

// 这个系统够不够新，能不能用 SetWindowDisplayAffinity 的"排除捕获"标记。
// 老系统上这个调用不但不失败，还会把窗口变成捕获画面里的一整块黑（实测 build 18363：
// 返回 TRUE，读回来的 affinity 就是 0x11 —— 内核照存，可那会儿的 DWM 只认"非零即
// 受保护内容"，一律涂黑）。所以必须自己拦住，让老系统退回"照旧被录进去"。
// GetVersionEx 会被兼容性清单骗，只有 RtlGetVersion 给的是真版本号
static bool captureAffinitySupported()
{
    static const bool supported = []() {
        OSVERSIONINFOW vi{ sizeof(vi) };
        auto rtlGetVersion = (LONG(WINAPI*)(OSVERSIONINFOW*))GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        return rtlGetVersion && rtlGetVersion(&vi) == 0 && vi.dwBuildNumber >= 19041;
    }();
    return supported;
}

// 切换窗口的"捕获亲和性"：exclude=true 把窗口从屏幕捕获里摘出去（它在屏幕上照常显示、
// 照常能点，但 BitBlt 抓屏 / DXGI 桌面复制都拿不到它），false 恢复成正常可被捕获。
// 返回是否真的生效。老系统直接返回 false，调用方据此走降级路径（如 SW_HIDE）
bool App::setCaptureAffinity(HWND hwnd, bool exclude)
{
    if (!hwnd || !captureAffinitySupported()) return false;
    return SetWindowDisplayAffinity(hwnd, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE) != FALSE;
}

void App::releaseShotBuffer()
{
    shotBuf.clear();
    shotBuf.shrink_to_fit();
}

// (SIZE_T)-1 让系统"能放多少放多少"：把不常碰的页换出去，活动工作集立刻掉下来。
// 私有提交量不变，但工具闲置时用户看到的内存就是这一项
void App::trimMemory()
{
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

App::App()
{
    g_liveApp = this;   // 先挂上：本构造函数里就会开窗口，窗口会回调 App::get()
    // 套间初始化失败（例如进程里已被别处按 MTA 初始化过）时，后面所有 WinRT/COM
    // 调用都会抛异常，而这里是从 wWinMain 直接进来的，没人接
    if (!ui::init()) {
        MessageBox(nullptr, L"无法初始化 COM 套间", L"系统提示", MB_OK);
        ExitProcess(-1);
    }
    // 名字别用 app：外层有个全局的 app 变量，遮蔽之后稍不注意就改错了对象
    auto ling = ui::App::get();
    ling->initArgs();
    // 设置先起来：下面的防多开要看 config.json 里的 common.singleInstance，
    // 设置页把它关掉之后就允许同时开多个（不抢互斥体、也不做转交）
    Setting::init();
    // 全局防多开：不管带不带命令行参数，第二个实例都把请求转交给已经在跑的那个
    // 然后自己退出（ui::App 内部发 WM_APP+1 给旧实例，旧实例在 Setting 里接）。
    // 早先这一层只做在托盘路径上，结果带 --auto-quit / --enter=xx 进来时能同时
    // 开出好几个铺满全屏的截图窗口，它们互相抢鼠标。
    // 开关在这：设置页把「只允许单实例」关掉（common.singleInstance=false）就
    // 真正允许多开 —— 之前这里是无条件调用，开关只是个摆设
    ling->refuseSecondInstance(Setting::get()->getSingleInstance());
    // 第二个实例退出前会给这里发 WM_APP+1：把主界面亮出来。
    // 这里做的是"切换"而不是"只显示"：第一次打开落托盘、第二次双击亮界面、
    // 第三次再双击又收回去 —— 用户要的就是这么一下一下地来回切。
    // toggleMainVisible 在没有窗口时会走 showMain（现建一个），所以首次也接得上。
    // 唯一例外是正在截图：那阵子把主界面推出来会把遮罩挤到后面、焦点也乱
    ling->onSecondInstance.add([] {
        if (WinCap::hasInstance()) return;
        // 双击桌面图标往往会在几百毫秒内连开两个进程（系统把这当成两次启动），
        // 两次请求不去重的话界面就是"亮一下又收回去"。这里 450ms 内的重复只算一次
        static ULONGLONG lastMs{ 0 };
        const ULONGLONG now = GetTickCount64();
        if (now - lastMs < 450) return;
        lastMs = now;
        WinQr::toggleMainVisible();
    });
    Lang::init();
    // 后台「压缩包防多开」：只处理"同一个压缩包被开出多个窗口"，留最新打开的那个。
    // 设置页有开关（config.json 的 common.zipGuard），关掉就能正常多开同一个包。
    // 放在这里而不是主界面里：它以托盘模式跑（--auto-start / --enter=tray）时也要生效
    if (Setting::get()->getZipGuard()) ZipKill::startGuard();
    // 启动方式：
    //   --auto-quit=true    框选截图，用完即走，截完进程自己退出
    //   --enter=pin        框选完直奔标注
    //   --enter=tray        只挂托盘图标（升级后重启新版本走这条），不弹窗
    // 什么都不带（双击 exe）：先挂托盘，要不要立刻出主界面按设置页的开关决定（见下）
    auto& args = ling->args;
    const bool autoQuit = (args.count(L"--auto-quit") && args[L"--auto-quit"] == L"true");
    const bool enterShot = (args.count(L"--enter") && args[L"--enter"] != L"tray");
    if (autoQuit || enterShot) {
        // 建窗失败（CreateWindowEx 失败 / onCreated 里 D2D、组合器抛异常）时拿不到窗口：
        // 这条路既没有托盘也没有主界面，进程会变成一个看不见的常驻进程，
        // 只能去任务管理器里杀 —— 与下面托盘路径"图标没挂上就退"同一个道理，直接退
        if (!WinCap::init()) ling->quit(0);
    }
    else {
        Tray::init();
        // 只跑托盘的模式（--auto-start / --enter=tray）连主窗口都不建：托盘图标要是
        // 没挂上，这个进程就既看不见也点不到，只剩任务管理器里一行——直接退，
        // 让下一次启动（开机自启）重新挂一次，比留一个不可达的常驻进程强
        if (!ling->hasTray()) {
            ling->quit(0);
            return;
        }
        // 默认启动（双击桌面图标、不带参数）要不要出主界面，交给设置页的
        // 「启动时显示主界面」开关（默认开，配置键 common.showMainOnStart）。
        // 关掉就是老手感：打开只落托盘，再双击一次图标才出主界面 —— 第二实例把请求
        // 转过来（onSecondInstance），我们在这边把界面亮出来。
        // --auto-start / --enter=tray 是明确要求只挂托盘的模式（开机自启、升级后重启），
        // 不看这个开关，免得开机时把界面糊到屏幕上
        const bool explicitTray = args.count(L"--auto-start") || args.count(L"--enter");
        if (!explicitTray && Setting::get()->getShowMainOnStart()) WinQr::showMain();
    }
}
