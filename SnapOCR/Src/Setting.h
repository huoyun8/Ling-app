#pragma once
#include <Ui/Ui.h>
#include <filesystem>
#include <winrt/Windows.Data.Json.h>
using namespace winrt::Windows::Data::Json;

class Setting
{
public:
	static void init();
	// 必须在 CoUninitialize 之前调用：configObj 是 WinRT 对象，晚一步释放就是野内存
	static void dispose();
	static Setting* get();
	std::filesystem::path getDataPath();
	void setShortcutKey(const std::wstring& type, const std::vector<std::wstring>& keys);
	std::wstring getShortcutKey(const std::wstring& type);
	// 返回值 = 注册表实际写入结果。被组策略 / 安全软件拦下时返回 false 且不写配置：
	// 界面要是显示"已开启"而开机并不启动，比直接报错更误导
	bool setAutoStart(bool autoStart);
	bool getAutoStart();
	// 后台「压缩包防多开」是否开启：开着一发现同一个包被开出多个窗口就自动关掉多余的。
	// 默认开；想正常多开同一个包的用户可以在设置里关掉它
	void setZipGuard(bool on);
	bool getZipGuard();
	// 截图 / 识码前要不要把主界面收起来（默认开）。
	// 主界面压在选区里会被一起截进去，识码时还可能把自己的二维码认成结果；
	// 但也有人习惯让窗口留在原地，所以做成开关
	void setHideMainOnCap(bool on);
	bool getHideMainOnCap();
	// 点关闭时是收进托盘还是真退出（默认收托盘：定时关机倒计时要继续走，
	// 老用户也一直这个行为）。关掉之后点关闭 = 退出进程
	void setCloseToTray(bool on);
	bool getCloseToTray();
	// 是否限制成单实例（默认开）：勾上时第二次启动只是把已在跑的那个亮出来；
	// 关掉就可以同时开多个（各有一套托盘与热键，配置以最后一次写入为准）
	void setSingleInstance(bool on);
	bool getSingleInstance();
	// 启动时是否直接显示主界面（默认开）。关掉就维持老手感：双击只落托盘，
	// 再双击图标（第二实例转发）才出主界面。--auto-start / --enter=tray 这类
	// 明确只挂托盘的命令行模式不受它影响
	void setShowMainOnStart(bool on);
	bool getShowMainOnStart();
	// 保存位置（设置页可改）。空串 = 还没设过，按桌面处理 —— 目录到底能不能用
	// 由 Util::resolveSaveDir 判定（目录被删、盘拔了都回落到桌面）。
	// 截图与录屏分开存：两类文件的去处经常不一样
	std::wstring getShotDir();
	void setShotDir(const std::wstring& dir);
	std::wstring getVideoDir();
	void setVideoDir(const std::wstring& dir);
	// 文字识别页的排版档位：auto / line / flat / raw（取值见 Ocr/OcrLayout.h 的 Layout::Mode）。
	// 存在 common 层，读不到时返回空串，调用方自己回落 auto
	std::wstring getOcrLayout();
	void setOcrLayout(const std::wstring& mode);
	void initShortcutKeys();
	// 录入快捷键期间要把全部热键注销：录入时按下正在用的组合会直接触发截图，
	// 界面根本收不到按键。录完原样注册回来。
	// 返回值 = 恢复注册时是否全部成功（组合已被别的程序占用就会失败，调用方据此提示）
	bool setHotkeysEnabled(bool enabled);
	// 是否允许此刻注册热键（录入快捷键期间为 false）。设置窗改完组合要按这个状态决定要不要立刻注册
	bool hotkeysEnabled() const;
	// 贴图窗口子工具栏（ToolSub）的状态。每个工具在 config.json 的 toolPin 下各占一组，
	// 组名就是 ToolMain 上的按钮 id（rect / ellipse / ... / eraser），键名由调用方给
	// （fill、width、colorIndex 之类，各工具语义不同）。
	// 取不到就返回 def —— 老版本的配置文件里没有这些键，用户手工改坏了也算取不到，都不该抛异常。
	// set 一律立即落盘：用户调一次工具状态就得记住一次。
	bool getToolFlag(const std::wstring& tool, const std::wstring& key, bool def);
	void setToolFlag(const std::wstring& tool, const std::wstring& key, bool val);
	float getToolNum(const std::wstring& tool, const std::wstring& key, float def);
	void setToolNum(const std::wstring& tool, const std::wstring& key, float val);
private:
	Setting();
	// toolPin.<tool> 那个 JsonObject。缺哪一层就现建一层挂上去 ——
	// SetNamedValue 得有个落脚的对象，而这两层在旧配置文件里都不存在
	JsonObject getToolObj(const std::wstring& tool);
	// common 那一层的读写。bool 版语义与 string 版一致：common 层不存在时
	// getter 一律返回 false（与历史行为逐条一致），setter 先把层现建出来
	std::wstring getCommonString(const wchar_t* key);
	void setCommonString(const wchar_t* key, const std::wstring& val);
	bool getCommonBool(const wchar_t* key, bool def);
	void setCommonBool(const wchar_t* key, bool val);
	std::filesystem::path initDataPath();
	// 决定配置文件用哪一份：exe 同目录有 config.json 就用它（绿色版，配置跟着程序走），
	// 否则用 %appdata%\SnapCore\config.json。二者只认一个，读哪儿就写哪儿。
	std::filesystem::path initConfigPath();
	void save();
private:
	const std::filesystem::path dataPath;
	// 必须声明在 dataPath 之后：initConfigPath 找不到 exe 同目录的配置时要回落到 dataPath 上，
	// 成员按声明顺序初始化
	const std::filesystem::path configPath;
	JsonObject configObj;
	bool hotkeysOn{ true };
};

