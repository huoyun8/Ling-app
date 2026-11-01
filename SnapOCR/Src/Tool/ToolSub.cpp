#include "pch.h"
#include "../Win/WinHost.h"
#include "../History.h"
#include "../Shape/ShapeBase.h"
#include "../Shape/ShapeMosaic.h"
#include "../Shape/ShapeEraser.h"
#include "../Shape/ShapeNumber.h"
#include "../Lang.h"
#include "../Setting.h"
#include "../Tip.h"
#include "ToolSub.h"
#include "../Res/Icons.generated.h"
#include "ToolMain.h"

using namespace Microsoft::WRL;

namespace {
	// 每个工具的滑块：值在 config.json 里的键名、值域、默认值（都是逻辑像素）。
	// 键名各工具语义不同 —— 矩形/椭圆/箭头/画笔是线宽，序号是圆半径，文字是字号。
	// 集中放这一张表，是因为值域有两处要用：beginTool 建滑块，以及在图形上滚滚轮
	// 改尺寸时（setShapeSliderVal）夹值 —— 分开写迟早会对不上。
	struct SliderCfg {
		const wchar_t* key;
		float min, max, def;
	};
	const std::pair<const wchar_t*, SliderCfg> sliderCfgs[]{
		{ L"rect",    { L"width",     1.f, 26.f,  2.f } },
		// 箭头与直线共用这一条：上限放到 32，直线才够画得和画笔一样粗
		{ L"arrow",   { L"width",     1.f, 32.f,  5.f } },
		{ L"number",  { L"radius",    6.f, 86.f, 9.f } },
		{ L"brush",   { L"width",     1.f, 60.f, 12.f } },
		// 文字字号：滑条值域 10~72，新建文本默认 12（拖四角可超过 72，滑条定格在最大）
		{ L"text",    { L"fontSize", 10.f, 72.f, 12.f } },
		// 马赛克与高斯模糊是同一颗主按钮下的两档模式，共用这一组强度值
		{ L"mosaic",  { L"width",    18.f, 68.f, 28.f } },
		{ L"eraser",  { L"width",    18.f, 68.f, 28.f } },
	};
	// 找不到就返回 nullptr：调用方传的都是本文件里的字面量或图形自己的工具名，
	// 真没命中说明表漏了一项，此时什么都不做比崩掉或按错值域夹要好
	const SliderCfg* findSliderCfg(const std::wstring& tool)
	{
		for (auto& [id, cfg] : sliderCfgs) {
			if (tool == id) return &cfg;
		}
		return nullptr;
	}

	// 录屏条不按"格子 + 滑块"排版，只有录制态的计时块不是格子内容，单独给宽度（逻辑像素）：
	//   · MP4 / GIF / 系统声 / 麦克风 / 开始 全部按格子排（一格一颗，与工具按钮同宽同高）
	//   · 录制态的计时是一块 112 宽的 Label
	constexpr float videoTimerW{ 112.f };

	// 序号双色的两个颜色按"选中的先后顺序"排：先点在先、后点在后
}

ToolSub::ToolSub(WinHost* win) :ui::WinBase(), win(win)
{
	// 跟着宿主窗口的缩放走：WinBase 构造里取的是系统 dpi，WinHost 可能在另一块缩放比例不同的屏上
	dpi = win->dpi;
	// 点按钮/滑块会把 ToolSub 激活，此后键盘消息进的是它而不是 WinHost。
	// 直接把按键转触给 WinHost 的同名事件，否则焦点在副编辑器时 ESC 等快捷键会失效
	onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
	// 录制中的计时：每秒 +1，到上限自动存盘收工（与原录屏条一致）
	onTimer.add([this](UINT id) {
		// ESC 兜底：录制中宿主是"分层 + 穿透"窗口，点一下桌面就丢了焦点，
		// WM_KEYDOWN 不会再来，键盘那条路（WinCap::onKey）等于失效。
		// 这里按边沿触发轮询 —— 按下那一刻才算一次，按住不放不会反复停录。
		// 窗口被藏起来也照样收 WM_TIMER，所以录制条"偶发丢失"时这条仍然有效
		if (id == kVideoEscTimer) {
			const SHORT st = GetAsyncKeyState(VK_ESCAPE);
			const bool down = (st & 0x8000) != 0;
			// 低 0x0001 位 = "上一次调用之后按下过"：按得很快（按下与抬起都落在
			// 一个轮询间隔里）时高位的瞬时状态会漏掉，这一位不会
			const bool pressed = (st & 0x0001) != 0 || (down && !escDown);
			escDown = down;
			if (pressed) videoAbort();
			return;
		}
		if (id != kVideoTimer) return;
		videoSec += 1;
		videoUpdateTimer();
		// MP4 最长 10 分钟、GIF 最长 2 分钟：到点自动存盘收工
		const int maxSeconds = (videoMp4 ? 10 : 2) * 60;
		if (videoSec >= maxSeconds) {
			videoSave(false);
			return;
		}
		// 每秒顺带保活一次：位置、置顶层级、可见性都重新落实。
		// 录的过程中选区/显示器/别的窗口层级都可能变，录制条一旦沉到宿主下面
		// 或被谁藏掉，用户看到的就是"运行态没了"（而且没有任何入口能再叫它出来）
		this->win->layoutTools();
	});
	// DPI 变了（工具条被挪到缩放比例不同的显示器上，或者用户改了系统缩放）：
	// Ui 层只会把窗口按系统给的建议矩形整体缩放一遍，我们自己定的那套摆放规则不会重跑，
	// 工具条就歪在别处了。位置也不能在 onDpiChanged 里直接改 —— 那个事件在 Ui 层应用建议矩形
	// 之前触发，改了马上被覆盖，所以这里只记个标记，等建议矩形应用后紧随而来的 WM_SIZE 再动手
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (!dpiChanged) return;
		dpiChanged = false;
		refreshSize();                    //宿主的摆放规则要用宽高，先按新 dpi 把尺寸定下来
		this->win->layoutTools();
	});
	// 工具栏不参与激活：编辑文本时点一下颜色/字号，WinHost 不该因此丢掉键盘焦点
	// （丢焦点 = WM_KILLFOCUS = TextBox 失焦 = 编辑被打断）。
	// 副条要留在置顶层，否则会被截图窗的压暗层盖住（试过：等于"副编辑器没了"）。
	// 常显的主条才去掉 TOPMOST（见 ToolMain）——它飘在别的程序上面正是那条常显的条
	createNativeWindow(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

ToolSub::~ToolSub()
{
}

void ToolSub::onCreated()
{
	tip = std::make_unique<Tip>(this);
	auto d2d = ui::D2D::get();
	// 画刷与设备（而非某次 BeginDraw 拿到的 context）绑定，建一次就够，paintBorder 每帧复用
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0xA8A8A8), brushBorder.GetAddressOf());
	// 画布先建，才排在 contentNode 之前，按钮才不会被背景盖住
	canvas = body->makeChild<ui::Canvas>();
	canvas->setPositionType(ui::Position::Absolute);
	canvas->setSizePercent(100.f, 100.f);
	contentNode = body->makeChild<ui::Node>();
	contentNode->setPositionType(ui::Position::Absolute);
	// 内容区严格按主条的口径：左右顶到底、上下也顶到窗口边（唯一例外是顶部让开箭头那 2px）。
	//
	// 以前四边各让开 1px 描边、上边再让 1px，把内容区压成 30 高，于是在 29 高的按钮上
	// 只剩 0.5 的余量 —— 而主条那边描边是"画在格子上"、不占布局，内容区是整整 34，
	// 同样 29 高的按钮上下各留 2.5。两边差这 2px，看起来就是"副条的图标外框比主条小一圈"。
	// Node 的 border 本来就不参与布局（只是沿外沿描一条线），这里跟着主条一样不让位，
	// 内容区 = 窗口高 - 箭头区 = 34，与主条完全一致；描边该压住的 1px 落在留白里，正好
	contentNode->setPosition(ui::Edge::Left, 0.f);
	contentNode->setPosition(ui::Edge::Top, marginTop);
	contentNode->setPosition(ui::Edge::Right, 0.f);
	contentNode->setPosition(ui::Edge::Bottom, 0.f);
	contentNode->setFlexDirection(ui::FlexDirection::Row);
	// 按钮本体比格子矮（四边各留 pad），要显式居中，否则默认 stretch 会把它们顶在上沿，
	// 上留白 0、下留白一整段 —— 这就是以前副条按钮看着比主条偏上的原因
	contentNode->setAlignItems(ui::Align::Center);
	// 滑块的数值提示：Slider 没有 hover 事件，就在窗口级 mousemove 里自己判位置。
	// 显示出来后横向跟着鼠标走（同 2.4.25）；离开滑块只收自己这一个提示，
	// 不能无条件 hide()：颜色按钮的 onLeave 排在这个回调后面，会把它刚显示出来的提示误关掉。
	onMouseMove.add([this](POINT pos) {
		if (!slider) return;
		if (slider->isPosIn(pos)) {
			// 显示的是鼠标底下那个位置的值，不是当前值 —— 光标只是在滑轨上路过时 value 并没变
			tip->showAt(slider, static_cast<float>(x + pos.x), y + slider->y + Tip::anchorInset * dpi,
				std::format(L"{}", static_cast<int>(std::round(slider->getValueAt(static_cast<float>(pos.x))))));
		}
		else {
			tip->hide(slider);
		}
	});
}

