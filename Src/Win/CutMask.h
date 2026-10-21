#pragma once
#include <include/Ling.h>

// 光标落在哪一块。选区内部是 Inside，外面被选区四边的延长线切成 8 块，各对应一个方位
enum class MaskHit { None, Inside, Left, Top, Right, Bottom, TopLeft, TopRight, BottomRight, BottomLeft };

// 框选遮罩。四块半透明遮罩 + 蓝色选区边框 + 左上角尺寸标签。
// 自己不持有绘制目标，paint 时由宿主窗口把当前的 context 传进来。
// 所有坐标都是宿主窗口的客户区坐标。
class CutMask
{
public:
	CutMask(Ling::WinBase* win);
	// 鼠标悬停时吸附到光标下的窗口矩形，选区真的变了才返回 true
	bool highlight(POINT pos);
	void startMakeRect(POINT pos);
	void makeRect(POINT pos);
	// 选区四边的延长线把窗口切成九块：中间是 Inside，外面八块各是一个方位。没框出选区时才是 None
	MaskHit hitTest(POINT pos) const;
	// 开始调整：记下方向和起始矩形。按的是边或角时，这一下就把那条边吸到光标处
	void startAdjust(POINT pos);
	// 调整中：按 startAdjust 记下的方向改选区
	void adjust(POINT pos);
	// 宽高都大于 0 才算真的框出了东西
	bool hasRect() const;
	// 清空当前选区与尺寸标签，回到还没框选的状态（选择区域过程中点右键 = 重新选择）
	void reset();
	void paint(ID2D1DeviceContext* ctx);
public:
	D2D1_RECT_F maskRect{};
	float strokeWidth{ 2.f };
	// 选区定死之后（录屏 / 滚动截图）标签就没用了，而它可能压在选区内部被录进去
	bool hideLabel{ false };
	// 智能吸附总开关，悬停吸附（highlight）与拖动吸附（makeRect）都归它管。
	// 默认开：光标移动时自动识别光标下的窗口 / 控件。按快捷键首次进入就是这个状态。
	// 标注阶段点右键 = 重新进入屏幕选区界面（WinCap::restartSelect），那一次会把它关掉：
	// 用户要的是纯手动十字选框，鼠标不能一动就自己吸出一个框来。
	bool autoSnap{ true };
private:
	void initWinRect();
	// 尺寸标签只在 maskRect 变化时重建，不必每帧现建
	void makeLayout();
	// 把屏幕矩形裁剪进宿主窗口客户区并转成客户区坐标，合法才加入识别表
	void pushClippedRect(const RECT& r);
	// 递归收集顶层窗口里的子控件矩形，让识别能细化到按钮 / 面板等元素
	static BOOL CALLBACK collectChild(HWND hwnd, LPARAM lp);
	// 把一个坐标吸附到识别表里最近的边界（阈值内）
	float snapCoord(float v, float threshold) const;
	// 拖动框选时的吸附阈值
	static constexpr float snapThreshold = 8.f;
private:
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBorder;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
	D2D1_RECT_F layoutRect{};
	std::vector<D2D1_RECT_F> winRect;
	POINT pressPos{};
	Ling::WinBase* win{ nullptr };
	float paddingMargin{3.f};
	MaskHit adjustHit{ MaskHit::None };
	// 调整全程以按下那一刻的矩形为基准算，不做累加，免得漂移
	D2D1_RECT_F adjustStartRect{};
	POINT adjustPressPos{};
	// 选区最小尺寸，别让它塌成 0 宽 0 高
	static constexpr float minSize{ 4.f };
};
