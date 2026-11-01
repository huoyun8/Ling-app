#pragma once
#include <Ui/Ui.h>
#include <functional>
#include "ToolMetrics.h"

class WinHost;
class Tip;
class ToolSub:public ui::WinBase
{
public:
	ToolSub(WinHost* win);
	~ToolSub();
	void showRectTools();
	void showArrowTools();
	void showNumberTools();
	void showBrushTools();
	void showTextTools();
	// 马赛克与高斯模糊是同一颗主按钮下的两档模式，都在副条里切：
	// 模式按钮（马赛克 / 高斯模糊）+ 形状按钮（选框 / 手绘）+ 强度滑块
	void showMosaicTools();
	// selecting = true 表示这一下是"点击选中"了这个工具（不是悬停预览）。
	// 马赛克/橡皮擦用它把形状一律拉回"选框"：用户定的口径"默认都是选框，
	// 需要手绘统一再点一下"。悬停预览传 false —— 只是从按钮上划过，
	// 不该把用户上次选好的手绘冲掉
	void showEraserTools(bool selecting = false);
	// 录屏（点主条上那颗录屏按钮弹出）。设置态与录制态共用这条副条，
	// 与其它工具一样"窗口贴着主条"，录制开始后主条会被宿主藏起来、这条留在原位
	void showVideoTools();
	void showVideoRecording();
	// 开始录制：主条那颗录制图标点一下直接走这里（副条那颗「开始」也是同一入口），
	// 先让宿主把录制张罗起来（建窗口/起线程/让出鼠标），成功了才切成录制态。
	// 失败（选区太小、编码器起不来）就留在设置态
	void videoStart();
	// 停录 + 保存：toClipboard=true 时落桌面并放进剪切板，否则只落桌面。都会结束整个流程。
	// 键面上的 Ctrl+S / Ctrl+C 也走这两条（宿主直接调）
	void videoSave(bool toClipboard);
	// 停录 + 丢弃（一帧都没录到也算这条），同样结束整个流程
	void videoDiscard();
	// 录制中按 ESC：丢弃这段录制并直接结束整个截图会话（一次 ESC 就退出）。
	// 录制条上的"丢弃"按钮仍走 videoDiscard —— 那条只是结束录制、留在标注态
	void videoAbort();