// 内容区里的子节点马上要被整体销毁：把所有还指着它们的成员指针作废。
// 这张清单必须与"建按钮时赋过值的成员"一一对应 —— 漏一个，下一次用到它的地方
// 就是拿野指针调成员函数。最典型的是 slider：窗口级 mousemove 每动一下都会读它，
// 而录屏条那条副条根本没有滑块（切过去之后旧滑块已经销毁了）
void ToolSub::forgetNodes()
{
	slider = nullptr;
	// 只有带颜色的工具才重建 colorBtns，不清就留着上一批的野指针
	colorBtns.clear();
	rectBtn = nullptr;          // 矩形/椭圆
	ellBtn = nullptr;
	arrowBtn = nullptr;         // 箭头/直线
	arrowLineBtn = nullptr;
	numCircleBtn = nullptr;     // 序号的两颗形状按钮（圆 / 方）
	numSquareBtn = nullptr;
	effectMosaicBtn = nullptr;  // 马赛克 / 高斯模糊 / 智能消除 / 形状
	effectBlurBtn = nullptr;
	effectEraseBtn = nullptr;
	effectShapeBtn = nullptr;
	vMp4 = nullptr;             // 录屏条的格式 / 音源 / 计时
	vGif = nullptr;
	vSpeaker = nullptr;
	vMic = nullptr;
	videoTimer = nullptr;
}

// 各工具在 config.json 里的那一组（组名 = ToolMain 的按钮 id）就是唯一的状态存放处：
// 切到某个工具时从里面读，用户一改就写回去，所以内存里不用再留一份每工具的状态。
//
// 返回值 = "内容区可用、后面可以继续建按钮"。contentNode 是 onCreated 建的：建窗失败
// （组合器抛异常 / CreateWindowEx 失败）时它是空的，而 showRectTools 这类入口仍可能被
// ToolMain 的按钮回调调到 —— 只在函数里早退是拦不住的，调用方紧接着就会拿它建子节点，
// 所以把结果交回给调用方，让它们一起早退
bool ToolSub::beginTool(const std::wstring& id)
{
	// contentNode 是 onCreated 建的：建窗失败（组合器抛异常 / CreateWindowEx 失败）时它是空的，
	// 而 showRectTools 这类入口仍可能被 ToolMain 的按钮回调调到 —— 只在函数里早退是拦不住的，
	// 调用方紧接着就会拿它建子节点，所以把结果交回给调用方，让它们一起早退。
	// contentNode 非空则 onCreated 一定跑过，tip（同在 onCreated、先建）也一定非空，后面直接用
	if (!contentNode) return false;
	// 按钮和滑块马上要被销毁，onLeave 不会触发，提示得手动收掉；再把这批按钮上的
	// 提示订阅摘掉 —— 绑定表里存的是裸指针，反过来（先销毁再摘）就是 use-after-free
	tip->hide();
	tip->unbindAll();
	forgetNodes();
	contentNode->removeAllChildren();
	noArrow = false;
	curToolId = id;
	auto cfg = findSliderCfg(id);
	if (!cfg) return true;      // 表里没有（录屏条）：这个工具没有滑块，内容照样要建
	curSliderKey = cfg->key;
	sliderMin = cfg->min;
	sliderMax = cfg->max;
	auto setting = Setting::get();
	// 夹一遍值域：配置文件可能是上个版本写的（值域变过），也可能被手工改坏，
	// 而这个值会直接当线宽/字号喂给 D2D，超出范围要么看不见要么慢得离谱
	// 字号不跨启动记忆：每次开程序，第一次点文本工具时字号都是默认的 12，
	// 之后在本程序里无论怎么拖滑条/拉四角，都只在本会话内记住
	if (id == L"text" && cfg->key == L"fontSize") {
		sliderVal = cfg->def;
	} else {
		sliderVal = std::clamp(setting->getToolNum(id, cfg->key, cfg->def), cfg->min, cfg->max);
	}
	// colors[selectColorIndex] 那几处都不做边界检查，越界就读到界外了
	auto idx = static_cast<UINT>(setting->getToolNum(id, L"colorIndex", 0.f));
	selectColorIndex = idx < colors.size() ? idx : 0;
	// 第二颜色只有序号用得到。config 里没有 colorIndex2（存的是 -1）就表示
	// "色板上没亮第二格"，此时按单色画（默认即单色）。
	// 与先色撞色也按没加处理 —— 两个格子选同一个颜色没有意义
	hasColor2 = false;
	selectColor2Index = selectColorIndex;
	const float idx2f = setting->getToolNum(id, L"colorIndex2", -1.f);
	if (id == L"number" && idx2f >= 0.f) {
		const auto idx2 = static_cast<UINT>(idx2f);
		if (idx2 < colors.size() && idx2 != selectColorIndex) {
			selectColor2Index = idx2;
			hasColor2 = true;
		}
	}
	return true;
}

void ToolSub::showRectTools()
{
	// 矩形/椭圆合并为一个工具，两个按钮。填充开关不再手写：直接走 makeToggleBtn
	// （箭头、序号、画笔用的都是它）—— 点一下即切换实心/空心、换图标、落盘、通知画布重绘，
	// 与其它工具的开关走完全同一条路径。以前这两个是手写的，只换了图标没通知画布，
	// 于是"点了实心/空心，画面不动"。
	if (!beginTool(L"rect")) return;
	initSize(2, true);
	// 上次退出前的填充态（矩形/椭圆各自独立）/ 形状从配置文件读回来。
	// 矩形填充向后兼容旧的 "fill" 键；椭圆用独立的 "ellFill" 键。
	isRectSolid = Setting::get()->getToolFlag(curToolId, L"rectSolid",
		Setting::get()->getToolFlag(curToolId, L"rectFill",
			Setting::get()->getToolFlag(curToolId, L"fill", isRectSolid)));
	isEllSolid = Setting::get()->getToolFlag(curToolId, L"ellSolid",
		Setting::get()->getToolFlag(curToolId, L"ellFill", isEllSolid));
	isEllipseShape = Setting::get()->getToolFlag(curToolId, L"ellipse", isEllipseShape);
	// 第1个：矩形。开 = shape-rect-solid（实心），关 = shape-rect-outline（空心）
	// 点法是两段式（用户定的口径）：
	//   · 点的是**另一种**形状 → 这一下只换形状（画布上最后一条也跟着变），填充不动；
	//   · 点的是**当前**形状 → 才切它的实心/空心。
	// 以前一颗按钮一次点击既换形状又翻填充，从圆切到方框时方框的填充会被顺手翻一下，
	// 形状和填充一起变，分不清是哪个在动
	rectBtn = makeToggleBtn(ui::icons::ShapeRectSolid, ui::icons::ShapeRectOutline,
		&isRectSolid, L"tool.rectSolid", L"tool.rectOutline", L"rectSolid", L"rectFill", ToolMetrics::iconFont,
		[this] { if (isEllipseShape) { selectShape(false); return false; } return true; });
	// 第2个：椭圆。开 = shape-ellipse-solid，关 = shape-ellipse-outline。
	// 两个填充态各记各的键，互不影响
	ellBtn = makeToggleBtn(ui::icons::ShapeEllipseSolid, ui::icons::ShapeEllipseOutline,
		&isEllSolid, L"tool.ellipseSolid", L"tool.ellipseOutline", L"ellSolid", L"ellFill", ToolMetrics::iconFont,
		[this] { if (!isEllipseShape) { selectShape(true); return false; } return true; });
	syncRectEll();
	initSlider();
	initColorBtns();
}

// 点矩形/椭圆按钮时同步"当前画哪个形状"。形状没变就什么都不做 ——
// 那一下点击的语义只是切换填充，别再重刷一遍按钮
void ToolSub::selectShape(const bool ellipse)
{
	if (isEllipseShape == ellipse) return;
	isEllipseShape = ellipse;
	Setting::get()->setToolFlag(curToolId, L"ellipse", isEllipseShape);
	syncRectEll();
	// 更聪明的一步：画布上最后一条若是另一种形状（矩形 ↔ 椭圆），原地换成这一种。
	// 用户的口径："画了方框，点椭圆的时候能变成椭圆"—— 联动不止改样式，形状本身也跟着换
	if (win->history) win->history->convertLastRectEllipse(ellipse);
	// 换了形状，滑条/颜色之后要落的那个目标也换了，通知画布按新形状重画最后一个
	win->onToolStyleChanged();
	notifySelected();
}

