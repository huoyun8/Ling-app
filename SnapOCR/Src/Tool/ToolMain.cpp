#include "pch.h"
#include "../Win/WinHost.h"
#include "../History.h"
#include "../Lang.h"
#include "../Tip.h"
#include "ToolMain.h"
#include "ToolMetrics.h"
#include "ToolSub.h"


ToolMain::ToolMain(WinHost* win) : ui::WinBase(), win(win)
{
	// 跟着宿主窗口的缩放走：WinBase 构造里取的是系统 dpi，WinHost 可能在另一块缩放比例不同的屏上
	dpi = win->dpi;
	// 初始位置由 WinHost::layoutTools() 统一决定，这里只算尺寸
	x = win->x;
	// win->h 是浮点、x/y 是整物理像素：这里显式取整，别再靠 float → int 的隐式截断（C4244）。
	// 截断从"三项相加之后"挪到"加 y 之前"，对整数 y 来说结果完全一样；
	// 反正这个值马上会被 layoutTools 覆盖，只为让读代码的人不觉得这里藏着一次静默转换
	y = win->y + static_cast<int>(win->h + 5.f * win->dpi);
	refreshSize();
	// 点按钮会把 ToolMain 激活，此后键盘消息进的是它而不是 WinHost。
	// 直接把按键转触给 WinHost 的同名事件，快捷键在两个窗口上表现一致。
	onKeyDown.add([this](UINT key) { this->win->onKeyDown(key); });
	// DPI 变了（工具条被挪到缩放比例不同的显示器上，或者用户改了系统缩放）：
	// Ui 层只会把窗口按系统给的建议矩形整体缩放一遍，我们自己定的那套摆放规则不会重跑，
	// 工具条就歪在别处了。位置也不能在 onDpiChanged 里直接改 —— 那个事件在 Ui 层应用建议矩形
	// 之前触发，改了马上被覆盖，所以这里只记个标记，等建议矩形应用后紧随而来的 WM_SIZE 再动手
	onDpiChanged.add([this]() { dpiChanged = true; });
	onSizeChanged.add([this]() {
		if (dpiChanged) {
			dpiChanged = false;
			refreshSize();                    //宿主的摆放规则要用宽高，先按新 dpi 把尺寸定下来
			this->win->layoutTools();
		}
	});
	// hover 弹出副编辑器的延迟显示/收起：显示延迟给"快速扫过"兜底，收起延迟给"滑进副条"留时间
	onTimer.add([this](UINT id) {
		// 录制中主条已经藏了，副条就是录制条。这两条定时器要是还跑，
		// 就会把录制条换回设置态（showSubPreview）或整个收掉（hideTools）——
		// 用户报的"运行态偶发丢失"正是这条路。录制期间一律不做事
		if (this->win->inVideoMode()) return;
		if (id == kSubShowTimer) {
			// 延迟显示到期：真正把副条摆出来。期间鼠标可能又移到了别的按钮上（pendingShowId
			// 已更新），也可能已经离开主条（onLeave 清了 pendingShowId），以当前值为准
			const std::wstring target = pendingShowId;
			pendingShowId.clear();
			if (target.empty() || target == curId) return;
			previewId = target;
			showSubPreview(target);
			this->win->layoutTools();
			return;
		}
		if (id != kSubHideTimer) return;
		// 光标还在主条或副条里就不收
		POINT pt{};
		GetCursorPos(&pt);
		if ((pt.x >= x && pt.x < x + (int)w && pt.y >= y && pt.y < y + (int)h)
			|| pointInSub(pt)) return;
		// 有选中工具（curId 非空）：离开后回落显示 curId 的副条（把 hover 预览清掉），
		// 而不是整个收起 —— 用户可能只是扫过别的按钮没点，最终还停在选中的工具上
		if (!curId.empty()) {
			if (!previewId.empty()) {
				previewId.clear();
				showSubPreview(curId);
				this->win->layoutTools();
			}
			return;
		}
		// 没选中任何工具：纯预览态，离开就整个收起
		previewId.clear();
		// 注意用 this->win 而不是 win：构造函数参数 win 与成员同名，直接写 win
		// 会被解析成构造函数参数（需显式捕获），this->win 才明确是成员
		if (this->win->toolSub) this->win->toolSub->hideTools();
		this->win->layoutTools();
	});
	// 不再带 WS_EX_TOPMOST：主条是截图窗的从属窗口（owner 在 WinCap::ensureAnnotation 里设），
	// z 序跟着截图窗走。自己再声明 TOPMOST 的话，它就跳进"置顶组"里去了 —— 用户看到的
	// "选择框在 1 层、标注器在 3 层、别的程序夹在中间"就是这么来的：选区边框被盖住、标注器盖不住。
	// NOACTIVATE 与副条一致：点按钮不抢焦点，键盘仍归截图窗
	createNativeWindow(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, WS_POPUP);
}

