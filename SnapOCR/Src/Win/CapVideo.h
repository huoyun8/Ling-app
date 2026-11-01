#pragma once
#include <string>
#include <atomic>
#include <memory>
#include <thread>

// 录制参数只在 CapVideo.cpp 里构造，头文件用前向声明就够 ——
// VideoMp4.hpp 会拉进整套 MediaFoundation 头，不该让 WinCap / ToolSub 这些
// 只是引用 CapVideo 的编译单元都跟着吃一遍。
namespace VideoMp4 { struct DESKTOPCAPTUREPARAMS; }
namespace VideoGif { struct GifParam; }

class WinCap;
// 屏幕录制。区域已经由 WinCap 的选区定好了，所以它不是窗口：
// 只负责起编码线程、停止、把成品落到桌面。界面在副编辑器（ToolSub）上。
class CapVideo
{
public:
	CapVideo(WinCap* win, bool useMp4, bool useSpeaker, bool useMic);
	~CapVideo();
	// 起编码线程（立刻返回，真正采集在后台线程里跑）。
	// 返回 false = 线程没起来，这一轮等于没开录，调用方不要切到录制态
	bool start();
	// 停录并把成品直接存到桌面（不弹另存为框）；toClipboard 时顺带放进剪切板。
	// 返回落盘后的完整路径，空串表示一帧都没录到（刚开录就停了）
	std::wstring stopAndSave(bool toClipboard, bool* copyFailed = nullptr);
	// 停录并丢掉临时文件
	void discard();
private:
	void startMp4();
	void startGif(bool useSpeaker, bool useMic);
	// 停线程，返回临时文件路径（0 帧时删掉空壳并返回空串）
	std::wstring stop();
private:
	WinCap* win;
	bool useMp4{ true }, useSpeaker{ false }, useMic{ false };
	bool started{ false };
	std::unique_ptr<VideoMp4::DESKTOPCAPTUREPARAMS> mp4Param;
	std::unique_ptr<VideoGif::GifParam> gifParam;
	std::jthread captureThread;
};