// 按当前形状点亮对应的形状按钮（选中态），并按各自的填充态刷新两个按钮的字形
// （矩形：shape-rect-solid / shape-rect-outline，读 isRectSolid；
//  椭圆：shape-ellipse-solid / shape-ellipse-outline，读 isEllSolid）
void ToolSub::syncRectEll()
{
	if (rectBtn) {
		rectBtn->setIcon(isRectSolid ? ui::icons::ShapeRectSolid : ui::icons::ShapeRectOutline);
		applyToggleStyle(rectBtn, !isEllipseShape);
	}
	if (ellBtn) {
		ellBtn->setIcon(isEllSolid ? ui::icons::ShapeEllipseSolid : ui::icons::ShapeEllipseOutline);
		applyToggleStyle(ellBtn, isEllipseShape);
	}
}

// 箭头工具：两颗形状按钮，箭头 / 直线 二选一（与矩形/椭圆的形状按钮同一条口径）
void ToolSub::showArrowTools()
{
	if (!beginTool(L"arrow")) return;
	initSize(2, true);
	// 实心/空心、画的是箭头还是直线，都从配置读回来
	isArrowSolid = Setting::get()->getToolFlag(curToolId, L"arrowSolid",
		Setting::get()->getToolFlag(curToolId, L"fill", isArrowSolid));
	isArrowStraight = Setting::get()->getToolFlag(curToolId, L"arrowStraight",
		Setting::get()->getToolFlag(curToolId, L"line", isArrowStraight));
	// 第1颗：箭头。实心/空心走 makeToggleBtn，与矩形/椭圆/序号同一条路径；
	// 当前是直线时，这一下只切回箭头（不顺手翻填充），与矩形/椭圆的"两段式"一致
	arrowBtn = makeToggleBtn(ui::icons::ShapeArrowSolid, ui::icons::ShapeArrowOutline,
		&isArrowSolid, L"tool.arrowSolid", L"tool.arrowOutline", L"arrowSolid", L"fill", ToolMetrics::iconFont,
		[this] {
			if (!isArrowStraight) return true;
			// 直线 → 箭头：只换当前形状，填充态原样保留
			isArrowStraight = false;
			Setting::get()->setToolFlag(curToolId, L"arrowStraight", false);
			syncArrow();
			win->onToolStyleChanged();
			notifySelected();
			return false;
		});
	// 第2颗：直线。箭头与直线只能选一种，已经是直线时再点不做任何事
	arrowLineBtn = contentNode->makeChild<ui::Button>();
	arrowLineBtn->setIcon(ui::icons::ShapeLine);
	applyButtonMetrics(arrowLineBtn, ToolMetrics::iconFont);
	tip->bind(arrowLineBtn, Lang::get(L"tool.straightLine"));
	arrowLineBtn->onClick.add([this](ui::Button*) {
		if (isArrowStraight) return;
		isArrowStraight = true;
		Setting::get()->setToolFlag(curToolId, L"arrowStraight", true);
		syncArrow();
		notifySelected();
	});
	syncArrow();
	initSlider();
	initColorBtns();
}

// 两颗按钮的选中态由 isArrowStraight 定，永远只有一颗亮着。
// 箭头的字形与提示由 makeToggleBtn 自己维护，这里只管"选中哪一颗"；直线字形是固定的，建按钮时设一次
void ToolSub::syncArrow()
{
	if (arrowBtn) applyToggleStyle(arrowBtn, !isArrowStraight);
	if (arrowLineBtn) applyToggleStyle(arrowLineBtn, isArrowStraight);
}

// 序号：圆、方两颗形状按钮（与矩形/椭圆那对同一条"两段式"口径）——
//   点另一颗形状只换形状；点当前形状才切它的实心/空心。圆、方各记各的填充态，
//   共 4 种样式：实心圆 / 空心圆 / 实心方 / 空心方。
// 单色/双色不占按钮：色板上亮几格颜色就是几色（默认一格 = 单色），
// 双色时按序号奇偶交替用两种颜色（1 色1 2 色2 3 色1 …），交互见 onColorSelect
void ToolSub::showNumberTools()
{
	if (!beginTool(L"number")) return;
	initSize(2, true);
	// 填充态向后兼容旧键：circleSolid ← numberSolid ← fill（历史上序号只有一种形状，
	// 那时的"实心/空心"记在 fill / numberSolid 里，圆、方都沿用这份旧偏好）
	isNumCircleSolid = Setting::get()->getToolFlag(curToolId, L"circleSolid",
		Setting::get()->getToolFlag(curToolId, L"numberSolid",
			Setting::get()->getToolFlag(curToolId, L"fill", isNumCircleSolid)));
	isNumSquareSolid = Setting::get()->getToolFlag(curToolId, L"squareSolid",
		Setting::get()->getToolFlag(curToolId, L"numberSolid", isNumSquareSolid));
	isNumberSquare = Setting::get()->getToolFlag(curToolId, L"square", isNumberSquare);
	// 第1颗：圆。开 = shape-number-circle-solid（实心圆），关 = shape-number-circle-outline（空心圆）。
	// 当前是方时，这一下只切回圆（不顺手翻填充），与矩形/椭圆的"两段式"一致
	numCircleBtn = makeToggleBtn(ui::icons::ShapeNumberCircleSolid, ui::icons::ShapeNumberCircleOutline,
		&isNumCircleSolid, L"tool.circleNumberSolid", L"tool.circleNumberOutline", L"circleSolid", L"",
		ToolMetrics::iconFont,
		[this] { if (isNumberSquare) { selectNumberShape(false); return false; } return true; });
	// 第2颗：方。开 = shape-number-square-solid，关 = shape-number-square-outline。
	// 两个填充态各记各的键，互不影响
	numSquareBtn = makeToggleBtn(ui::icons::ShapeNumberSquareSolid, ui::icons::ShapeNumberSquareOutline,
		&isNumSquareSolid, L"tool.squareNumberSolid", L"tool.squareNumberOutline", L"squareSolid", L"",
		ToolMetrics::iconFont,
		[this] { if (!isNumberSquare) { selectNumberShape(true); return false; } return true; });
	syncNumber();
	initSlider();
	initColorBtns();
}

// 点圆/方按钮时同步"当前画哪种徽章"。形状没变就什么都不做 ——
// 那一下点击的语义只是切换填充，别再重刷一遍按钮
void ToolSub::selectNumberShape(const bool square)
{
	if (isNumberSquare == square) return;
	isNumberSquare = square;
	Setting::get()->setToolFlag(curToolId, L"square", isNumberSquare);
	syncNumber();
	// 与矩形/椭圆同一条口径：画布上最后一条若是另一种形状，原地换成这一种
	applyNumberVariant();
	// 换了形状，滑条/颜色之后要落的那个目标也换了，通知画布按新形状重画最后一个
	win->onToolStyleChanged();
	notifySelected();
}

// 按当前形状点亮对应的形状按钮（选中态），并按各自的填充态刷新两个按钮的字形
// （圆：shape-number-circle-solid / circle-outline，读 isNumCircleSolid；
//  方：shape-number-square-solid / square-outline，读 isNumSquareSolid）。
// 字形由 makeToggleBtn 在建按钮和点击时维护，这里只管"选中哪一颗" —— 与 syncRectEll 一处口径
void ToolSub::syncNumber()
{
	if (numCircleBtn) applyToggleStyle(numCircleBtn, !isNumberSquare);
	if (numSquareBtn) applyToggleStyle(numSquareBtn, isNumberSquare);
}

// 联动：只换历史里最后一个未撤销的序号，和滑条/颜色那条规则一致。
// 最后一条不是序号就什么都不做，免得点一下样式把别处早画好的徽章也改了
void ToolSub::applyNumberVariant()
{
	if (!win->history) return;
	for (int i{ (int)win->history->shapes.size() - 1 }; i >= 0; i--) {
		auto s = win->history->shapes[i].get();
		if (!s || s->isUndo) continue;
		if (auto* n = dynamic_cast<ShapeNumber*>(s))
			n->setStyle(isNumberSquare, isNumberSquare ? isNumSquareSolid : isNumCircleSolid);
		return;
	}
}

// 画笔（原「线条」工具）：半透明 + 画法（手绘 / 选框）。
// 选框画的是"填充框"（实心矩形），手绘是跟着鼠标画的自由线。默认手绘
void ToolSub::showBrushTools()
{
	if (!beginTool(L"brush")) return;
	initSize(2, true);
	// 半透明：实线 = 实心铅笔，半透明 = 空心铅笔（与矩形/椭圆"实心=开"的表达一致）。
	// 提示按当前状态说：半透明 / 不透明
	makeToggleBtn(ui::icons::ToolLine, ui::icons::ToolLineSolid, &isBrushSemiTransparent,
		L"tool.semiTransparent", L"tool.opaque", L"semiTransparent");
	// 画法：开 = 选框（实心方框图标），关 = 手绘（实心圆图标）。默认手绘，配置读不到时按手绘来。
	// 只影响之后画的笔画，已经画好的那条不跟着变形。提示按当前状态说：选框 / 手绘
	makeToggleBtn(ui::icons::ShapeRectSolid, ui::icons::ShapeCircleSolid, &isBrushMarquee,
		L"tool.marquee", L"tool.handDraw", L"rect");
	initSlider();
	initColorBtns();
}

