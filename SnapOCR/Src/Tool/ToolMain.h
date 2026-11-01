#pragma once
#include <Ui/Ui.h>
// 图标名字常量（icons::ToolRect 等）由生成器产出，见 Res/Icons.generated.h；
// 包在头里而不是只在 Text.cpp 里，是为了让主条的名字表也受"写错=编译错"的保护。
#include "../Res/Icons.generated.h"
class WinHost;
class Tip;

// 工具条 / 副编辑条按钮的三档底色，两条工具条共用一份（ToolSub.cpp 也 include 本文件）。
// 工具条是白底，悬停与选中不再"加亮"而是用半透明黑压暗一档：
// 白底叠 8% 黑 ≈ #EBEBEB，15% ≈ #D9D9D9，一眼能分出悬停 / 选中，也不会把图标淹掉。
// 注意 ui::Color 是 0xRRGGBBAA —— alpha 在最后两位，写成 0x14000000 就是纯黑一块
inline constexpr uint32_t C_BTN_HOVER = 0x00000014;     // 黑 8%
inline constexpr uint32_t C_BTN_ON = 0x00000026;        // 黑 15%
inline constexpr uint32_t C_BTN_ON_HOVER = 0x00000038;  // 黑 22%

class ToolMain : public ui::WinBase
{
public:
	ToolMain(WinHost* win);
	~ToolMain();
	// 当前工具按钮（curId；没有选中时回退到 hover 预览的 previewId）的中心相对主条左边的偏移。
	// 副编辑条的箭头要靠它对准按钮
	float getBtnCenterX();
	// 取消当前选中：清空 curId、把所有按钮恢复常态配色，并重排工具组（curId 空了 ToolSub 会隐藏）。
	void cancelSelect();
	// 只清选中样式和 curId，不重排
	void clearSelectStyle();
	// 副编辑器里做了实质操作（点颜色 / 拖滑块 / 切开关）时由宿主转过来：把该工具
	// 从"hover 预览"（previewId）提升为"选中"（curId），点亮主条对应按钮。
	// 这样用户在副条上点一下就等于选中了工具，不必再回主条点一次。
	void promoteToSelected(const std::wstring& id);
public:
	std::wstring curId;
	// hover 预览的副编辑器 id（非空 = 正有一个副条因悬停被预览显示，而非点击选中）。
	// layoutTools（WinCap / WinPin）据此决定副条显隐：curId 或 previewId 任一非空都显示。
	// 与 curId 的区别：previewId 不点亮主条按钮选中态、不改变当前工具
	std::wstring previewId;
	// 待显示（延迟弹窗）的目标工具 id。onEnter 时先记在这里、启动 kSubShowTimer，
	// 定时器到期后才真正 showSubPreview —— 未到期前不重建副条
	std::wstring pendingShowId;
private:
	void onCreated() override;
	void onClick(ui::Button* btn);
	void onMinMaxInfo(MINMAXINFO* mmi);
	// 未选中态配色，选中态在 onClick 里就地设置
	void applyNormalStyle(ui::Button* btn);
	// 按当前 dpi 把窗口尺寸算出来并应用。构造时算一次，DPI 变了再算一次
	void refreshSize();
	// —— hover 弹出副编辑器 ——
	// 鼠标悬停在主条按钮上时预览显示对应副编辑器，但不改变 curId 选中态。
	// 与 onClick 共用同一套"按 id 显示副条"的分发，保证两者显示的东西永远一致
	// 按 id 显示对应的副编辑器。hover 预览与点击选中共用这一套分发，保证两者一致。
	// selecting = true 表示调用方是"点击选中"（onClick）：马赛克/橡皮擦会据此把
	// 形状拉回"选框"；悬停预览（定时器到期、移回当前按钮）一律传 false
	void showSubPreview(const std::wstring& id, bool selecting = false);
	// 鼠标离开主条按钮后的延迟收起：给一个短暂窗口让鼠标滑进副条，超时且
	// 光标既不在主条、也不在副条里才真正收起
	void scheduleSubHide();
	// 判断一个屏幕坐标是否落在副条（toolSub）窗口内
	bool pointInSub(const POINT& screenPt) const;
	// 定时器 id：主条按钮离开后的延迟收起（见 scheduleSubHide）
	static constexpr UINT kSubHideTimer = 102;
	// 定时器 id：主条按钮进入后的延迟显示（见 onEnter）。给一个短窗口让"快速扫过
	// 主条"不触发副条重建，只有真正停在某颗图标上（停满 kSubShowDelay）才弹出，
	// 避免鼠标横向扫过时副条反复销毁重建、宽度跳变造成的"跳界面"
	static constexpr UINT kSubShowTimer = 103;
	// 弹出延迟。60ms 那会儿几乎等于"一碰就弹"，鼠标顺着主条划过去，副条一路跟着
	// 销毁重建、宽度来回跳，看着乱；260ms 的停顿既滤掉"只是路过"，又不至于让人觉得迟钝。
	// 点击是另一条路（onClick 直接显示），不受这个延迟影响 —— 点了就立刻出来
	static constexpr UINT kSubShowDelay = 260;
private:
	WinHost* win;
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };
	// 尺寸全部来自 ToolMetrics（主副编辑条共用一套规则），这里不再单独定义
	std::vector<std::wstring> btnIds = { L"rect",L"arrow",L"number",L"brush",L"text",L"mosaic", L"eraser",L"video",L"|",L"undo",L"redo",L"|",L"close",L"pin",L"save",L"clipboard" };
	// 图标按**语义名**引用，且一律走 icons:: 常量 —— 名字写错在这里是编译错误，
	// 而不是"这颗按钮空着、运行期不报错"。工具位与 id 一一对应：tool-<id>，
	// 只有分隔符 L"|" 不是图标。
	std::vector<std::wstring> btnNames = { ui::icons::ToolRect,ui::icons::ToolArrowStraight,ui::icons::ToolNumber,ui::icons::ToolLine,ui::icons::ToolText,ui::icons::ToolMosaic,ui::icons::ToolEraser,ui::icons::RecordingStart,L"|",ui::icons::ActionUndo,ui::icons::ActionRedo,L"|",ui::icons::ActionClose,ui::icons::ActionPin,ui::icons::QuickSave,ui::icons::ActionCopy };
	std::vector<ui::Button*> btns;
	// 悬停提示。要 hwnd，所以在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
};
