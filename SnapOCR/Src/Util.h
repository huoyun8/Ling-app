#pragma once
#include <Ui/Ui.h>
#include <fstream>
#include <array>
#include <vector>

// 图像输出相关的工具函数。data 一律要求 BGRA、top-down、行紧凑（步长 = w*4），
// 这也是 WinPin::getImagePixels 交出来的格式。
class Util
{
public:
	// 往剪切板放三份：CF_DIBV5（Office / 微信 / WPS 这类原生程序认）+ 24 位 CF_DIB
	//（最保守的老格式兜底）+ "PNG" 注册格式（浏览器 / Electron 程序认）。
	// 返回 false = 一份都没写进去
	static bool saveToClipboard(const int w, const int h, BYTE* data);
	static bool saveToFile(const std::wstring& path, const int w, const int h, BYTE* data);
	// 弹系统另存为对话框，返回空串表示用户取消
	static std::wstring getSaveFilePath(HWND hwnd, const std::wstring& ext = L"png");
	// 桌面目录（Known Folders）。取不到返回空串
	static std::wstring desktopDir();
	// 实际落盘用的目录：dir 为空（= 还没设过，默认桌面）、或者那个目录已经不在了
	// （被删 / 盘拔了），一律回落到桌面 —— 别把"保存"变成一次无声失败
	static std::wstring resolveSaveDir(const std::wstring& dir);
	// 目录能不能真的写进去（只读目录 / 写保护的 U 盘 / 断连的网络盘都过得了
	// FOS_PATHMUSTEXIST，但截图落盘会失败）。设置页选目录时用它当场拦下
	static bool isDirWritable(const std::wstring& dir);
	// 不弹对话框：直接给"目录\时间戳.ext"，目录走 resolveSaveDir。都取不到时返回空串
	static std::wstring savePathIn(const std::wstring& dir, const std::wstring& ext = L"png");
	// 不弹对话框：直接给"桌面\时间戳.ext"。取桌面失败时返回空串，调用方自行兜底
	static std::wstring desktopSavePath(const std::wstring& ext = L"png");
	// 弹系统"选择文件夹"对话框（从 initial 开始），返回空串表示用户取消
	static std::wstring pickFolder(HWND hwnd, const std::wstring& initial);
	// 以当前时间生成默认文件名，精确到毫秒，避免连续保存时重名
	static std::wstring createFileName(const std::wstring& ext);
	// 纯文本进剪切板（CF_UNICODETEXT）：文件校验页的"复制"走这条
	static bool copyText(const std::wstring& text);
	// 文本存文件，UTF-8 带 BOM —— 记事本 / VS Code 都能正确认出中文
	static bool saveTextFile(const std::wstring& path, const std::wstring& text);
	// 把磁盘上的一个文件放进剪切板（CF_HDROP）：资源管理器里可以直接 Ctrl+V 粘出来。
	// 录屏结束后"存剪切板"走这条 —— 剪切板持有的是路径，文件本身不能删
	static bool addFileToClipboard(const std::wstring& path);
};
