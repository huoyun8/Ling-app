#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "../Qr/QrCodec.h"
#include "../Hash/FileHash.h"
#include "../Ocr/OcrLayout.h"
#include "VfBox.h"
#include "RnList.h"

// 程序主窗口：六页 —— 文字识别 / 二维码 / 文件校验 / 文件改名 / 定时关机 / 设置（页签从左到右即此顺序）。
// 纯 Win32（自建窗口类 + 自绘按钮），不走 Ui 布局，为的是把每一样东西钉死在
// 参考的那个像素坐标上：客户区 585×445@96dpi，边距 14，按钮 132×46，行距 58 / 12。
// 所有坐标都过 sc() 换算一遍 DPI，高分屏上是整体等比放大，不会东倒西歪。
class WinQr
{
public:
	// 页与控件 ID 对外可见：WinQr.cpp 里的控件分组表要按 id 建静态数组，
	// 页面切换 / 重排 / 命中检测都复用那一份，不再各自抄一遍字面量。
	// 这里只是"页的编号"，显示顺序由 WinQr.cpp 的 kTabIds 决定，两者互不影响。
#if SNAP_HAS_OCR
	// 文字识别排在 0：它就是主界面 —— 打开程序第一眼看到的那一页
	enum Page { PageOcr = 0, PageQr = 1, PageVerify = 2, PageRename = 3, PageShutdown = 4, PageSetting = 5 };
#else
	// 不带 OCR 引擎：没有「文字识别」页，二维码页直接当首页（编号整体前移，
	// setPage 的上界用的是 PageSetting，边界判断不用跟着改）
	enum Page { PageQr = 0, PageVerify = 1, PageRename = 2, PageShutdown = 3, PageSetting = 4 };
#endif

