#pragma once
#include <Ui/Ui.h>

class ToolMain;
class ToolSub;
class History;
class ShapeBase;
class ShapeText;

// 标注宿主的公共接口与状态。WinPin（独立标注窗）与 WinCap（框选窗内直接标注）都继承它，
// 让 ToolMain / ToolSub / History / 各 Shape 能以统一的宿主指针工作，而不是各自钉死在 WinPin 上。
// 纯虚方法由各宿主实现；工具条 / 历史 / 底图 / hover 等共享状态集中放在这里。
class WinHost : public ui::WinBase
{
public:
	virtual ~WinHost();

	// —— 纯虚：宿主各自的实现 ——
	// 把 ToolMain / ToolSub 摆到合适的位置（WinPin 按窗口摆，WinCap 按选区摆）
	virtual void layoutTools() = 0;
	// ToolSub 上的颜色 / 字号 / 粗体 / 斜体变了，转给正在编辑的文本立即生效
	virtual void onToolStyleChanged() = 0;
	// 所有 ShapeText 共用的文本输入框，第一次用到时才建
	virtual ui::TextBox* getTextBox() = 0;
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
	// —— 录屏 ——
	// 副编辑器上的录屏条只认下面这四个门面。WinPin 用默认实现：那边没有录屏，
	// beginVideo 返回 false，按下"开始"什么都不会发生（那个按钮本来也不会出现在贴图窗里）
	// 起录：选区已定、建编码线程、让出鼠标（这一步由宿主自己做）。起不来返回 false
	virtual bool beginVideo(bool mp4, bool speaker, bool mic) { return false; }
	// 停录：成品直接落桌面（不弹另存为框），toClipboard 时顺带放进剪切板。
	// 返回落盘路径，空串 = 一帧都没录到
	virtual std::wstring stopVideo(bool toClipboard) { return L""; }
	// 停录但把成品丢掉
	virtual void discardVideo() {}
	// 退出录屏：恢复到底图可见、鼠标归自己、工具条回来的状态
	virtual void exitVideo() {}
	// 现在是不是正在录。ToolMain / ToolSub 用它挡住"录制中把录制条收起来 / 换成设置态"的那几条路：
	// 录制中主条已经藏了，录制条是唯一的入口，一旦没了就只能杀进程（用户报的"运行态偶发丢失"）
	virtual bool inVideoMode() const { return false; }
	// 悬停探测：更新 shapeHover（可交互，只认最后一个 / 右键解开的那个）与
	// shapeShow（只用来显示手柄和虚线框）。pos 是窗口客户区坐标
	void probeHover(const POINT pos);
	// 当前工具"这一组"的最后一个未撤销标注。滑条（颜色/线宽/字号/强度）只该联动
	// 它自己那一组：拖文字的字号不能顺手把上一个矩形改粗，拖矩形线宽也不能改文字。
	// 没有同类标注时返回 nullptr —— 那就什么都不动，而不是去动别人的形状
	ShapeBase* shapeOfCurrentTool();
	// 这一条现在算不算"可操作的那一条"：只有历史里最后一个未撤销的标注
	// （或右键解锁出来的 editTarget）算。悬停、点击、画手柄全用这一个口径 ——
	// 用户定的规则："画了第一个椭圆又画第二个之后，第一个就既不能被选中也不能被编辑，
	// 想改只能回退，这套对所有形状一视同仁"。
	// 画手柄那一关尤其重要：悬停指针可能在工具条收走焦点的空档里过期，
	// 不在这里兜住，更早的标注就会把手柄亮出来（橡皮擦连画三下，第一下还带八个方块）
	bool isEditableShape(const ShapeBase* s) const;

protected:
	// 离屏合成：把底图上 src 那一块 + 所有未撤销的 shape 画进一张新位图，再读回像素。
	// WinCap（导出选区那一块）与 WinPin（导出整张贴图）共用这一套 —— GPU 目标位图、
	// Map/Unmap、逐行紧缩、alpha 补 255 每一步都踩过坑，两份分开写迟早走散。
	// src 是 screenImg 上的取样矩形；shiftX/shiftY 是画 shape 时的平移（shape 用宿主坐标）。
	// 输出格式：BGRA、top-down、行步长紧凑为 size.width*4。
	bool compositePixels(const D2D1_SIZE_U size, const D2D1_RECT_F& src,
		const float shiftX, const float shiftY, std::vector<BYTE>& pixels);

public:
	// Ctrl+滚轮的缩放倍数。WinPin 用；WinCap 恒为 1，标注直接用窗口坐标
	float scale{ 1.f };
	std::unique_ptr<ToolMain> toolMain;
	std::unique_ptr<ToolSub> toolSub;
	ShapeBase* shapeHover{ nullptr };
	// 画手柄用的那个。和 shapeHover 分开：鼠标只是从框附近路过（还没落在框上）时
	// 也该把虚线框亮出来，而"能点能拖"仍然只归最后一个（或右键解开的那一个），
	// 两者不能混成一个指针。两者都只可能指向"最后一个未撤销的标注"，
	// 由 probeHover 统一赋值、isEditableShape 统一把关
	ShapeBase* shapeShow{ nullptr };
	// 刚画完的那一条：四角/边上的小方框主动亮着，用户不必先把鼠标移过去找角。
	// 只对"最后一个未撤销的标注"生效，下一次按下、撤销/删除、重开选区都会清掉它，
	// 之后回到原来的"悬停才亮"规则（见 probeHover）
	ShapeBase* draggerPin{ nullptr };
	// 右键选中的编辑目标：默认只可编辑最后一个，右键点中它就把它解锁，
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
	ui::TextBox* textBox{ nullptr };
	ShapeText* editingText{ nullptr };
	bool isMouseDown{ false };
	POINT pressPos{ 0, 0 };
	// 自己认双击用的上一次按下时间与位置
	ULONGLONG lastDownTime{ 0 };
	POINT lastDownPos{ 0, 0 };
	// 上一次按下是不是新建了一个留得住的元素（现在只有序号）
	bool prevPressCreatedShape{ false };
};
