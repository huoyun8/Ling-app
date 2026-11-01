#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

// 改名页一行的状态。顺序即"越靠后越严重"，绘制时只按档位取色
enum RnStat {
	RnReady = 0,   // 可以改
	RnSame,        // 新旧同名，不用动
	RnBad,         // 名字非法（空 / 含 \ / : * ? " < > |）
	RnConflict,    // 目标文件已存在
	RnMissing,     // 源文件已经不在
	RnDone,        // 已经改成功
	RnFail,        // 改名失败（占用 / 只读 / 权限）
};

// 列表的一行：只存"盘上现在叫什么、规则算出来叫什么"，不缓存句柄
struct RnRow {
	std::wstring dir;      // 所在目录，带结尾 '\'
	std::wstring name;     // 当前文件名（含扩展名）
	std::wstring newName;  // 规则算出来的新文件名（含扩展名）
	std::wstring manualName; // 手工改过的完整文件名；非空时规则不再覆盖这一行
	unsigned long long size{ 0 };
	int status{ RnReady };
	bool checked{ true };
	bool done{ false };    // 这一行已经改成功（撤销后清掉）
};

// 改名页的列表。
//
// 为什么不用系统 ListView：主窗口是整幅双缓冲 + 每页整块重绘的自绘界面，
// 主题 ListView 混进来会带一圈凹陷边框和系统表头，跟别的页完全不是一个调子；
// 而这一页真正需要的只有"勾选 / 点格子改名 / 滚动"三件事，自己画反而更短。
// 行高、字号、列宽都在这里定死，父窗口只负责把 rows 指针交过来。
class RnList
{
public:
	static const wchar_t* className();
	// 建子窗口。失败返回空句柄
	HWND create(HWND parent, int id, int dpi);
	HWND hwnd() const { return wnd; }

	void setDpi(int value);
	// normal 画正文与表头，bold 只用于表头
	void setFonts(HFONT normal, HFONT bold);
	void setSource(std::vector<RnRow>* rows);
	// 空 = 全部格式；否则只显示扩展名（小写、不含点）落在表里的行。
	// 底部那排格式勾选是"多选"，所以这里是表而不是单个字符串
	void setFilterList(const std::vector<std::wstring>& extsLower);
	std::vector<std::wstring> filterList() const { return filt; }
	// 过滤框手打的关键字（已转小写）：非空时扩展名或文件名里包含它才算命中。
	// 和 setFilterList 是"且"的关系：先按格式筛，再按关键字筛
	void setKeyword(const std::wstring& lower);

	// 重算滚动条并整块重画。行数 / 状态变了就调它
	void refresh();
	// 只重画一行（勾选、编辑落定这类局部变化）
	void invalidateRow(int rowIndex);
	// 选中的行（rows 下标）。单击任意一格都先选中整行；双击「新文件名」才进编辑
	int selectedRow() const { return sel; }
	void selectRow(int rowIndex);
	// rows 下标 -> 可见序号；不在当前筛选里返回 -1
	int visibleIndex(int rowIndex) const;
	int visibleCount() const;

	// rows 下标 rowIndex 那一行第 col 列的矩形（客户区坐标）。
	// 行不在屏幕上时返回 false —— 调用方据此判断"这一格现在能不能编辑"
	bool cellRect(int rowIndex, int col, RECT& out) const;

	// 点了一格：rowIndex 是 rows 下标，col 0..5（0=勾选/序号 1=文件名 2=新文件名 …）
	std::function<void(int rowIndex, int col)> onClick;
	// 双击一格：只有「新文件名」列接它（原地改名字）
	std::function<void(int rowIndex, int col)> onDblClick;
	// 列宽被拖动过：父窗口那只"新文件名"编辑框还按老格子摆着，得收起来重摆
	std::function<void()> onResizeColumns;
	// 列表滚动了：页面上那只"新文件名"编辑框得跟着收起来，否则会浮在别的行上
	std::function<void()> onScroll;

private:
	static LRESULT CALLBACK procEntry(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT proc(UINT msg, WPARAM wp, LPARAM lp);
	void paint();
	void drawRow(HDC dc, const RECT& rowRect, int rowIndex, int displayIndex);
	int rowHeight() const { return scPx(22); }
	int headerHeight() const { return scPx(26); }
	int rowsPerPage() const;
	void clampTop();
	void syncScrollBar();
	// 窗口尺寸变了：重新记下客户区宽（列宽全靠它算）
	void onSize();
	// 命中：返回 rows 下标，col 由出参带回
	int hitTest(POINT pt, int* colOut) const;
	int scPx(int v) const { return MulDiv(v, dpi, 96); }
	int colLeft(int col) const;
	int colWidth(int col) const;
	// 每一列不许压到的最小宽（序号按位数算，其余按字宽算）
	int colMin(int col) const;
	// 每一列的最大宽（两列名字有上限，其余跟着客户区走）
	int colMax(int col) const;
	// 序号列宽度按行数伸缩（1/2/3 位数三档），省下的宽度退给两列名字
	int seqColWidth() const;
	// 表头列宽：分隔线命中（命中不到返回 -1，值为分隔线左侧那一列）
	int dividerAt(POINT pt) const;
	// 开始 / 进行 / 结束一次拖列宽。单位都是客户区 x
	void beginDrag(int div, int x);
	void dragTo(int x);
	void endDrag();
	// 这两列回到自动分配（双击分隔线）
	void resetDragCols(int div);

private:
	HWND wnd{ nullptr };
	int dpi{ 96 };
	// 表格可用宽（客户区宽，已经扣掉竖滚动条）。列宽全部从它算，
	// 于是"各列合计 = 客户区宽"永远成立，右边缘不会空一截
	int tableW{ 0 };
	HFONT fontNormal{ nullptr };
	HFONT fontBold{ nullptr };
	std::vector<RnRow>* rows{ nullptr };
	std::vector<std::wstring> filt;
	std::wstring keyw;
	bool matched(const RnRow& row) const;
	int top{ 0 };       // 第一个可见行的序号（可见序号，不是 rows 下标）
	int hot{ -1 };      // 悬停的 rows 下标
	int sel{ -1 };      // 选中的 rows 下标（整行高亮，-1 = 没选）
	bool tracking{ false };
	// —— 表头手拖列宽 ——
	// 0 = 还按默认规矩分配（序号跟位数走、两列名字平分剩余、状态列吃尾巴）；
	// 拖过一次就落成定值，之后窗口重画、列表重排都不再变回去。
	// 状态列（5）永远不写：它负责把剩余宽度吃干净，保证一行始终正好铺满
	int userW[6]{};
	int dragDiv{ -1 };    // 正在拖的分隔线（= 左侧列号）；-1 = 没在拖
	int dragStartX{ 0 };
	int dragStartL{ 0 };
	int dragStartR{ 0 };
	int hoverDiv{ -1 };   // 悬停的分隔线，用来把光标换成左右箭头 + 画一条亮线
};
