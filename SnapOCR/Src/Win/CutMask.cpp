#include "pch.h"
#include <dwmapi.h>
// 桌面图标层要 LVM_GETITEMCOUNT / LVM_GETITEMRECT / LVIR_ICON；WIN32_LEAN_AND_MEAN
// 下这个头不会自己进来。只用到常量，不调任何 comctl32 的辅助函数
#include <commctrl.h>
#include <cmath>
#include <cfloat>
#include <algorithm>
#include <cstring>
#include <Ui/Ui.h>
#include "CutMask.h"
#include "WinCap.h"
using namespace Microsoft::WRL;

namespace {
	// 顶层窗口枚举时的传递参数：rank 是 z 序位次（0 = 最上层）。
	// EnumWindows 按 z 序从上到下回调，这个次序是 collectHover 判断"谁压着谁"的
	// 唯一依据 —— 只靠面积挑候选修不好这个问题（背景窗口的小控件面积比前景窗口小，
	// 会盖过前景，表现为"吸不到最上层、还到处乱吸"）
	struct EnumCtx
	{
		CutMask* self;
		int rank;
		// 当前正在枚举的顶层窗口。子控件要记下"自己属于谁"，
		// 命中时才知道该不该跟着这个窗口一起露出来（见 collectHover 的遮挡判定）
		HWND topHwnd{ nullptr };
	};

	// 光标这一点上，屏幕上**眼睛真能看见**的那个顶层窗口。
	// 从系统 z 序最上层往下走，第一个"盖住这一点、又不是截图窗自己"的窗口就是答案。
	// 为什么不用 WindowFromPoint：截图窗自己是全屏置顶窗，永远挡在最前面，
	// 问系统只会得到"你自己"，等于没问。
	// 为什么不用识别表里算好的 rank：那张表 600ms 才重算一次，刚弹出来的窗口还没
	// 进表，位次就滑到了它后面那一层 —— 这正是"吸到背后那个窗口"的来源。
	HWND visibleRootAt(POINT screenPt, HWND selfHwnd)
	{
		for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
			if (h == selfHwnd) continue;
			// 先取矩形做命中测试：光标点上通常只有个位数窗口盖着，不命中的那些
			// 不必再问可见性 / 最小化 / 虚拟桌面 —— 一次省三四个系统调用。
			// 语义与原先"逐个先问可见性"完全一致：不可见窗口 PtInRect 命中了
			// 也照样跳过，继续往下找
			RECT rc{};
			if (!GetWindowRect(h, &rc)) continue;
			if (!PtInRect(&rc, screenPt)) continue;
			if (!IsWindowVisible(h) || IsIconic(h)) continue;
			DWORD pid = 0;
			GetWindowThreadProcessId(h, &pid);
			if (pid == GetCurrentProcessId()) {
				// 自己的窗口里只有主界面（二维码工具）算"看得见的内容"，
				// 工具条 / 贴图窗是截图界面的一部分，跳过
				wchar_t cls[64]{};
				GetClassNameW(h, cls, 64);
				// 自己的窗口里只有主界面算"看得见的内容"，工具条 / 贴图窗是截图界面的一部分，跳过。
				// 类名按品牌取：写死成 SnapCore_Main 的话，SnapOCR 版会把自己的主界面当成
				// 可拍摄内容，实时预览里就永远挂着它自己那张脸
				if (wcscmp(cls, SNAP_CLS_MAIN) != 0) continue;
			}
			BOOL cloaked = FALSE;
			DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
			if (cloaked) continue;   // 挂在别的虚拟桌面上
			return h;
		}
		return nullptr;
	}

	// 两个矩形是否"真的不一样"。差 1~2px 不算变：
	// 像素层的边界会落在抗锯齿的过渡像素上，同一块内容来回晃一像素很常见；
	// 每次晃都当成"选区变了"就得重烘一遍整屏缓存（Clear + 整屏 blit），白掉帧。
	// 2px 的偏差肉眼看不出来，但省下的是**一次整屏绘制**
	bool rectDiffers(const D2D1_RECT_F& a, const D2D1_RECT_F& b, float tol)
	{
		return std::abs(a.left - b.left) > tol || std::abs(a.top - b.top) > tol
			|| std::abs(a.right - b.right) > tol || std::abs(a.bottom - b.bottom) > tol;
	}

	// 光标下这个顶层窗口是不是"桌面"。桌面有两只宿主，形态取决于是不是静态壁纸：
	//   · 静态壁纸：Progman 直接挂着 SHELLDLL_DefView
	//   · 幻灯片 / 动态壁纸：DefView 被挪到某个 WorkerW 下面，Progman 只剩一张壁纸
	// 两只都要认，少认一只就是"换个壁纸智能选区就废了"
	bool isDesktopRoot(HWND h)
	{
		if (!h) return false;
		wchar_t cls[64]{};
		if (!GetClassNameW(h, cls, 64)) return false;
		return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;
	}

	// 桌面那个 SysListView32（图标全在里面，一个图标一个 item，没有一个 HWND）
	HWND findDeskList()
	{
		auto defOf = [](HWND parent) {
			return parent ? FindWindowExW(parent, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;
			};
		HWND def = defOf(FindWindowW(L"Progman", nullptr));
		if (!def) {
			for (HWND w = FindWindowExW(nullptr, nullptr, L"WorkerW", nullptr); w && !def;
				w = FindWindowExW(nullptr, w, L"WorkerW", nullptr))
				def = defOf(w);
		}
		return def ? FindWindowExW(def, nullptr, L"SysListView32", nullptr) : nullptr;
	}

	// 给别的进程发消息，20ms 封顶。桌面的列表属于 explorer：它卡住（正在拷贝几十 G、
	// 弹了模态框）时，默认的 SendMessage 会把**我们**一起钉住 —— 截图界面直接假死。
	// SMTO_ABORTIFHUNG：对方没在泵消息就立刻放弃，宁可这一帧不吸
	LRESULT listMsg(HWND h, UINT msg, WPARAM wp, LPARAM lp)
	{
		DWORD_PTR r = 0;
		if (!SendMessageTimeoutW(h, msg, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, 20, &r)) return 0;
		return (LRESULT)r;
	}
}

CutMask::CutMask(ui::WinBase* win) :win{ win }
{
	strokeWidth = 2 * win->dpi;
	paddingMargin *= win->dpi;
	auto d2d = ui::D2D::get();
	// D2D 图形设备没起来（远程桌面 / 驱动黑名单 / 显存不足）时 deviceContext 是空的。
	// 这里早退，后续 host 的 onCreated 也会因同样的判空早退，不会二次解引用。
	// 注意：这处是本窗口的【第一处】D2D 解引用，比 WinCap::onCreated 还早 —— 漏判空
	// 就会在"一按截图就崩"的路径上直接访问违例（catch(...) 在 /EHsc 下拦不住）
	if (!d2d || !d2d->deviceContext) return;
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brushText.GetAddressOf());
	// 选区外的压暗：纯黑 + 32% 不透明度。
	// 原来 46% 偏重，屏幕一暗下来整块背景都发闷，也看不清底下到底是哪个窗口 ——
	// 压暗只是为了"衬出选区"，不是要盖住背景，三分之一左右就够把选区托起来了
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.32f), brushBg.GetAddressOf());
	d2d->deviceContext->CreateSolidColorBrush(D2D1::ColorF(0x1677ff), brushBorder.GetAddressOf());
	initWinRect();
	lastEnumTick = GetTickCount64();
}

CutMask::~CutMask()
{
	// 智能选区三层（几何 / 像素 / onSettle 维护）全部活在 UI 线程上，
	// 没有后台线程、没有跨进程查询，析构无事可做
}