void ToolSub::showTextTools()
{
	if (!beginTool(L"text")) return;
	initSize(2, true);
	makeToggleBtn(ui::icons::TextBold, ui::icons::TextBold, &isTextBold,
		L"tool.bold", L"tool.bold", L"bold");
	makeToggleBtn(ui::icons::TextItalic, ui::icons::TextItalic, &isTextItalic,
		L"tool.italic", L"tool.italic", L"italic");
	initSlider();
	initColorBtns();
}

// 马赛克：主条上只有这一颗按钮，算法（马赛克 / 高斯模糊 / 智能消除）在副条里拆成三颗独立按钮，
// 点哪颗用哪颗；形状（选框 / 手绘）仍是一颗来回切的按钮，与算法无关，切算法不会动它
void ToolSub::showMosaicTools()
{
	if (!beginTool(L"mosaic")) return;
	// 四格：马赛克 / 高斯模糊 / 智能消除 / 形状
	initSize(4, false, true);
	// 算法状态与形状偏好各存一份：切算法只改 (isMosaicBlur, isMosaicErase)，形状按钮不受影响
	isMosaicBlur = Setting::get()->getToolFlag(curToolId, L"blur", isMosaicBlur);
	isMosaicErase = Setting::get()->getToolFlag(curToolId, L"erase", isMosaicErase);
	if (isMosaicErase) isMosaicBlur = false;    // 两个键同时为真（历史配置）以智能消除为准
	isMosaicMarquee = Setting::get()->getToolFlag(curToolId, L"rect", isMosaicMarquee);
	// 智能消除固定是选框：历史配置若留着"手绘"，进工具时就纠正回来并落盘
	if (isMosaicErase) {
		isMosaicMarquee = true;
		Setting::get()->setToolFlag(curToolId, L"rect", true);
	}
	// 三颗算法按钮只差图标 / 提示 / 目标档位，其余同构，用工厂去掉重复
	const auto makeEffectBtn = [this](const wchar_t* icon, const wchar_t* tipKey, int effect) {
		auto btn = contentNode->makeChild<ui::Button>();
		btn->setIcon(icon);
		applyButtonMetrics(btn, ToolMetrics::iconFont);
		tip->bind(btn, Lang::get(tipKey));
		btn->onClick.add([this, effect](ui::Button*) {
			if (mosaicEffect() == effect) return;
			isMosaicBlur = effect == 1;
			isMosaicErase = effect == 2;
			Setting::get()->setToolFlag(curToolId, L"blur", isMosaicBlur);
			Setting::get()->setToolFlag(curToolId, L"erase", isMosaicErase);
			// 智能消除只做选框：切过来时收回手绘偏好（形状按钮随后置灰）
			if (isMosaicErase && !isMosaicMarquee) {
				isMosaicMarquee = true;
				Setting::get()->setToolFlag(curToolId, L"rect", true);
			}
			refreshEffectMode();
			refreshEffectShape();
			// 通知画布：画好的那一条要就地换成新算法（与颜色/滑块/实心开关同一条路径）。
			// 漏了这一句，三颗算法按钮就只是"改了自己的高亮"，画面一动不动 ——
			// 只有之后新画的才用新算法，看着像按钮坏了
			win->onToolStyleChanged();
			notifySelected();
		});
		return btn;
	};
	effectMosaicBtn = makeEffectBtn(ui::icons::ToolMosaic, L"tool.pixelate", 0);
	effectBlurBtn = makeEffectBtn(ui::icons::ToolBlur, L"tool.blur", 1);
	effectEraseBtn = makeEffectBtn(ui::icons::ToolSmartErase, L"tool.smartErase", 2);
	refreshEffectMode();
	// 形状：一颗按钮来回切，选框 / 手绘对三种算法都生效
	effectShapeBtn = contentNode->makeChild<ui::Button>();
	applyButtonMetrics(effectShapeBtn, ToolMetrics::iconFont);
	effectShapeBtn->onClick.add([this](ui::Button*) {
		// 智能消除只做选框：按钮已经置灰，这里再拦一道，点了也不切
		if (isMosaicErase) return;
		isMosaicMarquee = !isMosaicMarquee;
		Setting::get()->setToolFlag(curToolId, L"rect", isMosaicMarquee);
		refreshEffectShape();
		notifySelected();
	});
	refreshEffectShape();
	initSlider();
}

// 三颗算法按钮：选中态跟 mosaicEffect() 走，永远只有一颗亮着
void ToolSub::refreshEffectMode()
{
	const int effect = mosaicEffect();
	if (effectMosaicBtn) applyToggleStyle(effectMosaicBtn, effect == 0);
	if (effectBlurBtn) applyToggleStyle(effectBlurBtn, effect == 1);
	if (effectEraseBtn) applyToggleStyle(effectEraseBtn, effect == 2);
}

// 形状按钮：选框 = 实心方框，手绘 = 实心圆。二选一必有一种生效，所以常亮。
// 提示也跟着当前形状走：选框 / 手绘。马赛克与高斯模糊共用这一个形状，切算法不会翻它。
// 智能消除例外：它只做选框，按钮固定显示方框并置灰（透明底 + 灰图标，悬停也不变色）
void ToolSub::refreshEffectShape()
{
	if (!effectShapeBtn) return;
	if (isMosaicErase) {
		effectShapeBtn->setIcon(ui::icons::ShapeRectSolid);
		effectShapeBtn->setBg(0);
		effectShapeBtn->setHoverBg(0);
		effectShapeBtn->setColor(0xAAAAAAFF);
		effectShapeBtn->setHoverColor(0xAAAAAAFF);
		tip->rebind(effectShapeBtn, Lang::get(L"tool.marquee"));
		return;
	}
	const bool rectShape = isMosaicMarquee;
	effectShapeBtn->setIcon(rectShape ? ui::icons::ShapeRectSolid : ui::icons::ShapeCircleSolid);
	applyToggleStyle(effectShapeBtn, true);
	tip->rebind(effectShapeBtn, Lang::get(rectShape ? L"tool.marquee" : L"tool.handDraw"));
}

void ToolSub::showEraserTools(bool selecting)
{
	if (!beginTool(L"eraser")) return;
	initSize(1, false, true);
	// 用户定的口径："点橡皮擦的时候默认都是选框，手绘也需要二次点一下"。
	// 点击选中时强制回选框并落盘；悬停预览不碰它（makeToggleBtn 自己会从配置读），
	// 免得只是从按钮上划过就把用户已经选好的手绘冲掉
	if (selecting) {
		isEraserMarquee = true;
		Setting::get()->setToolFlag(curToolId, L"rect", true);
	}
	// 形状与马赛克用同一套字形：开 = 实心方框 = 选框，关 = 实心圆 = 手绘。
	// 以前是 eraser-rect / eraser-brush 两颗，看着像"矩形填充的实心/空心"，
	// 分不清这颗到底是选框还是手绘 —— 用户要的就是马赛克那两颗"选框 / 手绘"的表达。
	// 只影响之后新画的橡皮擦，已经画好的那条不会被改成另一种形状
	makeToggleBtn(ui::icons::ShapeRectSolid, ui::icons::ShapeCircleSolid,
		&isEraserMarquee, L"tool.marquee", L"tool.handDraw", L"rect");
	initSlider();
}

