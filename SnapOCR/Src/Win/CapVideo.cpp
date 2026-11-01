#include "pch.h"
#include <wrl/client.h>
#include "CapVideo.h"
#include "WinCap.h"
#include "CutMask.h"
#include "../App.h"
#include "../Setting.h"
#include "../Lang.h"
#include "../Util.h"

// VideoMp4.hpp / VideoGif.hpp 里用的是裸 ComPtr，本工程的 pch 没有这条 using，
// 在包含它们之前补上，头文件本身保持与源码一致（不改一行）。
using namespace Microsoft::WRL;
#include "VideoMp4.hpp"
#include "VideoGif.hpp"

namespace {
	// 录制失败以前是静默 return，用户那边就是"录完什么都没有"，连缓存文件都不生成。
	// 失败原因基本都在对方机器上（混合显卡笔记本把进程放在独显上跑，桌面复制就不成立），
	// 实测让用户在系统图形设置里给本程序选"节能"（把进程放回核显）能解决，
	// 所以不去区分错误码了，统一给这一句最有用的话。
	// 录制界面是 WS_EX_TOPMOST 的，弹框得跟上，不然会被压在后面看不见
	void showRecordError(const std::wstring& text, const std::wstring& title)
	{
		MessageBoxW(nullptr, text.data(), title.data(), MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
	}

	// 可能管着录制区域那块屏的 (适配器, 输出) 组合
	struct DupTarget
	{
		ComPtr<IDXGIAdapter1> ad;
		UINT output{ 0 };
		RECT mon{}; //这块屏在虚拟桌面里的位置
	};

	// 按 DXGI 枚举顺序收集所有报告了 rect 所在那块屏的 (适配器, 输出)。
	// 双显卡笔记本上默认适配器可能是独显而内屏挂在核显上，独显的 EnumOutputs(0) 直接失败；
	// 收成一个列表而不是直接挑一个，是因为两个适配器会同时报告同一块屏，
	// 而 DuplicateOutput 只在真正拥有这块屏的那个适配器上成立
	void collectDupTargets(const RECT& rect, std::vector<DupTarget>& targets)
	{
		HMONITOR hMon = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
		if (!hMon) return;
		ComPtr<IDXGIFactory1> factory;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf())) || !factory) return;
		for (UINT i = 0;; ++i) {
			ComPtr<IDXGIAdapter1> ad;
			if (FAILED(factory->EnumAdapters1(i, ad.GetAddressOf())) || !ad) break;
			for (UINT j = 0;; ++j) {
				ComPtr<IDXGIOutput> output;
				if (FAILED(ad->EnumOutputs(j, output.GetAddressOf())) || !output) break;
				DXGI_OUTPUT_DESC desc{};
				if (FAILED(output->GetDesc(&desc)) || desc.Monitor != hMon) continue;
				targets.push_back({ ad, j, desc.DesktopCoordinates });
			}
		}
	}

	// 把桌面坐标的录制区域换算成某块屏的局部坐标 —— DesktopCapture 的裁剪是相对复制出来
	// 那张纹理算的，纹理原点是这块屏的左上角，不是虚拟桌面的原点。
	// 宽高对齐要求见 startMp4 里的注释，平移之后得重新对一遍
	RECT toLocalRect(const RECT& rect, const RECT& mon)
	{
		long l = (std::max)(rect.left, mon.left) - mon.left;
		long t = (std::max)(rect.top, mon.top) - mon.top;
		long r = (std::min)(rect.right, mon.right) - mon.left;
		long b = (std::min)(rect.bottom, mon.bottom) - mon.top;
		l = (l + 3) & ~3l;
		t = (t + 1) & ~1l;
		return { l, t, l + (std::max)(4l, (r - l) & ~3l), t + (std::max)(2l, (b - t) & ~1l) };
	}
}

CapVideo::CapVideo(WinCap* win, bool useMp4, bool useSpeaker, bool useMic)
	: win(win), useMp4(useMp4), useSpeaker(useSpeaker), useMic(useMic)
{
}

CapVideo::~CapVideo()
{
	discard();
}

bool CapVideo::start()
{
	if (started) return true;
	started = true;
	// std::jthread 构造要真起一个内核线程，系统线程池耗尽 / 内存告急时会抛
	// std::system_error。不接住就一路穿过 UI 线程的上层调用，直接把程序带走
	try {
		if (useMp4) startMp4();
		else startGif(useSpeaker, useMic);
	}
	catch (...) {
		// 线程没起来：退回未开始。stop() 仍能安全收尾 —— 它只会 join 一个
		// joinable 的线程，参数也会被 reset 掉
		started = false;
		return false;
	}
	// 线程对象建出来了才算真的开录（joinable 一直到 join/detach 才失效，
	// 线程自己跑完也仍然是 true）
	return captureThread.joinable();
}