// —— 智能选区增强：整屏像素副本 ——————————————————————————————
// 宿主（WinCap）把 takeScreenShot 已经填好的整屏缓冲直接交过来，这里只是接管所有权。
// 之前的做法是先用这张图建一份 GPU 的 CPU_READ 副本、再 Map 出来 memcpy 一份 ——
// 同一份桌面像素在会话里存了三份，二级缓存被反复冲掉，首帧也多花一次整屏拷贝。
// 之后的边缘查询全部走这块内存，不碰 GPU。
void CutMask::adoptPixels(std::vector<uint8_t>&& bits, int pw, int ph, int stride)
{
	pix = std::move(bits);
	pixReady = false;
	pixW = pw; pixH = ph; pixStride = stride;
	if (!pix.empty() && pw > 0 && ph > 0 && stride >= pw * 4)
		pixReady = true;
}

uint32_t CutMask::pixAt(int x, int y) const
{
	if (!pixReady || x < 0 || y < 0 || x >= pixW || y >= pixH) return 0;
	const uint8_t* p = pix.data() + (size_t)y * (size_t)pixStride + (size_t)x * 4;
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

int CutMask::colorDist(uint32_t a, uint32_t b)
{
	int dr = std::abs((int)(a & 0xFF) - (int)(b & 0xFF));
	int dg = std::abs((int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF));
	int db = std::abs((int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF));
	return dr + dg + db;
}

// 像素层：在**冻结截图**里量出"光标所在的那个框"。
//
// 为什么必须有这一层：
//   几何层（顶层窗口 + 子窗口）只能看到有 HWND 的东西。Electron / Chromium /
//   UWP 这些界面整块内容只有一个 HWND，按钮、列表项、导航行统统不是窗口 ——
//   几何层给出的永远是"整个窗口"。
//   像素层不跨进程：画面早就在内存里，四个方向各扫一遍，不到 0.1ms。
//   于是它可以挂在**每一帧**上算，手一动框就跟着变 —— 这才是 Snipaste 那种手感。
//
// 判据用的是"跳变 + 稳住"两级：
//   · 颜色差超过 kJump 才算一次跳变；
//   · 跳过去之后那一侧还必须连续 kStable 个像素保持同色。
// 第二条是用来滤文字笔画的 —— 笔画只有两三像素宽，跨过去马上又变回来；
// 真正的边界（按钮边框、容器底色交界）后面是一整片同色区。
bool CutMask::pixelBoxAt(POINT pos, D2D1_RECT_F& out) const
{
	if (!pixReady || pixW <= 2 || pixH <= 2) return false;
	if (pos.x < 1 || pos.y < 1 || pos.x >= pixW - 1 || pos.y >= pixH - 1) return false;

	// 桌面（Progman / 它旁边的 WorkerW）不走像素层，两个原因：
	//   · 图标边缘是抗锯齿软边，四个方向的"第一跳"会整个越过图标，咬到图标单元格的
	//     网格线上 —— 实测框住 71×141 的格子，而图标本身只有 35×39，框大了三倍；
	//   · 就算咬到格子，验边分数也在阈值附近抖（实测 34~39，需 40），同一个位置
	//     两次捕获能过与不能过看运气，选出来的框时对时错。
	// 桌面图标改由"桌面图标层"（deskIconAt，见下）专门处理：
	// 它拿到的是 explorer 自己算的槽位，不需要靠颜色跳变去猜边界
	if (isDesktopRoot(lastHitRoot)) {
		return false;
	}

	// 阈值跟 DPI 走：200% 缩放下边框有 2~3 物理像素，稳住的距离也得放宽
	const float s = win ? std::max(1.f, win->dpi / 96.f) : 1.f;
	const int kStable = std::max(4, (int)(6 * s));
	const int kJump = 42;        // 三通道绝对差之和（0~765）
	const int kSteadyTol = 14;

	// 从 (x0,y0) 沿 (dx,dy) 往外走，返回"第一个真正的边界"距出发点的像素数；-1 = 没找到
	auto scan = [&](int x0, int y0, int dx, int dy, int limit) -> int {
		for (int i = 1; i <= limit; i++) {
			const int x = x0 + dx * i, y = y0 + dy * i;
			if (x < 0 || y < 0 || x >= pixW || y >= pixH) return -1;
			const uint32_t c = pixAt(x, y);
			if (colorDist(c, pixAt(x - dx, y - dy)) < kJump) continue;
			bool steady = true;
			for (int k = 1; k <= kStable; k++) {
				const int nx = x + dx * k, ny = y + dy * k;
				if (nx < 0 || ny < 0 || nx >= pixW || ny >= pixH) break;
				if (colorDist(pixAt(nx, ny), c) > kSteadyTol) { steady = false; break; }
			}
			if (steady) return i;
		}
		return -1;
	};

	const int dl = scan(pos.x, pos.y, -1, 0, pos.x);
	const int dr = scan(pos.x, pos.y, +1, 0, pixW - 1 - pos.x);
	const int du = scan(pos.x, pos.y, 0, -1, pos.y);
	const int dd = scan(pos.x, pos.y, 0, +1, pixH - 1 - pos.y);
	if (dl < 0 || dr < 0 || du < 0 || dd < 0) return false;

	// 边界落在"跨过去的那一个像素"上，所以内侧那一边要从边界再让回 1 像素
	D2D1_RECT_F r{ (float)(pos.x - dl + 1), (float)(pos.y - du + 1),
				   (float)(pos.x + dr - 1), (float)(pos.y + dd - 1) };

	const float minSide = 8.f * s;
	if (r.right - r.left < minSide || r.bottom - r.top < minSide) return false;
	// 铺满整屏的框没有意义：压暗层四块宽度为 0，看着像"什么都没吸到"
	if (r.right - r.left >= pixW - 3.f && r.bottom - r.top >= pixH - 3.f) return false;

	// 验边。四个方向各自撞到的线**不一定**围得成一个元素：桌面空白处就是这样 ——
	//   · 往上是某个图标的下沿、往下是某个窗口的上沿，左右直接撞到屏幕边缘；
	//   · 四段拼起来是个大框，但它不是任何一个东西，选上去等于"吸了个寂寞"。
	// 判据：沿每条边取样，边线本身与内侧一格的颜色必须明显不同。真元素的四条边
	// 都是这么来的；撞到屏幕边（取不到样）直接判不合格
	const int xl = (int)r.left, xr = (int)r.right - 1;
	const int yt = (int)r.top, yb = (int)r.bottom - 1;
	if (xl < 1 || yt < 1 || xr >= pixW - 1 || yb >= pixH - 1) return false;

	// 沿边取样：起点 (x0,y0)，边方向 (ux,uy)，法线方向（指向框内）(nx,ny)，边长 len
	// 返回"看得出是边界"的取样点百分比；撞到屏幕边界返回 -1
	auto edgeScore = [&](int x0, int y0, int ux, int uy, int nx, int ny, int len) -> int {
		const int step = std::max(4, len / 24);
		int hits = 0, tries = 0;
		for (int d = 0; d <= len; d += step) {
			const int x = x0 + ux * d, y = y0 + uy * d;
			const int ix = x + nx, iy = y + ny;
			if (x < 1 || y < 1 || x >= pixW - 1 || y >= pixH - 1) return -1;
			if (ix < 0 || iy < 0 || ix >= pixW || iy >= pixH) return -1;
			if (colorDist(pixAt(x, y), pixAt(ix, iy)) > kJump / 2) hits++;
			tries++;
		}
		if (tries < 6) return -1;
		return hits * 100 / tries;
	};
	// 40%：边上允许有抗锯齿、圆角、半透明、图标羽化边缘的地方看不出边界，
	// 但不能一半以上都看不出来。实测桌面图标（抗锯齿 + 复杂壁纸）四条边在 44~55，
	// 而"随便四个方向撞到的线凑成的假框"只有 20~24 —— 中间留了足够的安全带
	const int need = 40;
	const int scL = edgeScore(xl, yt, 0, 1, 1, 0, yb - yt);
	const int scR = edgeScore(xr, yt, 0, 1, -1, 0, yb - yt);
	const int scT = edgeScore(xl, yt, 1, 0, 0, 1, xr - xl);
	const int scB = edgeScore(xl, yb, 1, 0, 0, -1, xr - xl);
	if (scL < need || scR < need || scT < need || scB < need) return false;

	out = r;
	return true;
}

// ———————————————— 桌面图标层 ————————————————
// 光标压在桌面上时，前面两层给出的框都不对（原因见 CutMask.h 里 DeskIcon 那段）。
// 这一层只做三件事，全部基于公开 Win32：
//   ① 找到桌面的 SysListView32；
//   ② 向它要每一项的图标槽位（LVM_GETITEMRECT + LVIR_ICON，跨进程要一块远程缓冲）；
//   ③ 在冻结画面里把槽位细化成图标本体的包围盒。
// 实测（1920×1080 / 100% 缩放 / 47 个图标）：一次全量扫 16ms，之后每次悬停只遍历
// 47 个矩形，零跨进程调用；细化一格 76×52 的像素扫描 ~20μs。
bool CutMask::refreshDeskIcons(bool force)
{
	const ULONGLONG now = GetTickCount64();
	// 表还在有效期内：直接用。过期了也只标待办 —— 一次全量扫 16ms，挂在 mousemove
	// 上就是"鼠标一动一顿"。真正刷新的时机是鼠标停住（onSettle），和识别表一个道理。
	// 节流不能只在"表里有内容"时才生效：桌面图标被设成隐藏（右键 → 查看 → 显示桌面图标
	// 取消勾选）时表里永远是空的，以前每走一步都重新全量扫一遍 —— 几十条跨进程消息 +
	// OpenProcess + VirtualAllocEx，鼠标在桌面上一动就一顿。改成 deskStamp 有过值就一律走
	// 节流，只有第一次（还没扫过）才当场扫，保证第一次悬停就能出框
	if (!force && deskStamp) {
		if (now - deskStamp < 800) return !deskIcons.empty();
		deskWant = true;
		return !deskIcons.empty();
	}
	deskStamp = now;
	deskIcons.clear();
	// 每次都用 IsWindow 复核：explorer 重启（或者用户从任务管理器里重启桌面）之后
	// 手里那个句柄就废了，不核对会一直对着一个死句柄发消息
	if (!deskList || !IsWindow(deskList)) deskList = findDeskList();
	if (!deskList) return false;

	const int count = (int)listMsg(deskList, LVM_GETITEMCOUNT, 0, 0);
	// 桌面图标几十个；上千说明这压根不是桌面列表（或者被塞了脏数据），别跟着跑
	if (count <= 0 || count > 4000) return false;

	DWORD pid = 0;
	GetWindowThreadProcessId(deskList, &pid);
	// explorer 和我们同一个用户、同一个完整性级别，读写它的地址空间是常规操作。
	// 万一失败（对方是受保护进程 / UIPI 拦了）就整条路放弃，退回老的"最近窗口"兜底
	HANDLE proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
	if (!proc) return false;
	// LVM_GETITEMRECT 的 lParam 是**对方进程里的指针**，且它第一个字段是"要哪个矩形"
	// （LVIR_ICON 等），所以每次调用前都要把请求写进去。一块缓冲反复用，别每项申请一次
	void* remote = VirtualAllocEx(proc, nullptr, sizeof(RECT), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!remote) { CloseHandle(proc); return false; }

	// 列表的客户区原点在屏幕上的位置（多显示器 / 任务栏在左时不是 0,0）
	POINT org{ 0, 0 };
	ClientToScreen(deskList, &org);

	deskIcons.reserve((size_t)count);
	for (int i = 0; i < count; i++) {
		RECT req{ LVIR_ICON, i, 0, 0 };   // left = 要哪个矩形，top = 项索引
		if (!WriteProcessMemory(proc, remote, &req, sizeof(req), nullptr)) break;
		if (!listMsg(deskList, LVM_GETITEMRECT, (WPARAM)i, (LPARAM)remote)) continue;
		RECT slot{};
		if (!ReadProcessMemory(proc, remote, &slot, sizeof(slot), nullptr)) continue;
		// 空项 / 取到脏数据：桌面槽位在任何 DPI 下都远大于 8px
		if (slot.right - slot.left < 8 || slot.bottom - slot.top < 8) continue;

		// 再要一次整项包围盒（图标 + 标签）。标签行数 explorer 自己算，一行、两行
		// 它都算得对 —— 官方那条虚线框用的就是这个 LVIR_BOUNDS。只按"槽位 + 固定
		// 45% 的标签带"估，两行标题的第二行必然被切掉
		RECT bounds{};
		bool hasBounds = false;
		{
			RECT reqB{ LVIR_BOUNDS, i, 0, 0 };
			if (WriteProcessMemory(proc, remote, &reqB, sizeof(reqB), nullptr)
				&& listMsg(deskList, LVM_GETITEMRECT, (WPARAM)i, (LPARAM)remote)
				&& ReadProcessMemory(proc, remote, &bounds, sizeof(bounds), nullptr)) {
				// 脏数据判据：整项不可能比图标本体还小
				hasBounds = bounds.right - bounds.left >= slot.right - slot.left
					&& bounds.bottom - bounds.top >= slot.bottom - slot.top;
			}
		}

		DeskIcon d;
		d.slot = slot;
		OffsetRect(&d.slot, org.x, org.y);
		if (hasBounds) {
			// 并上槽位：个别 shell 主题下 LVIR_BOUNDS 只给标签那条带，
			// 并集保证图标区一定在里面
			RECT item{};
			UnionRect(&item, &bounds, &slot);
			d.hit = item;
			OffsetRect(&d.hit, org.x, org.y);
		}
		else {
			// 取不到 BOUNDS（旧版 explorer / 跨进程被拦）：退回原来的估算，
			// 一行标题够用，两行会紧一点，但总比吸不到好
			const LONG labelH = std::max<LONG>(12, (slot.bottom - slot.top) * 45 / 100);
			d.hit = d.slot;
			d.hit.bottom += labelH;
		}
		deskIcons.push_back(d);
	}

	VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
	CloseHandle(proc);
	return !deskIcons.empty();
}

// 槽位 → 图标本体。explorer 给的槽位是"格"，图标图形居中放在里面（实测 76×52 的槽位
// 里图形是 48×48，左右各留 14px）。这里在冻结画面里扫出图形的真实包围盒：
//   · 好处：框贴住图形的抗锯齿边缘，"吸上就是它"，而不是吸到一个比图标大一圈的格；
//   · 代价：一格 76×52 = 3952 个像素，全是内存直读，约 20μs。
// 判据是"与壁纸底色差得多"。底色取槽位四角的主色 —— 静态壁纸是纯色，渐变壁纸
// 四个角也只差几个色阶（实测同一列相邻图标背景差 3~5）。
bool CutMask::refineDeskGlyph(const RECT& slotScreen, RECT& glyphScreen) const
{
	if (!pixReady) return false;
	// 屏幕坐标 → 截图客户区坐标（pix 就是这个坐标系）
	RECT r{ slotScreen.left - (LONG)win->x, slotScreen.top - (LONG)win->y,
			slotScreen.right - (LONG)win->x, slotScreen.bottom - (LONG)win->y };
	if (r.left < 1) r.left = 1;
	if (r.top < 1) r.top = 1;
	if (r.right > pixW - 1) r.right = pixW - 1;
	if (r.bottom > pixH - 1) r.bottom = pixH - 1;
	if (r.right - r.left < 8 || r.bottom - r.top < 8) return false;

	const uint32_t c[4] = { pixAt(r.left + 1, r.top + 1), pixAt(r.right - 2, r.top + 1),
						   pixAt(r.left + 1, r.bottom - 2), pixAt(r.right - 2, r.bottom - 2) };
	uint32_t bg = c[0];
	int bestN = 0;
	for (const uint32_t a : c) {
		int n = 0;
		for (const uint32_t b : c) if (colorDist(a, b) < 30) n++;
		if (n > bestN) { bestN = n; bg = a; }
	}

	// 60 是"明显不是底色"的门槛：图标边缘的抗锯齿过渡像素不参与，
	// 免得包围盒被一圈羽化边撑到槽位大小
	int minx = INT_MAX, miny = INT_MAX, maxx = -1, maxy = -1;
	for (int y = r.top; y < r.bottom; y++) {
		for (int x = r.left; x < r.right; x++) {
			if (colorDist(pixAt(x, y), bg) <= 60) continue;
			if (x < minx) minx = x;
			if (y < miny) miny = y;
			if (x > maxx) maxx = x;
			if (y > maxy) maxy = y;
		}
	}
	if (maxx < 0) return false;

	const int bw = maxx - minx + 1, bh = maxy - miny + 1;
	const int sw = r.right - r.left, sh = r.bottom - r.top;
	// 前景太小：底色估错了（壁纸和图标同色）—— 细化出来的框会小得没法用
	if (bw < sw * 2 / 5 && bh < sh * 2 / 5) return false;
	// 前景铺满整格：壁纸太花，这一格里分不出"图形"和"背景"—— 别硬细化，
	// 退回槽位（槽位至少是准的，只是宽了点）
	if (bw >= sw - 2 && bh >= sh - 2) return false;

	glyphScreen = RECT{ minx + (LONG)win->x, miny + (LONG)win->y,
						maxx + 1 + (LONG)win->x, maxy + 1 + (LONG)win->y };
	return true;
}

// 光标落在哪个桌面图标上：命中就给出两个粒度 —— 图标本体（默认选它）与槽位连文字。
// 两个粒度是"滚轮循环候选"想要的语义：同一个东西的紧 / 松两档，和
// "按钮 → 面板 → 窗口"是同一套循环
bool CutMask::deskIconAt(POINT pos, D2D1_RECT_F& glyphOut, D2D1_RECT_F& itemOut)
{
	if (!refreshDeskIcons(false)) return false;
	const POINT sp{ (LONG)(win->x + pos.x), (LONG)(win->y + pos.y) };
	// 同一位置上偶尔会叠着两个（图标被拖得半叠、或者长名字的标签伸进邻居的带里）：
	// 取"槽位面积最小"的那个，与识别表里"越小的元素越具体"同一条判据
	const DeskIcon* best = nullptr;
	long bestArea = LONG_MAX;
	for (const auto& d : deskIcons) {
		if (sp.x < d.hit.left || sp.x >= d.hit.right || sp.y < d.hit.top || sp.y >= d.hit.bottom) continue;
		const long a = (long)(d.slot.right - d.slot.left) * (long)(d.slot.bottom - d.slot.top);
		if (a < bestArea) { bestArea = a; best = &d; }
	}
	if (!best) return false;

	// 图标本体：优先用像素细化结果，细化不出来就用槽位 —— 槽位已经是"准的"，
	// 只是四周各多留了一点空白，比"什么都吸不到"好得多
	RECT g = best->slot;
	RECT refined{};
	if (refineDeskGlyph(best->slot, refined)) g = refined;

	auto toClient = [this](const RECT& r) {
		return D2D1::RectF((float)(r.left - (LONG)win->x), (float)(r.top - (LONG)win->y),
			(float)(r.right - (LONG)win->x), (float)(r.bottom - (LONG)win->y));
		};
	glyphOut = toClient(g);
	itemOut = toClient(best->hit);
	return true;
}

// 光标下的候选：先认最上层窗口，再在它内部挑最小的元素。
//
// 两条口径，粒度正好相反，别混：
//   · 桌面图标 —— 走下面 isDesktopRoot 那条专属分支，整格（槽位连文字标签）先入列，
//     默认就选中整格，滚轮才切到图标本体；
//   · 窗口 / 面板 —— 按面积**升序**排（见函数末尾），默认选中最小的那个元素，
//     悬停在按钮上就吸按钮、在面板上就吸面板。
//
// 之前这里只有"按面积升序取最小"一步 —— 完全没看 z 序。后果是两个方向都错：
//   · 前景是大窗口、背景窗口里有个小按钮，小按钮面积更小，于是光标停在前景窗口上
//     却吸到了背后那个按钮（表现：吸不到最上层的东西）；
//   · 任何窗口里的小控件都优先命中，桌面上一堆边角料控件让光标"到处乱吸"
//     （表现：太灵敏、吸到手边不相干的东西）。
// 面积只能决定"同一个窗口内部"谁更具体，跨窗口必须以 z 序为准：光标这一点上，
// 被前景窗口盖住的一切都不该参与竞争。
void CutMask::collectHover(POINT pos, std::vector<D2D1_RECT_F>& out)
{
	PROF_SCOPE("cm.collect.total");
	auto inside = [&pos](const D2D1_RECT_F& r) {
		return pos.x > r.left && pos.y > r.top && pos.x < r.right && pos.y < r.bottom;
		};

	// 铺满整个截图区的矩形不当几何候选（桌面窗口、最大化的全屏窗口都是这一类）。
	// 吸上去相当于"框住整屏"：压暗层四块的宽度是 0，屏幕看着一点没变化，
	// 但选区数据确实从"小图标"变成了"整屏" —— 而下一帧像素层又把它细化回小框，
	// 压暗层在"没有"和"盖满全屏"之间来回切，就是用户说的"鼠标还在智选区域里
	// 只挪了一点，屏幕就跳闪"
	const float fullTol = 4.f * (win ? win->dpi / 96.f : 1.f);
	const float fullR = (float)win->w - fullTol;
	const float fullB = (float)win->h - fullTol;
	auto isFullScreen = [fullTol, fullR, fullB](const D2D1_RECT_F& r) {
		return r.left <= fullTol && r.top <= fullTol && r.right >= fullR && r.bottom >= fullB;
		};

	// 先问系统："光标这一点上，屏幕上看得见的是哪个窗口"。
	// 智能选区要吸的是"眼睛看到的那个窗口"，被挡在后面的窗口哪怕正好有小控件
	// 压在这个坐标上，也不该被吸到 —— 原来那些"识别到背面"就是从这儿来的：
	// 以前只按枚举出来的位次判断遮挡，一旦最上层窗口没进识别表
	// （整屏被过滤、尺寸不足 6px、DWM 阴影边界算差一点），位次就指到了它后面那一层。
	const POINT screenPos{ (LONG)(win->x + pos.x), (LONG)(win->y + pos.y) };
	HWND hitRoot = nullptr;
	{
		PROF_SCOPE("cm.hover.root");
		hitRoot = visibleRootAt(screenPos, win->hwnd);
	}
	// 给调用方（highlight）用：它还要判断"这个窗口在不在识别表里"
	lastHitRoot = hitRoot;

	// —— 桌面图标：专项识别，命中就收工 ——
	// 桌面上的候选只有这一层说得对：几何层给的是整屏（被过滤掉），像素层咬的是
	// 单元格网格线，nearestWindow 会吸到隔壁那个窗口。所以命中图标就直接返回，
	// 后面三层不必再算（也免得它们把正确的框盖掉）
	if (isDesktopRoot(hitRoot)) {
		D2D1_RECT_F glyph{}, item{};
		bool got = false;
		{
			PROF_SCOPE("cm.hover.desk");
			got = deskIconAt(pos, glyph, item);
		}
		if (got) {
			// 两个粒度：整格（槽位连下方文字标签）/ 图标本体。
			// 这里是插入顺序，不是面积顺序：整格先入列 → hoverIdx 0 默认就框整格
			// （实测 76×72，与参考软件一致；图标本体 48×48 单独框出来"识别过头了"）。
			// 滚轮往里走才切到本体。桌面这条分支在排序之前就返回，所以不受下面升序影响
			out.push_back(item);
			out.push_back(glyph);
			return;
		}
	}

	// 手里有系统答案时以它为准；拿不到（命中的是我们自己的截图窗）才退回位次
	int topRank = INT_MAX;
	if (!hitRoot) {
		for (const auto& it : winRect) {
			if (!it.top || !inside(it.rc)) continue;
			if (it.win < topRank) { topRank = it.win; }
		}
	}

	long long _tbl0 = ::prof::enabled() ? ::prof::nowUs() : 0;
	for (const auto& it : winRect) {
		if (hitRoot) {
			// 已经点名到具体窗口：只认它自己和它的子控件，背后的窗口一律不参选
			if (it.hwnd != hitRoot) continue;
		}
		else if (topRank != INT_MAX && it.win != topRank) {
			// 有更上层的窗口压着：它下面的矩形一律不参选。
			// topRank 为 INT_MAX 说明没有任何顶层窗口命中（例如落在桌面空白）——
			// 这时退回老行为，只剩子控件矩形可用的情况也能吸
			continue;
		}
		if (!inside(it.rc)) continue;
		// 整屏矩形不参选：见上面 isFullScreen 那段说明
		if (isFullScreen(it.rc)) continue;
		out.push_back(it.rc);
	}
	if (_tbl0) PROF_ADD("cm.hover.table", ::prof::nowUs() - _tbl0);

	// 像素层：从冻结画面直接量出"光标所在的那个框"。
	// 这一层不跨进程、不看 z 序，四个方向各扫一遍不到 0.1ms，所以可以每帧都算 ——
	// 它的意义不在于"比几何层更准"，而在于"手一动框就跟上"：几何层够不到窗口内部
	// （Electron / Chromium 整块内容只有一个 HWND），像素层能贴着内容边走。
	// 框必须落在"眼睛看见的那个窗口"里面：越界就说明扫到别的窗口的内容上了
	if (hitRoot) {
		D2D1_RECT_F pb{};
		{
			PROF_SCOPE("cm.hover.pixel");
			if (pixelBoxAt(pos, pb)) {
				D2D1_RECT_F limit{ 0.f, 0.f, (float)pixW, (float)pixH };
				for (const auto& it : winRect) {
					// 子控件的 hwnd 记的是父顶层窗口，这里只会命中顶层窗口那一条
					if (it.hwnd != hitRoot) continue;
					limit = it.rc;
					break;
				}
				pb.left = std::max(pb.left, limit.left);
				pb.top = std::max(pb.top, limit.top);
				pb.right = std::min(pb.right, limit.right);
				pb.bottom = std::min(pb.bottom, limit.bottom);
				const float minSide = 8.f * (win ? win->dpi / 96.f : 1.f);
				if (pb.right - pb.left >= minSide && pb.bottom - pb.top >= minSide) out.push_back(pb);
			}
		}
	}

	// 系统点名的那个窗口没进识别表（刚弹出来 / 枚举之后界面又变了 / 尺寸被过滤）：
	// 直接用它自己的矩形。绝不能因为"表里没有"就往下层找 —— 下层不是用户看见的东西
	if (hitRoot && out.empty()) {
		RECT rc{};
		if (SUCCEEDED(DwmGetWindowAttribute(hitRoot, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)))) {
			const D2D1_RECT_F rel{
				(float)(rc.left - (int)win->x), (float)(rc.top - (int)win->y),
				(float)(rc.right - (int)win->x), (float)(rc.bottom - (int)win->y) };
			if (!isFullScreen(rel)) out.push_back(rel);
		}
	}

	// 光标落在所有窗口之外（桌面空白、或者贴着窗口边缘往外一点点）：
	// 退一步吸"最近的那个顶层窗口"。以前这里直接返回空，表现就是
	// "鼠标往豆包窗口边上一靠，什么都不吸" —— 必须精确进到窗口里面才算数，
	// 用起来非常别扭。距离阈给到 24px（按 dpi 折算），一靠上去就贴住
	if (out.empty()) {
		// 变量名不能叫 near：windef.h 里有 "#define near"（16 位时代的遗留），
		// 展开后这行就变成 D2D1_RECT_F {}; 直接语法错误
		D2D1_RECT_F nearRc{};
		// 兜底吸"最近的窗口"，整屏的同样不要（否则桌面上又会吸回整屏，白改）
		if (nearestWindow(pos, nearRc) && !isFullScreen(nearRc)) out.push_back(nearRc);
	}

	// 按面积升序：同一个窗口里越小的元素越具体。这是唯一需要的次序判据 ——
	// 跨窗口的竞争已经在上面用 z 序（hitRoot / topRank）解决，这里只排窗口内部。
	// 桌面图标不经过这里（它在上面的专属分支里已 return）
	{
		PROF_SCOPE("cm.hover.sort");
		std::sort(out.begin(), out.end(), [](const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
			const float aa = (a.right - a.left) * (a.bottom - a.top);
			const float ab = (b.right - b.left) * (b.bottom - b.top);
			return aa < ab;
			});
	}
}