// 录屏设置态：MP4 / GIF ‖ 系统声 / 麦克风 ‖ 开始。
// 与其它工具共用这条副条，所以"贴哪、箭头指哪"全部继承 updatePosition 那套规则。
void ToolSub::showVideoTools()
{
	// beginTool 查不到 video 的滑块配置，会在清完旧内容后带着 true 回来（只是没有滑块与颜色）——
	// 正好，这条本来就没有滑条和色块。它把 curToolId 记成 video，日志/落盘口径与其它工具一致
	if (!beginTool(L"video")) return;
	// 格式 / 音源是工具状态，跟马赛克形状、滑块值一样落盘：上次选了 GIF + 麦克风，
	// 这次进来还是它。读到哪项算哪项，坏配置 / 缺键回落到与成员初始化相同的默认
	videoMp4 = Setting::get()->getToolFlag(curToolId, L"mp4", true);
	videoSpeaker = Setting::get()->getToolFlag(curToolId, L"speaker", true);
	videoMic = Setting::get()->getToolFlag(curToolId, L"mic", false);
	// 五个按钮格（MP4 / GIF / 系统声 / 麦克风 / 开始）+ 两根分隔条。
	// 格式不再是一颗"来回切"的宽按钮：拆成两颗与工具按钮同宽同高的格子按钮，
	// 选中哪颗一眼就能看出来
	initSizeCustom(ToolMetrics::cell * 5 + ToolMetrics::sep * 2);
	// 格式按钮：只有文字，没有图标。宽高与工具按钮完全一致（btn），
	// 左右用 margin 把格子补到 cell —— 和主条按钮一样是"一格一颗"
	const auto makeFormatBtn = [this](const wchar_t* text) {
		auto btn = contentNode->makeChild<ui::Button>();
		btn->setText(text);
		// 尺寸 / 留白 / 单空格子与工具按钮同一套，只有字号取"条上纯文字"那一档
		applyButtonMetrics(btn, ToolMetrics::textFont);
		return btn;
	};

	// MP4 / GIF 两颗互斥：点哪颗就选哪颗，两颗都是"选中态"的表达（当前那颗高亮）
	vMp4 = makeFormatBtn(L"MP4");
	vMp4->onClick.add([this](ui::Button*) {
		if (videoMp4) return;
		videoMp4 = true;
		// 切到 MP4：系统声默认开、麦克风默认关
		videoSpeaker = true;
		videoMic = false;
		persistVideoOpts();
		videoApplyFormatStyle();
	});
	tip->bind(vMp4, Lang::get(L"video.mp4"));
	vGif = makeFormatBtn(L"GIF");
	vGif->onClick.add([this](ui::Button*) {
		if (!videoMp4) return;
		videoMp4 = false;
		// GIF 没有音轨：两路音源一律关掉（按钮本身也会被压灰）
		videoSpeaker = false;
		videoMic = false;
		persistVideoOpts();
		videoApplyFormatStyle();
	});
	tip->bind(vGif, Lang::get(L"video.gif"));
	makeSpliter();
	// 系统声 / 麦克风：GIF 模式下不可切换（点了没反应）
	vSpeaker = makeIconBtn(ui::icons::VideoSpeaker);
	vSpeaker->onClick.add([this](ui::Button*) {
		if (!videoMp4) return;
		videoSpeaker = !videoSpeaker;
		persistVideoOpts();
		videoApplyFormatStyle();
	});
	tip->bind(vSpeaker, Lang::get(L"video.recordSystem"));
	vMic = makeIconBtn(ui::icons::RecordingMic);
	vMic->onClick.add([this](ui::Button*) {
		if (!videoMp4) return;
		videoMic = !videoMic;
		persistVideoOpts();
		videoApplyFormatStyle();
	});
	tip->bind(vMic, Lang::get(L"video.recordMic"));
	makeSpliter();
	auto btnStart = makeIconBtn(ui::icons::RecordingResume);
	btnStart->onClick.add([this](ui::Button*) { this->videoStart(); });
	tip->bind(btnStart, Lang::get(L"video.startRecord"));
	// 原来那颗"退出录屏"（关闭图标）已去掉：这颗按下去只是从设置态退出去，
	// 用户的退出动作是关掉整个截图会话（ESC / 主条关闭），留在这里只会让人误以为它会开录
	videoApplyFormatStyle();
}

// 录制态：计时 ‖ 丢弃 / 存文件 / 存剪切板
void ToolSub::showVideoRecording()
{
	if (!contentNode) return;
	// 旧节点马上被销毁，指针先作废；提示同理（onLeave 不会触发）。
	// 这里没走 beginTool（录制态是直接从设置态切过来的），所以必须自己作废一遍 ——
	// 尤其是 slider：切到录屏这条副条根本没有滑块，留着它就等着 mousemove 去踩
	tip->hide();
	tip->unbindAll();
	forgetNodes();
	// 录制中主条是藏着的，这条已经不再挂在那颗按钮下面，顶上的小三角一并收掉
	noArrow = true;
	contentNode->removeAllChildren();
	initSizeCustom(videoTimerW + ToolMetrics::sep + ToolMetrics::cell * 3);

	videoTimer = contentNode->makeChild<ui::Label>();
	videoTimer->setWidth(videoTimerW);
	videoTimer->setHeightPercent(100.f);
	videoTimer->setAlignItems(ui::Align::Center);
	videoTimer->setJustifyContent(ui::Justify::Center);
	videoTimer->setFontSize(ToolMetrics::fontFor(ToolMetrics::textFont, dpi));
	videoUpdateTimer();
	makeSpliter();
	// 三条路都会停掉录制并结束整个流程
	auto btnDiscard = makeIconBtn(ui::icons::Trash);
	btnDiscard->onClick.add([this](ui::Button*) { this->videoDiscard(); });
	if (tip) tip->bind(btnDiscard, Lang::get(L"video.stopExit"));
	auto btnSave = makeIconBtn(ui::icons::QuickSave);
	btnSave->onClick.add([this](ui::Button*) { this->videoSave(false); });
	if (tip) tip->bind(btnSave, Lang::get(L"video.stopFile"));
	auto btnClipboard = makeIconBtn(ui::icons::ActionCopy);
	btnClipboard->onClick.add([this](ui::Button*) { this->videoSave(true); });
	if (tip) tip->bind(btnClipboard, Lang::get(L"video.stopClipboard"));
}

// 三颗按钮的选中态与配色。一律走 applyToggleStyle —— 与矩形/箭头等副条上那些开关
// 同一条口径（选中 = 底压暗 + 蓝图标，悬停 = 底再压暗一档），不再单独用一套浅蓝底
void ToolSub::persistVideoOpts()
{
	Setting::get()->setToolFlag(curToolId, L"mp4", videoMp4);
	Setting::get()->setToolFlag(curToolId, L"speaker", videoSpeaker);
	Setting::get()->setToolFlag(curToolId, L"mic", videoMic);
}

void ToolSub::videoApplyFormatStyle()
{
	// MP4 / GIF 是互斥的两颗：当前那颗走选中配色（与工具按钮同一条规则：底压暗、字变蓝）
	applyToggleStyle(vMp4, videoMp4);
	applyToggleStyle(vGif, !videoMp4);
	// GIF 不带声音（格式本身就没有音轨），两个音源按钮在 GIF 下彻底没得点。
	// 只把点击回调拦掉不够 —— 按钮还亮着、悬停还有反馈，用户会一直点它找"为什么没反应"。
	// 这里连同悬停色一起压成灰色，让"不可用"是看得见的
	const auto grey = [](ui::Button* btn) {
		if (!btn) return;
		btn->setBg(0);
		btn->setHoverBg(0);
		btn->setColor(0xC0C0C0ff);
		btn->setHoverColor(0xC0C0C0ff);
		btn->setBorderColor(0);
	};
	if (videoMp4) {
		applyToggleStyle(vSpeaker, videoSpeaker);
		applyToggleStyle(vMic, videoMic);
	}
	else {
		grey(vSpeaker);
		grey(vMic);
	}
	// 麦克风图标跟着开关走：开着是实心麦，关着是带斜杠的麦。
	// 只靠底色深浅表达状态，GIF 压灰后根本看不出"这路声音是关的"
	if (vMic) vMic->setIcon(videoMic ? ui::icons::RecordingMic : ui::icons::RecordingMicOff);
}

void ToolSub::videoUpdateTimer()
{
	if (!videoTimer) return;
	// GIF 上限 2 分钟，MP4 上限 10 分钟
	const int maxMinutes = videoMp4 ? 10 : 2;
	videoTimer->setText(std::format(L"{:02d}:{:02d} / {:02d}:00",
		videoSec / 60, videoSec % 60, maxMinutes));
}

void ToolSub::videoStart()
{
	// 宿主先把录制张罗起来（选区太小 / 设备起不来时会返回 false），成功了才切成录制态
	if (!win->beginVideo(videoMp4, videoSpeaker, videoMic)) return;
	videoSec = 0;
	showVideoRecording();
	// 录制条宽度和设置态不一样，让宿主按新尺寸重新贴一次（主条这时已经被藏起来了，
	// 位置仍取它最后停的地方 —— 录制条就留在原来那条工具条的位置上）
	win->layoutTools();
	setTimer(1000, kVideoTimer);
	// 键盘兜底：录制中宿主是穿透窗口，点一下桌面就没焦点，ESC 收不到。
	// 这里起一个轻量轮询，按 GetAsyncKeyState 自己判 ESC（见 onTimer）
	// 先读一次把陈旧的"按下过"标记吃掉：上一次操作里的 ESC 不能算到这一次头上
	GetAsyncKeyState(VK_ESCAPE);
	escDown = false;
	setTimer(kVideoEscPoll, kVideoEscTimer);
}

void ToolSub::videoSave(bool toClipboard)
{
	// 录制已经结束（键盘和录制条按钮几乎同时打进来）就别再走一遍：
	// 第二次进来 capVideo 已经是空的了，重复停录/退出会让界面状态乱掉
	if (!win->inVideoMode()) return;
	killTimer(kVideoTimer);
	killTimer(kVideoEscTimer);
	win->stopVideo(toClipboard);
	// 保存 / 复制成功就代表这次录屏已经结束：直接关掉整个截图会话
	// （压暗层、透明宿主、工具条一起收），不再退回标注态
	win->close();
}

void ToolSub::videoDiscard()
{
	// 放弃 = 事情结束：同样直接关窗。编码线程的收尾交给 WinCap::onClosed
	// （那边是异步丢弃，不堵 UI 线程）。
	// 两个计时器和另外两条结束路径一样先收掉：close() 只是投递消息，从这一刻到窗口
	// 真正销毁之间还有一轮消息，每秒那拍要是正好落在这段里，就会往一个正在关的窗口上
	// 补一次 layoutTools（还可能把 videoSec 又加一秒）
	if (!win->inVideoMode()) return;
	killTimer(kVideoTimer);
	killTimer(kVideoEscTimer);
	win->close();
}

