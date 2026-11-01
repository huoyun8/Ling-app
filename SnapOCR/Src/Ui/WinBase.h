#pragma once
#include "Layout.h"
#include <unordered_map>
#include <span>
#include <Windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <winrt/base.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

namespace ui {
	class Node;
	class WinBase
	{
		friend class Node;
	public:
		WinBase();
		~WinBase();
		void enableShadow();
		void disableWinAnimation();
		void disableBorderRadius();
		void show();
		void hide();
		void close();
		void minimize();
		void maximize();
		void restore();
		void refresh();
		// "该重画了"的请求入口。默认就是 refresh()；需要控频的宿主（截图窗每帧都要
		// 整屏重绘，鼠标 125Hz 地来）覆写它做合帧，避免每来一个消息就整屏重画一次
		virtual void requestPaint() { refresh(); }
		// 建窗。失败（CreateWindowEx 返回空 / 组合器或 D2D 抛异常）返回 false，且 hwnd 保持为空、
		// 已经建出来的窗口会被就地销毁。**调用方必须看返回值**：
		// 拿到一个 hwnd 为空的窗口对象，之后每一次 show / layout / 绘制都是空指针；
		// 更糟的是这些调用往往发生在按钮回调里，而 winrt::delegate 的 Invoke 是 noexcept 的，
		// 异常一穿出去就是 std::terminate —— 用户看到的就是"点一下程序直接没了"
		bool createNativeWindow(DWORD exStyle = NULL, DWORD style = WS_POPUP | WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
		// 窗口句柄是否还有效。hwnd 被销毁后这个值本身仍非空（还可能被系统复用给别的窗口），
		// 所有 public 方法都先过一遍它，避免往一个已经不存在的窗口上发消息
		bool alive() const { return hwnd != nullptr && IsWindow(hwnd) != FALSE; }
		void setTimer(UINT elapse, UINT id);
		void killTimer(UINT id);
		void setTitle(const std::wstring& title);
		void setSize(float w, float h);
		void setPosition(int x, int y);
		void setCenter();
		virtual void layout();
		void setMinSize(float w, float h);
		std::wstring openFileDialog(std::span<const COMDLG_FILTERSPEC> filter);
	public:
		int x{ 0 }, y{ 0 };
		float w{ 0 }, h{ 0 }, minW{ 800 }, minH{600};
		float dpi{ 1.0 };
		HWND hwnd{ nullptr };
		std::wstring title;
		bool isMouseIn{ false }, isMaximized{ false };
		winrt::Windows::UI::Composition::Compositor compositor{ nullptr };
		 
		winrt::event<winrt::delegate<POINT>> onMouseMove;
		winrt::event<winrt::delegate<POINT, bool>> onMouseDown; 
		winrt::event<winrt::delegate<POINT, bool>> onMouseUp;
		winrt::event<winrt::delegate<POINT, float>> onMouseWheel; 
		winrt::event<winrt::delegate<bool*>> onCursor; 
		winrt::event<winrt::delegate<UINT>> onKeyDown;
		winrt::event<winrt::delegate<UINT>> onKeyUp;
		// WM_CHAR：参数是字符码（已经过键盘布局与 IME 转换），文本输入取字符要用它而不是 onKeyDown。
		// 码点 > 0xFFFF 时需要使用者自己拼代理对。
		winrt::event<winrt::delegate<UINT>> onChar;
		// IME 消息通知：带 msg 与 wParam，接收方要自己分辨哪种通知才该摆位。
		// 以前是 void()，TextBox 只能一律重设位置 —— 输入法每次模式切换、状态窗
		// 开关都会触发一遍，两边互相覆盖，候选框就一下在这、一下在那
		winrt::event<winrt::delegate<UINT, UINT>> onIME;
		// WM_SETFOCUS / WM_KILLFOCUS：窗口级焦点。控件自身的焦点态由控件维护，
		// 窗口失焦时通常要让控件一并失焦（否则光标会在没焦点的窗口里继续闪）。
		winrt::event<winrt::delegate<>> onFocus;
		winrt::event<winrt::delegate<>> onBlur;
		// WM_CAPTURECHANGED：鼠标捕获被系统或别人抢走（弹窗、UAC、别的窗口 SetCapture）。
		// 控件收到后必须把自己"按下/拖动中"的状态复位，否则拖拽标记会永远留着 ——
		// 之后再路过控件，鼠标移动仍被当成拖动，值/选区会跟着光标乱跳
		winrt::event<winrt::delegate<>> onCaptureLost;
		winrt::event<winrt::delegate<UINT>> onTimer;
		winrt::event<winrt::delegate<>> onSizeChanged;
		winrt::event<winrt::delegate<>> onDpiChanged;
		winrt::event<winrt::delegate<>> onDestroy;
		winrt::event<winrt::delegate<>> onMaximize;
		winrt::event<winrt::delegate<>> onMinimize;
		winrt::event<winrt::delegate<>> onRestore;
		winrt::event<winrt::delegate<>> onMoved;

		std::unique_ptr<Node> body;   // 必须在事件字段之后声明，先于事件字段析构
	protected:
		virtual void onCreated() {};
		virtual LRESULT onHitTest(const POINT) { return HTCLIENT; };
		LRESULT borderHitTest(const POINT pt);
		virtual void onMinMaxInfo(MINMAXINFO* mmi);
		virtual BOOL setCursor();
	private:
		std::wstring& getWinClsName(HINSTANCE hIns);
		static LRESULT CALLBACK winProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
		void mouseMove(POINT pos);
		void mouseLeave();
		void mouseWheel(WPARAM wParam, LPARAM lParam);
		void dpiChange(WPARAM wParam, LPARAM lParam);
		void sizeChange(WPARAM wParam, LPARAM lParam);
		void posChange(POINT pos);
		int paint();
	private:
		winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget winTarget{ nullptr };
		bool isDirty{ false };
		// layout 期间又置脏的次数。见 paint()：补一次 invalidate 让重排落屏，
		// 连补三次还在置脏就说明有节点每帧都 refresh，必须停下，别把 CPU 空转到 100%
		unsigned layoutRedirty{ 0 };
	};
}
