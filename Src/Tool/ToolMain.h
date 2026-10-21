#pragma once
#include <include/Ling.h>
class WinHost;
class Tip;
class ToolMain : public Ling::WinBase
{
public:
	ToolMain(WinHost* win);
	~ToolMain();
	float getBtnCenterX();
	// 取消当前选中：清空 curId、把所有按钮恢复常态配色，并重排工具组（curId 空了 ToolSub 会隐藏）。
	void cancelSelect();
public:
	std::wstring curId;
private:
	void onCreated() override;
	void onClick(Ling::Button* btn);
	void onMinMaxInfo(MINMAXINFO* mmi);
	// 未选中态配色，选中态在 onClick 里就地设置
	void applyNormalStyle(Ling::Button* btn);
	// 按当前 dpi 把窗口尺寸算出来并应用。构造时算一次，DPI 变了再算一次
	void refreshSize();
private:
	WinHost* win;
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };
	// 逻辑像素，交给 Ling 的 setter 时由其内部乘 dpi
	static constexpr float btnSize{ 32.f };
	static constexpr float spliterW{ 1.f };
	std::vector<std::wstring> btnIds = { L"rect",L"arrow",L"number",L"line",L"text",L"mosaic", L"eraser",L"|",L"undo",L"redo",L"|",L"close",L"pin",L"save",L"clipboard" };
	// rect=矩形+椭圆交叠(合并主图标) e691，pin=实心图钉 e690，mosaic=正方形马赛克 e692；全部来自 iconfont 字形
	std::vector<std::wstring> btnCodes = { L"\ue691",L"\ue604",L"\ue776",L"\ue601",L"\ue6ec",L"\ue692",L"\ue6be",L"|",L"\ued85",L"\ued8a",L"|",L"\ue62d",L"\ue690",L"\ue608",L"\ue6ad" };
	std::vector<Ling::Button*> btns;
	// 悬停提示。要 hwnd，所以在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
};

