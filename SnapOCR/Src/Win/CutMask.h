#pragma once
#include <Ui/Ui.h>
#include <vector>

// 光标落在哪一块。选区内部是 Inside，外面被选区四边的延长线切成 8 块，各对应一个方位
enum class MaskHit { None, Inside, Left, Top, Right, Bottom, TopLeft, TopRight, BottomRight, BottomLeft };

// 框选遮罩。四块半透明遮罩 + 蓝色选区边框 + 左上角尺寸标签。
// 自己不持有绘制目标，paint 时由宿主窗口把当前的 context 传进来。
// 所有坐标都是宿主窗口的客户区坐标。
//
// —— 智能选区（贴边选区域）的实现分层 ——
// 本类的智能选区是**自研独立实现**，三层协作，全部基于公开 Win32 API。
// 吸附只服务"第一次选区"：悬停预览与首次拖框时把框吸准；选区落定之后再拖边角
// 或移动，一律纯鼠标跟随，不再贴边（贴边会让手在走、框在蹦）
//   几何层（initWinRect / collectHover / nearestWindow）：
//      EnumWindows 按 z 序枚举可见窗口（DWM 扩展框架边界），逐窗口 EnumChildWindows
//      收子控件；visibleRootAt 从系统 z 序实时判"光标下眼睛看得见的那个窗口"，
//      跨窗口遮挡以它为准，窗口内部按面积**升序**（越小的元素越具体）。
//      桌面图标不用这个次序，它走下面的桌面图标层：默认整格。
//   像素层（loadPixels / pixelBoxAt）：
//      整屏 CPU 副本上沿四个方向找颜色突变，能吸到没有 HWND 的内容边界
//      （浏览器内容区、图片、代码行、视频画面），亚毫秒、每帧可算。
//   桌面图标层（refreshDeskIcons / deskIconAt / refineDeskGlyph）：
//      桌面上前两层都给不出正确的框 —— 整块桌面只有一个 Progman/WorkerW（整屏，
//      被过滤），像素层会咬到图标单元格的网格线。这一层直接向桌面的
//      SysListView32 要每一项的图标槽位，再用冻结画面在槽位里扫出图标本体。
//   维护（onSettle）：鼠标停定后做节流重枚举（弹菜单 / 换页签后的新内容）。
// 历史上曾引入过一套基于 UI Automation 的语义层（跨进程元素链 + 后台线程 +
// 桌面图标缓存），第 37 轮已整体剥离 —— 它的能力（细化到无 HWND 元素）像素层
// 已经覆盖且更快，而代价是主线程跨进程停顿与一整套线程基础设施。
// 第 35 轮补的"桌面图标层"是这条原则的延续：不做 UIA，只发 LVM_GETITEMRECT。
class CutMask
{
public:
	CutMask(ui::WinBase* win);
	~CutMask();
	// 鼠标悬停时吸附到光标下的窗口矩形，选区真的变了才返回 true
	bool highlight(POINT pos);
	void startMakeRect(POINT pos);
	void makeRect(POINT pos);
	// 选区四边的延长线把窗口切成九块：中间是 Inside，外面八块各是一个方位。没框出选区时才是 None
	MaskHit hitTest(POINT pos) const;
	// 只认"选区边/角附近 band 像素（里外都算）"这一圈：命中边角返回对应方位，
	// 距边超过 band 的（不管在框内还是框外）一律 Inside / None。
	// 与 hitTest 的九宫格分工不同：hitTest 回答"这个点是哪个方位的"，把半个屏幕都算成 Left；
	// hitEdge 回答"这一点能不能抓起那条边"，用于"选中了工具也允许拖选区"——
	// 只有贴着边/角才抢过去，框外远处的点击仍然归画笔
	MaskHit hitEdge(POINT pos, float band) const;
	// 开始调整：记下方向和起始矩形。按的是边或角时，这一下就把那条边吸到光标处
	void startAdjust(POINT pos);
	// 同上，但方向由调用方指定（hitEdge 判出来的是"带内"方位，比 hitTest 更准）
	void startAdjust(POINT pos, MaskHit hit);
	// 调整中：按 startAdjust 记下的方向改选区
	void adjust(POINT pos);
	// 宽高都大于 0 才算真的框出了东西
	bool hasRect() const;
	// 清空当前选区与尺寸标签，回到还没框选的状态（选择区域过程中点右键 = 重新选择）
	void reset();
	// withFrame=false 只画四块压暗层（不画选区边框和尺寸标签）。
	// 截图窗把"底图 + 压暗层"烘进一张缓存位图，边框/标签每帧现画：
	// 压暗层是全屏 alpha 混合里最贵的一趟，烘进去之后悬停帧只剩一次整屏 blit
	// 压暗层 / 选区边框 / 尺寸标签是可分开画的：
	//   · 烘焙缓存时只画压暗层（withMask=true, withFrame=false）
	//   · 缓存命中时只画边框和标签（withMask=false, withFrame=true）
	//   · 没有缓存就一次画全（两个都 true）
	// 这两个开关必须分开。早先只有一个 withFrame，缓存命中那条路上为了"只补边框"
	// 传了 false，于是每帧又把压暗层叠了一遍 —— 两层 32% 叠成 54%，用户看到的
	// 就是"压暗发黑、背景发闷"
	void paint(ID2D1DeviceContext* ctx, bool withMask = true, bool withFrame = true);
public:
	D2D1_RECT_F maskRect{};
	float strokeWidth{ 2.f };
	// 智能吸附总开关，悬停吸附（highlight）与拖动吸附（makeRect）都归它管。
	// 默认开：光标移动时自动识别光标下的窗口 / 控件，按快捷键首次进入就是这个状态。
	// 标注阶段右键重进选区（WinCap::restartSelect）先把它关掉回到"+"，鼠标第一次
	// 移动 / 第一次按下拖框时再由 WinCap 打开 —— 先到+，移动才会智能选区
	bool autoSnap{ true };
	// —— 智能选区增强（对齐 Snipaste 的三大能力）——
	// ① 像素级边缘吸附：把一份 CPU 可读的整屏位图喂进来，之后 makeRect/adjust 的每条边
	//    在窗口 / 控件边界之外，还会就近贴到画面里的颜色突变处（浏览器内容区、图片、
	//    代码行、视频画面……凡是眼睛能看出来的边都能吸）。
	//    直接接管宿主手里那份缓冲（BGRA / stride = pw*4 / 自上而下），不拷贝：
	//    以前是从 GPU 位图 Map 出来再 memcpy 一遍，同一次会话里整屏像素要留三份
	//    （shotBuf、GPU 副本、这里的数组），4K 下就是一百多 MB
	void adoptPixels(std::vector<uint8_t>&& bits, int pw, int ph, int stride);
	// ② 同一位置叠了多个元素时（按钮上还有输入框、面板里还有面板），滚轮逐个循环候选
	void cycleHover(POINT pos, int dir);
	// 放大镜取色用：整屏像素副本是否就绪 + 读某个像素（返回 BGR 打包：低字节=B）。
	// 直接读截图时留在内存里的整屏副本，O(1)；替代此前每帧 GetPixel 实时取屏 ——
	// 那个走 GDI，慢，个别显卡驱动/远程桌面上一次要几十毫秒，拖动放大镜时像卡死
	bool pixelReady() const { return pixReady; }
	uint32_t pixelAt(int x, int y) const { return pixAt(x, y); }
	// 鼠标停住之后的维护（宿主定时器 101 驱动，见 highlight）：节流重枚举识别表。
	// 重枚举是 EnumWindows + 逐窗口 EnumChildWindows，桌面忙的时候几毫秒到几十毫秒，
	// 不能挂在 mousemove 上；鼠标停定这一下做正合适。由宿主调用，所以必须是 public
	void onSettle();
	// 悬停断开后"挂住"的到期回调（宿主定时器 103 驱动，见 highlight）：
	// 光标停在没有已知目标的空白处超过 kHoldMs 才真正收框，避免压暗层一闪一闪
	void onHoldExpire();
private:
	void initWinRect();
	// 尺寸标签只在 maskRect 变化时重建，不必每帧现建
	void makeLayout();
	// 把屏幕矩形裁剪进宿主窗口客户区并转成客户区坐标，合法才加入识别表。
	// winRank 是所属顶层窗口的 z 序位次（0 = 最上层），子控件继承父窗口的位次。
	// hwnd 记的是所属顶层窗口（不是控件自己）：用于"只认系统点名那个窗口"的过滤
	void pushClippedRect(const RECT& r, int winRank, bool topLevel, HWND hwnd);
	// 递归收集顶层窗口里的子控件矩形，让识别能细化到按钮 / 面板等元素
	static BOOL CALLBACK collectChild(HWND hwnd, LPARAM lp);
	// 整屏像素副本（BGRA/X，stride 对齐），只读
	uint32_t pixAt(int x, int y) const;
	static int colorDist(uint32_t a, uint32_t b);
	// 像素层：在冻结截图里量出"光标所在的那个框"（四个方向各走到最近的真实边界）。
	// 这是唯一能"每帧都算"的识别层 —— 几何层看不到没有 HWND 的内容，
	// 像素层是内存直读，亚毫秒，手一动就跟上
	bool pixelBoxAt(POINT pos, D2D1_RECT_F& out) const;
	// 收集光标下的候选：先按 z 序定出最上层的那个窗口，只有它的矩形参选，
	// 再按面积升序（同一个窗口里越小的元素越具体）。桌面图标走专属分支，
	// 不受这个次序影响：整格（槽位 + 文字标签）先入列，默认选整格
	// 纯几何层 + 像素层，都是微秒级
	void collectHover(POINT pos, std::vector<D2D1_RECT_F>& out);
	// 光标落在所有窗口之外时，找"最近的顶层窗口"：距离阈内就吸它。
	// 用户把鼠标往窗口边上一靠就该选中那个窗口，而不是必须精确进到窗口里面
	bool nearestWindow(POINT pos, D2D1_RECT_F& out) const;
	// —— 桌面图标层 ——
	// 桌面不是窗口树能描述的：整块桌面只有一个 Progman/WorkerW，四十几个图标全在
	// 它挂的那一个 SysListView32 里，没有任何一个是窗口。前两层在桌面上都跑偏：
	//   · 几何层：Progman 铺满整屏，被"整屏不当候选"过滤掉，等于没有；
	//   · 像素层：图标边缘是抗锯齿软边，四个方向的"第一跳"会越过图形咬到单元格的
	//     网格线（实测框出 71×141 的格子，而图标本体只有 35~48px），验边分数还在
	//     阈值附近抖，同一个位置两次捕获能过与不能过看运气。
	// 所以桌面单开一层：向 SysListView32 要每一项的图标槽位（explorer 自己算的，
	// 准），再用冻结画面在槽位里扫出图标本体（贴住图形的抗锯齿边缘，紧）。
	// 表缓存在内存里，悬停时只是几十个矩形的比较，零跨进程调用；刷新交给 onSettle。
	struct DeskIcon
	{
		RECT slot{};   // 图标槽位（屏幕坐标）—— explorer 给的，上下已经贴住图形
		RECT hit{};    // 选区 / 命中区 = LVIR_BOUNDS 整项包围盒（图标 + 全部标签行，
		               // 两行标题也完整包住），取不到时退回"槽位 + 45% 标签带"
	};
	// 刷新桌面图标表。force=true 立即扫（冷启动 / 停定维护），false 走 800ms 节流，
	// 过期只标待办、不现场扫。返回表里是否有内容
	bool refreshDeskIcons(bool force);
	// 光标落在某个桌面图标上时给出两个粒度：图标本体 / 槽位连文字标签
	bool deskIconAt(POINT clientPos, D2D1_RECT_F& glyphOut, D2D1_RECT_F& itemOut);
	// 在槽位里按"四角主色当壁纸底色"扫出图标本体的包围盒（返回屏幕坐标）。
	// 壁纸太花 / 前景太少（背景估计错了）时返回 false，调用方退回用槽位
	bool refineDeskGlyph(const RECT& slotScreen, RECT& glyphScreen) const;
private:
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBorder;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
	D2D1_RECT_F layoutRect{};
	// 识别表的一项。z 序必须跟着一起存：只靠面积挑候选会让背景窗口的小控件
	// 盖过前景窗口（详见 collectHover 的注释）
	struct SnapItem
	{
		D2D1_RECT_F rc{};
		int win{ 0 };     // 所属顶层窗口的 z 序位次，0 = 最上层
		bool top{ false };// 这一项本身是顶层窗口，不是子控件
		HWND hwnd{ nullptr }; // 所属顶层窗口句柄，用于"只认系统点名窗口"的过滤
	};
	std::vector<SnapItem> winRect;
	// 光标当前位置的候选元素（面积升序）与滚轮循环下标
	std::vector<D2D1_RECT_F> hoverCands;
	size_t hoverIdx{ 0 };
	// collectHover 最近一次算出的"光标真正压着的那个窗口"。
	// collectHover 是纯计算＋出候选，调用方（highlight）拿到候选之后还要判断
	// "这个窗口在不在识别表里"，所以把它单独存一份，别让调用方再算一遍
	HWND lastHitRoot{ nullptr };
	// 识别表上次重枚举的时刻（毫秒）。光标落在所有已知矩形之外时，
	// 界面可能刚变过（弹出菜单 / Tooltip / 切换标签页），节流重枚举让新内容也能被吸到
	ULONGLONG lastEnumTick{ 0 };
	// 需要重枚举识别表。这件事要 EnumWindows + 逐窗口 EnumChildWindows，
	// 桌面忙的时候几毫秒到几十毫秒 —— 挂在 mousemove 上就是"动一下就顿一下"，
	// 所以只标待办，真正干活交给 onSettle（鼠标停住之后）
	bool enumWant{ false };
	// 悬停候选断开后的"挂住"截止时刻（GetTickCount64）。桌面图标格间几十毫秒就跨过去一格，
	// 立刻收框会让压暗层亮一下暗一下；挂住 kHoldMs 期间落到新目标就作废，真的停空才收
	ULONGLONG holdUntil{ 0 };
	// 最近一次"悬停智选"落下的框。挂住到期时只收这一份 —— 用户自己拖出来的框
	// （draw / adjust）不能因为这个 700ms 的防闪定时器到点而被抹掉
	D2D1_RECT_F previewRect{};
	// —— 桌面图标层的状态 ——
	// deskList：桌面那个 SysListView32；deskIcons：它的每一项；deskStamp：上次扫的时刻。
	// deskWant 与 enumWant 同一个道理：过期了只标待办，扫的工作交给 onSettle
	HWND deskList{ nullptr };
	std::vector<DeskIcon> deskIcons;
	ULONGLONG deskStamp{ 0 };
	bool deskWant{ false };
	// 整屏像素副本：GPU 位图读不了像素，进截图时由宿主（WinCap）复制一份 CPU 可读的喂进来
	std::vector<uint8_t> pix;
	int pixStride{ 0 }, pixW{ 0 }, pixH{ 0 };
	bool pixReady{ false };
	POINT pressPos{};
	ui::WinBase* win{ nullptr };
	float paddingMargin{ 3.f };
	MaskHit adjustHit{ MaskHit::None };
	// 调整全程以按下那一刻的矩形为基准算，不做累加，免得漂移
	D2D1_RECT_F adjustStartRect{};
	POINT adjustPressPos{};
	// 选区最小尺寸，别让它塌成 0 宽 0 高
	static constexpr float minSize{ 4.f };
};