	// 文件校验页的网格：摆控件在 WinQrVerify.cpp、画进度条在 WinQr.cpp，
	// 两边必须看同一份数字，所以提到头文件里，谁也别再抄一遍。
	// 底部这一条带子里左右两栏**等高**：321（按钮行顶边）到 421（进度条底边）。
	//  - 右边：五个勾选竖排，行距 20 / 行高 20，五行正好 321..421；
	//  - 左边：按钮 321..351，剩 70 高给两条进度条（24 高），
	//    top 362 / 397，第二条 397..421 —— 两栏顶边、底边都对得上。
	// 页面底 431，两栏下面各留 10 的页边，谁也不多出一块。
	enum VfGrid {
		VfListY = 44, VfListH = 271, VfListW = 557,
		// 结论只把结果框整圈外框染色（相同绿 / 不同红），不再占框内一栏宽度
		VfBtnY = 321, VfBtnH = 30,
		VfChkX = 470, VfChkY = 321, VfChkW = 101, VfChkH = 20, VfChkPitch = 20,
		// 行号是"条子的顶边"，实际条子画在 +3 处（见 drawVerifyPage），
		// 所以 359+3=362、394+3=397 才是两条进度条的顶边
		VfRowFile = 359, VfRowTotal = 394,
		// 进度条与按钮行同宽：14..448。两个标签原来是独立的 STATIC 控件，
		// 现在画进条子内部（左标签 + 右百分比），少了 34 宽的一列，条子能往左撑到头。
		// 右边不能跟着结果框一起到 571 —— 那儿是五个算法勾选（470..571），
		// 条子压上去就直接盖住勾选框了
		VfBarX = 14, VfBarW = 434, VfBarH = 24,
	};
	enum Ctrl : int {
		TabQr = 4120, TabShutdown = 4121, TabSetting = 4122, TabVerify = 4123, TabRename = 4124,
#if SNAP_HAS_OCR
		// —— 文字识别页（主界面）——
		TabOcr = 4130,
		// 4300 段：搜索框 + 查找 + 排版 + 结果框 + 识别/复制/清空 + 字符数
		// （4308 空着：原来那颗「关压缩软件」已经去掉 —— 设置页本来就有全局热键，
		// 页面上再摆一颗只是把底部第四格占掉，字数额反而只能在顶行挤着）
		OcrSearch = 4304, OcrBtnFind = 4305,
		OcrEdit = 4300, OcrBtnRec = 4301, OcrBtnCopy = 4302, OcrBtnClear = 4303,
		OcrLblCount = 4306, OcrBtnLayout = 4307,
#endif
		QrEdit = 4101, QrBtnDownload = 4102, QrBtnScreen = 4103,
		QrBtnCopy = 4105, QrBtnUpload = 4106, QrBtnClear = 4107,
		// 4108 / 4109 空着：原来二维码页右列那两颗「关闭压缩软件」「截图」已经去掉
		// （截图有 F1 热键、关压缩软件有全局热键，页面上不必各占一格）。
		// 号段不复用，免得和旧配置里存的 id 撞上
		SdEditDay = 4520, SdEditHour = 4521, SdEditMin = 4522, SdEditSec = 4523,
		SdUpDay = 4600, SdDnDay = 4601, SdUpHour = 4602, SdDnHour = 4603,
		SdUpMin = 4604, SdDnMin = 4605, SdUpSec = 4606, SdDnSec = 4607,
		SdStart = 4560, SdPause = 4561, SdReset = 4563,
		SdLblDay = 4584, SdLblHour = 4585, SdLblMin = 4586, SdLblSec = 4587,
		// 设置页快捷键三行（截图 / 识别 / 压缩软件全关）。号段空着不复用，
		// 免得和旧配置里存的控件 id 撞上
		SetRecCap = 5213, SetRecQr = 5207, SetRecZip = 5214,
		SetDispCap = 5211, SetDispQr = 5208, SetDispZip = 5209,
		SetLblCap = 5221, SetLblQr = 5224, SetLblZip = 5225,
		SetChkAutoStart = 5241,
		// 后台「压缩包防多开」开关：管的是"同一个压缩包被开出多个窗口"时自动关掉多余的，
		// 跟设置页那颗「压缩软件全关」热键（一次性全关）是两件事
		SetChkZipGuard = 5242,
		// 「截图时隐藏主界面」：控制截图 / 识码这几个入口要不要先把主窗口收起来
		SetChkHideMain = 5243,
		// 「关闭时最小化到托盘」：开着的话点关闭只是收进托盘（进程还在，定时关机之类
		// 的后台活继续走）；关着点关闭就是真退出
		SetChkCloseTray = 5244,
		// 「只允许单实例」：勾上时第二个进程只把已在跑的窗口亮出来；
		// 关掉就是各开各的（热键 / 托盘各一套，注册失败的那个会收到提示）
		SetChkSingleInstance = 5245,
		// 「启动时显示主界面」：勾上双击直接出界面，不勾只驻留托盘
		SetChkShowMainOnStart = 5246,
		// 保存位置两行（截图 / 视频）：路径框只读显示当前目录，「更改」弹系统选文件夹框
		SetDirShotLbl = 5251, SetDirShotDisp = 5252, SetDirShotBtn = 5253,
		SetDirVideoLbl = 5254, SetDirVideoDisp = 5255, SetDirVideoBtn = 5256,
		// —— 文件校验页 ——
		VfList = 5301, VfBrowse = 5302, VfClear = 5303, VfCopy = 5304,
		VfSave = 5305, VfStop = 5306,
		// 第 4 行原来是 SHA1，原地换成 SHA256（id 沿用 5313，勾选位置也不动）。
		// 5315 之前试过给"第六行"用，现在已经没有这一行，号段空着不复用
		VfChkVer = 5310, VfChkDate = 5311, VfChkMd5 = 5312, VfChkSha256 = 5313,
		VfChkCrc = 5314,
		// 5320 / 5322（原来「文件」「总计」两个 STATIC 标签）已废：文字画进进度条里，
		// —— 文件改名页（实现见 WinRename.cpp / RnList.cpp）——
		// 号段整体让到 54xx：53xx 是文件校验页的，旧配置里可能存着
		RnListView = 5400,
		// 工具栏七颗：添加文件 / 添加文件夹 / 复制名字 / 清空 / 重置 / 撤销 / 执行改名
		// （5406 空着：原来那颗「刷新」只会重读大小、不解决任何实际问题，已去掉）
		RnAddFiles = 5401, RnAddFolder = 5402, RnCopy = 5403, RnClear = 5404,
		RnReset = 5405,
		// 撤销 / 执行改名（执行那颗是主色）
		RnUndo = 5407, RnRun = 5408,
		// 「文件名」组：三个大小写 + 数字递增
		RnCaseUpper = 5410, RnCaseLower = 5411, RnCaseTitle = 5412, RnNumEnable = 5413,
		// 转换 = 只读七项列表；改为 = 同样式的只读下拉（暂空，留作后续扩展）
		RnConvert = 5417, RnConvTo = 5435,
		// 前缀 / 后缀 = 可手打，候选是占位符 token（挂在「追加」面板第二行）
		RnPrefix = 5418, RnSuffix = 5419,
		// 「添加」开关：勾上 = 前缀 / 后缀两条规则参与改名，不勾 = 整条忽略
		RnAffixOn = 5436,
		// 「扩展名」组：全部大写 / 全部小写 / 保持不变 / 删除 + 改为下拉
		RnExtUpper = 5420, RnExtLower = 5421, RnExtKeep = 5422, RnExtDrop = 5423,
		RnExtSet = 5424,
		// 「替换」双栏：查找 / 替换为
		RnFind = 5430, RnReplace = 5431,
		// 「过滤」下拉：按已拖入文件的格式筛列表
		RnFilter = 5432,
		// 「替换」面板的两颗互斥开关：区分大小写 / 替换所有
		RnRepCase = 5433, RnRepAll = 5434,
		// 「序号」面板：序号开关（选中 = 这条规则生效）+ 起始值 / 增量 / 位数
		RnSeqOn = 5450, RnSeqStart = 5451, RnSeqStep = 5452, RnSeqDigits = 5453,
		// 「删除」面板：空格 / 小写字母 / 中文 / 特殊字符 / 数字 / 大写字母 六颗开关 + 文本
		RnDelSpace = 5460, RnDelLower = 5461, RnDelCjk = 5462, RnDelSpecial = 5463,
		RnDelDigit = 5464, RnDelUpper = 5465, RnDelText = 5466,
		// 底部：全选 / 反选
		RnCheckAll = 5440, RnInvert = 5441,
		// 列表里"点新文件名那一格"弹出来的原地编辑框
		RnEdit = 5442,
		// 没有控件了。号段空着不复用，免得和旧配置里存的控件 id 撞上
	};