// 最近的那个顶层窗口：点到矩形边缘的欧氏距离，24px 以内算"靠上去了"。
// 同距离时 z 序靠前的优先 —— 两个窗口边缘贴在一起时，吸的该是上面那个
bool CutMask::nearestWindow(POINT pos, D2D1_RECT_F& out) const
{
	const float maxDist = 24.f * (win ? win->dpi / 96.f : 1.f);
	bool found = false;
	float bestD = FLT_MAX;
	int bestRank = INT_MAX;
	for (const auto& it : winRect) {
		if (!it.top) continue;   // 子控件不参与"靠近吸附"，它们只在自己窗口被命中后才有意义
		const float dx = std::max({ it.rc.left - (float)pos.x, 0.f, (float)pos.x - it.rc.right });
		const float dy = std::max({ it.rc.top - (float)pos.y, 0.f, (float)pos.y - it.rc.bottom });
		const float d = std::sqrt(dx * dx + dy * dy);
		if (d > maxDist) continue;
		if (d < bestD - 0.5f || (std::abs(d - bestD) <= 0.5f && it.win < bestRank)) {
			bestD = d;
			bestRank = it.win;
			out = it.rc;
			found = true;
		}
	}
	return found;
}


bool CutMask::highlight(POINT pos)
{
	PROF_SCOPE("cm.highlight.total");
	// autoSnap 关着时（右键重进选区后、鼠标还没动的那一段）不做悬停吸附：先到"+",
	// 鼠标一动 WinCap 就把它打开，识别立刻恢复，与首次进入同一条节奏
	if (!autoSnap) return false;
	// 智能识别：收集"包含光标"的所有矩形，窗口内部按面积升序排（越小的元素越具体），
	// 于是光标悬停在按钮上就吸按钮、在面板上就吸面板；
	// 桌面图标另算：整格（槽位 + 文字标签）优先，滚轮往里才是图标本体。
	// 三层全部活在 UI 线程上：几何层（窗口 / 子控件）与像素层（颜色突变）都是内存操作，
	// 微秒级，挂在 mousemove 上没有负担
	hoverCands.clear();
	collectHover(pos, hoverCands);
	// 光标不在任何已知矩形里：界面可能刚变过（截图期间弹出菜单、Tooltip、切换标签页、
	// 播放中的视频换了画面）。节流重枚举识别表再试一次，别让新内容吸不到。
	// 只标待办不现场做：重枚举是 EnumWindows + 每个窗口一趟 EnumChildWindows，
	// 桌面上窗口一多就是几毫秒起，挂在 mousemove 上就是"一路拖着还在顿"。
	// 真正干活的时机是鼠标停住之后（onSettle）
	if (hoverCands.empty()) {
		ULONGLONG now = GetTickCount64();
		if (now - lastEnumTick > 600) enumWant = true;
	}
	else if (lastHitRoot) {
		// 有候选、但"光标压着的这个窗口"根本不在识别表里（刚弹出来的，枚举之后
		// 界面又变了）：子控件的矩形就无从谈起，趁早补一次枚举。
		// 只在这一种情况下标待办 —— "只要候选为空就重枚举"会把 Electron / 桌面图标
		// 这类给不出几何候选的场景变成每次停住都全量枚举，"停下立刻落框"被拖成一百多毫秒
		bool known = false;
		for (const auto& it : winRect) {
			if (it.hwnd == lastHitRoot) { known = true; break; }
		}
		ULONGLONG now = GetTickCount64();
		if (!known && now - lastEnumTick > 600) enumWant = true;
	}
	// 排一次"停定维护"。每次移动都会重设同一个定时器 —— 等于天然防抖：
	// 手一直在动就一直不维护，停下 30ms 才做节流重枚举
	if (win) win->setTimer(30, 101);

	const bool insideCur = pos.x > maskRect.left && pos.y > maskRect.top
		&& pos.x < maskRect.right && pos.y < maskRect.bottom;

	if (hoverCands.empty()) {
		if (insideCur) return false;   // 粘滞：光标还在当前选框里，保持不动
		// 三层都没吸到、光标又已离开当前选框（桌面图标的格间空隙、窗口边缘的空白都是这样）：
		// 以前立刻收框，压暗层跟着闪一下亮 —— 桌面图标一格挨一格，格间空隙几十毫秒就跨过去，
		// 一路扫过去就是一串"暗-亮-暗-亮"。这里改成挂住 kHoldMs，期间落到新目标就作废；
		// 真停在没有目标的地方超过 kHoldMs，才由 onHoldExpire 把框和压暗层一起收掉
		if (hasRect()) {
			static constexpr ULONGLONG kHoldMs{ 700 };
			holdUntil = GetTickCount64() + kHoldMs;
			if (win) win->setTimer((UINT)kHoldMs, 103);
			return false;
		}
		// 这一条是"人已经走了还挂在上一处"的陈框兜底：截图期间界面变过、候选全数消失，
		// onSettle 的重枚举落地时自然把新内容选上
		return false;
	}
	if (hoverIdx >= hoverCands.size()) hoverIdx = 0;
	// 又吸到了目标：挂起作废，别让 103 到期时把刚落好的框收掉
	if (holdUntil) {
		holdUntil = 0;
		if (win) win->killTimer(103);
	}
	auto target = hoverCands[hoverIdx];
	const bool rectChanged = rectDiffers(maskRect, target, 2.f);
	if (rectChanged) {
		maskRect = target;
		// 同 applySelection：记下这一份"悬停预览框"，挂住到期只认它
		previewRect = target;
		makeLayout();
		// 这里不自己 requestPaint：调用方（WinCap::onMove）紧接着还要摆放大镜，
		// 由它统一在最后排一帧 —— 在这儿先画一次等于白画一帧放大器还在旧位置的画面。
		// 返回值已经告诉调用方"选区变了"，要不要重画由它决定
		return true;
	}
	return false;
}

