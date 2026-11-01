#pragma once
#include <Ui/Ui.h>
class WinHost;

// 八夹点矩形这一次拖动动的是哪几条边。矩形、椭圆、马赛克选框、橡皮选框
// 四个类的按下/拖动/夹点摆位逐字相同，逻辑收在这里；各家的额外动作（最小尺寸、
// 椭圆的中心半径、马赛克的位图作废）仍留在自己那儿，按这个返回值分派
enum class RectDrag
{
	None,        // 这一档不是矩形操作（比如笔画的端点）
	Corner,      // 四角：两条边一起动，可以拉出反向框
	Vertical,    // 上下边：只动 top/bottom
	Horizontal,  // 左右边：只动 left/right
	Move,        // 整体平移
};

class ShapeBase
{
public:
	ShapeBase(WinHost* win);
	virtual ~ShapeBase();
	virtual void paint(ID2D1DeviceContext* ctx) = 0;
	virtual void paintDragger(ID2D1DeviceContext* ctx) {};
	virtual void mouseMove(const float x, const float y) { };
	virtual void mouseDrag(const float x, const float y) {};
	virtual void mouseDown(const float x, const float y) { };
	virtual void mouseUp(const float x, const float y) { };
	virtual void mouseWheel(const float x, const float y, const short delta) {};
	virtual void setCursor() {};
	// 新建这一笔如果只是按下马上弹起（没有拖动），默认当成什么也没画，元素直接丢掉。
	// 单击本身就是正常用法的元素（number 落徽章、text 进编辑）覆盖它返回 true
	virtual bool isValidWithoutDrag() { return false; };
	// 要不要把夹点（虚线框 + 手柄）画出来。默认"命中才算"。
	// ShapeText 会额外接受"框外一小圈 / 框顶到旋转柄那条走廊"这类没有落点的靠近，
	// 那些位置只用来显示，不参与点击（hoverDraggerIndex 仍是 -1），所以分开一个判定
	virtual bool showDragger() const { return hoverDraggerIndex >= 0; }
	// 工具条上的滑块 / 颜色 / 填充一变，就按当前样式重画自己。
	// 各 shape 的样式原来是"构造时从 ToolSub 抄一份快照"，画完再拖滑块就不动了 ——
	// 用户要的是"拖的时候就看着它变"，所以由宿主把这一下转给最后一个标注（见 onToolStyleChanged）
	virtual void applyStyle() {};
	// 取当前几何的外接矩形。矩形/椭圆会覆盖它 —— 工具条上点另一颗形状按钮时，
	// 要把旧的那只原地换成新的另一种形状，框得原样搬过去
	virtual bool bounds(D2D1_RECT_F& out) const { return false; }
	// 鼠标正压在"四角缩放柄"上。编辑中的文字要靠它拦住 WinCap 那一步让位
	// （编辑中落在文本框里的点击本来整个交给 TextBox，四角就按不动了）
	virtual bool onScaleHandle() const { return false; }
	bool isInRect(const D2D1_RECT_F rect, const float x, const float y);
public:
	WinHost* win;
	// 必须给初值：除了 History 的撤销/重做，没人给它赋过值，而 WinCap/WinPin 的 paint 循环
	// 一上来就读 !isUndo。不给初值的话这块内存是垃圾，新建的标注可能第一帧就画不出来
	bool isUndo{ false };
	int hoverDraggerIndex{ -1 };
protected:
	float draggerSize{ 6.f };
	// 矩形/椭圆等形状的最小尺寸钳制：宽高小于 2px 时按中心扩到 2px
	void enforceMinSize(D2D1_RECT_F& r);
	// —— 八夹点矩形的共用几何（矩形 / 椭圆 / 马赛克选框 / 橡皮选框） ——
	// 按下时记锚点：把"这一次拖动的固定那条边"抄进 pressX/pressY。
	// 0~3、4~7 是角/边，8 是整体平移；其余档位（含刚落笔的 -1）兜底按"锚点 = 按下点"
	static void rectPressAnchor(int idx, float x, float y, const D2D1_RECT_F& rect,
		float& pressX, float& pressY);
	// 拖动时按锚点和鼠标点重算 rect
	static RectDrag rectDragTo(int idx, float pressX, float pressY, float x, float y,
		D2D1_RECT_F& rect);
	// 八只夹点摆到四个角 + 四条边中点。out 由调用方给（各自的 draggers 数组，长度 8）
	void rectDraggersTo(const D2D1_RECT_F& rect, D2D1_RECT_F* out) const;
	// 折线笔画的命中：只有"边缘那一圈"算命中。粗笔画整片都能拖的话，
	// 就没法在已有笔画上面再画一笔了 —— 鼠标一按下会变成拖动旧元素
	bool hitStrokeEdge(const std::vector<D2D1_POINT_2F>& pts, float strokeWidth,
		float x, float y) const;
	// 夹点命中：0~count-1 依次试，首个命中返回它的序号，都不中返回 -1。
	// count 由调用方给（手绘类 draggers 有 8 格但只用前 2 个）
	int draggerHit(const std::vector<D2D1_RECT_F>& draggers, int count, float x, float y);
	// 八夹点档位 → 光标：0/4 对角、1/5 纵向、2/6 反对角、3/7 横向、8 平移
	void draggerCursor(int idx) const;
	// 手绘类共用的首/尾两颗端点夹点摆到折线两端
	void endDraggersTo(const std::vector<D2D1_POINT_2F>& pts, D2D1_RECT_F* out) const;
	Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDragger;
private:
};