	void hideTools();
	// 录制中把录制条"保活"：窗口要是被谁藏了（宿主重排、别的定时器误收），
	// 这里无条件 show 回来并同步 isVisible 镜像。录制条是录制中唯一的入口，
	// 一旦没了用户就只能杀进程（用户报的"运行态偶发丢失"）
	void ensureShown();
	bool hasContent();
	float getDesiredHeight();
	void updatePosition(const RECT& workArea);
	// 当前选中的颜色，直接可喂给 CreateSolidColorBrush。颜色是每工具各存一份的
	// （见 beginTool），切工具时颜色按钮会重建并按配置文件里那一份重新选中。
	// mosaic/eraser 没有颜色按钮，它们的图形也用不到颜色。
	D2D1_COLOR_F getSelectedColor() const;
	// 同一个颜色的 RGBA 原值。ui::Color 没有从 D2D1_COLOR_F 构造的口子，
	// TextBox::setColor 这类要 ui::Color 的地方得用它。
	UINT32 getSelectedColorValue() const;
	// 序号第 val 个徽章该用的颜色：双色时按奇偶交替（1 色1、2 色2、3 色1 …），单色时永远用色1
	D2D1_COLOR_F getNumberColor(int val) const;
	// 滑块当前值（逻辑像素语义，用作线宽/字号等；D2D 里当物理像素用的话记得乘 dpi）。
	// 值存在 sliderVal 里而不是问 Slider 节点要 —— 节点每次切换工具都被销毁重建。
	float getSliderVal() const;
	// 该工具滑块的最大值（逻辑像素）。拖四角放大字号时可以超过它，
	// 此时滑条定格在最大、字号不再被夹回值域内，所以调用方要能问到这条线
	float sliderMaxOf(const std::wstring& tool) const;
	// 在图形上滚滚轮改了尺寸之后回填过来（序号的圆半径、矩形/椭圆的线宽），入参是物理像素。
	// 返回夹到该工具滑块值域内的物理像素值 —— 调用方拿它当最终尺寸，图形就不会滚出滑块的范围。
	// 正显示着这个工具的工具条时滑块跟着动，顺带落盘；滚的是别的工具画的图形时只更新配置。
	float setShapeSliderVal(const std::wstring& tool, float px);
	// ToolMain 与 ToolSub 之间的间距，WinHost::layoutTools() 计算整组高度时要用
	static constexpr float mainGap{ ToolMetrics::mainGap };
	// 窗口比"可见框"多出来的那一截（顶上留给箭头）。窗口顶边靠它上移，这段落进
	// 主副条之间的空隙里 —— 箭头不计进副条尺寸，可见框与主条等高。
	// WinHost 算整组高度时要把它从空隙里扣掉，所以放在 public
	static constexpr float arrowH{ ToolMetrics::arrowZone };
public:
	// isEllipseShape：矩形工具当前画的是椭圆(true)还是矩形(false)。矩形/椭圆已合并成一个工具。
	// isRectSolid / isEllSolid：矩形与椭圆的"是否填充"各自独立，互不影响（点矩形只切矩形的填充，点椭圆只切椭圆的填充）。
	bool isRectSolid{ false }, isEllSolid{ false }, isEllipseShape{ false }, isArrowSolid{ true }, isTextBold{ false }, isTextItalic{ false }, isEraserMarquee{ true };
	// 效应三选一（配置层只有 bool 键，所以用两个布尔表示）：
	// false/false = 马赛克、true/false = 高斯模糊、false/true = 智能消除，三者互斥。
	// ShapeMosaic 建的时候读它们决定用哪种像素算法
	bool isMosaicBlur{ false }, isMosaicErase{ false };
	// 0 = 马赛克 / 1 = 高斯模糊 / 2 = 智能消除
	int mosaicEffect() const { return isMosaicErase ? 2 : (isMosaicBlur ? 1 : 0); }
	// 形状偏好（true 选框 / false 手绘）：马赛克与高斯模糊共用这一份。
	// 切效应工具只换算法，不碰它 —— 原来两档各记一份，切模式时形状按钮跟着翻，看着像"顺手改了画法"
	bool isMosaicMarquee{ true };
	// 序号徽章 4 种样式 = 圆/方 × 实心/空心，三个布尔承载：
	// isNumberSquare = 当前形状（false 圆 / true 方）；isNumCircleSolid / isNumSquareSolid
	// 是圆、方各自的填充态（点当前形状才切它的填充，与矩形/椭圆同口径）。
	// 单色/双色不再是开关 —— 色板上有没有第二个颜色（hasColor2）就是答案：默认单色，
	// 点一个新颜色就变成双色，再点已亮的那格就回到单色（见 onColorSelect）
	bool isNumberSquare{ false };
	bool isNumCircleSolid{ true }, isNumSquareSolid{ true };
	// 箭头工具两个形状按钮共用的二选一状态：false = 箭头，true = 直线。
	// 与矩形/椭圆同一条口径 —— 箭头是当前形状时点它切实心/空心，是直线时点它只切回箭头
	bool isArrowStraight{ false };
	// 画笔的两项状态：半透明、画法（false = 手绘，true = 选框）。
	// 选框画的是"填充框"（实心矩形），与矩形的填充开关无关
	bool isBrushSemiTransparent{ false }, isBrushMarquee{ false };
private:
	void onCreated() override;
	void layout() override;
	void onMinMaxInfo(MINMAXINFO* mmi);
	void paintBorder(ID2D1DeviceContext* ctx);
	void onColorSelect(ui::Button* btn);
	// 用户在副编辑器做了实质操作（点颜色 / 拖滑块 / 切开关 / 切形状 / 切模式）后调用：
	// 让主编辑器把当前工具从"hover 预览"提升为"选中"，用户不必再回主条点一次
	void notifySelected();
	// 工具按钮的尺寸 / 留白 / 字号只在这里定一次（全部来自 ToolMetrics）。
	// 各工具建按钮时一律走它，不允许再单独 setHeight(btnSize - 2.5) 之类的特例数字 ——
	// 那正是"有的按钮对、有的不对"的来源
	void applyButtonMetrics(ui::Button* btn, float fontSize);
	// 副条上"一格一颗"的纯图标按钮（录屏条那两颗音源 / 开始 / 丢弃 / 存文件 / 存剪切板）。
	// 设置态与录制态建的是同一种按钮，两处共用这一份，别各写一遍
	ui::Button* makeIconBtn(const wchar_t* icon);
	// 副条里的分隔竖线（1 逻辑像素宽、撑满整行）
	void makeSpliter();
	void initColorBtns();
	// 颜色按钮上的对勾：按当前选中的一、二号色重画（序号双色时两格都打勾）。
	// 建按钮时与点颜色/切单色双色后都走它，避免对勾状态散在多处
	void syncColorMarks();
	void initSlider();
	// 选中/未选中两套配色，与 ToolMain 的选中效果保持一致
	void applyToggleStyle(ui::Button* btn, bool selected);
	// 建一个可切换的工具按钮：初始态从配置文件读（写回 flag），点击时翻转 flag、刷新配色并落盘。
	// tipKeyOn / tipKeyOff 是开 / 关两种状态下各自的提示文字语言键（如 tool.rectSolid / tool.rectOutline），
	// 鼠标悬停时看到的是"当前是什么样"，不是笼统的"矩形填充"。
	// cfgKey 是这个开关在 config.json 里的键名（fill / semiTransparent / bold / …，同一工具下不能重名）。
	// onText/offText 是开关两种状态下分别显示的字形（开启用 onText，关闭用 offText）：
	// 这样图标能随状态切换（实心=开、空心=关），而不只是选中变色。不需要区分时两者传同一字形。
	// gate：点击时先问一句"这一下该不该翻转"。返回 false 表示拦下这一下（开关状态、图标、
	// 画布都不动，由 gate 自己去做它认为该做的事）。矩形/椭圆两个按钮就靠它实现
	// "第一次点先换形状，第二次点才切实心/空心"
	ui::Button* makeToggleBtn(const std::wstring& onIcon, const std::wstring& offIcon, bool* flag,
		const std::wstring& tipKeyOn, const std::wstring& tipKeyOff, const std::wstring& cfgKey,
		const std::wstring& legacyCfgKey = L"", float fontSize = ToolMetrics::iconFont,
		std::function<bool()> gate = nullptr);
	// 矩形/椭圆的填充开关：与其它工具一样走 makeToggleBtn（点一下切实心/空心）。
	// 按钮上另外挂了一个 selectShape，顺手把"当前画哪个形状"记下来
	void selectShape(bool ellipse);
	// 箭头两个形状按钮（箭头 / 直线）的字形与选中态同步，见 .cpp
	void syncArrow();
	// 序号两颗形状按钮（圆 / 方）的字形与选中态，按 isNumberSquare 与两个填充态刷新
	void syncNumber();
	// 点圆/方按钮时同步"当前画哪种徽章"（圆 ↔ 方），画布上最后一条序号也跟着换，
	// 与矩形/椭圆的 selectShape 同一条口径
	void selectNumberShape(bool square);
	// 画布上"最后一条"序号就地换成当前样式（形状 + 填充 + 单色/双色的配色），与滑条/颜色联动同一口径
	void applyNumberVariant();
	// syncRectEll 按当前形状点亮选中态、按填充态刷新两个按钮字形。
	void syncRectEll();
	// 马赛克与高斯模糊共用一条副条：马赛克 / 高斯模糊两颗按钮 + 形状按钮（选框/手绘）。
	// 三颗模式按钮的选中态由 refreshEffectMode 刷新，形状按钮字形由 refreshEffectShape 刷新
	void refreshEffectMode();
	void refreshEffectShape();
	// 每个 show*Tools 开头都要做的事：收提示、清旧内容、把指向被销毁子节点的指针全部作废、
	// 记下当前工具，再把这个工具存在 config.json 里的滑块值和颜色读回来（读不到就用默认值 /
	// 第一个颜色）。滑块的键名与值域查 .cpp 里那张表，id 必须是表里有的（就是 ToolMain 的按钮 id）。
	// 返回 false = 内容区建不出来（onCreated 没跑到 / 建窗失败），调用方要一并早退 ——
	// 只在这里早退拦不住，紧接着的 makeChild 就是空指针解引用
	bool beginTool(const std::wstring& id);
	// 内容区即将被整体清空：把指向那些子节点的成员指针一次全部作废。
	// 清单与"建按钮时赋过值的成员"严格对应，beginTool 与 showVideoRecording 共用一份 ——
	// 分两处各写一遍的话，迟早会漏掉一个变成野指针（slider 就是这么被 mousemove 踩到的）
	void forgetNodes();
	// 按内容算出窗口尺寸并应用。btnCount 只数工具按钮，不含颜色按钮。
	// centerOnBtn 为 true 时窗口居中对齐到 ToolMain 上选中的那个按钮，否则与 ToolMain 左对齐。
	// sepCount：这条副条里有几根分隔条（马赛克那条有一根），宽度要算进去
	void initSize(int btnCount, bool withColors, bool centerOnBtn = false, int sepCount = 0);
	// 内容不变、只是 dpi 变了：按上次 initSize 的入参重算一遍尺寸
	void refreshSize();
	// 录屏条不按"格子 + 滑块"算宽度（那条没有滑块，MP4/GIF 是 42 宽的文字块，
	// 计时是 112 宽的 Label），这里直接给内容总宽；refreshSize 走自定义分支重算
	void initSizeCustom(float contentW);
	// 按 videoMp4 / videoSpeaker / videoMic 刷新三颗按钮的选中态与配色（与原录屏条一致）
	void videoApplyFormatStyle();
	// 格式 / 音源三项一起落盘（工具状态，与马赛克形状、滑块值同口径，键都在 video 组下）。
	// 调用点都是"一次操作改了其中一至三项"，三个都写省得各处数自己改了几个
	void persistVideoOpts();
	// 计时文字：GIF 上限 2 分钟、MP4 上限 10 分钟（到点自动存盘收工，见构造函数里的 kVideoTimer）
	void videoUpdateTimer();
	// 逻辑像素 → 物理像素
	float toPx(float logical) const;
private:
	ui::Node* contentNode;
	std::vector<ui::Button*> colorBtns;
	// 当前的滑块。切换工具时会被销毁重建，重建后由 initSlider 重新赋值。
	// 存下来是为了在窗口的 onMouseMove 里判断鼠标是否在它上面，好显示数值提示。
	// **beginTool 清空内容区时必须把它置空**：录屏条这条副条根本没有滑块，不清就会留着
	// 上一颗工具的野指针，鼠标一进副条（窗口级 mousemove 每动一下就跑）就是 UAF
	ui::Slider* slider{ nullptr };
	// 悬停提示。要 hwnd，所以在 onCreated 里才建得起来
	std::unique_ptr<Tip> tip;
	// 尺寸与字号全部来自 ToolMetrics（主副编辑条共用一套规则）。
	// 这里不再出现 32 / 29.5 / 13 / 15 这种数字 —— 要调就调单元格那一个值
	static constexpr float btnSize{ ToolMetrics::cell };   // 一格按钮的边长
	static constexpr float sliderSize{ ToolMetrics::sliderW };
	static constexpr float sliderMargin{ ToolMetrics::sliderPad };
	static constexpr float marginTop{ ToolMetrics::arrowZone };   // 顶部箭头区域高度
	static constexpr float borderW{ ToolMetrics::border };
	// 箭头尖端相对窗口左边的偏移，由 updatePosition 按屏幕坐标算出，是物理像素
	float arrowX{0.f};
	WinHost* win;
	// 背景/边框画刷缓存：layout() 每次刷新都会调 paintBorder，别在里面重复建
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg, brushBorder;
	// 铺满窗口的画布，画的是背景与带箭头的边框，按钮都在 contentNode 上，盖在它上面
	ui::Canvas* canvas{ nullptr };
	bool isVisible{ false };
	bool hasTools{ false };
	bool centerOnBtn{ false };
	// 收起顶上那个指向主条按钮的小三角。录制态用：那时主条是藏着的，
	// 录制条也不再指着某颗按钮（它改成贴选区右下角了），留着箭头只会指向虚空
	bool noArrow{ false };
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };
	// 上一次 initSize 的入参，DPI 变了要照原样再算一遍尺寸
	int sizeBtnCount{ 0 };
	bool sizeWithColors{ false };
	int sizeSepCount{ 0 };
	// 录屏条走的是自定义宽度（见 initSizeCustom）：DPI 变了照 customW 重算
	bool sizeCustom{ false };
	float customW{ 0.f };
	// —— 录屏 ——
	// 格式（MP4=true / GIF=false）、是否录系统声 / 麦克风、已录秒数。
	// 这三项只在设置态改，录制中要按它们决定编码器与计时上限。
	// 默认按用户口径：MP4 + 系统声开 + 麦克风关
	bool videoMp4{ true }, videoSpeaker{ true }, videoMic{ false };
	int videoSec{ 0 };
	// 设置态的格式按钮（MP4 / GIF 两颗，互斥选中）与两颗音源按钮，与录制态的计时 Label：
	// 每次重建都会换新指针，切换状态时先在这里作废（removeAllChildren 会把旧节点销毁）
	ui::Button* vMp4{ nullptr }, * vGif{ nullptr }, * vSpeaker{ nullptr }, * vMic{ nullptr };
	ui::Label* videoTimer{ nullptr };
	// 计时器 id：录制中每秒 +1
	static constexpr UINT kVideoTimer = 106;
	// 录制中 ESC 的轮询定时器：宿主这时是"分层 + 穿透"窗口，点一下桌面就丢了焦点，
	// WM_KEYDOWN 根本不来，ESC 必然收不到。只能自己按 GetAsyncKeyState 兜底
	static constexpr UINT kVideoEscTimer = 107;
	// 轮询间隔（毫秒）：越短越不会漏掉"极快的一下"。50ms 实测能漏掉，
	// 再配合 GetAsyncKeyState 的"按下过"低位，这个间隔足够稳
	static constexpr UINT kVideoEscPoll = 30;
	// ESC 上一个轮询周期的按下状态，用来做边沿触发（按住不放不反复停录）
	bool escDown{ false };
	UINT selectColorIndex{ 0 };
	// 序号的两个颜色是**有序的**：奇数号用色1、偶数号用色2。
	// selectColorIndex 是色1（单色时就是唯一的那个颜色），selectColor2Index 是色2；
	// hasColor2 表示色板上是否真的亮着两个颜色 —— 没亮就是单色（默认即单色）。
	// 最多亮两格，按"先进先出"滑动：依次点 1、2、3 后保留 2、3；再点 4 保留 3、4。
	// 点已亮的那格 = 取消勾选它（对勾 toggle），另一格独自亮着回到单色；
	// 只剩一格时再点不动 —— 至少得留一色
	UINT selectColor2Index{ 0 };
	bool hasColor2{ false };
	// 滑块值。每次切换工具都由 beginTool 从 config.json 里换成那个工具自己的那份。
	float sliderVal{ 2.f };
	float sliderMin{ 1.f }, sliderMax{ 20.f };
	// 当前工具在 config.json 里的组名（即 ToolMain 的按钮 id）与滑块值的键名，
	// 落盘时要用。没选工具时是空的，此时不该有任何写入。
	std::wstring curToolId, curSliderKey;
	// 矩形/椭圆工具的两个按钮：图标随实心/空心联动，syncRectEll 用来刷新字形与选中态
	ui::Button* rectBtn{ nullptr }, * ellBtn{ nullptr };
	// 箭头工具的两颗形状按钮：箭头（随实心/空心换字形）、直线
	ui::Button* arrowBtn{ nullptr }, * arrowLineBtn{ nullptr };
	// 序号的两颗形状按钮：圆序号 / 方序号（互斥选中，图标随各自填充态换字形）
	ui::Button* numCircleBtn{ nullptr }, * numSquareBtn{ nullptr };
	// 马赛克副条的三颗按钮：马赛克、高斯模糊（互斥，点哪颗用哪颗）、形状（选框 / 手绘）
	ui::Button* effectMosaicBtn{ nullptr }, * effectBlurBtn{ nullptr }, * effectEraseBtn{ nullptr }, * effectShapeBtn{ nullptr };
	std::vector<UINT32> colors = { 0XCF1322FF, 0XD48806FF, 0X389E0DFF, 0X13C2C2FF, 0X0958D9FF, 0X722ED1FF, 0XEB2F96FF, 0X000000FF, 0XFFFFFFFF };
};