	~WinQr();
	// 已经开着就拉到前台，不建第二个
	static void init();
	// 「识别」热键入口（原来叫扫码热键）：拉起主界面 → 框选 →
	// 在二维码页优先识码，识不出码或不在二维码页就按文字识别走，见 grabRecognize
	static void decodeFromScreen();
	// 托盘菜单的"设置"走这里：主界面存在就直接切到设置页，没有就建一个再切
	static void showSettingPage();
	// 消息循环层的按键拦截（main.cpp 在 TranslateMessage 之前调）：
	// 设置页那三个快捷键显示框是只读 EDIT，按键落在它们身上回不到主窗口，
	// 只能在这一层截 —— 焦点在某一行上按 Delete / 退格就把这行清空。
	// 返回 true 表示这条消息已被吃掉，调用方不用再派发
	static bool handleAppKey(const MSG& msg);
	// 又双击了一次 exe / 防多开把请求转过来：把已经在跑的主界面亮出来（不切页）
	static void showMain();
	// 托盘态触发的识别（热键按下时界面不在屏幕上）专用：把"等结果出来再亮界面"的
	// 标记消费掉并显示主界面。标记没置位时什么都不做，所以可以在完成路径上无条件调用
	static void revealAfterGrab();
	// 隐藏 / 显示主界面（默认热键 F10）：主界面在屏幕上就收起来，不在就亮出来并抢焦点
	static void toggleMainVisible();
	// 截图开始前把主界面收起来（设置页开关控制，默认开）。
	// 用一对函数而不是直接 ShowWindow，是为了记住"这次是我们藏的"：
	// 用户在截图过程中自己按了 Esc、或者干脆把窗口关掉，收尾时就不该再把它弹出来。
	// 返回 true 表示确实藏了（调用方据此决定要不要配一次 restore）
	static bool hideForCapture();
	// 截图会话结束（WinCap 关窗）时把 hideForCapture 藏起来的窗口放回去。
	// 没藏过就什么都不做，所以可以在关窗路径上无条件调用。
	// 藏起来不足 kRestoreGapMs 时不当场放（那会闪一下），交给 TidRestoreMain 到点再放
	static void restoreAfterCapture();
	// 主界面当前是否正处于"为截图而隐藏"状态
	static bool mainHiddenForCap();
	// 截图窗创建失败时给用户一句明确的话。失败本身已经被 init() 的判空接住
	// （不崩了），但静默失败的表现是"界面闪一下、什么都没发生" —— 用户完全
	// 不知道是图形设备没起来，只会反复重点。这里只在状态翻转时提示一次
	static void showCapFailTip();
	// 「压缩软件全关」热键入口：一次扫一遍并结束正在跑的压缩软件进程。
	// 原来只有二维码页那颗按钮能触发，现在按钮撤了，全局热键直接走这里（实现在 WinQr.cpp）
	static void killZipByHotkey();
	// 退出流程里调：必须在 CoUninitialize 之前，静态析构就太晚了
	static void dispose();
	static bool hasInstance();
private:
	// 工作线程 -> UI 线程的消息
	enum AppMsg : UINT { MsgVfProgress = WM_APP + 20, MsgVfDone = WM_APP + 21, MsgOcrDone = WM_APP + 22 };

	WinQr();
	WinQr(const WinQr&) = delete;
	WinQr& operator=(const WinQr&) = delete;

