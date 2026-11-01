#pragma once
#include <dispatcherqueue.h>
#include <shellapi.h>
#include <vector>
#include <winrt/Windows.System.h>
namespace ui {
	bool init();
	void dispose();
	// 一个窗口都没有之后等这么久再释放图形设备。为什么要等而不当场释放，
	// 见 WinBase 析构里那段说明（关窗瞬间拆 D3D 在某些驱动上会卡住 UI 线程）
	constexpr UINT kIdleD2DDisposeTimer = 0x5A;
	constexpr UINT kIdleD2DDisposeMs = 20000;
	class WinBase;
	class App
	{
		public:
			~App();
			static App* get();
			void quit(int code = 0);
			void exit(int code = 0);
			/// <summary>
			/// 如果发现系统中已经存在一个应用实例，
			/// 则当前应用马上退出，此方法返回true
			/// 并触发前一个应用实例的onSecondInstance事件。
			/// enforce=false（设置里关了"只允许单实例"）时不抢互斥体、不做转交，
			/// 只把托盘/热键共用的 message-only 窗口建好 —— 多实例并存是用户明确要的
			/// </summary>
			bool refuseSecondInstance(bool enforce = true);
			/// <summary>
			/// 注册全局热键，返回是否注册成功。键名认不出来、或者这个组合已经被别的程序占了
			/// 都会失败 —— 失败要不要告诉用户由调用方决定
			/// </summary>
			bool regHotKey(const std::wstring& keyStr, const UINT msgId);
			void unRegHotKey(const UINT msgId);
			void initTray(const UINT msgId,const std::wstring& tip);
			void disposeTray();
			// 托盘图标现在是不是真的挂在任务栏上。关窗要不要"收进托盘"必须先问它：
			// 图标不在了还把窗口藏起来，就等于把唯一的入口藏掉，进程只能等任务管理器
			bool hasTray() const { return trayOk; }
			// 软件内的轻提示：一块半透明深色小条，贴在屏幕下方中间，1.8 秒后自己消失。
			// 存盘/录完那一下的反馈走它 —— 以前走的是托盘通知气泡（NIF_INFO），
			// 那东西在任务栏右下角、与程序界面完全脱节，还容易被系统通知横幅挤掉
			void showToast(const std::wstring& text);
			void initArgs();
			UINT popupMenu(HMENU menu);
		public:
			winrt::event<winrt::delegate<>> onBeforeQuit;
			winrt::event<winrt::delegate<>> onSecondInstance;
			winrt::event<winrt::delegate<UINT>> onHotKey;
			winrt::event<winrt::delegate<bool,bool>> onTrayMouseEvent;
			std::wstring appID;
			// 单实例互斥体。只靠 FindWindowEx 判"有没有人在跑"挡不住同时启动：
			// 两个进程会在同一瞬间都查不到窗口、然后各建一个
			HANDLE instanceMutex{ nullptr };
			std::unordered_map<std::wstring, std::wstring> args;
			winrt::Windows::System::DispatcherQueue dq;
			// 托盘图标、全局热键、单实例检测共用的 message-only 窗口。
			// 需要在它上面挂定时器（例如"空转一段时间后放掉图形设备"）时取它
			HWND msgWnd() const { return msgHwnd; }
			// 当前打开的窗口，按创建顺序。WinBase 构造时登记、析构时注销。
			// 最后一个窗口注销时挂一个延时器，空转到点再放掉 D2D 设备（见 WinBase 析构）
			std::vector<WinBase*> windows;
		private:
			static void init();
			// 由 ui::dispose 调用：托盘图标、DispatcherQueue 都得在 CoUninitialize 之前放掉，
			// 留给进程退出后的静态析构就是往已经拆掉的 COM 套间上打 Release
			static void dispose();
			App();
			friend bool ui::init();
			friend void ui::dispose();
		private:
			void initMsgWin();
			static LRESULT CALLBACK winProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
			// 轻提示窗口的过程。作为成员函数是为了能回写 toastHwnd（窗口销毁后就悬空了）
			static LRESULT CALLBACK toastProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
			static void initDispatcherQueueCtrl();
			// 下面两个**必须**显式给初值。App 是用 new App() 建的，而 App 有用户提供的
			// 构造函数，这时 new App() 不做值初始化，没写进初始化列表的成员就是堆上的
			// 随机值 —— 曾经就是这里踩的坑：msgHwnd 一旦是随机的非空指针，
			// initMsgWin() 开头的 `if (msgHwnd) return;` 会直接返回，那个 message-only
			// 窗口永远建不出来。于是三件事同时坏掉，症状就是"关掉窗口后进程一直留在后台"：
			//   · Shell_NotifyIcon 拿着野句柄 → 托盘图标挂不上，界面上看不到小图标；
			//   · RegisterHotKey 也用这个句柄 → F1/F2/F10 全部没反应；
			//   · FindWindowEx(HWND_MESSAGE,...) 找不到窗口 → 防多开失效，还能再开一个实例；
			// 而关窗默认是"收进托盘"，没有托盘图标就等于没有回去的入口，进程只能等任务管理器。
			HWND msgHwnd{ nullptr };
			std::unique_ptr<NOTIFYICONDATA> tray;
			// 托盘图标是否真的挂上了。关窗行为按它决定，见 WinQr 的 WM_CLOSE：
			// 图标不在就老老实实退进程，绝不再把窗口藏起来
			bool trayOk{ false };
			UINT trayMsgId{ 0 };
			// 当前这条轻提示的窗口。没有提示时为 nullptr；窗口被销毁时会自己清掉
			HWND toastHwnd{ nullptr };
			// RegisterWindowMessage(L"TaskbarCreated") 的结果：explorer 重启后靠它重挂托盘图标
			UINT taskbarCreatedMsg{ 0 };
	};
}