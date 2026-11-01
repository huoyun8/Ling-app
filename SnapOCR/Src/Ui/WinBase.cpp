#include "UiPch.h"
#include "WinBase.h"
#include "Node.h"
#include "UiApp.h"
#include "D2D.h"

namespace ui {
	WinBase::WinBase() :compositor{ Composition::Compositor() }
	{
		App::get()->windows.push_back(this);
		dpi = static_cast<float>(GetDpiForSystem()) / 96.f;
	}

	// 进程里一个窗口都不剩时，图形设备没有任何用处 —— 连同显卡驱动在本进程里的那份分配一起
	// 还给系统。下次建窗口时 D2D::get() 会自动重建，代价是第一帧多几十毫秒
	WinBase::~WinBase()
	{
		winTarget = nullptr;
		body.reset();
		auto app = App::get();
		if (!app) return; //进程退出后的静态析构：单例和 COM 套间都没了，别再往上打电话
		std::erase(app->windows, this);
		// 不在"最后一个窗口关闭"时立刻销毁 D3D/D2D 设备。原做法的问题：截图/贴图窗口
		// 一关、应用回到托盘待命的那个瞬间，要在 UI 线程上同步拆掉整套图形设备 ——
		// 某些显卡驱动（尤其设备丢失/驱动重置后）这一步会阻塞数秒甚至卡死，表现恰好是
		// "托盘假死、全局热键失灵"（热键和托盘都靠这条 UI 线程的消息循环）。
		// 现在改成延迟释放：一个窗口都不剩时挂一个 20 秒的定时器，到点还没窗口才拆设备。
		// 空转时拆不会挡住任何操作（用户此刻就在等），拆完那一整套 D3D/DXGI/D2D/DWrite
		// 连同驱动侧分配一起还给系统，实测能落下二三十 MB
		if (app->windows.empty() && app->msgWnd()) {
			SetTimer(app->msgWnd(), kIdleD2DDisposeTimer, kIdleD2DDisposeMs, nullptr);
		}
	}