// 宽度 = 各按钮格子之和（btnIds 里的 "|" 是分隔条，占 sep 宽），高度 = 一个格子。
// 格子边长来自 ToolMetrics，与副编辑条同一套 —— 两条工具的按钮因此是同一个尺寸
void ToolMain::refreshSize()
{
	float logicW{ 0.f };
	for (auto& id : btnIds) {
		logicW += (id == L"|") ? ToolMetrics::sep : ToolMetrics::cell;
	}
	// 高度 = 一个格子 + 上下两条边框。副条的内容区也是一个格子、外框同样两条边框，
	// 两边按同一个口径算，看起来才是同高的两条 —— 以前主条只报格子高、副条却把顶部
	// 箭头区也算进总高，副条就比主条高出一截（用户看到的"副编辑器明显比主编辑器大"）
	setSize(logicW, ToolMetrics::cell + ToolMetrics::border * 2.f);
}

ToolMain::~ToolMain()
{
}

// 返回"副条现在该指向的那颗按钮"的中心相对 ToolMain 左边的偏移（物理像素）。
// 优先级：hover 预览（previewId）> 当前选中（curId）。
// 预览时副条显示的就是被悬停那颗工具的内容，箭头和居中都该指着它 ——
// 用户的口径："主条已经选了方框，悬停预览时副条不能只定在方框下方，
// 也要出现在自己图标的下方；不选择还是会回到方框"。
// 以前这里 curId 优先，预览别的工具时副条（和箭头）仍钉在选中的方框上
// btnIds 含分隔符而 btns 不含，所以要单独维护 btns 的下标，不能拿 i 去索引 btns。
// 按"格子"累加而不是按按钮实际宽度：按钮在格子里四边各留了 pad，
// 拿按钮宽度累加会把每一条缝都漏掉，越往右偏得越多（箭头就指不准了）
float ToolMain::getBtnCenterX()
{
	const std::wstring& id = previewId.empty() ? curId : previewId;
	float result{ 0.f };
	size_t btnIndex{ 0 };
	for (size_t i = 0; i < btnIds.size(); i++)
	{
		if (btnIds[i] == L"|") {
			// 分隔条不在 btns 里，宽度与 onCreated 里 spliter 的 setWidth(sep) 一致
			result += ToolMetrics::sep * dpi;
			continue;
		}
		if (id == btnIds[i]) {
			// btns 与 btnIds 的非分隔符项一一对应，但建窗失败时 onCreated 没跑过，
			// btns 是空的 —— 不判越界就是拿空 vector 去下标
			if (btnIndex >= btns.size()) return result;
			result += ToolMetrics::cell * dpi / 2.f;
			return result;
		}
		result += ToolMetrics::cell * dpi;
		btnIndex++;
	}
	return result;
}

