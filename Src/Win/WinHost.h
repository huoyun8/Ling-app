#pragma once
#include <include/Ling.h>

class ToolMain;
class ToolSub;
class History;
class ShapeBase;
class ShapeText;

// 标注宿主的公共接口与状态。WinPin（独立标注窗）与 WinCap（框选窗内直接标注）都继承它，
// 让 ToolMain / ToolSub / History / 各 Shape 能以统一的宿主指针工作，而不是各自钉死在 WinPin 上。
// 纯虚方法由各宿主实现；工具条 / 历史 / 底图 / hover 等共享状态集中放在这里。
class WinHost : public Ling::WinBase
{
public:
	virtual ~WinHost();

	// —— 纯虚：宿主各自的实现 ——
	// 把 ToolMain / ToolSub 摆到合适的位置（WinPin 按窗口摆，WinCap 按选区摆）
	virtual void layoutTools() = 0;
	// ToolSub 上的颜色 / 字号 / 粗体 / 斜体变了，转给正在编辑的文本立即生效
	virtual void onToolStyleChanged() = 0;
	// 所有 ShapeText 共用的文本输入框，第一次用到时才建
	virtual Ling::TextBox* getTextBox() = 0;
	// ShapeText 进入 / 退出编辑时登记自己。传 nullptr 表示没有在编辑的文本
	virtual void setEditingText(ShapeText* shape) = 0;
	// 窗口客户区坐标（物理像素）→ shape 的坐标。WinPin 里除缩放倍数；WinCap 里恒等（直接用窗口坐标）
	virtual POINT toImgPos(const POINT& pos) const = 0;
	// 弹另存为对话框，把合成结果存成 PNG
	virtual void saveToFile() = 0;
	// 把底图与所有未撤销的 shape 合成后写入剪切板
	virtual void copyToClipboard() = 0;
	// 钉到桌面上（WinPin 收起工具条；WinCap 生成本图后交给贴图窗展示）
	virtual void pinToScreen() = 0;

public:
	// Ctrl+滚轮的缩放倍数。WinPin 用；WinCap 恒为 1，标注直接用窗口坐标
	float scale{ 1.f };
	std::unique_ptr<ToolMain> toolMain;
	std::unique_ptr<ToolSub> toolSub;
	ShapeBase* shapeHover{ nullptr };
	// 右键选中的编辑目标：默认只可编辑最后一个，右键点中某个更早的标注时把它解锁，
	// onMove 会优先悬停它；右键点空白 / 新画一笔会把它重置回"只编辑最后一个"
	ShapeBase* editTarget{ nullptr };
	// 本次按下新建出来的 shape（不是拖已有元素）。抬手时只对它做"有没有画出东西"的判定
	ShapeBase* newShape{ nullptr };
	// 本次按下之后光标有没有真的移动过。判"按下马上弹起"只认这个
	bool hasDragged{ false };
	std::unique_ptr<History> history;
	// 底图。ShapeMosaic 读它算马赛克块，ShapeEraser 拿它当"擦回原样"的画刷。
	// WinPin 里是整块截图像素；WinCap 里是全桌面，shape 都用窗口坐标，两者对得上
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> screenImg;
	// 文本输入框与当前正在编辑的 ShapeText。非空表示"编辑中"
	Ling::TextBox* textBox{ nullptr };
	ShapeText* editingText{ nullptr };
	bool isMouseDown{ false };
	POINT pressPos{ 0, 0 };
	// 自己认双击用的上一次按下时间与位置
	ULONGLONG lastDownTime{ 0 };
	POINT lastDownPos{ 0, 0 };
	// 上一次按下是不是新建了一个留得住的元素（现在只有序号）
	bool prevPressCreatedShape{ false };
};