// 鼠标停住之后的维护（宿主定时器 101 驱动，30ms 防抖）。
// 只做一件事：把 highlight 里标了待办的"节流重枚举"做掉。重枚举是
// EnumWindows + 逐窗口 EnumChildWindows，桌面忙的时候几毫秒到几十毫秒，
// 挂在 mousemove 上就是"动一下就顿一下"；而本函数的触发条件就是
// "鼠标已经停住 30ms"，正是做这件重活的时机
void CutMask::onSettle()
{
	PROF_SCOPE("cm.settle.total");
	if (win) win->killTimer(101);
	if (!autoSnap) return;
	if (enumWant) {
		enumWant = false;
		initWinRect();
		lastEnumTick = GetTickCount64();
	}
	// 桌面图标表过期的实际刷新点。放在"鼠标已经停住 30ms"之后，和识别表同一个理由：
	// 一次全量扫要跨进程发几十条消息（实测 16ms），不能挂在 mousemove 上。
	// 只在真的在桌面上标的待办（deskIconAt 里），所以平时不会白扫
	if (deskWant) {
		deskWant = false;
		refreshDeskIcons(true);
	}
}


// 悬停候选断开后的"挂住"到期（宿主定时器 103 驱动）。挂住期内光标一直没落到
// 任何识别目标上，到点就把选区框连同压暗层一起收掉 —— 500ms~1s 的这段缓冲，
// 正是为了让人在桌面图标的格间空隙里穿行时，压暗层不再一亮一暗地闪
void CutMask::onHoldExpire()
{
	if (win) win->killTimer(103);
	holdUntil = 0;
	if (!hasRect()) return;
	// 只在"框还是那份悬停预览"时才收。用户在这 700ms 里自己拖了框、或者拖了边角
	// （矩形已经不是 previewRect），这就是他要的结果，不能抹掉 —— 抹掉的表现是
	// "刚框完，选区连同压暗层一起消失，屏幕上只剩工具条"
	if (rectDiffers(maskRect, previewRect, 1.f)) return;
	maskRect = D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	previewRect = D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	layout.Reset();
	hoverIdx = 0;
	if (win) win->requestPaint();
}


