#pragma once
#include <Ui/Ui.h>
#include "WinHost.h"

// 独立贴图窗口（钉屏态）。WinPin 只由 WinCap 贴屏创建，出来的是"成品图"：
// 不带编辑器（不建 ToolMain / ToolSub），左键拖动、Ctrl+滚轮缩放、
// 双击 / 回车 / Ctrl+C 复制、Ctrl+S 保存、Esc 关闭。标注宿主公共的状态与接口
// 见 WinHost，这里只留本窗口特有的东西：底图像素的进入通道、缩放、离屏合成、双击/拖窗等。
class WinPin : public WinHost
{
public:
	~WinPin();
	static void init(int x, int y, int w, int h);
	// 底图不来自 WinCap 的截屏，而是外部给的一块 BGRA、top-down、行紧凑（步长 = w*4）像素。
	// 标注完直接贴屏（WinCap::pinToScreen）走的就是这条路。
	static void initFromData(int x, int y, int w, int h, std::vector<BYTE>& data);
	// 当前屏幕上还有没有贴图窗口。用完即走模式靠它判断"活干完了没"
	static bool hasWindow();
	// 退出流程里调：窗口对象是文件级静态变量，交给静态析构就在 CoUninitialize 之后了
	static void dispose();
	// WinHost 的纯虚接口。钉屏态不建工具条，没有东西可摆 —— 空实现
	void layoutTools() override;
	// 兜底把窗口钉在最上层（建窗时已带 WS_EX_TOPMOST）。钉屏态没有工具条可收
	void pinToScreen() override;
	// 把底图与所有未撤销的 shape 合成后写入剪切板，成功即关窗
	void copyToClipboard() override;
	// 弹另存为对话框，把合成结果存成 PNG，成功即关窗
	void saveToFile() override;
	// 所有 ShapeText 共用的文本输入框（WinHost 纯虚接口；贴图窗没有文字编辑，恒返回空）
	ui::TextBox* getTextBox() override;
	// ShapeText 进入 / 退出编辑时登记自己（贴图窗没有文字编辑，不会有人调用）
	void setEditingText(ShapeText* shape) override;
	// ToolSub 上的颜色 / 字号 / 粗体 / 斜体变了，转给正在编辑的文本立即生效（贴图窗没有副编辑器，空实现）
	void onToolStyleChanged() override;
private:
	WinPin(int x, int y, int w, int h, const std::vector<BYTE>* data = nullptr);
	// init / initFromData 的实现体：两条入口只差底图来源，建窗失败时的收尾完全一样
	static void create(int x, int y, int w, int h, const std::vector<BYTE>* data);
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
	ui::Canvas* canvas{ nullptr };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
	// 贴图与背后界面的隔离带：单条 1px 半透明黑，占窗口最外侧 1px，不遮挡内容
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> outerEdgeBrush;
	// 贴图"到场提示"：窗口刚贴到桌面上时，四周那圈 1px 细线依次换几种颜色，
	// 换完整轮后恢复成普通边框。起因是贴图完成后界面毫无动静，用户分不清到底贴上没有
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> flashBrush;
	int flashTicks{ 0 };    // 还剩几步变色
	int flashIdx{ 0 };      // 当前用第几种颜色
	// 右上角的倍数提示
	Microsoft::WRL::ComPtr<IDWriteTextLayout> scaleTip;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushTipBg, brushTipText;
	bool isClosed{ false };
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };
};