// 录制中按 ESC：丢弃这段录制，并直接结束整个截图会话。
// 以前走的是 videoDiscard + exitVideo，只退回到标注/选区态，
// 用户得再按一次 ESC 才关得掉 —— 反馈就是"按了两次 ESC"。
// 与按钮上的「丢弃」完全同一件事（都收两个计时器、都不调 stopVideo、都关窗，
// 编码线程与临时文件的清理在 WinCap::onClosed 里统一做），直接委托
void ToolSub::videoAbort()
{
	videoDiscard();
}

float ToolSub::sliderMaxOf(const std::wstring& tool) const
{
	auto cfg = findSliderCfg(tool);
	return cfg ? cfg->max : 0.f;
}

float ToolSub::setShapeSliderVal(const std::wstring& tool, float px)
{
	auto cfg = findSliderCfg(tool);
	if (!cfg) return px;
	auto logical = std::clamp(px / dpi, cfg->min, cfg->max);
	// 正显示着这个工具的工具条（在自己刚画的图形上滚滚轮，基本都是这种情况）：走滑块，
	// 让滑块上的位置也跟着动，它的 onValueChanged 会同步 sliderVal 并落盘。
	// 值没变时 Slider::setValue 会提前返回，不会白写一次盘
	if (curToolId == tool && slider) {
		slider->setValue(logical);
	}
	else {
		// 滚的是别的工具画出来的图形（比如选着椭圆工具，在一个已有的矩形边框上滚）：
		// 只更新那个工具的配置，别去动当前这条工具条的滑块
		Setting::get()->setToolNum(tool, cfg->key, logical);
	}
	return logical * dpi;
}

void ToolSub::layout()
{
	ui::WinBase::layout();
	if (!canvas) return;
	auto ctx = canvas->startPaint();
	if (!ctx) return;
	ctx->Clear(0);
	paintBorder(ctx);
	canvas->finishPaint();
}

void ToolSub::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

void ToolSub::paintBorder(ID2D1DeviceContext* ctx)
{
	auto d2d = ui::D2D::get();
	if (!d2d || !d2d->d2dFactory) return;   // 设备建失败：直接跳过装饰描边
	// 几何体每帧都要按当前 w/h/arrowX 重建，用局部 ComPtr，别存成员：
	// 存成员时 GetAddressOf() 不会 Release 旧对象，等于每帧漏一个 ID2D1PathGeometry。
	ComPtr<ID2D1PathGeometry> borderPath;
	d2d->d2dFactory->CreatePathGeometry(borderPath.GetAddressOf());
	// 建不出来就只画个矩形底板：没有这个判断的话下一句 borderPath->Open 就是空指针
	if (!borderPath) return;
	ComPtr<ID2D1GeometrySink> sink;
	if (FAILED(borderPath->Open(sink.GetAddressOf())) || !sink) return;
	// 以下都是物理像素：D2D 直接画在 surface 上，不像 Ui 层的 setter 会自己乘 dpi。
	// D2D 的描边以路径为中心线，所以路径放在距边 borderWPx/2 处，描边正好填满最外侧 borderWPx 像素。
	// 右/下边界要用 surface 的整数尺寸（窗口和 surface 都是 (int)w/(int)h），
	// 直接用带小数的 w/h 会让描边画到 surface 之外，看起来就是底边缺一条。
	auto borderWPx{ borderW * dpi };
	auto half{ borderWPx / 2.f };
	auto top{ toPx(marginTop) + half };
	auto arrowHalf{ ToolMetrics::arrowHalf * dpi };
	auto right{ std::floor(w) - half };
	auto bottom{ std::floor(h) - half };
	sink->BeginFigure({ half,top }, D2D1_FIGURE_BEGIN_FILLED);
	if (noArrow) {
		// 不画箭头：直接四条边围一圈（与下面对称，只是少了顶上那个凹口）
		D2D1_POINT_2F plain[4] = {
			D2D1_POINT_2F{right,top},
			D2D1_POINT_2F{right,bottom},
			D2D1_POINT_2F{half,bottom},
			D2D1_POINT_2F{half,top}
		};
		sink->AddLines(plain, 4);
	}
	else {
		D2D1_POINT_2F points[6] = {
			D2D1_POINT_2F{arrowX - arrowHalf,top},
			D2D1_POINT_2F{arrowX,half},
			D2D1_POINT_2F{arrowX + arrowHalf,top},
			D2D1_POINT_2F{right,top},
			D2D1_POINT_2F{right,bottom},
			D2D1_POINT_2F{half,bottom}
		};
		sink->AddLines(points, 6);
	}
	sink->EndFigure(D2D1_FIGURE_END_CLOSED);
	sink->Close();
	ctx->FillGeometry(borderPath.Get(), brushBg.Get());
	ctx->DrawGeometry(borderPath.Get(), brushBorder.Get(), borderWPx);
}

void ToolSub::onColorSelect(ui::Button* btn)
{
	if (!btn) return;
	// colorBtns 与 colors 一一对应，但 selectColorIndex 是从 config.json 读出来的，
	// 配置被改坏 / 上个版本存了更大的值时它会越界。beginTool 里夹过一次，
	// 这里再兜一次 —— 越界读 colorBtns[i] 是未定义行为，后面接 dynamic_cast 就是崩溃
	if (colorBtns.empty()) return;
	if (selectColorIndex >= colorBtns.size()) selectColorIndex = 0;
	if (selectColor2Index >= colorBtns.size()) selectColor2Index = 0;
	int clicked{ -1 };
	for (size_t i = 0; i < colorBtns.size(); i++) {
		if (colorBtns[i] == btn) {
			clicked = static_cast<int>(i);
			break;
		}
	}
	if (clicked < 0) return;
	auto setting = Setting::get();
	// 序号的颜色全在色板上：最多亮两格（= 双色，奇偶交替取色），默认只亮一格（= 单色）。
	//   点未亮的格 —— 没亮第二格时它就是色2（这一下进入双色）；已亮两格时色2 升色1、它补位色2
	//   （先进先出：依次点 1、2、3 保留 2、3，再点 4 保留 3、4）
	//   点已亮的格 —— 取消勾选它，另一格独自亮着回到单色；唯一那格再点不动（至少留一色）
	if (curToolId == L"number") {
		const bool lit1 = clicked == static_cast<int>(selectColorIndex);
		const bool lit2 = hasColor2 && clicked == static_cast<int>(selectColor2Index);
		if (lit1 || lit2) {
			if (!hasColor2) return;             // 唯一那格，取消就一色不剩了
			if (lit1) selectColorIndex = selectColor2Index;   // 点谁删谁：色1下岗则色2顶上
			hasColor2 = false;
			setting->setToolNum(curToolId, L"colorIndex2", -1.f);
		}
		else {
			if (hasColor2) selectColorIndex = selectColor2Index;
			selectColor2Index = static_cast<UINT>(clicked);
			hasColor2 = true;
			setting->setToolNum(curToolId, L"colorIndex2", static_cast<float>(selectColor2Index));
		}
		setting->setToolNum(curToolId, L"colorIndex", static_cast<float>(selectColorIndex));
		syncColorMarks();
		applyNumberVariant();
		win->onToolStyleChanged();
		notifySelected();
		return;
	}
	if (colorBtns[selectColorIndex] == btn) return;
	selectColorIndex = static_cast<UINT>(clicked);
	setting->setToolNum(curToolId, L"colorIndex", static_cast<float>(selectColorIndex));
	syncColorMarks();
	win->onToolStyleChanged();
	notifySelected();
}

void ToolSub::notifySelected()
{
	// 用户在副条上做了实质操作：把当前工具从"hover 预览"提升为"选中"。
	// 主编辑器据此点亮对应按钮，之后鼠标在主条上悬停别的按钮再离开，会回落到这个工具
	if (win->toolMain && !curToolId.empty()) {
		win->toolMain->promoteToSelected(curToolId);
	}
}

void ToolSub::applyButtonMetrics(ui::Button* btn, float fontSize)
{
	if (!btn) return;
	// 本体边长 = 格子 - 四边留白；左右用 margin 补回格子宽度，
	// 于是 N 个按钮总宽恒等于 N * cell，initSize 的算法才对得上。
	// 宽高都写死，不用 flexGrow 均分：均分要看窗口宽度是否一分不差，差 1px 就会出现
	// "这颗宽一点、那颗窄一点"（马赛克那条带分隔条，正好少算 1px 就露馅）
	btn->setWidth(ToolMetrics::btnAt(dpi));
	btn->setHeight(ToolMetrics::btnAt(dpi));
	btn->setMarginLeft(ToolMetrics::padAt(dpi));
	btn->setMarginRight(ToolMetrics::padAt(dpi));
	btn->setFlexGrow(0.f);
	// 图标走矢量路径绘制，这里的"字号"是图标盒边长，按物理像素取整避免半像素模糊
	btn->setFontSize(ToolMetrics::fontFor(fontSize, dpi));
}

