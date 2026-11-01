#pragma once
#include <string>
#include <vector>
#include <windows.h>

#include "OcrLayout.h"

// 文字识别（OCR）驱动层。
//
// 识别本身不是本文件实现的：真正干活的是 PaddleOCR PP-OCRv6（检测 det + 识别 rec，
// 走 DirectML 推理）的独立进程 ocr-process.exe。这里只做四件事：
//   1) 找模型：内嵌资源（RCDATA）→ %LOCALAPPDATA%\SnapCore\ocr → exe 同目录 ocr\，
//      前两者取不到时把内嵌的那份释放到本地缓存；
//   2) 起进程：CreateProcess + 两条匿名管道（stdin/stdout），CREATE_NO_WINDOW；
//   3) 说协议：20 字节帧头（"SOCR" + 版本 + 帧类型 + 请求号 + 载荷长度）请求/应答；
//   4) 传像素：图像写进临时"槽文件"（32 字节头 + BGRA），只把路径和长度告诉子进程，
//      免得几十兆像素在管道里来回拷。
//
// 协议与 Rust 线（SnapCore/SnapOCR 的 F 版，main.rs：ocr_round / OcrEngine）完全一致 ——
// 引擎本身是同一个子进程，所以换语言重写驱动不影响它。
//
// 线程模型：recognize() 会阻塞（首次含约 1 秒模型加载），只能在后台线程调用；
// 内部用一把锁把整轮识别串起来，两个线程同时调不会把协议帧搅乱。
class OcrEngine
{
public:
	// 资源是否就绪。false = exe 里没内嵌、同目录也没有 ocr\，界面据此给出提示
	static bool available();
	// 同步识别一张图。bgra 为 top-down BGRA、行紧凑（stride = w*4），与 WinCap 抓出来的格式一致。
	// 成功返回 true 并把多行文本（\r\n 连接）写进 text；失败返回 false，原因写进 err（已是界面文案）
	// lines 非空时，额外把每行的文本/置信度/外接矩形按引擎原始顺序填进去，供排版解析用
	static bool recognize(const BYTE* bgra, int w, int h, std::wstring& text, std::wstring& err,
		std::vector<OcrLine>* lines = nullptr);
	// 关掉常驻的引擎进程（空闲超时 / 退出程序时调）。引擎不在时是空操作
	static void shutdown();
	// 上一次使用引擎的时刻（毫秒，GetTickCount64）。空闲回收用它判断
	static ULONGLONG lastUseTick();
};