// 只有 Select 阶段会调（WinCap::onWheel），dir: +1 下一个 / -1 上一个
void CutMask::cycleHover(POINT pos, int dir)
{
	if (!autoSnap) return;
	hoverCands.clear();
	collectHover(pos, hoverCands);
	if (hoverCands.size() < 2) return; // 只有一个候选，没什么好循环的
	int n = (int)hoverCands.size();
	hoverIdx = ((int)hoverIdx + dir + n) % n;
	auto target = hoverCands[hoverIdx];
	if (maskRect.left != target.left || maskRect.top != target.top ||
		maskRect.right != target.right || maskRect.bottom != target.bottom) {
		maskRect = target;
		makeLayout();
		win->requestPaint();
	}
}

void CutMask::initWinRect()
{
	winRect.clear();
	// 枚举上下文：rank 就是 z 序位次。EnumWindows 按 z 序从上到下回调，
	// 所以第一个被接受的窗口是最上层，后面依次往下 —— 这个次序是 collectHover
	// 判断"谁压着谁"的唯一依据，不能丢
	EnumCtx ctx{ this, 0 };

	// 只认外部窗口：本程序自己的截图窗 / 工具条 / 贴屏窗都是置顶全屏或悬浮，不参与识别，
	// 否则光标一动就会先被"自己的全屏矩形"匹配到，智能识别永远吸不到桌面上别的窗口
	EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL
		{
			auto c = reinterpret_cast<EnumCtx*>(lparam);
			const int rank = c->rank++;
			if (!hwnd) return TRUE;
			if (!IsWindowVisible(hwnd)) return TRUE;
			DWORD wpid = 0;
			GetWindowThreadProcessId(hwnd, &wpid);
			if (wpid == GetCurrentProcessId()) {
				// 自己的窗口分两类，处理完全相反：
				//   · 截图窗本身、工具条、贴图窗（都是 Ui 库通用窗口类 "ui" 的悬浮/全屏窗）
				//     —— 必须跳过。它们是截图界面的一部分，全屏置顶，留在表里光标一动
				//     就先被自己的全屏矩形匹配到，永远吸不到桌面上的东西；
				//   · 主界面「二维码工具」（窗口类 SnapCore_Main）—— 必须留下。
				//     用户截自己这个窗口时，想吸的正是里面的输入框和按钮；
				//     把它一起排除掉，就只剩它背后那个窗口可吸 —— 表现就是
				//     "只认外面的窗口，里面的死活吸不到"
				wchar_t cls[64]{};
				GetClassNameW(hwnd, cls, 64);
				// 类名必须走品牌宏：写死 "SnapCore_Main" 的话 SnapOCR 版会把自己的主界面
				// 也当成"截图界面的一部分"排除掉，用户截自己主界面时吸不到里面的控件
				if (wcscmp(cls, SNAP_CLS_MAIN) != 0) return TRUE;
			}
			BOOL cloaked = FALSE;
			DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
			if (cloaked) return TRUE;
			// 取扩展边框（不含 DWM 阴影）。部分窗口（UWP / 老式窗口 / 会话切换中）
			// 这个调用会失败，此时 rect 是未初始化的栈值 —— 直接拿去当矩形，识别表里
			// 就多了一块坐标随机的"窗口"，智能选区会吸到屏幕上根本不存在的东西。
			// 失败就退回 GetWindowRect（含不可见边框，但坐标是准的）
			RECT rect{};
			// FAILED 只包 DWM 这一次调用：把后面的 || 一起包进去的话，整个表达式先收敛成
			// bool（0/1）再进 FAILED —— 0 和 1 都不 < 0，条件恒 false，下面的 GetWindowRect
			// 兜底就成了死代码（DWM 失败的窗口会被直接过滤掉，而不是退回 GetWindowRect）
			const HRESULT frameHr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(RECT));
			if (FAILED(frameHr) || rect.right <= rect.left || rect.bottom <= rect.top) {
				if (!GetWindowRect(hwnd, &rect)) return TRUE;
			}
			if (rect.right - rect.left <= 6 || rect.bottom - rect.top <= 6) return TRUE;
			c->self->pushClippedRect(rect, rank, true, hwnd);  // 顶层窗口本身
			// 子控件记下所属顶层窗口：光标压在这个窗口上时，只有"属于它"的矩形才算数
			c->topHwnd = hwnd;
			EnumChildWindows(hwnd, collectChild, lparam);     // 再收集它里面的子控件
			return TRUE;
		}, reinterpret_cast<LPARAM>(&ctx));
}