void CapVideo::startMp4()
{
	auto& cutMask = win->cutMask;
	auto videoTempPath = Setting::get()->getDataPath();
	mp4Param = std::make_unique<VideoMp4::DESKTOPCAPTUREPARAMS>();
	// 编码格式不在这里定，交给下面采集线程里那个"HEVC 不行就退 H.264"的循环
	// 录制区域先夹回桌面范围，再做对齐 —— 只会往里缩，不会越出桌面。
	// HEVC 编码器要求宽高是偶数，链路中间的 RGB32->NV12 转换还会按对齐后的 stride
	// 去读我们交出去的缓冲区，宽度不是 4 的倍数时，有的驱动编码器会读到缓冲区外面，
	// 首帧就崩在驱动里。所以宽度对齐到 4、高度对齐到 2，代价是最多少录右侧 3 像素、底部 1 像素
	long rcLeft = (std::max)((long)win->x, (long)(win->x + cutMask->maskRect.left));
	long rcTop = (std::max)((long)win->y, (long)(win->y + cutMask->maskRect.top));
	long rcRight = (std::min)((long)(win->x + win->w), (long)(win->x + cutMask->maskRect.right));
	long rcBottom = (std::min)((long)(win->y + win->h), (long)(win->y + cutMask->maskRect.bottom));
	// 左上角往里取整，宽高往下取整，两头都不会超出上面夹好的范围
	rcLeft = (rcLeft + 3) & ~3l;
	rcTop = (rcTop + 1) & ~1l;
	const long rcW = (std::max)(4l, (rcRight - rcLeft) & ~3l);
	const long rcH = (std::max)(2l, (rcBottom - rcTop) & ~1l);
	mp4Param->rx = { rcLeft, rcTop, rcLeft + rcW, rcTop + rcH };
	mp4Param->f = (videoTempPath / L"temp.mp4").wstring();
	mp4Param->EndMS = 0;
	mp4Param->fps = 30;
	mp4Param->vbrm = 2;
	mp4Param->vbrq = 50;
	mp4Param->Qu = 50;
	mp4Param->MustEnd = false;
	VideoMp4::setAudio(mp4Param.get(), useSpeaker, useMic);
	// 文案在这里先取出来按值带进线程：Lang 对象不往采集线程里带
	auto errText = Lang::get(L"video.recordFailed");
	auto errTitle = Lang::get(L"about.sysTip");
	captureThread = std::jthread([this, errText, errTitle](std::stop_token st) {
		// 候选列表的 push_back 在内存告急时会抛 —— 异常穿出线程函数就是
		// std::terminate，整个进程直接带走。接住它按"录制失败"收尾
		try {
			HRESULT hr = MFStartup(MF_VERSION);
			if (FAILED(hr)) {
				showRecordError(errText, errTitle);
				return;
			}
			hr = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
			if (FAILED(hr)) {
				// MFStartup 已经成功，这条出口也得关掉，否则 MediaFoundation 的
				// 引用计数一直挂着，下次再录时初始化状态就不干净
				MFShutdown();
				showRecordError(errText, errTitle);
				return;
			}
			// 逐个候选试到桌面复制能成立为止。Prepare 挂得很早（缓存文件都还没建），
			// 所以换个候选整个重来最省事。targets 得活到 DesktopCapture 返回：
			// dp.ad 是裸指针，生命周期归我们管
			std::vector<DupTarget> targets;
			collectDupTargets(mp4Param->rx, targets);
			const RECT deskRx = mp4Param->rx; //每个候选都从桌面坐标重新换算
			int code = 0;
			// 编码格式也得按可用性退让。HEVC 文件小得多，但它的编码器不是每台机器都有 ——
			// 缺了的时候 AddStream 照样成功，要到 SetInputMediaType 解析编码器时才失败(-10)。
			// 退回 H.264：它从 Win7 起就带软件编码器
			for (auto codec : { MFVideoFormat_HEVC, MFVideoFormat_H264 }) {
				mp4Param->VIDEO_ENCODING_FORMAT = codec;
				if (targets.empty()) {
					mp4Param->rx = deskRx;
					code = VideoMp4::DesktopCapture(*(mp4Param.get())); //一个都没匹配上，走库原来的默认行为
				}
				for (auto& t : targets) {
					if (mp4Param->MustEnd) break; //已经点了停止，别再起新的
					mp4Param->ad = t.ad.Get();
					mp4Param->nOutput = t.output;
					mp4Param->rx = toLocalRect(deskRx, t.mon);
					code = VideoMp4::DesktopCapture(*(mp4Param.get()));
					if (code != -2) break; //成功了，或者失败在跟适配器无关的地方
				}
				if (mp4Param->MustEnd) break;
				// 只有这两个码是"这台机器认不了这个编码器"，换编码器才有意义
				if (code != -5 && code != -10) break;
			}
			if (code != 0) showRecordError(errText, errTitle);
			CoUninitialize();
			MFShutdown();
		}
		catch (...) {
			// 唯一能抛的是 collectDupTargets 的 push_back，走到那一步时 MF 和
			// COM 都已初始化成功 —— 出口同样要配对关掉，别把引用计数带挂
			CoUninitialize();
			MFShutdown();
			showRecordError(errText, errTitle);
		}
		});
}

