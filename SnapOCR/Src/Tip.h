#pragma once
#include <Ui/Ui.h>

// 鼠标悬停提示。包的是系统 tooltip 控件（comctl32 的 TOOLTIPS_CLASS），
// 用 TTF_TRACK 模式：位置由我们自己算，什么时候显示/隐藏也由我们自己说。
//
// 一个窗口配一个实例，宿主窗口负责持有它（tooltip 是宿主 hwnd 的子窗口）。
// 因为要 hwnd，所以只能在 onCreated() 里建，不能在构造函数里建。
//
// 显示要等 1 秒（鼠标只是路过时不该弹东西），隐藏立刻。
class Tip
{
public:
	Tip(ui::WinBase* win);
	~Tip();
	// 最省事的接法：鼠标进按钮就排上提示，离开就收。文字固定不变。
	void bind(ui::Button* btn, const std::wstring& text);
	// 换一颗按钮上的提示文字：文字是捕获进 enter 回调里的，改不了，只能摘了重绑。
	// 给"图标和提示都随状态切换"的按钮用
	void rebind(ui::Button* btn, const std::wstring& text);
	// 摘掉所有已绑定按钮上的订阅并清空绑定表。
	// **必须**在按钮被销毁之前调（ToolSub::beginTool 里 removeAllChildren 之前）：
	// 绑定表存的是裸指针，按钮销毁之后再逐个 remove 就是 use-after-free ——
	// 以前只在析构里 remove，切过一次工具之后这张表里就全是野指针了
	void unbindAll();
	// 提示的底边中点贴在 owner 上边缘处，位置由 owner 的布局结果算，调用方不用管坐标。
	void showAbove(ui::Node* owner, const std::wstring& text);
	// 自己指定锚点（屏幕坐标，物理像素）：提示的底边中点对齐到 (screenX, screenY)。
	// 同一个 owner 反复调用可以让提示跟着鼠标走（滑块的数值提示就是这么做的）。
	void showAt(ui::Node* owner, float screenX, float screenY, const std::wstring& text);
	// 只有当前提示确实属于 owner 时才隐藏。
	// 必须带上 owner：多个控件的 hover 回调触发顺序不固定，
	// 无条件 hide() 会把别人刚显示出来的提示误关掉。
	void hide(ui::Node* owner);
	void hide();
public:
	// 锚点相对控件上边缘往里的距离：提示压在控件上边缘 4 像素处，与 2.4.25 的观感一致。
	// 自己算锚点的调用方（比如跟着鼠标走的滑块提示）也用它，免得两处数值不一样。
	static constexpr float anchorInset{ 4.f };
private:
	// 真正把提示摆出来/更新掉，1 秒计时到点后才会走到这里
	void showNow();
	// 提示按当前文字要占多大（不需要提示已经显示出来）
	SIZE measure();
	// 用提示控件自己的字体量一段文字有多大（不含气泡的边框和内边距）
	SIZE measureText(const std::wstring& str);
	// 量出气泡的固定内边距，整个进程只需要量一次
	void learnPadding();
	struct BoundBtn
	{
		ui::Button* btn{ nullptr };
		winrt::event_token enterTok{}, leaveTok{};
	};
private:
	ui::WinBase* win;
	HWND tipHwnd{ nullptr };
	// 当前提示的归属节点。只做指针比较，不解引用 —— 控件被销毁后这个指针可能是野的。
	ui::Node* owner{ nullptr };
	// bind 过的按钮与它们的订阅。ToolSub 每次切换工具都会把按钮整批销毁重建，
	// 订阅不摘掉就会一直挂在 hello 里的 event 上、指向已经释放的节点；
	// Tip 析构前要逐个 remove，所以得记下来
	std::vector<BoundBtn> boundBtns;
	std::wstring text;
	float anchorX{ 0.f }, anchorY{ 0.f };
	// visible: 提示已经在屏上；否则要么没提示，要么正在等那 1 秒
	bool visible{ false };
	winrt::event_token timerTok{};
	// 定时器 id 要避开宿主窗口里其他人在用的（Ui 的 TextBox 从 0x4200 起）
	static constexpr UINT timerId{ 0x5100 };
	static constexpr UINT delayMs{ 1000 };
};