void CutMask::pushClippedRect(const RECT& r, int winRank, bool topLevel, HWND hwnd)
{
	// 别名别叫 win：类里有个同名成员，遮蔽之后很容易改错对象
	auto host = this->win;
	int hostL{ (int)host->x }, hostT{ (int)host->y };
	int hostR{ (int)(host->x + host->w) }, hostB{ (int)(host->y + host->h) };
	if (r.right <= hostL || r.bottom <= hostT || r.left >= hostR || r.top >= hostB) return;
	RECT c = r;
	if (c.left < hostL) c.left = hostL;
	if (c.top < hostT) c.top = hostT;
	if (c.right > hostR) c.right = hostR;
	if (c.bottom > hostB) c.bottom = hostB;
	if (c.right - c.left <= 6 || c.bottom - c.top <= 6) return;
	SnapItem item;
	item.rc = D2D1::RectF(
		(float)(c.left - host->x), (float)(c.top - host->y),
		(float)(c.right - host->x), (float)(c.bottom - host->y));
	item.win = winRank;
	item.top = topLevel;
	item.hwnd = hwnd;
	winRect.push_back(item);
}

BOOL CALLBACK CutMask::collectChild(HWND hwnd, LPARAM lp)
{
	auto c = reinterpret_cast<EnumCtx*>(lp);
	if (hwnd && IsWindowVisible(hwnd)) {
		RECT r;
		if (GetWindowRect(hwnd, &r) && r.right - r.left > 6 && r.bottom - r.top > 6) {
			// 子控件继承父窗口的 z 序位次：它跟着父窗口一起被压住 / 一起露出来。
			// hwnd 记的是**父**顶层窗口（不是控件自己）：collectHover 靠它实现
			// "系统点名的那个窗口没进表、但它名下的子控件照样参选"
			c->self->pushClippedRect(r, c->rank - 1, false, c->topHwnd);
		}
		// 这里**不能**再递归 EnumChildWindows：EnumChildWindows 本身就是深度遍历，
		// 会把所有后代都回调一遍。以前每个子窗口又对自己的子树再枚举一次，
		// 同一个控件的矩形被重复加入识别表 N 次（N = 它在树里的深度）：
		//   · 识别表膨胀数倍，collectHover 每次都线性扫全表，拖动时掉帧；
		//   · 滚轮循环候选里同一块窗口出现好几遍，"滚一下就没了"；
		//   · 浏览器 / 资源管理器这种几百上千个控件的窗口，一次重枚举能卡住上百毫秒
	}
	return TRUE;
}

