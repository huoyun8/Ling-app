#include "UiPch.h"
#include <Windows.h>
#include <winuser.h>
#include "UiUtil.h"
#include "D2D.h"
#include "UiApp.h"
#include <objbase.h>

namespace ui {
    static std::unique_ptr<App> app;


    // appID 既是任务栏分组 / AppUserModelID，也是单实例检测那只 message-only 窗口的标题。
    // 两版必须不同 —— 相同的话 SnapCore 在跑时双击 SnapOCR 会被当成"第二个实例"直接退出
    App::App() :dq{ winrt::Windows::System::DispatcherQueue::GetForCurrentThread() }, appID{ SNAP_BRAND_W }
    {
        SetCurrentProcessExplicitAppUserModelID(appID.data());
    }

    App::~App()
    {
        // 轻提示窗口没有 owner，进程退出前自己收掉，别留一个孤儿窗口在屏幕上
        if (toastHwnd && IsWindow(toastHwnd)) DestroyWindow(toastHwnd);
        disposeTray();
    }

    App* App::get()
    {
        return app.get();
    }

    void App::dispose()
    {
        app.reset();
    }

    void App::quit(int code)
    {
        onBeforeQuit();
        PostQuitMessage(code);
    }

    void App::exit(int code)
    {
        ExitProcess(code);
    }

    bool App::refuseSecondInstance(bool enforce)
    {
        // 设置页关了"只允许单实例"：不抢互斥体、不转交，允许真正多开。
        // 托盘图标、全局热键仍挂在 message-only 窗口上，这里照样得建
        if (!enforce) {
            initMsgWin();
            return false;
        }
        // 找的是 initMsgWin() 建的那个窗口 —— 它挂在 HWND_MESSAGE 下，属于
        // message-only window。FindWindow 只在桌面窗口的子孙里搜，根本不看
        // message-only 这一族，所以必须用 FindWindowExW 并显式指定 HWND_MESSAGE 当父窗口。
        //
        // 光靠这个查找还挡不住同时启动：用户双击图标时系统常在几十毫秒内连开两个
        // 进程（冷启动尤其明显），两个都查不到窗口，于是各建一个 message-only 窗口、
        // 各挂一套托盘图标和全局热键 —— 热键那边后注册的会失败，托盘却会真的多出
        // 一个图标。所以先拿命名互斥体：CreateMutex 是原子的，只有第一个能拿到
        // "新建"的语义
        const std::wstring mutexName = L"Local\\" + appID + L"_singleton";
        instanceMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        const DWORD lastErr = instanceMutex ? GetLastError() : ERROR_SUCCESS;

        auto notifyRunning = [](HWND target) {
            // 把"可以抢前台"的授权转给已经在跑的那个：本进程是用户刚启动的，
            // 持有前台授权，转让之后那边 SetForegroundWindow 才不是只闪一下任务栏
            DWORD pid{ 0 };
            GetWindowThreadProcessId(target, &pid);
            if (pid) AllowSetForegroundWindow(pid);
            // 通知已经在跑的那个实例（它收到 WM_APP+1 会把自己亮出来），然后本进程退出。
            // 消息进的是对方的队列，这里马上 ExitProcess 不影响投递
            PostMessageW(target, WM_APP + 1, 0, 0);
        };

        HWND hwnd = FindWindowExW(HWND_MESSAGE, nullptr, L"STATIC", appID.c_str());
        if (hwnd) {
            notifyRunning(hwnd);
            App::exit(0);
            return true;
        }
        // 互斥体已经存在但窗口还没建出来：前一个实例正好走在"拿到互斥体 → 建窗口"
        // 这一段里。等它把窗口建出来再通知，别让用户这一次双击什么反应都没有
        if (lastErr == ERROR_ALREADY_EXISTS) {
            for (int i = 0; i < 20; i++) {
                Sleep(50);
                hwnd = FindWindowExW(HWND_MESSAGE, nullptr, L"STATIC", appID.c_str());
                if (!hwnd) continue;
                notifyRunning(hwnd);
                App::exit(0);
                return true;
            }
            // 等满一秒还没有窗口：前一个实例大概卡在建窗之前，或者已经挂了。
            // 照样退出 —— 宁可这一次双击没反应，也不要两个实例并存
            App::exit(0);
            return true;
        }
        initMsgWin();
        return false;
    }