void ToolMain::onCreated()
{
	tip = std::make_unique<Tip>(this);
	body->setBg(0xFFFFFFFF);
	body->setBorder(ToolMetrics::border, 0xA8A8A8ff);
	body->setAlignItems(ui::Align::Center);
	body->setFlexDirection(ui::FlexDirection::Row);
	for (size_t i = 0; i < btnIds.size(); i++)
	{
		auto& id = btnIds[i];
		if (id == L"|") {
			auto spliter = body->makeChild<ui::Node>();
			// 传 sep 而不是 dpi：Node 的 setWidth 内部已经乘过一次 dpi，
			// 再传 dpi 就是 dpi²（150% 缩放下 2.25px、200% 下 4px），
			// 而 getBtnCenterX 按同样的 sep 记这一格的宽度，两者对不上就会让副条慢慢偏出去
			spliter->setWidth(ToolMetrics::sep);
			// body 的 alignItems 是 Center，只定宽不定高的话子节点在交叉轴上按内容高度算，
			// 而 Node 没有内容 —— 高度直接是 0，分隔条根本画不出来（以前就是这样，
			// 撤销/重做前后那两条分隔符一直是"隐形"的）。显式撑满一行才有那条竖线
			spliter->setHeightPercent(100.f);
			spliter->setBg(0xDDDDDDff);
		}
		else {
			auto btn = body->makeChild<ui::Button>();
			btn->setId(id);
			btn->setIcon(btnNames[i].c_str());
			// 按钮本体 = 格子减去四边留白，纵横都按这一条来；左右留白用 margin 补上，
			// 于是每个格子的宽度仍是 cell，与 refreshSize 的累加严格对上
			// 每颗按钮严格占一格：宽高都是 btn，左右 margin 把这一格补到 cell。
			// 不再靠 flexGrow 均分 —— 均分的前提是窗口宽度一分不差，差 1px 就会
			// "有的按钮宽一点、有的窄一点"，而固定宽高在哪种缩放下都一致
			btn->setWidth(ToolMetrics::btnAt(dpi));
			btn->setHeight(ToolMetrics::btnAt(dpi));
			btn->setMarginLeft(ToolMetrics::padAt(dpi));
			btn->setMarginRight(ToolMetrics::padAt(dpi));
			btn->setFlexGrow(0.f);
			btn->setHoverBg(C_BTN_HOVER);   // 悬停：半透明黑压暗一档（与副编辑条一致）
			// 图标是矢量路径，这里的"字号"就是图标盒边长，按物理像素取整避免半像素模糊
			btn->setFontSize(ToolMetrics::fontFor(ToolMetrics::iconFont, dpi));
			btn->onClick.add([this](ui::Button* btn) {onClick(btn);});
			// hover 弹出副编辑器：进入即预览显示，离开后延迟收起（给鼠标滑进副条留时间）
			btn->onEnter.add([this](ui::Button* btn) {
				// 动作类按钮（撤销/重做/保存/复制/关闭/图钉）没有副编辑器，跳过。
				// 分隔符不是按钮。其余工具按钮按 id 预览
				if (btn->id == L"undo" || btn->id == L"redo" || btn->id == L"save"
					|| btn->id == L"clipboard" || btn->id == L"close" || btn->id == L"pin") {
					// "不预览"不等于"什么都不做"：悬停规范是"停在哪颗按钮上，
					// 副条就显示哪颗的内容；那颗没有副条，就把当前停留的收掉"。
					// 不收的后果：鼠标停在"撤销"上，副条还挂着上一个工具（矩形）的内容，
					// 与"悬停谁显示谁"的规范不一致，看起来像副条卡住不收
					killTimer(kSubShowTimer);   // 取消别的按钮还没到期的延迟显示
					killTimer(kSubHideTimer);
					pendingShowId.clear();
					// 有选中工具（curId 非空）：正预览别的工具时移到动作按钮，回落显示选中工具；
					// 没选中工具：纯预览态，直接整个收起
					if (!curId.empty()) {
						if (!previewId.empty()) {
							previewId.clear();
							showSubPreview(curId);
							win->layoutTools();
						}
					}
					else if (!previewId.empty()) {
						previewId.clear();
						if (win->toolSub) win->toolSub->hideTools();
						win->layoutTools();
					}
					return;
				}
				// 已经有选中工具（curId 非空）时，hover 到别的按钮仍允许预览它的副条
				// （用户可能想换工具）；hover 回"当前选中"那颗则回落显示 curId 的内容。
				// 是否真的换工具，等用户在副条里点击才定 —— 见 promoteToSelected。
				if (btn->id == curId) {
					// 正预览着别的工具，鼠标移回当前选中那颗：清掉预览，回落显示 curId
					if (!previewId.empty() || !pendingShowId.empty()) {
						killTimer(kSubShowTimer);
						killTimer(kSubHideTimer);
						pendingShowId.clear();
						previewId.clear();
						showSubPreview(curId);
						win->layoutTools();
					}
					return;
				}
				// 正预览同一个，不必重复
				if (btn->id == previewId) {
					killTimer(kSubShowTimer);
					pendingShowId.clear();
					return;
				}
				// 延迟显示：先记下目标、启动定时器，到期才真正重建副条。
				// 鼠标横向快速扫过主条时（每颗停留 < 延迟）不会触发，也就不跳界面
				pendingShowId = btn->id;
				killTimer(kSubHideTimer);   // 取消可能挂着的延迟收起
				killTimer(kSubShowTimer);
				setTimer(kSubShowDelay, kSubShowTimer);
			});
			btn->onLeave.add([this](ui::Button* btn) {
				(void)btn;
				// 离开主条按钮：取消还没到期的"延迟显示"，再走延迟收起
				killTimer(kSubShowTimer);
				pendingShowId.clear();
				scheduleSubHide();
			});
			tip->bind(btn, Lang::get(std::format(L"tool.{}", id)));
			btns.push_back(btn);
		}
	}
	// 关闭按钮：图标改红色（常态与悬停都保持红色）
	for (auto b : btns) {
		if (b->id == L"close") {
			b->setColor(0xE63946FF);
			b->setHoverColor(0xE63946FF);
		}
	}
	show();
}