void CutMask::makeLayout()
{
	PROF_SCOPE("cm.layout");
	layout.Reset();
	// 尺寸标签只拼一行坐标信息（X/Y/R/B/W/H），不写窗口名、按钮名
	auto layoutStr = std::format(L"X:{} Y:{} R:{} B:{} W:{} H:{}",
		maskRect.left, maskRect.top, maskRect.right, maskRect.bottom,
		maskRect.right - maskRect.left, maskRect.bottom - maskRect.top);
	layout = ui::D2D::get()->makeTextLayout(layoutStr, 12 * win->dpi);
	if (!layout) return;
	DWRITE_TEXT_METRICS tm = {};
	layout->GetMetrics(&tm);
	layoutRect = D2D1::RectF(maskRect.left, maskRect.top - paddingMargin - tm.height - paddingMargin * 2, maskRect.left + tm.width + paddingMargin * 2, maskRect.top - paddingMargin);
	// 标签被顶出窗口上边时折回 maskRect 内部
	if (layoutRect.top < 0) {
		auto h = layoutRect.bottom - layoutRect.top;
		auto w = layoutRect.right - layoutRect.left;
		layoutRect.top = maskRect.top + paddingMargin / 2;
		layoutRect.bottom = layoutRect.top + h;
		layoutRect.left = maskRect.left + paddingMargin;
		layoutRect.right = layoutRect.left + w;
	}
	layout->SetMaxWidth(layoutRect.right - layoutRect.left);
	layout->SetMaxHeight(layoutRect.bottom - layoutRect.top);
}

void CutMask::startMakeRect(POINT pos)
{
	pressPos = pos;
	// 用户下笔了：悬停预览的"挂住"立刻作废，别让它的定时器在拖框期间到点
	if (holdUntil) {
		holdUntil = 0;
		if (win) win->killTimer(103);
	}
}

void CutMask::makeRect(POINT pos)
{
	// 用普通 LONG 局部变量，不要 structured binding：std::minmax 绑定出来的是 const 引用不能赋值
	//
	// 两个点都先夹进宿主客户区：拖着框往屏幕外拉时 WM_MOUSEMOVE 的坐标会跑到负值或
	// 超过窗口尺寸，不夹的话选区会超出底图范围 —— 后续按这个矩形去 BitBlt / 建 bitmap
	// 要么失败（截不到图），要么拿到一块越界的内存
	const LONG wMax = win->w > 0 ? (LONG)win->w : 0;
	const LONG hMax = win->h > 0 ? (LONG)win->h : 0;
	const LONG ax = std::clamp((LONG)pressPos.x, 0L, wMax);
	const LONG ay = std::clamp((LONG)pressPos.y, 0L, hMax);
	const LONG bx = std::clamp((LONG)pos.x, 0L, wMax);
	const LONG by = std::clamp((LONG)pos.y, 0L, hMax);
	LONG left = std::min(ax, bx);
	LONG right = std::max(ax, bx);
	LONG top = std::min(ay, by);
	LONG bottom = std::max(ay, by);
	// 手动拖框这里**一律不吸附**。
	// 以前拖框时四条边会先跳齐窗口/控件边界，再叠一层像素级边缘吸附——手按下去
	// 往前拉，框自己往旁边蹦，和"我在画一个我想要的框"这件事直接冲突：
	// 想框的边永远差那么几像素，还得反着躲吸力。
	// 分工是这样的：
	//   · 悬停智选（highlight）—— 鼠标没按下时的预览，负责"一键吸准"
	//   · 手动拖框（本函数）—— 用户已经自己画了，就不许改他的数
	//   · 拖边角（adjust）—— 选区落定之后的精修，同样完全跟随鼠标
	if (right - left < (LONG)minSize) right = left + (LONG)minSize;
	if (bottom - top < (LONG)minSize) bottom = top + (LONG)minSize;
	// 贴边按下时上面那次"补到最小尺寸"会把对边推出客户区，反着再夹回来
	if (right > wMax) { right = wMax; left = std::max(0L, right - (LONG)minSize); }
	if (bottom > hMax) { bottom = hMax; top = std::max(0L, bottom - (LONG)minSize); }
	maskRect.left = (float)left;
	maskRect.right = (float)right;
	maskRect.top = (float)top;
	maskRect.bottom = (float)bottom;
	makeLayout();
	win->requestPaint();
}

bool CutMask::hasRect() const
{
	return maskRect.right > maskRect.left && maskRect.bottom > maskRect.top;
}

