#pragma once
#include <Ui/Ui.h>

class App
{
	public:
		~App();
		static void init();
		// 消息循环退出后、ui::dispose 之前调用：单例和窗口对象里存着 WinRT / D2D 对象
		//（配置和语言是 JsonObject，贴图窗口攥着位图和画刷），等到进程退出后的静态析构时
		// CoUninitialize 早跑完了，析构里那句 Release 打到的是已经拆掉的对象 ——
		// 表现为读取访问权限冲突
		static void dispose();
	static App* get();
	void takeScreenShot(int x, int y, int w, int h,ID2D1Bitmap1** img);
		std::tuple<int, int, int, int> getScreenArea();
		// 切换窗口的捕获亲和性（exclude=true 摘出捕获 / false 恢复）。
		// 需要 Windows 10 2004（build 19041）以上；更老的系统返回 false，调用方据此降级。
		// 用途：截图时让主界面"留在屏幕上、抓屏却拿不到它"，避免藏窗口造成的闪跳
		static bool setCaptureAffinity(HWND hwnd, bool exclude);
		// 一次截图会话结束时把整屏 CPU 副本交还：缓冲区本来是为"下一次截图"复用的，
		// 常驻要占 8.3MB（1080p）/ 33MB（4K），而截完就回托盘待命是这个工具最常见的
		// 状态，让它常年占着不划算 —— 下次截图 resize 会重新要
		void releaseShotBuffer();
		// 空闲时把工作集还给系统：截图 / 录屏收尾后整屏位图与编码缓冲都已放掉，
		// 但堆和图形驱动的页还挂在工作集上 —— 任务管理器里看着像没释放
		void trimMemory();
		// 整屏 CPU 副本本身。截图窗建好后把它整个交给 CutMask 做智能选区的像素层
		// （move 过去，不再拷贝一份），之后 App 这边是空的
		std::vector<BYTE>& shotBuffer() { return shotBuf; }
	private:
		App();
		// takeScreenShot 的整屏 CPU 缓冲，复用不重新分配：1080p 是 8.3MB、4K 是 33MB，
		// 每次截图都新分配 + 首次 touch 的页错误要十几毫秒，全花在"点截图到出遮罩"
		// 的中间态上（用户看到的就是"闪一下"）。截图窗开关频繁，缓冲区常驻正合适
		std::vector<BYTE> shotBuf;
	};

