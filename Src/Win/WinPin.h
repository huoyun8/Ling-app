#pragma once
#include <include/Ling.h>
#include "WinHost.h"

// 独立标注 / 贴图窗口。标注宿主公共的状态与接口见 WinHost，这里只留本窗口特有的东西：
// 底图像素的进入通道、缩放、离屏合成、双击/拖窗等。
class WinPin : public WinHost
{
public:
	~WinPin();
	static void init(int x, int y, int w, int h);
	// 底图不来自 WinCap 的截屏，而是外部给的一块 BGRA、top-down、行紧凑（步长 = w*4）像素。
	// 滚动截图（WinLong）拼出来的长图走这条路进贴图窗口。
	static void initFromData(int x, int y, int w, int h, std::vector<BYTE>& data);
	// 当前屏幕上还有没有贴图窗口。用完即走模式靠它判断"活干完了没"
	static bool hasWindow();
	// 退出流程里调：窗口对象是文件级静态变量，交给静态析构就在 CoUninitialize 之后了
	static void dispose();
	void layoutTools() override;
	// 贴图：把标注好的这张图钉在桌面上，收起 ToolMain / ToolSub，只剩图本身
	void pinToScreen() override;
	// 把底图与所有未撤销的 shape 合成后写入剪切板，成功即关窗
	void copyToClipboard() override;
	// 弹另存为对话框，把合成结果存成 PNG，成功即关窗
	void saveToFile() override;
	// 所有 ShapeText 共用的文本输入框，第一次用到时才建
	Ling::TextBox* getTextBox() override;
	// ShapeText 进入 / 退出编辑时登记自己
	void setEditingText(ShapeText* shape) override;
	// ToolSub 上的颜色 / 字号 / 粗体 / 斜体变了，转给正在编辑的文本立即生效
	void onToolStyleChanged() override;
private:
	WinPin(int x, int y, int w, int h, const std::vector<BYTE>* data = nullptr);
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi) override;
	void onDown(POINT pos, BOOL isRight);
	void onMove(POINT pos);
	void onUp(POINT pos, BOOL isRight);
	void onKey(UINT key);
	void onTimerCB(UINT id);
	void onClosed();
	BOOL setCursor() override;
	// 离屏合成出最终图像的像素（BGRA、top-down、行步长紧凑为 size.width*4）。
	// 只画底图和未撤销的 shape，不含蓝色边框和夹点。
	bool getImagePixels(std::vector<BYTE>& pixels, D2D1_SIZE_U& size);
	// 另存为对话框会抢走前台并把 WinPin 激活，取消保存后用它把窗口层级和前台窗口恢复原样
	void restoreWindowState(HWND foregroundBeforeDialog);
	// 把窗口尺寸掰成"底图像素 × scale"
	void applyWinSize();
	// 底图的像素尺寸，也就是导出图的尺寸
	D2D1_SIZE_U getImgSize() const;
	// 窗口客户区坐标（物理像素）→ 底图坐标
	POINT toImgPos(const POINT& pos) const override;
	// 缩放到新倍数
	void applyScale(float newScale, POINT anchor);
	void paintScaleTip(ID2D1DeviceContext* ctx);
private:
	// 整个窗口内容都画在这块画布上，走 swap chain 后端
	Ling::Canvas* canvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
	// 1px 半透明内阴影，叠在蓝色边框内侧给贴屏窗口一个轻轮廓
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> innerShadowBrush;
	// 右上角的倍数提示
	Microsoft::WRL::ComPtr<IDWriteTextLayout> scaleTip;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushTipBg, brushTipText;
	bool isClosed{ false };
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };
	// 钉屏态：贴屏出来的窗口是成品图，不带编辑器（ToolMain/ToolSub 一直隐藏）。
	// 可拖动、双击/回车复制、Esc 关闭。WinPin 目前只由 WinCap 贴屏创建，故恒为钉屏态
	bool pinned{ true };
};