void ToolMain::applyNormalStyle(ui::Button* btn)
{
	btn->setBg(0);
	btn->setHoverBg(C_BTN_HOVER);   // 悬停：半透明黑压暗一档（与副编辑条一致）
	// 图标恢复深色；关闭按钮保持红色
	if (btn->id == L"close") {
		btn->setColor(0xE63946FF);
		btn->setHoverColor(0xE63946FF);
	}
	else {
		btn->setColor(0x333333FF);
		btn->setHoverColor(0x333333FF);
	}
}

// 取消选中：与 onClick 选中某个按钮是对称操作，只是没有新的选中项。
// 编辑器全程可见：取消选中只是回到"拖动选区"态，子编辑器由 WinHost 重排时继续显示（不收起）。
void ToolMain::clearSelectStyle()
{
	if (curId.empty()) return;
	for (auto b : btns)
	{
		if (b->id == curId) {
			applyNormalStyle(b);
		}
	}
	curId.clear();
	previewId.clear();
	pendingShowId.clear();
	killTimer(kSubShowTimer);
}

void ToolMain::cancelSelect()
{
	if (curId.empty()) return;
	clearSelectStyle();
	win->layoutTools();
}

void ToolMain::promoteToSelected(const std::wstring& id)
{
	// 已经是选中态：副条上点的那一下对应的就是当前工具，无需再动
	if (id == curId) return;
	// 点亮新工具、取消旧工具的高亮（复用 onClick 的选中配色）
	for (auto b : btns)
	{
		if (b->id == curId) applyNormalStyle(b);
		if (b->id == id) {
			b->setBg(C_BTN_ON);
			b->setHoverBg(C_BTN_ON_HOVER);
			b->setColor(0x2E6BE6FF);
			b->setHoverColor(0x2E6BE6FF);
		}
	}
	curId = id;
	// 预览态落地为选中态：清掉 previewId / 待显示，副条显隐改由 curId 决定
	previewId.clear();
	pendingShowId.clear();
	killTimer(kSubShowTimer);
	killTimer(kSubHideTimer);
}