	void WinBase::disableWinAnimation()
	{
		BOOL attrib = TRUE;
		DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &attrib, sizeof(attrib));
	}

	void WinBase::disableBorderRadius()
	{
		DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_DONOTROUND;
		DwmSetWindowAttribute( hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
	}

	void WinBase::show()
	{
		if (!alive()) return;
		ShowWindow(hwnd, SW_SHOW);
	}

	void WinBase::hide()
	{
		if (!alive()) return;
		ShowWindow(hwnd, SW_HIDE);
	}

	void WinBase::close()
	{
		// 先把成员置空再销毁：DestroyWindow 会同步派发 WM_DESTROY → onDestroy → onClosed，
		// 那条链里（WinCap / WinPin 的 onClosed）可能又调到 close()，不置空就是可重入的
		// DestroyWindow（对正在销毁的窗口再调一次，行为未定义）
		auto h = hwnd;
		hwnd = nullptr;
		if (h && IsWindow(h)) DestroyWindow(h);
	}

	void WinBase::minimize()
	{
		if (!alive()) return;
		ShowWindow(hwnd, SW_MINIMIZE);
	}

	void WinBase::maximize()
	{
		if (!alive()) return;
		ShowWindow(hwnd, SW_MAXIMIZE);
	}

	void WinBase::restore()
	{
		if (!alive()) return;
		ShowWindow(hwnd, SW_RESTORE);
	}

	void WinBase::refresh()
	{
		isDirty = true;
		// 建窗失败 / 已经关掉的窗口：置脏就够了，别往无效句柄上 InvalidateRect
		if (!alive()) return;
		InvalidateRect(hwnd, nullptr, FALSE);
	}

	void WinBase::enableShadow()
	{
		if (!alive()) return;
		MARGINS margins = { 1,1,1,1 };
		DwmExtendFrameIntoClientArea(hwnd, &margins);
		int value = 2;
		DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &value, sizeof(value));
		DwmSetWindowAttribute(hwnd, DWMWA_ALLOW_NCPAINT, &value, sizeof(value));
	}

	void WinBase::setTimer(UINT elapse, UINT id)
	{
		if (!alive()) return;
		SetTimer(hwnd, WM_APP + id, elapse, nullptr);
	}

	void WinBase::killTimer(UINT id)
	{
		if (!alive()) return;
		KillTimer(hwnd, WM_APP + id);
	}

	void WinBase::setTitle(const std::wstring& title)
	{
		this->title = title;
		if (alive()) {
			SetWindowText(hwnd, title.data());
		}
	}

	void WinBase::setSize(float w, float h)
	{
		this->w = w * dpi;
		this->h = h * dpi;
		if (alive()) {
			SetWindowPos(hwnd, nullptr, 0, 0, int(this->w), int(this->h), SWP_NOMOVE | SWP_NOZORDER | SWP_NOREDRAW);
		}
	}

	void WinBase::setPosition(int x, int y)
	{
		this->x = x;
		this->y = y;
		if (alive()) {
			SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOREDRAW);
		}
	}

	void WinBase::setCenter()
	{
		RECT workArea;
		SystemParametersInfo(SPI_GETWORKAREA, 0, &workArea, 0);
		int workWidth = workArea.right - workArea.left;
		int workHeight = workArea.bottom - workArea.top;
		// 工作区左上角偏移（处理多显示器时主屏可能不在 (0,0) 的情况）
		int workLeft = workArea.left;
		int workTop = workArea.top;
		// w/h 现在是物理像素，直接与屏幕坐标做算术
		auto x = workLeft + static_cast<int>((workWidth - w) / 2);
		auto y = workTop + static_cast<int>((workHeight - h) / 2);
		setPosition(x, y);
	}

	void WinBase::layout()
	{
		// 建窗失败时 body 是空的（createNativeWindow 在失败路径上不会建它）。
		// 以前这里直接解引用，一个建不出来的窗口就等于一次必定发生的空指针崩溃
		if (!body) return;
		// body 是根节点，尺寸完全由窗口物理尺寸决定 —— 直接设给布局引擎，不走 Node::setSize （那会把 w/h 当成逻辑值再乘一遍 dpi，导致翻倍）
		FlStyleSetWidth(body->node, w);
		FlStyleSetHeight(body->node, h);
		FlNodeCalculateLayout(body->node, w, h);
		body->layout();
	}

	void WinBase::setMinSize(float w, float h)
	{
		minW = w * dpi;
		minH = h * dpi;
	}

	std::wstring WinBase::openFileDialog(std::span<const COMDLG_FILTERSPEC> filter)
	{
		ComPtr<IFileOpenDialog> fileOpen;
		auto hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(fileOpen.GetAddressOf()));
		if (FAILED(hr)) return L"";
		hr = fileOpen->SetFileTypes(static_cast<UINT>(filter.size()), filter.data());
		if (FAILED(hr)) return L"";
		hr = fileOpen->Show(hwnd);
		if (FAILED(hr)) return L"";
		ComPtr<IShellItem> pItem;
		hr = fileOpen->GetResult(&pItem);
		if (FAILED(hr)) return L"";
		PWSTR pszFilePath = nullptr;
		hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
		if (FAILED(hr)) return L"";
		std::wstring result{ pszFilePath };
		CoTaskMemFree(pszFilePath);
		return result;
	}

	bool WinBase::createNativeWindow(DWORD exStyle, DWORD style)
	{
		auto hIns = GetModuleHandle(nullptr);
		auto cls = getWinClsName(hIns);
		hwnd = CreateWindowEx(WS_EX_NOREDIRECTIONBITMAP | exStyle, cls.data(), title.data(), style,
			x, y, (int)w, (int)h, NULL, NULL, hIns, NULL);
		// 【崩溃点 1】建窗失败时 hwnd 是 NULL。以前这里不检查，继续拿 NULL 去
		// SetWindowLongPtr / ICompositorDesktopInterop::CreateDesktopWindowTarget，
		// 后者返回 E_INVALIDARG，而它是个 WinRT 调用 —— 会抛 hresult_error。
		// 这条路径经常是从按钮回调（winrt::delegate，Invoke 是 noexcept）里进来的，
		// 异常一穿出去就是 std::terminate：用户看到的是"点一下，程序直接没了"
		if (!hwnd) return false;
		SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
		// 【崩溃点 2】组合器这一段全是 WinRT 调用，任何一步失败都会抛异常。
		// 这里是 C++ 库与 WinRT 的边界，异常一律就地吞掉：把已经建出来的窗口销毁，
		// hwnd / body / winTarget 全部回到"没建过"的状态，按失败返回给调用方处理
		try {
			auto interop = compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
			auto r = reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(winTarget));
			auto hr = interop->CreateDesktopWindowTarget(hwnd, false, r);
			if (FAILED(hr)) winrt::throw_hresult(hr);
			if (!winTarget) winrt::throw_hresult(E_FAIL);
			body = std::unique_ptr<Node>(new Node(this));
			winTarget.Root(body->visual);
			onCreated();
			layout();
		}
		catch (...) {
			body.reset();
			winTarget = nullptr;
			if (hwnd) {
				// 先摘掉 USERDATA：winProc 拿不到 self 就不会再派发 onDestroy / onTimer 之类的
				// 事件，免得"建到一半失败"也跑一次界面收尾（可能连带回调宿主、排一次延迟析构）
				SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
				DestroyWindow(hwnd);
				hwnd = nullptr;
			}
			return false;
		}
		return true;
	}

	LRESULT WinBase::borderHitTest(const POINT pt)
	{
		int border = static_cast<int>(4 * dpi);
		bool onLeft = pt.x >= 0 && pt.x < border;
		bool onRight = pt.x <= w && pt.x >= w - border;
		bool onTop = pt.y >= 0 && pt.y < border;
		bool onBottom = pt.y <= h && pt.y >= h - border;
		if (onTop && onLeft)  return HTTOPLEFT;
		if (onTop && onRight) return HTTOPRIGHT;
		if (onBottom && onLeft)  return HTBOTTOMLEFT;
		if (onBottom && onRight) return HTBOTTOMRIGHT;
		if (onLeft)   return HTLEFT;
		if (onRight)  return HTRIGHT;
		if (onTop)    return HTTOP;
		if (onBottom) return HTBOTTOM;
		return HTCLIENT;
	}

	void WinBase::onMinMaxInfo(MINMAXINFO* mmi)
	{
		RECT workAreaRect{};
		// SystemParametersInfo 失败时 workAreaRect 保持全 0，ptMaxSize 就成了 0×0，
		// 最大化后窗口被压成一条线。失败就退回主屏尺寸
		if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &workAreaRect, 0)
			|| workAreaRect.right <= workAreaRect.left || workAreaRect.bottom <= workAreaRect.top) {
			workAreaRect = RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
		}
		mmi->ptMaxPosition.x = workAreaRect.left;
		mmi->ptMaxPosition.y = workAreaRect.top;
		mmi->ptMaxSize.x = workAreaRect.right - workAreaRect.left;
		mmi->ptMaxSize.y = workAreaRect.bottom - workAreaRect.top;
		// minW/minH 是 float（构造时按 dpi 乘过），ptMinTrackSize 是 LONG：
		// 显式转一次，别让编译器每次构建都报 C4244
		mmi->ptMinTrackSize.x = static_cast<LONG>(minW);
		mmi->ptMinTrackSize.y = static_cast<LONG>(minH);
	}

	std::wstring& WinBase::getWinClsName(HINSTANCE hIns)
	{
		static std::wstring clsName = [&hIns] {
			WNDCLASSEXW wcex{};
			wcex.cbSize = sizeof(WNDCLASSEX);
			wcex.style = CS_HREDRAW | CS_VREDRAW;
			wcex.lpfnWndProc = &WinBase::winProc;
			wcex.hInstance = hIns;
			wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
			// hbrBackground 必须是 NULL，不能是 COLOR_WINDOW。
			// 这一类窗口全是 WS_EX_NOREDIRECTIONBITMAP + CompositionTarget：窗口内容由
			// 合成器提供，窗口自己一次 GDI 绘制都不做（WM_ERASEBKGND 直接返回 1）。
			// 挂上 COLOR_WINDOW（白刷）之后，窗口"首次显示/从隐藏恢复"的那一两帧，
			// 系统会先用类背景刷把客户区刷成白的，合成器下一帧才把内容盖上去 ——
			// 屏幕大特效上就是整屏闪一下白。截图窗口铺满桌面，这个白闪最刺眼
			wcex.hbrBackground = nullptr;
			wcex.lpszMenuName = nullptr;
			// 类名带品牌前缀：截图窗 / 贴图窗 / 工具条这一族全部注册在同一个类上，
			// 两版同时运行时同名类会让后来者的 RegisterClassEx 直接失败（返回 0），
			// 窗口一个都建不出来
			wcex.lpszClassName = SNAP_CLS_UI;
			wcex.hIcon = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(1));
			wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(1));
			//wcex.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
			//wcex.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
			ATOM atom = RegisterClassEx(&wcex);
			if (atom == 0) {
				assert("RegisterClassEx failed");
			}
			return wcex.lpszClassName;
			}();
		return clsName;
	}

	LRESULT WinBase::winProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		// 整条消息派发兜一层 try：回调（winrt::delegate 的 Invoke 是 noexcept）、
		// 布局、绘制里任何一处抛异常，穿出 WndProc 就是未定义行为 ——
		// 实际表现是"点一下窗口进程直接消失"，比这一条消息没处理严重得多
		try {
		auto self = reinterpret_cast<WinBase*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
		if (!self) {
			return DefWindowProc(hwnd, msg, wParam, lParam);
		}
		else if (msg == WM_ERASEBKGND) {
			return 1;
		}
		else if (msg == WM_PAINT) {
			return self->paint();
		}
		else if (msg == WM_SHOWWINDOW) {
			if (wParam == TRUE) {
				return 1;
			}
		}
		else if (msg == WM_NCHITTEST) {
			return self->onHitTest({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
		}
		else if (msg == WM_SETCURSOR) {
			if (LOWORD(lParam) == HTCLIENT) return self->setCursor();
		}
		else if (msg == WM_RBUTTONDOWN) {
			self->onMouseDown(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, true);
			return 0;
		}
		else if (msg == WM_RBUTTONUP) {
			self->onMouseUp(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, true);
			return 0;
		}
		else if (msg == WM_LBUTTONDOWN) {
			self->onMouseDown(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, false);
			return 0;
		}
		else if (msg == WM_LBUTTONUP) {
			self->onMouseUp(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, false);
			return 0;
		}
		else if (msg == WM_MOUSEMOVE) {
			self->mouseMove({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
		}
		else if (msg == WM_MOUSELEAVE) {
			self->mouseLeave();
		}
		else if (msg == WM_MOUSEWHEEL) {
			self->mouseWheel(wParam, lParam);
		}
		else if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
			self->onKeyDown((UINT)wParam);
		}
		else if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
			self->onKeyUp((UINT)wParam);
		}
		// 以下几条都不吞消息，继续交给 DefWindowProc：
		// WM_CHAR 由 WM_KEYDOWN 经 TranslateMessage 派生，IME 的组字结果也走这里；
		// 拦下 IME 消息会让输入法失去默认的组字窗口行为。
		else if (msg == WM_CHAR) {
			self->onChar((UINT)wParam);
		}
		// IME 的四条消息一次处理完，保证每条都恰好过一次 DefWindowProc：
		//   · 组字开始 / 上下文关联：先摆位再放行 —— 输入法开窗那一刻就用我们的坐标
		//   · 组字更新：只放行，组字窗由输入法自己维护
		//   · 通知：先放行再摆位 —— 候选框是它先摆、我们跟着挪
		// 以前分成三个分支，STARTCOMPOSITION 那条漏了 return，消息会继续落到后面的
		// 分支里再走一遍 DefWindowProc，首次组字时输入法的内部状态因此错位
		// （表现就是"第一次跑、提交一次之后正常"）
		else if (msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_COMPOSITION
			|| msg == WM_IME_NOTIFY || msg == WM_IME_SETCONTEXT) {
			if (msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_SETCONTEXT)
				self->onIME((UINT)msg, (UINT)wParam);
			const LRESULT r = DefWindowProcW(hwnd, msg, wParam, lParam);
			if (msg == WM_IME_COMPOSITION || msg == WM_IME_NOTIFY)
				self->onIME((UINT)msg, (UINT)wParam);
			return r;
		}
		else if (msg == WM_SETFOCUS) {
			self->onFocus();
		}
		else if (msg == WM_KILLFOCUS) {
			self->onBlur();
		}
		else if (msg == WM_CAPTURECHANGED) {
			self->onCaptureLost();
		}
		else if (msg == WM_TIMER) {
			self->onTimer((UINT)(wParam - WM_APP));
		}
		else if (msg == WM_DPICHANGED) {
			self->dpiChange(wParam, lParam);
		}
		else if (msg == WM_SIZE) {
			self->sizeChange(wParam, lParam);
			return 0;
		}
		else if (msg == WM_MOVE) {
			self->posChange({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
		}
		else if (msg == WM_GETMINMAXINFO) {
			self->onMinMaxInfo((PMINMAXINFO)lParam);
		}
		else if (msg == WM_DESTROY) {
			self->onDestroy();
		}
		return DefWindowProc(hwnd, msg, wParam, lParam);
		}
		catch (...) {
			// 吞掉：这一条消息的效果可能只做了一半，但进程活着，
			// 下一次重绘 / 下一次鼠标事件会把窗口状态拉回来
			return DefWindowProc(hwnd, msg, wParam, lParam);
		}
	}

	BOOL WinBase::setCursor()
	{
		bool flag{ false };
		onCursor(&flag);
		if (flag) return TRUE;
		SetCursor(LoadCursor(nullptr, IDC_ARROW));
		return TRUE;
	}

	void WinBase::mouseMove(POINT pos)
	{
		if (!isMouseIn) {
			isMouseIn = true;
			TRACKMOUSEEVENT tme{ sizeof(TRACKMOUSEEVENT) };
			tme.dwFlags = TME_LEAVE;
			tme.hwndTrack = hwnd;
			TrackMouseEvent(&tme);
		}
		onMouseMove(pos);
	}

	void WinBase::mouseLeave()
	{
		isMouseIn = false;
		onMouseMove(POINT{ INT_MAX, INT_MAX });
	}

	void WinBase::mouseWheel(WPARAM wParam, LPARAM lParam)
	{
		if (!hwnd) return;
		POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		ScreenToClient(hwnd, &pt);
		auto delta{ (short)HIWORD(wParam) };
		auto space = (delta / (float)WHEEL_DELTA) * 60.f * dpi;
		onMouseWheel(pt, space);
	}

	void WinBase::dpiChange(WPARAM wParam, LPARAM lParam)
	{
		const UINT newDPI = HIWORD(wParam);
		dpi = newDPI / 96.f;
		// 建窗失败 / 已销毁的窗口：没有节点树可重推，直接返回，别再往下算建议矩形
		if (!body) return;
		body->applyDpiChange();
		onDpiChanged();
		RECT* prcNewWindow = reinterpret_cast<RECT*>(lParam);
		const int newW = prcNewWindow->right - prcNewWindow->left;
		const int newH = prcNewWindow->bottom - prcNewWindow->top;
		SetWindowPos(hwnd, nullptr, prcNewWindow->left, prcNewWindow->top, newW, newH, SWP_NOZORDER | SWP_NOACTIVATE);
	}

	void WinBase::sizeChange(WPARAM wParam, LPARAM lParam)
	{
		if (wParam == SIZE_MINIMIZED) {
			mouseMove(POINT{INT_MAX,INT_MAX});
			onMinimize();
			return;
		}
		if (wParam == SIZE_MAXIMIZED) {
			isMaximized = true;
			onMaximize();
		}
		else if (wParam == SIZE_RESTORED) {
			if (isMaximized) {
				isMaximized = false;
				onRestore();
				onMouseMove(POINT{ INT_MAX, INT_MAX });
			}
		}
		w = static_cast<float>(GET_X_LPARAM(lParam));
		h = static_cast<float>(GET_Y_LPARAM(lParam));
		if (w <= 0 || h <= 0) return;
		layout();
		onSizeChanged();
	}

	void WinBase::posChange(POINT pos)
	{
		this->x = pos.x;
		this->y = pos.y;
		onMoved();
	}
	int WinBase::paint()
	{
		PAINTSTRUCT ps;
		BeginPaint(hwnd, &ps);
		EndPaint(hwnd, &ps);
		if (!isDirty) return 0;
		// 先清标志再 layout：layout 过程中子节点可能因内容重排而再次 refresh()
		// （例如宽度变化导致文本重新换行、高度变了要求二次布局）。若在 layout 之后
		// 才清，那次 refresh 置的 isDirty 会被这里抹掉，重排就丢了一帧。
		isDirty = false;
		layout();
		// 上面那次置脏必须自己补一条 invalidate：BeginPaint/EndPaint 已经把无效区
		// 清干净了，光留着 isDirty 不会凭空多出一条 WM_PAINT —— 只要没有下一个输入
		// 事件，这一帧就永远停在"重排之前"的内容上。文字编辑时最直观：打一个字，
		// 文字先按新宽度排好了，虚线框要等你打下一个字才跟着跳一下
		if (isDirty) {
			if (layoutRedirty < 3) {
				layoutRedirty++;
				InvalidateRect(hwnd, nullptr, FALSE);
			}
			else {
				// 有人每帧都 refresh（死循环式的重排）：放弃补帧，别再空转
				isDirty = false;
			}
		}
		else {
			layoutRedirty = 0;
		}
		return 0;
	}
}