// 副条上"一格一颗"的纯图标按钮：尺寸 / 留白 / 单空格子全部交给 applyButtonMetrics，
// 与工具按钮同一条口径。录屏条的两档（设置态 / 录制态）共用这一个
ui::Button* ToolSub::makeIconBtn(const wchar_t* icon)
{
	auto btn = contentNode->makeChild<ui::Button>();
	btn->setIcon(icon);
	applyButtonMetrics(btn, ToolMetrics::iconFont);
	return btn;
}

// 两根分隔条之间的竖线：1 逻辑像素宽，高度撑满一行。
// 必须显式撑满：副条内容是 Center 对齐，只定宽不定高的话交叉轴按内容算，
// 而 Node 没有内容 —— 高度直接是 0，那条线根本画不出来
void ToolSub::makeSpliter()
{
	auto sep = contentNode->makeChild<ui::Node>();
	sep->setWidth(ToolMetrics::sep);
	sep->setHeightPercent(100.f);
	sep->setBg(0xDDDDDDff);
}

void ToolSub::initColorBtns()
{
	// contentNode->removeAllChildren() 已经把上一批按钮销毁了，这里必须同步清空，
	// 否则 colorBtns 会越积越长且前面全是野指针，selectColorIndex 也会越界。
	colorBtns.clear();
	// 与 colors 一一对应的语言键后缀
	static const std::vector<std::wstring> colorNames{ L"red",L"yellow",L"green",L"cyan",L"blue",L"purple",L"pink",L"black",L"white" };
	for (size_t i = 0; i < colors.size(); i++)
	{
		auto btn = contentNode->makeChild<ui::Button>();
		// 色块按钮是"容器"，本身不带图标字形，所以尺寸用同一套度量、字号交给里面的色块
		// 与工具按钮同一套度量：宽高都是 btn、左右 margin 补到 cell，不再参与均分
		btn->setWidth(ToolMetrics::btnAt(dpi));
		btn->setHeight(ToolMetrics::btnAt(dpi));
		btn->setMarginLeft(ToolMetrics::padAt(dpi));
		btn->setMarginRight(ToolMetrics::padAt(dpi));
		btn->setFlexGrow(0.f);
		btn->setAlignItems(ui::Align::Center);
		btn->setJustifyContent(ui::Justify::Center);
		btn->setHoverBg(C_BTN_HOVER);   // 色块按钮也是白底，按同一档压暗
		btn->onClick.add([this](ui::Button* btn) {this->onColorSelect(btn);});
		tip->bind(btn, Lang::get(std::format(L"color.{}", colorNames[i])));
		colorBtns.push_back(btn);

		auto label = btn->makeChild<ui::Label>();
		label->setAlignItems(ui::Align::Center);
		label->setJustifyContent(ui::Justify::Center);
		label->setSize(ToolMetrics::swatch, ToolMetrics::swatch);
		// 重建后要把对勾画在当前选中的那一项上（序号双色时两格都打勾），
		// 统一交给 syncColorMarks，别在循环里再判一遍
		label->setFontSize(ToolMetrics::fontFor(ToolMetrics::swatchFont, dpi));
		label->setBg(colors[i]);
		label->setBorderRadius(2.f);
		if (i == colors.size() - 1) {
			label->setColor(0x000000FF);
			label->setBorder(1.f, 0xA8A8A8FF);
		}
		else {
			label->setColor(0xFFFFFFFF);
		}
	}
	syncColorMarks();
}

// 颜色按钮上的对勾：一号色一定打勾；色板上亮着第二格（= 双色）时二号色也打勾，其余清空。
// 点颜色之后都靠它把两格一起重画（只改一格会留下旧对勾）
void ToolSub::syncColorMarks()
{
	for (size_t i = 0; i < colorBtns.size(); i++) {
		auto* btn = colorBtns[i];
		if (!btn || btn->children.size() < 2) continue;
		auto* label = dynamic_cast<ui::Label*>(btn->children[1].get());
		if (!label) continue;
		const bool marked = i == selectColorIndex
			|| (curToolId == L"number" && hasColor2 && i == selectColor2Index);
		if (marked) label->setIcon(ui::icons::UiCheck);
		else label->setText(L"");
	}
}

void ToolSub::applyToggleStyle(ui::Button* btn, bool selected)
{
	// 调用方是各个 show*Tools 与刷新函数，按约定都拿得到按钮；这里兜一手空指针，
	// 免得以后某个"刷新某一组状态"的新入口在按钮重建的间隙被调到就当场崩
	if (!btn) return;
	// 注意：ui::Color 是 0xRRGGBBAA。旧代码 0xA6000000 的 alpha=00 是全透明，
	// 所以选中态完全看不出来。
	// 配色（三档都用半透明黑，白底上叠出灰阶，不再用"更亮的浅蓝"——
	// 白底加亮没有层次，按下去了也看不出来）：
	//   选中：黑 15% ≈ #D9D9D9 + 鲜蓝图标 2E6BE6 —— 底暗字亮，一眼可见；
	//   选中悬停：黑 22%；普通悬停：黑 8%；
	//   未选中：透明底 + 深灰图标。常量见 ToolMain.h（两条工具条共用一份）
	if (selected) {
		btn->setBg(C_BTN_ON);
		btn->setHoverBg(C_BTN_ON_HOVER);
		btn->setColor(0x2E6BE6FF);
		btn->setHoverColor(0x2E6BE6FF);
	}
	else {
		btn->setBg(0);
		btn->setHoverBg(C_BTN_HOVER);
		btn->setColor(0x333333FF);
		btn->setHoverColor(0x333333FF);
	}
}

ui::Button* ToolSub::makeToggleBtn(const std::wstring& onIcon, const std::wstring& offIcon, bool* flag,
	const std::wstring& tipKeyOn, const std::wstring& tipKeyOff, const std::wstring& cfgKey,
	const std::wstring& legacyCfgKey, float fontSize, std::function<bool()> gate)
{
	// 上次退出前的状态在配置文件里，先取回来盖掉内存里那份（成员的初值只是"从没设置过"时的默认）。
	// legacyCfgKey 是改名前的旧键：新键没写过就退回旧键，用户之前存的偏好不丢
	auto setting = Setting::get();
	*flag = setting->getToolFlag(curToolId, cfgKey,
		legacyCfgKey.empty() ? *flag : setting->getToolFlag(curToolId, legacyCfgKey, *flag));
	auto btn = contentNode->makeChild<ui::Button>();
	btn->setIcon(*flag ? onIcon.c_str() : offIcon.c_str());
	applyButtonMetrics(btn, fontSize);
	applyToggleStyle(btn, *flag);
	// 提示按当前状态绑：开 = tipKeyOn（如"实心矩形"），关 = tipKeyOff（如"空心矩形"）
	tip->bind(btn, Lang::get(*flag ? tipKeyOn : tipKeyOff));
	// flag 指向 ToolSub 的成员，生命周期与 this 相同，btn 也挂在 this 的节点树上，捕获裸指针安全。
	// cfgKey / onIcon / offIcon / 两个提示键都按值捕获：调用方传进来的是临时量
	btn->onClick.add([this, flag, cfgKey, onIcon, offIcon, tipKeyOn, tipKeyOff, gate](ui::Button* b) {
		// gate 先跑：返回 false 就当场收手 —— 状态、图标、配置、画布都不动。
		// 矩形/椭圆用它做到"第一次点只换形状，第二次点才切填充"，
		// 不然从椭圆切到矩形会顺带把矩形自己的填充翻一下，看着像"形状和填充一起变了"
		if (gate && !gate()) return;
		*flag = !*flag;
		b->setIcon(*flag ? onIcon.c_str() : offIcon.c_str());   // 图标随状态切换：实心=开、空心=关
		applyToggleStyle(b, *flag);
		Setting::get()->setToolFlag(curToolId, cfgKey, *flag);
		// 提示文字也跟着切：悬停时看到的是"现在是什么样"，不是当初建的默认态
		tip->rebind(b, Lang::get(*flag ? tipKeyOn : tipKeyOff));
		win->onToolStyleChanged();
		notifySelected();
	});
	return btn;
}

void ToolSub::initSlider()
{
	slider = contentNode->makeChild<ui::Slider>();
	// 尺寸从字段来，别写字面量：initSize 按同样的字段算窗口宽度，
	// 两边各写一份的话改了一处就会宽度不匹配（flex 会把误差压在按钮和滑块上）。
	slider->setWidth(sliderSize);
	slider->setMarginLeft(sliderMargin);
	slider->setMarginRight(sliderMargin);
	slider->setHeightPercent(100.f);
	// 值域与当前值都从字段来：切换工具时滑块会被销毁重建，靠 beginTool 把配置里那份带过来。
	// setValue 排在 onValueChanged 之前，加载配置这一下不会反过来又写一次盘
	slider->setRange(sliderMin, sliderMax);
	slider->setValue(sliderVal);
	slider->setStep(1.f);
	slider->onValueChanged.add([this](ui::Slider*, float val) {
		sliderVal = val;
		Setting::get()->setToolNum(curToolId, curSliderKey, val);
		// 正在编辑的文本要立刻跟着变字号，不然得点完再看效果
		win->onToolStyleChanged();
		notifySelected();
	});
	slider->setThumbColor(0x888888FF);
	slider->setHoverThumbColor(0x888888FF);
	slider->setTrackColor(0x888888FF);
	slider->setFillColor(0x888888FF);
}