void ToolMain::onClick(ui::Button* btn)
{
	// 关闭整个贴图窗口。WinHost 的 onDestroy 里会连带关掉 ToolMain / ToolSub，
	// 但 C++ 对象的释放被推迟到下一轮消息循环，所以这里 return 之后栈上访问 this 仍是安全的。
	if (btn->id == L"close") {
		win->close();
		return;
	}
	// 录制：点这颗就直接开录（收尾在录制条上）。MP4/GIF、系统声、麦克风这些设置
	// 走悬停菜单 —— 悬停仍然弹出副条，改完再点主条这颗就按新设置开录。
	// 走的是副条那颗「开始」的同一条路径（beginVideo → 切录制态），不另起一套
	else if (btn->id == L"video") {
		// 悬停预览态先清掉：否则开录后 previewId 还挂着，停录回来副条会自己弹回来
		previewId.clear();
		pendingShowId.clear();
		killTimer(kSubShowTimer);
		killTimer(kSubHideTimer);
		if (win->toolSub) win->toolSub->videoStart();
		return;
	}
	// 贴图：把标注好的这张图钉在桌面上不消失（长按可拖动移动），
	// 收起两条工具条，只剩图本身 —— 与右键"收起工具条只剩图"是同一个状态
	else if (btn->id == L"pin") {
		win->pinToScreen();
		return;
	}
	// 下面这几个都是"执行一次动作"而不是"切换绘图工具"，做完就返回，不动 curId 和选中态。
	else if (btn->id == L"undo") {
		if (win->history) win->history->undo();
		return;
	}
	else if (btn->id == L"redo") {
		if (win->history) win->history->redo();
		return;
	}
	else if (btn->id == L"save") {
		win->saveToFile();
		return;
	}
	else if (btn->id == L"clipboard") {
		win->copyToClipboard();
		return;
	}
	// 再次点击已选中的按钮 = 取消选中（开关式）。cancelSelect 里已经做了配色复位、
	// 隐藏 ToolSub 和重排，这里直接返回，不要再往下走选中流程。
	if (btn->id == curId) {
		cancelSelect();
		return;
	}
	for (auto b:btns)
	{
		if (b->id == curId)
		{
			applyNormalStyle(b);
		}
		if (b->id == btn->id)
		{
			// 选中：半透明黑压暗（比悬停再深一档），图标换成鲜蓝 —— 底暗、字亮，
			// 一眼能看出"现在是这个工具"。之前是浅蓝近白底，白工具条上几乎看不出来
			b->setBg(C_BTN_ON);
			b->setHoverBg(C_BTN_ON_HOVER);
			b->setColor(0x2E6BE6FF);
			b->setHoverColor(0x2E6BE6FF);
		}
	}
	curId = btn->id;
	// 点击选中后 hover 预览态失效：清掉 previewId 和待显示，副条显隐改由 curId 决定
	previewId.clear();
	pendingShowId.clear();
	killTimer(kSubShowTimer);
	killTimer(kSubHideTimer);
	// toolSub 可能没建起来（ensureAnnotation 的兜底路径把它 reset 掉了）：
	// 少了它只是没有子编辑器，绝不能在按钮回调里解引用空指针
	if (!win->toolSub) {
		win->layoutTools();
		return;
	}
	// 这一下是点击选中：马赛克/橡皮擦要把形状拉回"选框"（默认选框，手绘再点一下）
	showSubPreview(curId, true);
	// curId 变化后 ToolMain 可能要上移给 ToolSub 腾位置，交给 WinHost 重新排布整组
	win->layoutTools();
}

// 按 id 显示对应的副编辑器。hover 预览与点击选中共用这一套分发，保证两者一致。
// 只负责"把副条摆出来"，不动 curId、不改按钮选中态 —— 选中态是 onClick 的事。
// 调用方（onEnter）负责在调用前设 previewId、调用后 layoutTools。
// selecting 只有 onClick 传 true：橡皮擦据此把形状拉回"选框"（用户定的口径），
// 悬停预览传 false，不改变用户上次选好的形状。
// 马赛克不吃这个参数：进条即从配置读形状档，点击选中也不强制复位（与原版一致）
void ToolMain::showSubPreview(const std::wstring& id, bool selecting)
{
	if (!win->toolSub) {
		return;
	}
	if (id == L"rect") {
		win->toolSub->showRectTools();
	}
	else if (id == L"arrow") {
		win->toolSub->showArrowTools();
	}
	else if (id == L"number") {
		win->toolSub->showNumberTools();
	}
	else if (id == L"brush") {
		win->toolSub->showBrushTools();
	}
	else if (id == L"text") {
		win->toolSub->showTextTools();
	}
	else if (id == L"mosaic") {
		win->toolSub->showMosaicTools();
	}
	else if (id == L"eraser") {
		win->toolSub->showEraserTools(selecting);
	}
	else if (id == L"video") {
		win->toolSub->showVideoTools();
	}
	else {
		win->toolSub->hideTools();
	}
}

void ToolMain::onMinMaxInfo(MINMAXINFO* mmi)
{
	mmi->ptMinTrackSize.x = 1;
	mmi->ptMinTrackSize.y = 1;
}

// 鼠标离开主条按钮后，给一个 300ms 窗口让用户把鼠标滑进副条。超时后 onTimer
// 会判断光标位置，既不在主条也不在副条才收。300ms 是"够滑过去、又不会觉得副条赖着不走"的平衡点
void ToolMain::scheduleSubHide()
{
	killTimer(kSubHideTimer);
	setTimer(300, kSubHideTimer);
}

// 判断屏幕坐标是否落在副条（toolSub）窗口内。副条是 WS_POPUP 无边框窗口，
// x/y 是左上角屏幕坐标、w/h 是物理像素宽高，直接比较即可
bool ToolMain::pointInSub(const POINT& pt) const
{
	const auto inside = [&pt](const ui::WinBase& s) {
		return pt.x >= s.x && pt.x < s.x + (int)s.w && pt.y >= s.y && pt.y < s.y + (int)s.h;
	};
	return win->toolSub && win->toolSub->alive() && inside(*win->toolSub);
}