void CutMask::reset()
{
	if (win) win->killTimer(103);
	holdUntil = 0;
	maskRect = D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	previewRect = D2D1::RectF(0.f, 0.f, 0.f, 0.f);
	layout.Reset();   // 尺寸标签跟旧选区一起清掉，paint 里 layout 为空就直接不画
	hoverCands.clear();
	hoverIdx = 0;
	win->requestPaint();
}

MaskHit CutMask::hitTest(POINT pos) const
{
	if (!hasRect()) return MaskHit::None;
	const auto& r = maskRect;
	const float px = (float)pos.x, py = (float)pos.y;
	// 选区的四条边各自延长，把整个窗口切成九块：中间那块就是选区自己，
	// 落在里面一律整体拖动；外面八块各对应一个方位，落在哪块就调哪条边/哪个角
	const int col = px < r.left ? 0 : (px > r.right ? 2 : 1);
	const int row = py < r.top ? 0 : (py > r.bottom ? 2 : 1);
	static constexpr MaskHit grid[3][3]{
		{ MaskHit::TopLeft,    MaskHit::Top,    MaskHit::TopRight },
		{ MaskHit::Left,       MaskHit::Inside, MaskHit::Right },
		{ MaskHit::BottomLeft, MaskHit::Bottom, MaskHit::BottomRight },
	};
	return grid[row][col];
}

MaskHit CutMask::hitEdge(POINT pos, float band) const
{
	if (!hasRect()) return MaskHit::None;
	const auto& r = maskRect;
	const float px = (float)pos.x, py = (float)pos.y;
	// 角优先：另外两条边的"带"会在这片区域重叠，先判角才不会把角判成边
	const bool nearL = std::fabs(px - r.left) <= band;
	const bool nearR = std::fabs(px - r.right) <= band;
	const bool nearT = std::fabs(py - r.top) <= band;
	const bool nearB = std::fabs(py - r.bottom) <= band;
	const bool inX = px >= r.left - band && px <= r.right + band;
	const bool inY = py >= r.top - band && py <= r.bottom + band;
	if (nearT && nearL) return MaskHit::TopLeft;
	if (nearT && nearR) return MaskHit::TopRight;
	if (nearB && nearL) return MaskHit::BottomLeft;
	if (nearB && nearR) return MaskHit::BottomRight;
	if (nearT && inX) return MaskHit::Top;
	if (nearB && inX) return MaskHit::Bottom;
	if (nearL && inY) return MaskHit::Left;
	if (nearR && inY) return MaskHit::Right;
	if (px > r.left && px < r.right && py > r.top && py < r.bottom) return MaskHit::Inside;
	return MaskHit::None;
}

void CutMask::startAdjust(POINT pos)
{
	startAdjust(pos, hitTest(pos));
}

void CutMask::startAdjust(POINT pos, MaskHit hit)
{
	adjustHit = hit;
	adjustStartRect = maskRect;
	adjustPressPos = pos;
	// 抓边或抓角：按下这一下就把对应的边挪到光标位置。
	// 内部整体拖动不能这么干 —— 那会让选区瞬间跳到光标为中心的位置。
	if (adjustHit != MaskHit::None && adjustHit != MaskHit::Inside) {
		adjust(pos);
	}
}

void CutMask::adjust(POINT pos)
{
	if (adjustHit == MaskHit::None) return;
	auto r = adjustStartRect;
	const float px = (float)pos.x, py = (float)pos.y;
	if (adjustHit == MaskHit::Inside) {
		// 整体平移，尺寸不变，夹在宿主客户区内
		const float rw = std::min(r.right - r.left, win->w), rh = std::min(r.bottom - r.top, win->h);
		const float left = std::clamp(r.left + px - adjustPressPos.x, 0.f, win->w - rw);
		const float top = std::clamp(r.top + py - adjustPressPos.y, 0.f, win->h - rh);
		r = D2D1::RectF(left, top, left + rw, top + rh);
	}
	else {
		// 精修阶段一律纯手拖，不再贴边。
		// 吸附只属于"第一次选区"：悬停智选（highlight / 首次拖框）负责把框吸准；
		// 选区一旦落定，框的位置就是用户认可的结果 —— 这时候再抓边去贴窗口/控件/
		// 像素边界，拖动中那条边会突然跳一下，手在走框在蹦，屏幕上就是一串闪。
		// 所以这里直接取光标位置，只夹在宿主客户区内
		const float cx = std::clamp(px, 0.f, win->w);
		const float cy = std::clamp(py, 0.f, win->h);
		switch (adjustHit)
		{
		case MaskHit::Left: r.left = cx; break;
		case MaskHit::Right: r.right = cx; break;
		case MaskHit::Top: r.top = cy; break;
		case MaskHit::Bottom: r.bottom = cy; break;
		case MaskHit::TopLeft: r.left = cx; r.top = cy; break;
		case MaskHit::TopRight: r.right = cx; r.top = cy; break;
		case MaskHit::BottomRight: r.right = cx; r.bottom = cy; break;
		case MaskHit::BottomLeft: r.left = cx; r.bottom = cy; break;
		default: break;
		}
		// 拖过头（比如左边越过右边）时归一化，接下来就自然变成"在拖另一条边"
		const float l = std::min(r.left, r.right), rr = std::max(r.left, r.right);
		const float t = std::min(r.top, r.bottom), b = std::max(r.top, r.bottom);
		r = D2D1::RectF(l, t, rr, b);
		// 塌到最小尺寸以下时，把动着的那条边推回来，不动锚定的那条
		const bool moveLeft = adjustHit == MaskHit::Left || adjustHit == MaskHit::TopLeft || adjustHit == MaskHit::BottomLeft;
		const bool moveTop = adjustHit == MaskHit::Top || adjustHit == MaskHit::TopLeft || adjustHit == MaskHit::TopRight;
		if (r.right - r.left < minSize) {
			if (moveLeft) r.left = r.right - minSize;
			else r.right = r.left + minSize;
		}
		if (r.bottom - r.top < minSize) {
			if (moveTop) r.top = r.bottom - minSize;
			else r.bottom = r.top + minSize;
		}
	}
	if (r.left == maskRect.left && r.top == maskRect.top &&
		r.right == maskRect.right && r.bottom == maskRect.bottom) return;
	maskRect = r;
	makeLayout();
	win->requestPaint();
}

void CutMask::paint(ID2D1DeviceContext* ctx, bool withMask, bool withFrame)
{
	// 判断"要不要画"只看有没有选区，不看尺寸标签的 layout：
	// layout 是 makeTextLayout 失败（字体/字号异常）时的副产物，拿它当守卫会让
	// 整个压暗层和选区边框一起消失，选区还在却看不见
	if (!hasRect()) return;
	// 四块压暗层：这一趟是全屏 alpha 混合，最贵。烘焙进缓存位图时只画它，
	// 之后每帧只补边框和标签 —— 两边都画就会叠成两层暗
	if (withMask) {
		ctx->FillRectangle(D2D1::RectF(0.f, 0.f, win->w, maskRect.top), brushBg.Get());
		ctx->FillRectangle(D2D1::RectF(0.f, maskRect.bottom, win->w, win->h), brushBg.Get());
		ctx->FillRectangle(D2D1::RectF(0.f, maskRect.top, maskRect.left, maskRect.bottom), brushBg.Get());
		ctx->FillRectangle(D2D1::RectF(maskRect.right, maskRect.top, win->w, maskRect.bottom), brushBg.Get());
	}
	if (!withFrame) return;
	auto halfStrokeWidth{ strokeWidth / 2.f };
	ctx->DrawRectangle(D2D1::RectF(maskRect.left - halfStrokeWidth, maskRect.top - halfStrokeWidth, maskRect.right + halfStrokeWidth, maskRect.bottom + halfStrokeWidth), brushBorder.Get(), strokeWidth);
	if (!layout) return;
	ctx->FillRectangle(layoutRect, brushBg.Get());
	ctx->DrawTextLayout({ layoutRect.left+ paddingMargin, layoutRect.top+ paddingMargin }, layout.Get(), brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}