	static LRESULT CALLBACK wndProcEntry(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT wndProc(UINT msg, WPARAM wp, LPARAM lp);
	LRESULT hintProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	static LRESULT CALLBACK hintProcEntry(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

	// 建窗口与拆窗口：dispose()（早拆，COM 还活着）和析构（晚拆）共用一份
	void destroyWindow();

	// 立刻把主界面摆回来（restoreAfterCapture 的立即分支与 TidRestoreMain 共用）
	static void restoreMainNow();

	void createControls();
	void applyPage();
	void relayout();
	void setPage(int page);
	// 控件工厂：统一的样式位，避免每个地方手写一遍漏掉 WS_CHILD。
	// 尺寸一律留空，由 relayout() 独占坐标 —— 几何只允许有一处定义
	HWND mkButton(int id, const wchar_t* text, bool ownerDraw = true);
	HWND mkEdit(int id, const wchar_t* text, DWORD extra);
	HWND mkStatic(int id, const wchar_t* text, DWORD extra);

	// —— 绘制 ——
	void onPaint();
	void printTo(HDC dc, UINT flags);
	void drawQrPage(HDC hdc);
	void drawShutdownPage(HDC hdc);
	void drawVerifyPage(HDC hdc);
	// 设置页：一张白卡片 + 三组标题（快捷键 / 保存位置 / 常规）与组间的分隔线。
	// 控件都在卡片里的三列上，坐标见 WinQr.cpp 的 SET_* 常量
	void drawSettingPage(HDC hdc);
	// 自绘进度条：轨道 + 已完成段 + 条内文字。
	// 文字分两段，都在条子内部：左边是「文件:」「总计:」标签，正中是百分比。
	// 两段都要跨"已填充 / 未填充"两种底色，所以各自按填充边界裁两次
	void drawProgressBar(HDC hdc, const RECT& bar, double pct,
		const std::wstring& left, const std::wstring& center);
	void onDrawItem(DRAWITEMSTRUCT* dis);
	void drawTab(DRAWITEMSTRUCT* dis);
	void drawItemLabel(const DRAWITEMSTRUCT* dis, const std::wstring& label);
	HBRUSH brushOf(COLORREF color);

	// —— 二维码页 ——
	void refreshQr();
	void invalidateQrBitmap();
	// 按 side 渲染 qrBmp 并落到缓存 DIB 上（side 变了才重建 DIB）。
	// 预览 blit 是 1:1，所以 side 就是 drawQrPage 算出的显示尺寸
	void renderQrDib(int side);
	bool renderQrToPixels(int side, std::vector<BYTE>& out);
	// 抓一块屏幕像素：负责收/放主界面、防"按 Esc 取消后主界面再也回不来"、
	// 建抓屏窗口。work 在像素到位的同一帧里回调（UI 线程），像素归它所有。
	// 二维码识别和文字识别共用这一条通路 —— 两条路对"隐身 / 恢复"的要求完全一样
	bool beginGrab(std::function<void(std::vector<BYTE>&&, int, int)> work);
	// 识别热键的总入口：二维码页先当二维码认，认不出来（或不在二维码页）
	// 就按文字识别处理 —— 在二维码页时结果直接填进输入框，顺手生成二维码
	void grabRecognize();
	// 二维码页「识别二维码」按钮和二维码页上的识别热键共用这一条：
	// 先识码，这块里没有码时（只有带引擎的版本）回落文字识别
	void qrGrabWithFallback();
#if SNAP_HAS_OCR
	// 文字识别结果往哪儿去：false = 文字识别页的结果框（默认），
	// true = 二维码页的输入框（热键在二维码页回落时用）
	bool ocrToQrEdit{ false };
	// 热键在别的页按下时：先只管框选识别，等结果回来（ocrOnDone）再切到文字识别页。
	// 早先是在按键那一刻就 setPage，界面当场跳页 —— 用户看到的是"还没框选就换页了"
	bool ocrSwitchOnDone{ false };
	// —— 文字识别页 ——
	void createOcrPage();
	void relayoutOcr();
	void startScreenOcr();
	void ocrFillText(const std::wstring& text);
	std::wstring ocrText();
	void ocrSyncScrollbar();
	void ocrSyncCount();
	void ocrCopy();
	void ocrClear();
	void ocrFind();
	void ocrFindLive();
	void ocrCycleLayout();     // 切换排版档位：按已缓存的坐标重排，不重新识别
	void ocrStartWorker(std::vector<BYTE>&& pixels, int w, int h);
	void ocrOnDone();
	void ocrJoinWorker();
	// 引擎常驻占约 100MB：空闲超过 kOcrIdleMs 就收掉，下次识别再拉起来
	void ocrIdleCheck();
#endif
	void copyImage();
	void saveImage();
	void pickLogo();
	void clearInput();

	// —— 定时关机页 ——
	int sdValue(int editId) const;
	void sdSetValue(int editId, long long v);
	void sdNudge(int editId, int dir);
	long long sdTotalSeconds() const;
	void sdStart();
	void sdPause();
	// 恢复：以"现在 + 剩余"重新立截止时刻（「开始」与「暂停」两条路共用）
	void sdResume();
	void sdClear();
	void sdTick();
	void sdUpdateStatus();
	// 关机没成：退回「未开始」并把进度条归零，输入格保持原样
	void sdAbort(const wchar_t* msg);
	// 最后 10 秒把主界面顶出来（并切到关机页），给用户留一次"暂停/重设"的机会
	void sdWarnWindow();
	// 把秒数拆成天/时/分/秒写进那四个数字格（倒计时过程中每秒回写一次，
	// 格子里看到的就是"还剩多久"，而不是当初设了多少）
	void sdWriteRemain(long long secs);

	// —— 文件校验页（实现见 WinQrVerify.cpp） ——
	void createVerifyPage();
	void relayoutVerify();
	void showVerifyPage(bool visible);
	void vfBrowse();
	void vfClear();
	void vfCopyText();
	void vfSave();
	void vfStop();
	// 把"停止"按钮的可用态与红色态同步到当前 vfBusy：进度条在走（忙）时才红、才可点；
	// 闲着就是灰且禁用。自绘按钮不会因为状态变化自己重画，必须显式 invalidate 一次
	void vfSyncStopBtn();
	void vfStart(std::vector<std::wstring> files);
	void vfJoinWorker();
	void vfOnProgress();
	void vfOnDone();
	// 把工作线程攒好的一段文本按行拆开，塞进结果框（只留最新两条记录）
	void vfAppendRecord(const std::wstring& text);
	// 结果框里粘贴：剪贴板文字当一条记录塞进框，并跟上一份结果对比
	void vfPasteCompare();
	// 两份摘要对比：任一相同算法一致就算一致，没有可比项返回 VfNone
	int vfJudge(const std::wstring cur[3]) const;
	// 状态变了之后把结论交给结果框（相同绿框 / 不同红框，不写字）
	void vfSyncVerdict();
	void refreshVerdict();          // 只重画结果框那一圈边框
	unsigned vfAlgoMask() const;

	// —— 文件改名页（实现见 WinRename.cpp，列表控件见 RnList.cpp） ——
	void createRenamePage();
	void relayoutRename();
	void showRenamePage(bool visible);
	void drawRenamePage(HDC hdc);
	// 规则变了就重算每一行的新文件名，然后刷列表。所有"点一下按钮"的入口
	// 最后都汇到这一个函数
	void rnApplyRules();
	// 把一批路径（文件或文件夹，文件夹递归）并进 rnRows
	void rnAddPaths(const std::vector<std::wstring>& paths);
	// WM_DROPFILES 落到改名页时走这里（HDROP 在这里 DragFinish）
	void rnDropFiles(HDROP drop);
	// 工具栏前两颗：系统对话框选文件（可多选）/ 选文件夹
	void rnPickFiles();
	void rnPickFolder();
	void rnSyncFilter();
	// 转换下拉直接摆在「文件名」面板右半边（不再有模式按钮切换），前缀 / 后缀挪去序号面板
	void rnSetPanel(int panel);
	void rnSyncPanel();
	// 页面上那四个切换标签是画出来的（没有 HWND），命中在这里判
	int rnTabHit(int x, int y) const;
	void rnSetCase(int mode);
	void rnSetExtMode(int mode);
	void rnToggleRepCase();
	void rnToggleRepAll();
	// 序号开关：选中 = 这条规则生效，不选中 = 编号不参与改名
	void rnToggleSeq();
	// 「添加」开关：选中 = 前缀 / 后缀参与改名，不选中 = 两条都忽略
	void rnToggleAffix();
	// 删除面板六颗开关：which = 0 空格 / 1 小写 / 2 大写 / 3 数字 / 4 中文 / 5 特殊字符
	void rnToggleDel(int which);
	// 序号三个数字框（起始值 / 增量 / 位数）读成整数，非法或越界就退回默认值
	int rnSeqInt(int id, int fallback, int lo, int hi) const;
	// 一个标签字串的实际前进宽（逻辑单位）：标签后面跟着输入框的，框要按这个宽贴上去
	int rnLabelW(const std::wstring& s);
	// 标签与框贴合的位置：由 relayoutRename 算一次，绘制时照着摆
	int rnFindX{}, rnReplLblX{}, rnReplaceX{};
	int rnSeqStartLblX{}, rnSeqStartX{}, rnSeqStepLblX{}, rnSeqStepX{}, rnSeqDigLblX{}, rnSeqDigX{};
	int rnConvX{}, rnConvToX{};          // 「转换：/改为：」标签后面的下拉 x
	int rnPfxLblX{}, rnPfxX{}, rnSufLblX{}, rnSufX{}; // 追加面板第二行「添加」开关与「前缀：/后缀：」
	int rnExtSetX{}, rnExtSetW{};
	int rnDelTxtX{};
	// 在「改为」那个下拉里挑了一项：这一组（全大写/全小写/不变/删除/改为）互斥
	void rnPickExtSet();
	void rnRun();
	void rnUndo();
	void rnClear();
	void rnReset();
	// 所有规则 + 它们的输入框回到刚导入时的状态（不动行数据）：重置与撤销共用
	void rnClearRules();
	void rnCheckAll(bool invert);
	void rnCopyNames();
	void rnBeginEdit(int rowIndex);
	void rnCommitEdit();
	void rnCancelEdit();
	void rnHideEditor();
	void rnRebuildFilter();
	void rnApplyFmt();
	int rnFmtHit(int x, int y) const;
	void rnToggleFmt(int index);
	std::vector<std::wstring> rnConvertItems() const;
	std::vector<std::wstring> rnAffixItems() const;
	std::vector<std::wstring> rnExtItems() const;
	// 规则输入的一次性快照：重算整表时只读一遍控件，再按行套规则。
	// 原来 rnCompose 每行都要回读 查找 / 替换为 / 删除文本 / 前缀 / 后缀 / 改为，
	// 一屏十行就是六十次跨窗口取值；更要紧的是 <now>/<date>/<time> 每行各取一次
	// 系统时间，赶上跨秒，同一批文件名里会混进两个不同的时间戳
	struct RnRuleSet {
		std::wstring find, replace;      // 查找 / 替换为（原样，不裁空白）
		std::wstring delText, extSet;    // 删除「文本：」/「改为」（已裁空白）
		std::wstring prefix, suffix;     // 前缀 / 后缀（原样，里面可以夹占位符）
		std::wstring convTo;             // 文件名页「改为：」——主名整体换成它（已裁空白、去非法字符）
		int conv{ 0 };                   // 长框「转换」选的是第几项（0 = 不转换）
		SYSTEMTIME now{};                // 整表共用一个时刻
	};
	RnRuleSet rnReadRules() const;
	// 按当前规则算一条新文件名（ordinal 从 1 起，数字递增用）
	std::wstring rnCompose(const RnRow& row, int ordinal, const RnRuleSet& rules) const;
	void rnRecalcStatus();
	std::wstring rnText(int id) const;
	// 长框「转换」当前选的是第几项（0 = 不转换）
	int rnConvertMode() const;
	// 前缀 / 后缀下拉里选了候选：只把 token（第一个空格前那段）填进框
	void rnPickAffixToken(int id);
	static LRESULT CALLBACK rnEditProcEntry(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT rnEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	WNDPROC rnEditOld{ nullptr };

	// 叠在同一格的三只下拉（转换 / 前缀 / 后缀）：平时只占一行高，
	// 点开前把窗口往下抻高，系统的下拉列表才有地方画（见 rnComboProc）
	static LRESULT CALLBACK rnComboProcEntry(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT rnComboProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	WNDPROC rnComboOld{ nullptr };
	HWND rnComboTall{ nullptr };   // 当前被抻高的那只（收起来要缩回去）

	// —— 「压缩软件全关」热键（调用方：Setting 里的全局热键 / killZipByHotkey） ——
	// 一次扫一遍，不做常驻看护：这就是一个动作，不是开关状态
	void killZipNow();

	// 二维码页原来那两颗「截图」「关压缩软件」已经去掉（4108 / 4109 号段空着），
	// 连 startCapture() 一起删了：它只服务那颗按钮，截图热键走的是 WinCap 自己的入口

	// —— 设置页 ——
	void beginCapture(int row);
	void endCapture();
	void pollCaptureKeys();
	void refreshShortcutRows();
	// 把某一行的热键清掉并让那颗按钮闪一下"已保存"：录入中按退格、显示框里按 Delete
	// 两条入口共用同一段（各自抄一份时，两条路的落盘 / 提示迟早会走偏）
	void clearShortcutRow(int row);
	// 把配置里的保存目录（空 = 桌面）写进两行路径框。显示的是"实际会落盘的那个目录"，
	// 与 Util::resolveSaveDir 的回落规则一致
	void refreshSaveDirs();
	// 点「更改」：弹选文件夹框，选完落盘并刷新显示。用户取消就什么都不做
	void chooseSaveDir(bool video);

	// —— 轻提示 ——
	void showToast(const std::wstring& text);
	// 提前收掉正在显示的提示条（识别完成这类"进度提示"不该等自动超时）
	void hideToast();

	// 字号只有这几档，全程序共用。以前每个控件各写一个字面量，
	// 结果同一屏里能出现 13/14/15/18/22 五种大小，看着就是"没排版"
	enum FontSize {
		FsMono = 15,    // 等宽结果框（文件校验）：15 是"两条记录 15 行塞进 288 高"的上限
		// 结果框里一行装不下时自动降到这一档（SHA256 是 72 个字符，框一窄就得缩）
		FsMonoSmall = 13,
		FsBody = 18,    // 正文：标签、复选框、单行输入、页签、矮按钮
		FsCtrl = 22,    // 大控件：46 高的按钮、二维码输入框、定时关机单位标签
		FsNum = 28,     // 大数字：定时关机的倒计时；文件校验结论栏（相同/不同）也用这档
		FsTitle = 34,   // 页面标题
	};

	// 字体按 (DPI, 字号, 字重, 字族) 缓存，同一组合只 CreateFont 一次
	enum FontFace { FaceUi = 0, FaceMono = 1 };
	HFONT fontOf(int height, int weight, bool negative = false, int face = FaceUi);
	// DPI 缩放：所有布局常量都是 96dpi 下的值，用这个换算
	int sc(int v) const;
	static WinQr* instanceOf(HWND hwnd);

	enum TimerId {
		TidShutdown = 98, TidCapture = 99, TidHover = 100, TidGrabCheck = 101,
		TidRecSaved = 102, TidRestoreMain = 103
#if SNAP_HAS_OCR
		// OCR 引擎空闲回收定时器。号段固定 105，加在末尾不影响前面的 id
		, TidOcrIdle = 105
#endif
	};

	HWND hwnd{ nullptr };
	// 打开程序第一眼看到的那一页：带引擎的版本是「文字识别」（它就是主界面），
	// 不带引擎的版本是「二维码」
#if SNAP_HAS_OCR
	int page{ PageOcr };
#else
	int page{ PageQr };
#endif
	int dpi{ 96 };

	// —— 二维码状态 ——
	QrBitmap qrBmp;
	HBITMAP qrHbmp{ nullptr };       // 由 qrBmp 渲染出的 DIB，省得每帧 CreateBitmap
	// qrHbmp 的边长：预览走 1:1 blit，所以它 = drawQrPage 的显示尺寸（DPI 变了才重建）；
	// 「下载 / 复制」仍按 kQrPixels 全分辨率单独渲染，不走这张 DIB
	int qrHbmpSide{ 0 };
	// 渲染二维码用的像素缓冲 + 上面对应的 DIB 内存首地址。三者（qrPx / qrHbmp /
	// qrDibBits）都只建一次、之后逐帧复用：输入框每敲一个字都会触发一次重渲染，
	// 每帧新分配 1.44MB 的缓冲在这一条路径上是纯浪费
	std::vector<BYTE> qrPx;
	void* qrDibBits{ nullptr };
	std::vector<BYTE> logoPixels;
	int logoW{ 0 }, logoH{ 0 };
	// 扫码时主界面是隐身的。用户按 Esc 取消框选时截图窗口直接关掉、完成回调根本不会来，
	// 靠这个标记 + 定时器兜住"取消后主界面再也回不来"的情况
	bool grabPending{ false };
	// 这次扫码的隐身用的是「捕获亲和性」（窗口留在屏幕）还是 SW_HIDE（窗口真没了）。
	// Esc 取消的兜底恢复要按这个区分：前者摘标记即可，后者才要 ShowWindow
	bool grabExcludedByAffinity{ false };

#if SNAP_HAS_OCR
	// —— 文字识别状态 ——
	// 识别是秒级的活（首次还要加载模型），放 UI 线程会把窗口冻住 —— 结果是
	// "点完识别界面没反应"，用户再点一次就叠了第二次。所以丢到工作线程，
	// 线程只写这几个受锁保护的字段 + PostMessage，绝不碰控件
	std::thread ocrThread;
	std::atomic<bool> ocrBusy{ false };
	std::mutex ocrMutex;
	std::wstring ocrResult;          // 工作线程攒好的文本
	bool ocrOk{ false };             // 本轮是否识别成功
	std::wstring ocrFail;            // 失败原因（已成人话）
	std::vector<OcrLine> ocrLines;   // 本轮每行的文本+坐标；换排版档位时直接重排，不用再识别一次
	Layout::Mode ocrLayout{ Layout::Mode::Auto };  // 当前档位，持久化在 config 的 common.ocrLayout
	static constexpr ULONGLONG kOcrIdleMs{ 180000 };  // 引擎空闲 3 分钟回收
#endif

	// —— 定时关机状态 ——
	bool sdRunning{ false };
	bool sdPaused{ false };
	bool sdDone{ false };            // 到点且关机调用已发出（失败会立刻回 false）
	long long sdRemain{ 0 };
	long long sdMax{ 1 };            // 进度条分母；<=1 表示没有可画的进度
	// 上一次同步控件时"正在跑且没暂停"的值：三颗按钮的颜色由它决定，
	// 只在它变化时重画（tick 每秒调一次 sdUpdateStatus，无条件重画纯属白烧）
	bool sdUiBusy{ false };
	// 倒计时的基准是"截止时刻"，不是"每秒减一"：定时器本身会被系统合并、
	// 界面重绘会拖一点，逐次减一跑 1 分钟实测能拖成 65 秒。
	// 每 tick 用 (截止 - 现在) 反算剩余秒数，误差永远不超过一个定时器周期。
	// 用 GetTickCount64（开机计时）而不是系统时间：机器睡眠期间倒计时跟着停，
	// 与"睡着的时候没人看见屏幕"这个直觉一致，也和老行为对齐
	ULONGLONG sdDeadline{ 0 };
	// "最后 10 秒提醒"只弹一次。用标志位而不是 ==10：定时器抖动时可能整个跳过 10 这一档
	bool sdWarned{ false };

	// —— 文件校验状态 ——
	// 大文件是秒级到分钟级的活，必须丢到工作线程：UI 线程一转窗口就假死，
	// 连"停止"都点不动。线程只碰下面几个受锁保护的字段，绝不直接动控件
	std::thread vfThread;
	std::atomic<bool> vfCancel{ false };
	std::mutex vfMutex;
	// 一个元素 = 一个文件的完整文本。必须是"每文件一条"，不能拼成一整个字符串：
	// 小文件处理得飞快，UI 一次刷新能攒下好几个，拼在一起就成了一条记录，
	// 文件之间连分隔线都没有（结果框是"每条记录一个文件、只留最新两条"）
	std::vector<std::wstring> vfPending;
	// 结果框：自绘（不是 EDIT），滚动、行距、结论边框都归它自己管。
	// 记录条数上限也在里面（只留最新两条）
	VfBox vfb;
	std::wstring vfSig;              // 本次校验的指纹（路径+各算法结果）
	std::wstring vfPrevSig;          // 上一次校验的指纹，用来做"两次对比"
	// 上一份结果的三种摘要（MD5 / SHA256 / CRC32，文件算的或粘贴的，谁在后算谁）。
	// 单独存一份，是因为结果框只留两条记录 —— 前一条被顶掉后还得接着跟它比
	std::wstring vfPrevDig[3];
	bool vfHasPrevDig{ false };
	// 本轮文件校验算出来的摘要（工作线程写、UI 在 vfOnDone 里读）。
	// 不能直接写 vfPrevDig：那是"上一轮的基线"，边算边覆盖就没得比了
	std::wstring vfRunDig[3];
	bool vfRunDigValid{ false };
	// 本轮算成功几个文件：粘进来的对照值是"单个文件的摘要"，
	// 一批文件跟它比没有对应关系，不能拿最后一个文件的摘要去冒充整批
	int vfRunDigFiles{ 0 };
	HashProgress vfProg;             // 两条进度条的计数（工作线程写、UI 读）
	// 结论状态：决定结果框那一圈外框的颜色（相同绿 / 不同红 / 其余灰）
	enum VfVerdict { VfNone = 0, VfWaiting = 1, VfFirst = 2, VfSame = 3, VfDiff = 4 };
	int vfVerdict{ VfNone };
	bool vfBusy{ false };
	bool vfHasPrev{ false };

	// —— 效验页五个勾选的状态 ——
	// 不放在按钮里：自绘（BS_OWNERDRAW）按钮对 BM_SETCHECK 的支持不可靠，
	// 实测勾上再画出来还是空的。状态由窗口自己拿，按钮只当"可点的矩形"用。
	// 顺序固定 0=版本 1=日期 2=MD5 3=SHA256 4=CRC32，见 vfChkIndex
	bool vfChk[5]{ true, true, true, true, true };
	static int vfChkIndex(int id); // 不是勾选返回 -1

	// —— 文件改名页状态 ——
	// 行数据留在窗口里、列表只拿一个指针（RnList::setSource）：改名 / 撤销 / 过滤
	// 都要整表重排，数据放进控件会拧成两套
	std::vector<RnRow> rnRows;
	std::vector<std::pair<std::wstring, std::wstring>> rnUndoPairs; // (改后的路径, 原路径)
	RnList rnb;
	int rnCase{ -1 };       // -1 不选 / 0 全部大写 / 1 全部小写 / 2 首字母大写
	// 当前露出来的规则面板：0 文件名 / 1 扩展名 / 2 替换 / 3 序号 / 4 删除。
	// 五组规则互相独立、同时生效，这个值只管"露哪一组的控件"
	int rnPanel{ 0 };
	bool rnRepCase{ false };  // 替换：区分大小写（默认不区分，与资源管理器一致）
	bool rnRepAll{ true };    // 替换：替换所有（关掉只换第一处）
	bool rnSeqOn{ false };    // 序号：这条规则是否生效（= 「数字递增」同一颗开关，默认不选）
	bool rnAffixOn{ false };  // 添加：前缀 / 后缀这条规则是否生效（默认不选，框里留着字也不套用）
	// 删除：六类字符各一颗开关，选中 = 这一类从名字里抹掉
	bool rnDelSpace{ false }, rnDelLower{ false }, rnDelUpper{ false };
	bool rnDelDigit{ false }, rnDelCjk{ false }, rnDelSpecial{ false };
	int rnSeqStart{ 1 };      // 序号：起始值
	int rnSeqStep{ 1 };       // 序号：增量
	int rnSeqDigits{ 1 };     // 序号：位数（0/1 = 不补零，2 起补零到该宽度；默认 1）
	int rnExtMode{ -1 };    // -1 全不选（默认，扩展名原样不动）/ 0 不变 / 1 大写 / 2 小写 / 3 删除 / 4 改为
	int rnEditRow{ -1 };    // 正在原地编辑的行（rnRows 下标）
	bool rnSyncing{ false }; // 重建下拉时屏蔽通知，别自己触发自己
	// 底部那排格式勾选：rnFmtAll 是"已拉入文件里出现过的格式"，rnFmtOn 是勾着的；
	// rnFmtOn 空 = 全勾（默认），和上面「过滤」下拉共用同一份状态
	std::vector<std::wstring> rnFmtAll;
	std::set<std::wstring> rnFmtOn;
	// 过滤框里手打的字（小写）。非空时按它筛（扩展名或文件名包含它就算命中），
	// 空的时候才回到底部那排格式勾选
	std::wstring rnFmtText;

	// 设置页三颗勾选也走自绘，状态同样由窗口自己拿（理由同上）。
	// 主题复选框是透明背景画字的，被页签盖过之后会留别人的残影
	bool setChkAutoStart{ false };
	bool setChkZipGuard{ true };
	bool setChkHideMain{ true };
	// 关闭时最小化到托盘（默认开，保持历来"关窗不杀进程"的行为）；
	// 关掉之后点关闭 = 真退出
	bool setChkCloseTray{ true };
	// 只允许单实例（默认开，与历来行为一致）
	bool setChkSingleInstance{ true };
	// 启动时直接显示主界面（默认开）；关掉则只驻留托盘，再双击图标才出界面
	bool setChkShowMainOnStart{ true };

	// —— 快捷键录入状态 ——
	// 三行按 index 对齐：0=截图 cap / 1=扫码 qrcap / 2=压缩软件全关 zipkill
	int captureRow{ -1 };
	// 副编辑器里短暂显示"已保存"，用它记还剩几条计时
	int recSavedRow{ -1 };
	DWORD capturedMods{ 0 };
	// 各修饰键"已经松开了多少拍"（Ctrl/Shift/Alt/Win 顺序）。累积的同时要在
	// 松开后清位，否则按错一次 Ctrl 就再也录不进纯 F8 了
	int modHoldTicks[4]{};
	HWND prevFocus{ nullptr };

	// —— 轻提示 ——
	HWND toastHwnd{ nullptr };

	// 字体与画刷缓存：建一次用完，销毁时统一 DeleteObject。
	// 参考实现是每个控件临时 CreateFont 且不删，那在高 DPI 重绘时是持续泄漏
	std::unordered_map<long long, HFONT> fontCache;
	std::unordered_map<COLORREF, HBRUSH> brushCache;
	// 整幅双缓冲的后台画布。原来每次 WM_PAINT 都 CreateCompatibleBitmap 一张
	// 585×445 的位图再删掉（≈1MB 的分配/释放），切页、悬停这种高频重绘下既慢又脏；
	// 现在按客户区大小建一次留着，尺寸变了（改 DPI / 高 DPI 缩放）才重建
	HDC bufDc{ nullptr };
	HBITMAP bufBmp{ nullptr };
	SIZE bufSize{};
	HFONT setFont{ nullptr };
	HFONT editFont{ nullptr };
	HFONT sdEditFont{ nullptr };
	HFONT sdLabelFont{ nullptr };
	int hoverCtrl{ 0 };
};