void CapVideo::startGif(bool useSpeaker, bool useMic)
{
	// GIF 不带声音，两个开关对这条路径没有意义（上层在切到 GIF 时也会把它们关掉）
	(void)useSpeaker; (void)useMic;
	auto& cutMask = win->cutMask;
	auto videoTempPath = Setting::get()->getDataPath();
	gifParam = std::make_unique<VideoGif::GifParam>();
	gifParam->x = (int)(win->x + cutMask->maskRect.left);
	gifParam->y = (int)(win->y + cutMask->maskRect.top);
	gifParam->w = (int)(cutMask->maskRect.right - cutMask->maskRect.left);
	gifParam->h = (int)(cutMask->maskRect.bottom - cutMask->maskRect.top);
	gifParam->path = (videoTempPath / L"temp.gif").wstring();
	captureThread = std::jthread([this](std::stop_token st) {
		// createGif 里的缓冲分配（整屏 BGRA / 每帧 rgb 的 resize）在内存告急时会抛
		// bad_alloc —— 穿出线程函数就是 std::terminate。接住它：framesWritten 保持
		// 0，stop() 按空壳删文件，上层提示"没录到画面"，和 open 失败一条收尾路
		try {
			VideoGif::createGif(gifParam.get());
		}
		catch (...) {
		}
		});
}

std::wstring CapVideo::stop()
{
	if (!started && !mp4Param && !gifParam) return L"";
	started = false;
	std::wstring filePath;
	if (mp4Param) {
		mp4Param->MustEnd = true;
		filePath = mp4Param->f;
	}
	if (gifParam) {
		gifParam->isFinish = true;
		filePath = gifParam->path;
	}
	if (captureThread.joinable()) {
		captureThread.join();
	}
	// 刚开录就点了停止，一帧都没采到 —— 这时 Finalize 出来的是个 0 字节的空壳。
	// 别拿它去走存盘流程（那会存出来一个打不开的文件），直接删掉、回空路径。
	// gif 那条路同理：0 帧时 cgif_rgb_close 只写出一个文件头，同样是打不开的壳
	const bool noFrames = (mp4Param && mp4Param->framesWritten == 0)
		|| (gifParam && gifParam->framesWritten == 0);
	if (noFrames) {
		DeleteFileW(filePath.c_str());
		filePath.clear();
	}
	// 参数置空即"已停止"，再进来时不会重复 join
	mp4Param.reset();
	gifParam.reset();
	return filePath;
}

std::wstring CapVideo::stopAndSave(bool toClipboard, bool* copyFailed)
{
	if (copyFailed) *copyFailed = false;
	auto src = stop();
	if (src.empty()) return L"";
	// 直接落到设置页选定的视频目录（默认桌面），不弹另存为框
	auto dst = Util::savePathIn(Setting::get()->getVideoDir(), useMp4 ? L"mp4" : L"gif");
	if (dst.empty() || !CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
		// 复制不成功（目标盘满了 / 目录只读 / 被占用）不能顺手把源删掉 ——
		// 那是用户刚才录的全部内容，删了就彻底没了。留在数据目录里让他能自己找回
		// 调用方据此把提示从"没录到画面"换成"保存失败"：这两种失败的原因和后续
		// 动作完全不同（一个是白录了，一个是内容还在、只是没落到目标目录）
		if (copyFailed) *copyFailed = true;
		return L"";
	}
	DeleteFileW(src.c_str());
	if (toClipboard) Util::addFileToClipboard(dst);
	return dst;
}

void CapVideo::discard()
{
	auto src = stop();
	if (!src.empty()) DeleteFileW(src.c_str());
}