    bool App::regHotKey(const std::wstring& keyStr, const UINT msgId)
    {
        std::wstring lowerName = keyStr;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);
        auto arr = Util::splitStr(lowerName, L'+');
        UINT modifiers = 0;
        UINT keyCode{ 0 };
        for (auto& key : arr)
        {
            if (key == L"ctrl") {
                modifiers |= MOD_CONTROL;
            }
            else if (key == L"alt") {
                modifiers |= MOD_ALT;
            }
            else if (key == L"shift") {
                modifiers |= MOD_SHIFT;
            }
            else if (key == L"win" || key == L"lwin" || key == L"rwin") {
                modifiers |= MOD_WIN;
            }
            else {
                keyCode = Util::strToKey(key);
            }
        }
        //键名认不出来（配置文件被手工改坏了之类）：注册不了，交给调用方处理
        if (keyCode == 0) return false;
        initMsgWin();
        // 失败的常见原因是 ERROR_HOTKEY_ALREADY_REGISTERED：这个组合已经被别的程序占了
        return RegisterHotKey(msgHwnd, WM_APP + msgId, modifiers, keyCode) != FALSE;
    }

    void App::unRegHotKey(const UINT msgId)
    {
        UnregisterHotKey(msgHwnd, WM_APP + msgId);
    }

    void App::initTray(const UINT msgId, const std::wstring& tip)
    {
        if (tray.get()) return;
        initMsgWin();
        // 消息窗口建不出来（msgHwnd 为空）时，Shell_NotifyIcon 只会拿着空句柄失败，
        // 与其留一个"以为有图标、其实没有"的状态，不如干脆不建：调用方靠 hasTray()
        // 就能知道没图标，关窗时直接退进程，不会把窗口藏起来
        if (!msgHwnd) return;
        tray = std::make_unique<NOTIFYICONDATA>();
        ZeroMemory(tray.get(), sizeof(NOTIFYICONDATA));
        tray->cbSize = sizeof(NOTIFYICONDATA);
        tray->hWnd = msgHwnd; 
        tray->uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        trayMsgId = WM_APP + msgId;
        tray->uCallbackMessage = trayMsgId;
        tray->hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(1));
        if (!tip.empty()) {
            wcscpy_s(tray->szTip, tip.data());
        }        
        // 结果是布尔：False 表示这次真没挂上（explorer 正在重启、任务栏没就绪）。
        // 记下来给 hasTray() 用 —— 以前这里不看返回值，图标没挂上也当挂上了
        trayOk = Shell_NotifyIcon(NIM_ADD, tray.get()) != FALSE;
        // 失败也留着这份 NOTIFYICONDATA：explorer 起来了会广播 TaskbarCreated，
        // 到那时还能拿它重挂一次（见 winProc）。现在就把数据丢掉，图标就再也回不来了
    }

    void App::disposeTray()
    {
        if (tray.get()) {
            if (trayOk) Shell_NotifyIcon(NIM_DELETE, tray.get());
            tray.reset();
        }
        trayOk = false;
    }

    // ———————————————— 软件内轻提示 ————————————————
    // 一块半透明深色小条，贴在光标所在显示器的下方中间，1.8 秒后自己消失。
    // 观感与二维码页那条轻提示一致（深蓝灰底 + 白字），但那条归 WinQr 的窗口所有，
    // 截图/录屏这两处是"窗口马上要关掉"的收尾，提示必须比主窗口活得久，所以放在 App 上
    namespace {
        constexpr wchar_t kToastClass[] = SNAP_CLS_TOASTX;
        constexpr COLORREF kToastBg = RGB(45, 57, 74);
        constexpr UINT kToastMs = 1800;
        constexpr BYTE kToastAlpha = 200;          // 约 78% 不透明：看得出底下的内容，字仍然清楚
        constexpr int kToastFontPx = 18;           // 96dpi 下的字高，与 WinQr 的正文同级
        constexpr int kToastPadX = 22;             // 左右内边距
        constexpr int kToastH = 44;                // 整条高度
        constexpr int kToastBottom = 72;           // 离工作区底边的高度（任务栏之上）
        constexpr int kToastMinW = 140;

        HFONT toastFont(UINT dpi)
        {
            LOGFONTW lf{};
            lf.lfHeight = -MulDiv(kToastFontPx, (int)dpi, 96);
            lf.lfWeight = FW_NORMAL;
            lf.lfCharSet = DEFAULT_CHARSET;
            wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
            return CreateFontIndirectW(&lf);
        }
    }

    void App::showToast(const std::wstring& text)
    {
        if (text.empty()) return;
        static bool clsReged = false;
        if (!clsReged) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = toastProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kToastClass;
            RegisterClassExW(&wc);
            clsReged = true;
        }
        if (!toastHwnd || !IsWindow(toastHwnd)) {
            // 不激活、不进任务栏、不吃鼠标（TRANSPARENT）：它只是一句话，
            // 用户正在做的事不能被它打断
            toastHwnd = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                kToastClass, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            if (!toastHwnd) return;
            // 整窗一个透明度。分层窗口 + LWA_ALPHA 下 GDI 画的内容照样显示，
            // 所以这里不需要搭一套 D2D/UpdateLayeredWindow，几行就够
            SetLayeredWindowAttributes(toastHwnd, 0, kToastAlpha, LWA_ALPHA);
        }
        SetWindowTextW(toastHwnd, text.c_str());

        UINT dpi = GetDpiForWindow(toastHwnd);
        if (!dpi) dpi = 96;   // 窗口还没落到任何显示器上时它会返回 0
        // 按文字实测宽度定尺寸：长文件名与短句都能一眼读完
        SIZE ts{};
        HDC dc = GetDC(toastHwnd);
        if (dc) {
            HFONT f = toastFont(dpi);
            HGDIOBJ old = SelectObject(dc, f);
            GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &ts);
            SelectObject(dc, old);
            DeleteObject(f);
            ReleaseDC(toastHwnd, dc);
        }
        POINT pt{};
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{ sizeof(MONITORINFO) };
        RECT wa{};
        if (mon && GetMonitorInfoW(mon, &mi)) wa = mi.rcWork;
        else {
            wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        }
        const int padX = MulDiv(kToastPadX, (int)dpi, 96);
        const int wMin = MulDiv(kToastMinW, (int)dpi, 96);
        const int wMax = (wa.right - wa.left) * 7 / 10;
        int w = ts.cx + padX * 2;
        if (w < wMin) w = wMin;
        if (w > wMax) w = wMax;
        const int h = MulDiv(kToastH, (int)dpi, 96);
        const int x = wa.left + ((wa.right - wa.left) - w) / 2;
        const int y = wa.bottom - h - MulDiv(kToastBottom, (int)dpi, 96);

        SetWindowPos(toastHwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(toastHwnd, nullptr, TRUE);
        // 同一个 id 再 SetTimer 就是重新计时：连存两张图，提示不会叠成两条
        SetTimer(toastHwnd, 1, kToastMs, nullptr);
    }

    LRESULT CALLBACK App::toastProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            if (dc) {
                RECT rc{};
                GetClientRect(h, &rc);
                HBRUSH bg = CreateSolidBrush(kToastBg);
                FillRect(dc, &rc, bg);
                DeleteObject(bg);
                wchar_t buf[512]{};
                GetWindowTextW(h, buf, 512);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(255, 255, 255));
                UINT dpi = GetDpiForWindow(h);
                if (!dpi) dpi = 96;
                HFONT f = toastFont(dpi);
                HGDIOBJ old = SelectObject(dc, f);
                const int padX = MulDiv(kToastPadX, (int)dpi, 96);
                RECT tr{ padX, 0, rc.right - padX, rc.bottom };
                DrawTextW(dc, buf, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(dc, old);
                DeleteObject(f);
                EndPaint(h, &ps);
            }
            return 0;
        }

        case WM_TIMER:
            // SetTimer 挂在提示窗口自己身上，销毁它顺带就把计时器收了
            DestroyWindow(h);
            return 0;

        case WM_NCDESTROY:
            if (auto a = App::get()) {
                if (a->toastHwnd == h) a->toastHwnd = nullptr;
            }
            break;

        default:
            break;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    void App::initArgs()
    {
        LPWSTR* argv;
        int argc{ 0 };
        LPWSTR cmdLine = GetCommandLine();
        argv = CommandLineToArgvW(cmdLine, &argc);
        // 返回空时 argc 不会被赋值，按它循环就是拿未初始化的值做下标
        if (!argv) return;
        for (int i = 1; i < argc; ++i) {
            std::wstring arg{ argv[i] };
            auto index = arg.find(L"=");
            if (index != std::wstring::npos) {
                args[arg.substr(0, index)] = arg.substr(index + 1);
            }
            else {
                args.insert({ arg,L"true" });
            }
        }
        LocalFree(argv);
    }

    UINT App::popupMenu(HMENU menu)
    {
        POINT pt;
        GetCursorPos(&pt);
        // 参考 MS KB 135788：托盘弹菜单前必须把 owner 拉到前台, 菜单才能收到"外部点击"
        // 和"失焦"事件从而自动关闭；否则会一直挂着。菜单结束后再补一个 WM_NULL, 兜住
        // 部分 shell 版本的兼容问题。
        SetForegroundWindow(msgHwnd);
        UINT selectedCmd = TrackPopupMenuEx(menu,TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_RETURNCMD,pt.x,pt.y,msgHwnd,nullptr);
        PostMessage(msgHwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
        return selectedCmd;
    }

    void App::init()
    {
        App::initDispatcherQueueCtrl();
        app.reset(new App());
    }

    void App::initMsgWin()
    {
        if (msgHwnd) return;
        msgHwnd = CreateWindow(L"STATIC", appID.data(), 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetInstanceModule(NULL), NULL);
        // 建不出来就保持为空，绝不拿一个来路不明的句柄往下用：
        // 托盘图标、全局热键、单实例检测全部挂在这个窗口上，句柄错一个，三样一起失效
        if (!msgHwnd) return;
        SetWindowLongPtr(msgHwnd, GWLP_WNDPROC, (LONG_PTR)App::winProc);
        SetWindowLongPtr(msgHwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        // 本程序常用于开机自启 / 截取管理员窗口，会被提权运行。这时普通权限的第二实例
        // 发来的 WM_APP+1 会被 UIPI 直接挡掉 —— 用户双击图标，进程起来了又退了，
        // 旧实例什么都不知道，看起来就是"双击毫无反应"。单独放行这一条消息，
        // 其它消息照旧受完整性级别约束
        ChangeWindowMessageFilterEx(msgHwnd, WM_APP + 1, MSGFLT_ALLOW, nullptr);
        // explorer.exe 重启后会广播这条消息：系统不会告诉你"你的图标被丢了"，
        // 不自己重新挂，托盘图标就永远回不来（进程还在、入口没了）
        taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    }

    LRESULT App::winProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto self = reinterpret_cast<App*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (!self) {
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        else if (msg == WM_APP + 1)
        {
            self->onSecondInstance();
            return 0;
        }
        else if (self->taskbarCreatedMsg && msg == self->taskbarCreatedMsg)
        {
            // 任务栏重建：把图标挂回去，顺便把"图标在不在"重新判定一次，
            // 关窗行为（收托盘 / 真退出）跟着这个状态走
            if (self->tray.get()) {
                self->trayOk = Shell_NotifyIcon(NIM_ADD, self->tray.get()) != FALSE;
            }
            return 0;
        }
        else if (msg == WM_HOTKEY) {
            self->onHotKey((UINT)wParam - WM_APP);
            return 0;
        }
        else if (msg == self->trayMsgId)
        {
            if (lParam == WM_LBUTTONDOWN) {
                self->onTrayMouseEvent(true,false);
            }
            else if (lParam == WM_RBUTTONDOWN)
            {
                self->onTrayMouseEvent(true, true);
            }
            else if (lParam == WM_LBUTTONUP)
            {
                self->onTrayMouseEvent(false, false);
            }
            else if (lParam == WM_RBUTTONUP)
            {
                self->onTrayMouseEvent(false, true);
            }
        }
        else if (msg == WM_TIMER && wParam == kIdleD2DDisposeTimer)
        {
            // 空转到点：先撤定时器。窗口都关干净了才真拆设备 —— 这 20 秒里用户可能又点了
            // 截图，那时 windows 非空，什么都不能动（设备正被那张窗口用着）
            KillTimer(hwnd, kIdleD2DDisposeTimer);
            if (self->windows.empty()) D2D::dispose();
            return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    void App::initDispatcherQueueCtrl()
    {
        DispatcherQueueOptions options{ sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE };
        static winrt::Windows::System::DispatcherQueueController controller{ nullptr };
        auto hr = CreateDispatcherQueueController(options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller)));
        if (FAILED(hr))
        {
            MessageBox(NULL, L"无法创建DispatcherQueueController", L"系统提示", MB_OK);
            ExitProcess(-1);
        }
    }
}

// 进程级初始化/收尾（原先单独在 Ui.cpp，与 App 一起放，少一个文件）
namespace ui {

	bool init() {
		HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		if (FAILED(hr)) return false;
		App::init();
		return true;
	}
	void dispose() {
		D2D::dispose(); //正常情况下最后一个窗口关掉时就没了，这里兜住"还开着窗口就退出"那条路
		App::dispose(); //顺序要紧：先放掉持有 WinRT/COM 对象的单例，再拆套间
		CoUninitialize();
	}
}