float ToolSub::toPx(float logical) const
{
	return std::floor(logical * dpi);
}

float ToolSub::getDesiredHeight()
{
	// 返回物理像素：调用方（WinHost::layoutTools）拿它和屏幕坐标、ToolMain->h 一起算，那些都是物理值。
	//
	// 高度 = 箭头区 + 一个格子 + 两条边线。这三段的含义必须跟主条对齐：
	//   · 主条窗口 = cell + border*2（描边画在格子上、不占布局）→ 内容区整整 cell+2*border
	//   · 副条要把顶部那 2px 箭头区**额外**加出来，内容区才同样是 cell + 2*border
	// 以前没收这 2px，内容区只剩 30，29 高的按钮上下只有 0.5 的余量，
	// 而主条是 2.5 —— 同一颗图标放在两条上，外框明显一大一小
	return toPx(ToolMetrics::arrowZone)
		+ toPx(ToolMetrics::border) * 2
		+ toPx(ToolMetrics::cell);
}

// 宽度 = 工具按钮格 + 颜色按钮格 + 分隔条 + 滑块（含左右留白）。
// 一律按"格子"算：每个按钮格恒为 cell，按钮本体的留白由 margin 吃在格子内部，
// 所以这里不需要再关心按钮实际多宽。
// 注意**不加**两条边线的宽度：主条的窗口宽度就是格子之和（描边压在最外侧那 1px 留白上，
// 不占位）。副条同理，内容区从窗口 0 起、到窗口右沿止，左右留白才与主条一模一样
void ToolSub::initSize(int btnCount, bool withColors, bool centerOnBtn, int sepCount)
{
	hasTools = true;
	sizeCustom = false;
	this->centerOnBtn = centerOnBtn;
	sizeBtnCount = btnCount;
	sizeWithColors = withColors;
	sizeSepCount = sepCount;
	auto count = btnCount + (withColors ? static_cast<int>(colors.size()) : 0);
	// 宽度按逻辑像素一次算准（setSize 内部乘 dpi，与子节点 setWidth 的口径相同）：
	// 每格按钮 cell + 分隔条 + 滑块（含左右留白）。
	// 以前漏了分隔条：马赛克那条比别的少 1px，按钮一均分就有的宽有的窄，
	// 滑块也被挤短一截 —— 用户看到的"占位不一样、滑条长度不一样"就是这个
	const float logicW = ToolMetrics::cell * count
		+ ToolMetrics::sep * sepCount
		+ ToolMetrics::sliderW
		+ ToolMetrics::sliderPad * 2.f;
	setSize(logicW, getDesiredHeight() / dpi);
}

void ToolSub::refreshSize()
{
	if (!hasTools) return;   //没内容时窗口是藏着的，等下次 show*Tools 自然会按新 dpi 算
	if (sizeCustom) {
		// 录屏条：宽度是内容直接给出来的那个数，只按新 dpi 重新乘一遍
		initSizeCustom(customW);
		return;
	}
	initSize(sizeBtnCount, sizeWithColors, centerOnBtn, sizeSepCount);
}

// 录屏条的两档宽度（设置态 / 录制态）都不是"格子 + 滑块"能算出来的，
// 所以直接给内容总宽。centerOnBtn 固定 true：hover 预览时副条是"居中到被悬停的那颗按钮"
// （见 updatePosition），点下去之后必须落在同一个地方，否则用户看到的就是
// "预览在这儿、一点就跳到左边去了"。录屏条固定挂在"录屏"那颗按钮下面，与预览位置一致
void ToolSub::initSizeCustom(float contentW)
{
	hasTools = true;
	sizeCustom = true;
	customW = contentW;
	centerOnBtn = true;
	setSize(contentW, getDesiredHeight() / dpi);
}

D2D1_COLOR_F ToolSub::getSelectedColor() const
{
	return ui::Color(colors[selectColorIndex]).getD2DColor();
}

UINT32 ToolSub::getSelectedColorValue() const
{
	return colors[selectColorIndex];
}

D2D1_COLOR_F ToolSub::getNumberColor(int val) const
{
	// 色板上亮着第二格时按奇偶交替：1 号用先色、2 号用后色、3 号用先色 …（1红2蓝3红）。
	// 只亮一格（默认）时永远用先色
	const UINT32 color = (hasColor2 && val % 2 == 0) ? colors[selectColor2Index] : colors[selectColorIndex];
	return ui::Color(color).getD2DColor();
}

float ToolSub::getSliderVal() const
{
	return sliderVal*dpi;
}

bool ToolSub::hasContent()
{
	return hasTools;
}

void ToolSub::hideTools()
{
	// 录制中这条就是录制条，是唯一的入口：主条已经藏了，再把它收掉就只能杀进程
	// （用户报的"运行态偶发丢失"）。不收，也不清 hasTools —— 那条路会在
	// layoutTools 里被当成"没内容"跳过，录制条同样回不来
	if (win && win->inVideoMode()) return;
	hasTools = false;
	if (tip) tip->hide();
	if (!isVisible) return;
	hide();
	isVisible = false;
}

void ToolSub::ensureShown()
{
	if (!hasTools) return;
	// 以窗口真实的可见状态为准（isVisible 只是个镜像，宿主可能在外面直接 hide 过）
	if (hwnd && IsWindowVisible(hwnd)) {
		isVisible = true;
		return;
	}
	show();
	isVisible = true;
}

// ToolSub 永远紧贴 ToolMain 下方（三种模式都是），所以只跟着 ToolMain 走。
// x 默认与 ToolMain 左对齐；窄工具栏（mosaic/eraser）改为居中对齐到 ToolMain 上
// "副条正指着的那颗按钮"（预览时是被悬停的那颗，见 ToolMain::getBtnCenterX）。
// 预览态一律居中：主条上已经选了方框、鼠标只是悬停在别的按钮上时，
// 副条要出现在自己那颗图标的下方，而不是钉在方框下面 —— 不选择，移开就回方框。
// 被屏幕边界裁剪后，同步修正箭头位置让它继续指向那颗按钮。
void ToolSub::updatePosition(const RECT& workArea)
{
	if (!hasTools) return;
	// 主条是副条的定位基准（下面还要读它的 x/y/h 和"指着哪颗按钮"）。
	// 两条工具条在 WinCap::ensureAnnotation 里是成对建/成对 reset 的，理论上不会只剩副条，
	// 但返回值宽度后面就跟着解引用，先判一下比事后查崩溃便宜
	if (!win->toolMain) return;
	// btnCenterX 是"副条正指着那颗按钮"中心相对 ToolMain 左边的偏移
	auto btnCenterX = win->toolMain->getBtnCenterX();
	auto mainX = static_cast<float>(win->toolMain->x);
	// 预览态（previewId 非空）也走居中：副条显示的是被悬停工具的内容，
	// 就该落在悬停的那颗图标下面
	const bool previewing = !win->toolMain->previewId.empty();
	auto px = (centerOnBtn || previewing) ? mainX + btnCenterX - w / 2.f : mainX;
	// 居中之后不许往左越出主条左边界：悬停的是最左边那颗按钮、副条又宽（马赛克/橡皮擦
	// 那条带颜色和滑杆）时，居中会把整条推到主条左边一大截 —— 看上去像"飘到别处去了"。
	// 左边一律以主条为墙，右边越界由下面的屏幕边界兜住
	if (px < mainX) px = mainX;
	// 窗口顶边要往上收一个"箭头区"：窗口比可见的框高出那 2px，是留给指向主条按钮的
	// 那个小三角的。那 2px 落在主副条之间的缝里，所以**箭头不占额外空间** ——
	// 可见的框仍然是 34 高，与主条一模一样，整组的总高度也和以前一致。
	// 不往上收的后果：框往下长 2px，副条看着就是"比主条高一截"（用户一眼就看出来了）
	const float arrowPx = toPx(arrowH);
	auto py = static_cast<float>(win->toolMain->y) + win->toolMain->h + mainGap - arrowPx;
	auto upperX = workArea.right - static_cast<int>(w);
	if (upperX < workArea.left) upperX = workArea.left;
	auto finalX = static_cast<int>(px);
	if (finalX < workArea.left) finalX = workArea.left;
	if (finalX > upperX) finalX = upperX;
	// 箭头始终指向按钮中心的屏幕位置，换算成窗口内坐标
	arrowX = mainX + btnCenterX - finalX;
	setPosition(finalX, static_cast<int>(py));
	// 以窗口真实的可见状态为准，别只信 isVisible 这个镜像变量：
	// 宿主可能在外面直接 hide() 过（比如 WinCap::restartSelect、右键收起工具条），
	// 那样 isVisible 还是 true、窗口却已经藏起来了，这里就会跳过 show，工具条再也不出来
	bool reallyVisible = hwnd && IsWindowVisible(hwnd);
	if (reallyVisible) {
		isVisible = true;
		refresh();
	}
	else {
		show();
		isVisible = true;
	}
}

