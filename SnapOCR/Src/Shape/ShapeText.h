#pragma once
#include <Ui/Ui.h>
#include "ShapeBase.h"

// 屏幕文字。
//
// 编辑态与非编辑态用的是两套东西：
//   编辑中  —— 文本由 WinHost 上那个共用的 ui::TextBox 画（它有自己的 swapchain，
//              光标、选区、IME 都归它），本 shape 只负责那圈虚线框；
//   编辑完 —— TextBox 隐藏，文本由本 shape 自己的 textLayout 画进 WinHost 的画布，
//              这样它才会出现在保存/复制出去的图里（导出走的是离屏 paint(ctx)）。
class ShapeText : public ShapeBase
{
public:
	ShapeText(WinHost* win);
	~ShapeText();
	void paint(ID2D1DeviceContext* ctx) override;
	void paintDragger(ID2D1DeviceContext* ctx) override;
	void mouseDrag(const float x, const float y) override;
	void mouseDown(const float x, const float y) override;
	void mouseMove(const float x, const float y) override;
	void mouseUp(const float x, const float y) override;
	void setCursor() override;
	// 单击进编辑态，留不留由 finishEdit 按文本是否为空决定，这里不能提前删
	bool isValidWithoutDrag() override { return true; };
	// 手柄（旋转柄 / 四角柄）画不画：命中手柄或框本身算，落在"框外一小圈"和
	// "框顶连到旋转柄那条走廊"上也算 —— 那两处只显示、不接点击，
	// 为的是鼠标从框上移向手柄的途中不断档（断一下框就闪掉，看着像"要点一下才有"）
	//
	// 编辑中同样挂手柄，但只认"鼠标落到虚线框上"这一档（见 handlesVisible）：
	// 刚点下去开始打字时鼠标本来就在框里，那阵子只有一圈虚线框、四角一个都不要；
	// 鼠标一移到框边/四角/顶上旋转柄上，手柄立刻出来，而且当场可拉可转 ——
	// 拉角只改字号（TextBox 换字号重排，不打断输入），转柄把角度同时推给 TextBox，
	// 它那一层跟着转，所以"输入的这段字"就是斜着长出来的，不用先收笔
	// 鼠标离开框（落到更远处）就收起全部手柄 —— 用户的口径"移开就收起来"
	//
	// 还有一档必须留一记：编辑中把鼠标移到框外一小圈（nearHover）时，只亮框、不亮手柄，
	// 命中仍按"点在框外"算（hoverDraggerIndex 置 0），下一次点击该收笔就收笔
	bool showDragger() const override
	{
		return handlesVisible() || nearHover;
	}
	// 四角缩放柄 / 右上角关闭柄：编辑中也接。它们压在文本框的角上，让出去就等于
	// "输入时角上按不动"；按下时会把这一下从 TextBox 手里要过来（swallowNextDown），
	// 缩放只动字号、关闭直接收笔，都不动光标。
	// 宿主（WinCap / WinPin）也是拿这个判断"这一下要不要交给 shape"，
	// 所以 kHitClose 必须一并算进来，否则点 × 会被 TextBox 吃掉
	bool onScaleHandle() const override
	{
		return (hoverDraggerIndex >= kHitCornerTL && hoverDraggerIndex <= kHitCornerBL)
			|| hoverDraggerIndex == kHitClose;
	}
	// 收尾：把 TextBox 里的文字取回来自己画，空文本则把自己从 history 里删掉。
	// 除了本类内部，WinHost（导出图片前）和 History（删除 shape 前）也会调。
	void finishEdit();
	// ToolSub 上的颜色/字号/粗斜体变了，编辑中的话立即生效
	void applyStyle();
public:
	bool isEditing{ false };
	// 鼠标正走在"框顶中点 → 旋转柄"那条走廊上（判定见 mouseMove）。
	// 走廊本身不接点击，但手柄要一直亮着：不然从框边往旋转柄走的那几像素里
	// 四角会闪掉一次，看着像"手柄时有时无"
	bool handleTrail{ false };
	// 绕框中心的旋转角（弧度，屏幕上顺时针为正）。0 = 不转。
	// 编辑中一样生效：编辑时字由 TextBox 那一层画，这个角度会同步推给 TextBox，
	// 让那个轴对齐的真控件自己转过来（见 syncRotationToBox），框和字始终同一个姿态
	float angle{ 0.f };
public:
	// hoverDraggerIndex 里本类自己占的那几档。0/1/8 是原来那三档
	// （编辑中移到框外 / 点在框里 / 点在边上），9 是顶上那只旋转柄，10~13 是四角
	static constexpr int kHitRotate = 9;
	// 顶上那只"旋转柄"要不要挂出来：静息是一个圆点，悬停换成白底圆盘 +
	// 系统现成的旋转字形 ↻（Segoe UI Symbol 的 U+21BB），不自绘圆弧箭头。
	// 关掉它只影响显示与命中，旋转算法（mouseDown/mouseDrag 里那两段）一直在
	static constexpr bool kShowRotateHandle = true;
	static constexpr int kHitCornerTL = 10;
	static constexpr int kHitCornerTR = 11;
	static constexpr int kHitCornerBR = 12;
	static constexpr int kHitCornerBL = 13;
	// 右上角那只角柄改成"关闭"：柄里画 ×，点一下直接把这个文本框关掉。
	// 命中码单独占一档，不落在 10~13 里 —— 不然 onScaleHandle 会把这一下
	// 当成拉角，按下那一刻就吞掉事件去量距离了
	static constexpr int kHitClose = 14;
private:
	void startEdit();
	void makeTextLayout();
	// 从 ToolSub 拉一份当前样式，并重建画刷
	void setAttr();
	// 四角/旋转柄到底要不要画。非编辑态：命中任意一档就画；
	// 编辑中：只有落在虚线框本身（框边 8、四角）或旋转柄上才画 ——
	// 落在文字区域里（1）是正在输入，不该冒手柄；更远处（0）也不该冒（见 showDragger 注释）
	bool handlesVisible() const;
	// 自己是不是 history 里最后画的那个（可见的最上层）。只让它接受手柄/× 的命中：
	// 被别的图形压住时手柄根本不画，若还认命中，就成了"看不见的按钮也能点删"
	bool isLastShape() const;
	// 把 angle 推给正在编辑的 TextBox：编辑中的字由它画，它自己转不了就永远对不上框
	void syncRotationToBox();
	// 把当前 rect / 字号推给正在编辑的 TextBox（拖角缩放时用）：
	// 字号变了 TextBox 会自己重排，位置得跟着挪回来，否则它只从左上角往下长，越拉越偏
	void applyFrameToBox();
	// 把 TextBox 的实际盒子抄进 rect（窗口坐标 → 底图坐标）。paint 和 onTextChanged 都会调
	void syncFrameFromBox();
	// 画那圈虚线框。虚线本身交给 ID2D1StrokeStyle（D2D 自带），本函数只负责
	// 把矩形内缩半个笔宽 —— 不内缩的话 1px 的描边正好压在像素缝上，会被抗锯齿
	// 摊到相邻两行、各 50% 覆盖，出来就是"发灰、断断续续"的一圈（见 .cpp 里的说明）
	void paintDashFrame(ID2D1DeviceContext* ctx);
	// 鼠标压在旋转柄上时画的那对旋转箭头：左右两段圆弧 + 上下两个箭头。
	// 静息态是圆点，悬停后不再是"圆盘里套字形"，而是箭头本身（用户口径：不要圆内加个箭头）
	void paintRotateArrows(ID2D1DeviceContext* ctx, const D2D1_POINT_2F rp);
	// 角柄里的白色图形：三个缩放的画 45° 双箭头（slope 跟角的走向一致），
	// 右上角那个关闭的画 ×
	void paintCornerGlyph(ID2D1DeviceContext* ctx, D2D1_POINT_2F cp,
		float side, bool closeButton, float slope);
	// 延迟一轮消息循环把自己从 history 摘掉。调用点都在本对象的事件回调里，
	// 当场 erase 会把 this 一起释放（见 .cpp）
	void removeLater();
	// 懒建那支 2 实 2 虚的 stroke style（第一次绘制时才建，见 .cpp）
	void ensureDashStyle();
	// —— 旋转共用的几个换算 ——
	// rect 存的是"没转过"的轴对齐盒子，所有命中判定都在这个坐标系里做；
	// 这两个函数负责在世界坐标和它之间来回换（绕中心转 ∓angle）
	D2D1_POINT_2F center() const;
	D2D1_POINT_2F toWorld(float lx, float ly) const;
	void toLocal(float x, float y, float& lx, float& ly) const;
	D2D1_POINT_2F rotHandlePos() const;      // 旋转柄圆心（世界坐标）
	D2D1_POINT_2F cornerPos(int i) const;    // i: 0=左上 1=右上 2=右下 3=左下
	float handleRadius() const;
	// 把 ctx 的变换叠上一层绕中心的旋转（angle==0 时什么都不做），画完用 restore 还原
	void applyRotation(ID2D1DeviceContext* ctx, D2D1_MATRIX_3X2_F& saved) const;
	// 对角拉大小 = 等比缩放：字号和框一起按同一比例变（字号钳在 10~180 物理像素，
	// 但不受下方滑条 10~72 逻辑像素值域的约束）
	void scaleTo(float k);
private:
	std::wstring text;
	// 物理像素。ToolSub::getSliderVal() 给的就是物理值，而 TextBox::setFontSize 收逻辑值，
	// 传过去时要除回 dpi。
	float fontSize{ 20.f };
	bool isBold{ false }, isItalic{ false };
	UINT32 colorValue{ 0 };
	D2D1_COLOR_F color{};
	// 边框盒，窗口客户区坐标、物理像素。文字画在它内缩 borderPadding 的位置。
	D2D1_RECT_F rect{};
	Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
	// 2 实 2 虚。建不出来时（无硬件加速的极端环境）是空的，DrawRectangle
	// 收到空 strokeStyle 就画实线框，功能不受影响
	Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashedStrokeStyle;
	// 文字到虚线框的间距。取 TextBox 默认的 setPadding(6.f)，两边画出来才对得上
	float borderPadding;
	float pressX{ 0.f }, pressY{ 0.f };
	// 刚点出来、鼠标还停在落笔点那一刻。此刻框只有一个字大，整块都落在四角的
	// 判定范围里，这时候判角，一按下去四角就全亮（用户不要这个）。
	// 鼠标一离开落笔点就清掉 —— 之后哪怕框还是空的、只有一个光标，四角也照常能拉
	bool freshBox{ false };
	float freshX{ 0.f }, freshY{ 0.f };
	// 编辑期间挂在 TextBox 上的两个订阅。TextBox 是共用的，退出编辑必须摘掉，
	// 否则下一个 ShapeText 编辑时会把文字写进已经结束的那个里。
	winrt::event_token textChangedTok{}, focusTok{};
	// —— 旋转的在途状态 ——
	// interacting：正按着旋转柄。paintDragger 只在"悬停且没按下"时被宿主调用，
	// 拖动过程中框不会自己重画，所以这一段时间改由 paint 里补画那圈虚线框
	bool interacting{ false };
	// 落在"框外一小圈 / 框顶到旋转柄的走廊"上：只让手柄显示出来，不当成命中。
	// 与 hoverDraggerIndex 分开是有意的 —— 那些位置点击仍然算"点在框外"，
	// 该新建文字就新建文字，不能被一个看不见的宽判定区吃掉
	bool nearHover{ false };
	float rotStartAngle{ 0.f };   // 按下时的角度
	float rotGrabAngle{ 0.f };    // 按下点相对中心的极角
	float scaleStartFont{ 0.f };  // 按下时的字号
	// 上一次"所见"的滑条值（物理像素）。拖角把字号放大到滑条上限以上时滑条定格，
	// 之后如果只是换颜色/粗体（滑条值没变），不能把字号按滑条重置回 60；
	// 真动了滑条（值变了）才让滑条接管字号
	float lastSliderPhysical{ -1.f };
	float scaleStartDist{ 1.f };  // 按下点到中心的距离（缩放比例的分母）
	D2D1_RECT_F rectStart{};      // 按下时的框：缩放全程以它为基准，不吃累计误差
	// 旋转柄的填充白（只在画手柄时用，懒建）
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushHandleFill;
};

