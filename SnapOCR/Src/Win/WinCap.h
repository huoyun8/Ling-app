#pragma once
#include <Ui/Ui.h>
#include <cstdint>
#include <functional>
#include <imm.h>
#include "WinHost.h"

class CutMask;
class CapVideo;


// 截图主窗口。铺满整个虚拟桌面，拖框结束后并不马上让位，而是留下来当宿主：
// 选区可以继续调整（四边 + 四角拖改大小），框选完成后直接在框选窗口内标注。
// 标注宿主公共状态见 WinHost。
class WinCap:public WinHost
{
public:
	~WinCap();
	// 建窗成功返回 true。失败时（CreateWindowEx 返回空、组合器/D2D 抛异常）
	// 内部会把对象收干净再返回 false —— 绝不能留一个 hwnd 为空的壳，
	// 否则 WinCap::get() 永远非空，之后每次截图都被判成"上一次还没结束"，再也截不了
	static bool init();
	static WinCap* get();
	// 现在是否真有一个能用的截图窗口。防多开那条路要用它判断"此刻是不是正在截图"：
	// 是的话就别把主界面硬推出来（遮罩会被挤到后面，焦点也乱）
	static bool hasInstance();
	// 一次性"抓像素"通道：给二维码识别这种"要选区像素、不要标注界面"的调用方用。
	// 拖框抬起的瞬间把选区像素交给 handler，本窗口随即关闭，完全不进标注阶段。
	// handler 在 UI 线程同步回调，像素是 BGRA / top-down / 行紧凑（步长 = w*4）。
	// 已经有一个截图窗口开着时返回 false —— 不同调用方抢同一个全屏窗口只会打架
	using GrabHandler = std::function<void(std::vector<BYTE>&& pixels, int w, int h)>;
	static bool initForGrab(GrabHandler handler);
	// 这个实例是否已经不能用了：已关窗 / 句柄失效 / 窗口已被销毁。
	// 调用方据此判断"壳还在但已经死了"，就地回收后重开，而不是一路当作"上一次还没结束"卡死
	bool isDead() const;
	// 窗口代次：init 时分配，onClosed 里把延迟析构绑到这一代上 ——
	// 期间若已经开了新窗口，旧的那次 reset 必须作废，否则会把新窗口收掉
	unsigned gen{ 0 };
	// 退出流程里调：窗口对象是文件级静态变量，交给静态析构就在 CoUninitialize 之后了
	static void dispose();
	// 是否允许"智能选区"（悬停吸附窗口/控件、拖动时贴边、像素级边缘吸附、滚轮循环候选）。
	// 纯截图工具始终允许智能选区（窗口/控件自动识别、边缘吸附、滚轮循环候选）。
	bool allowSnap() const;
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> getCutImg();
	// 重绘请求：默认走 WinBase::refresh，但截图窗每帧都是整屏重绘，
	// 鼠标按 125Hz 送 WM_MOUSEMOVE 时"一个消息一次整屏"会直接把消息队列压住
	// （WM_PAINT 优先级最低，永远排在输入消息后面）—— 表现就是框追不上手。
	// 这里按 ~120fps 合帧，超出的那次交给一次性定时器补
	void requestPaint() override;
	// 选区边/角"可抓取带"的半宽（像素）。选中了工具也想拖动 / 缩放选区时，
	// 只有贴着边角这一圈才抢给调整，带外仍然归画笔
	float edgeGrabBand() const { return 6.f * dpi; }
	// 工具条统一定位规则：右边与选区右边对齐，下方空间够就摆在选区右下方，
	// 不够就摆右上方，上下都不够就盖在选区右下角内部（留一点边距）。
	// 返回 false = 没有选区可依附，落点没动过。outX/outY 非空时回传算出的屏幕坐标，
	// 录屏要据此判断录制条有没有压进画面（见 layoutTools 的 Video 分支）
	bool layoutTool(ui::WinBase* tool, int* outX = nullptr, int* outY = nullptr);
	// 下面都是给工具条用的门面 ————————————————————
	void startPin();
	void saveToFile() override;
	void copyToClipboard() override;
	// —— 标注宿主接口（WinHost 纯虚）——
	void layoutTools() override;
	void onToolStyleChanged() override;
	ui::TextBox* getTextBox() override;
	void setEditingText(ShapeText* shape) override;
	POINT toImgPos(const POINT& pos) const override;
	void pinToScreen() override;
	// —— 录屏（WinHost 门面，副编辑器上的录屏条调）——
	bool beginVideo(bool mp4, bool speaker, bool mic) override;
	std::wstring stopVideo(bool toClipboard) override;
	void discardVideo() override;
	void exitVideo() override;
	bool inVideoMode() const override { return stage == CapStage::Video; }
public:
	// 标注窗口用的就是这一份选区，它自己不再框选
	std::unique_ptr<CutMask> cutMask;
	// 本次截图是不是"抓完像素就交出去"的模式（见 initForGrab）。窗口会复用，
	// 每次 init 都必须把它清掉，否则上一次二维码识别设的钩子会影响普通截图
	bool grabMode{ false };
	GrabHandler grabHandler;
private:
	WinCap();
	void onCreated() override;
	void layout() override;
	BOOL setCursor() override;
	LRESULT onHitTest(const POINT pos) override;
	void setPixPos(POINT pos);
	void getPixImg(POINT pos);
	void paintPix(ID2D1DeviceContext* ctx);
	// 放大镜底下的四行取色信息（HEX / RGB / CMYK / POS），按光标当前位置现算。
	// 绘制和"按 C 复制"共用这一份 —— 格式只写一处，日后改格式不会两边不一致
	std::vector<std::wstring> pixInfoLines();
	// 选区阶段按 C：把上面四行一起复制进剪切板
	void copyPixInfo();
	void onKey(UINT key);
	void onDown(POINT pos, bool isRight);
	void onMove(POINT pos);
	void onUp(POINT pos, bool isRight);
	void onWheel(POINT pos, float space);
	void onTimerCB(UINT id);
	// ESC 的唯一收尾路径：任何状态下按一下都直接结束整个截图（录制中顺带把这段录制丢掉）
	void quitByEsc();
	void onClosed();
	// WinCap 满屏、工具条叠在选区内：WinCap 一旦获焦（被点击）会盖住工具条，
	// 获焦后把它们重新置顶，让编辑器始终可见
	void raiseTools();
	// 命令行 --enter=pin 时框选完成直接进入标注界面（等于替用户按住 Ctrl）
	bool enterByArg();
	// 右键门面：把窗口整体退回"按快捷键首次进入"的屏幕选区界面 ——
	// 清掉选区 / 标注 / 工具条，放大镜摆回光标处，十字光标等待重新拖框。
	void restartSelect();
	// 首次进入 Adjust 阶段时建出标注用的工具条与历史（懒建，避免截图窗口一开始就带着工具条）
	void ensureAnnotation();
	// 离屏把"选区底图 + 未撤销 shape"合成出最终图像的像素（与 WinPin::getImagePixels 同格式）
	bool getImagePixels(std::vector<BYTE>& pixels, D2D1_SIZE_U& size);
	// 只要选区底图的原始像素，不含任何标注 shape。二维码识别走这条路
	bool grabPixels(std::vector<BYTE>& pixels, D2D1_SIZE_U& size);
	std::tuple<int, int, int, int> getCMYK(const BYTE& r, const BYTE& g, const BYTE& b);
private:
	// Select 拖框取色 -> Adjust 调整选区并直接进入标注界面 -> Video 录制中
	enum class CapStage { Select, Adjust, Video };
	CapStage stage{ CapStage::Select };
	// 录制中：只负责编码线程，界面在副编辑器上（见 CapVideo）
	std::unique_ptr<CapVideo> capVideo;
	// 本轮录的是 gif：GDI BitBlt 认不了 WDA_EXCLUDEFROMCAPTURE，录制条压在
	// 选区里就会被一帧不落地录进去，布局时要专门把它挪出画面（见 layoutTools）
	bool videoIsGif{ false };
	Microsoft::WRL::ComPtr<ID2D1Bitmap1> pixImg;
	// 放大镜四行小字的 DWrite layout 缓存：HEX/RGB/CMYK 只在颜色变时文本才变，
	// 每帧重建 4 个 CreateTextLayout 实测约 100μs —— 放大镜是每帧都画的，
	// 这笔钱没必要花。POS 行每帧都变，等于天然只缓存前三行
	Microsoft::WRL::ComPtr<IDWriteTextLayout> pixTextLayout[4];
	std::wstring pixTextLast[4];
	// 放大镜字带右侧"色彩信息 / 按C复制"两行提示：静态文本，建成一次一直复用
	Microsoft::WRL::ComPtr<IDWriteTextLayout> pixHintLayout[2];
	// 整屏位图的 CPU 可读副本原来存在这里（snapImg）：智能选区要读像素，而 GPU 位图
	// 读不了。现在那份整屏内存直接由 CutMask 接管（见 CutMask::adoptPixels），
	// 会话里不再多留一张整屏 GPU 位图
	D2D1_RECT_F pixSrcRect{};
	// 铺满窗口的画布，走 swap chain 双缓冲：底图、蒙版、放大镜每帧都重画
	ui::Canvas* canvas{ nullptr };
	POINT pixPos;
	bool isPress{ false }, isClosed{ false };
	// 智能选区阶段挂起的输入法上下文。中文输入法开着时，'C' 会被当成拼音组字键
	// 吞掉（WM_KEYDOWN 根本不来），"按 C 复制"就失效。所以进截图先把它摘掉，
	// 进标注阶段再还原 —— 标注里的文字工具要能打中文
	HIMC savedImc{ nullptr };
	// 右键"回到刚按快捷键那一帧"的待命状态：此时智能吸附是关的，
	// 必须等光标真的动了（位移超过 2px）才恢复识别。
	bool snapArmed{ false };
	POINT snapArmPos{};
	// onDpiChanged 与 onSizeChanged 之间的接力标记，见构造函数里的注释
	bool dpiChanged{ false };
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> crossBrush;
	// 合帧节奏：上次真正重绘的时刻 + "已经排了一次补帧"的标记（定时器 id 102）
	ULONGLONG lastFrameTick{ 0 };
	bool framePending{ false };
	// 上一次鼠标位置：系统会补发坐标完全没变的 WM_MOUSEMOVE（右键松手、焦点切换），
	// 那些帧没有任何东西需要重画
	POINT lastMovePos{ -100000, -100000 };
	static constexpr ULONGLONG kFrameMs{ 8 };   // ≈120fps 封顶
	// ESC 轮询（定时器 104）：输入法组字 / 文本框有焦点时 WM_KEYDOWN 可能根本不来，
	// 只认键状态才敢说"任何状态按一下 ESC 就退出"
	static constexpr UINT kEscPollTimer{ 104 };
	bool escDown{ false };      // 上一轮采样时 ESC 是否按着，用来取"刚按下"这一沿
	bool escClosing{ false };   // 已经因 ESC 进入收尾，挡住同一轮里的第二次触发
};
