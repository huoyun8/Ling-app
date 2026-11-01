#include "pch.h"
#include "OcrEngine.h"

#include <compressapi.h>   // 系统自带解压（LZMS / XPRESS-Huff），链接 Cabinet.lib

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ———————————————— 协议常量 ————————————————
// 帧头 20 字节：magic(4) + 版本(2) + 帧类型(2) + 请求号(8) + 载荷长度(4)，全部小端。
// 类型号是引擎那边的 ABI，别改；改了就等着 "OCR 未就绪"。
namespace
{
	constexpr uint16_t kProtoVer = 5;
	enum : uint16_t {
		KHello = 1, KReady = 2, KSubmit = 3, KComplete = 5, KShutdown = 6,
		KPrepare = 8, KSessionReady = 9,
		KAttach = 12, KBufferAttached = 13,
		KRecognize = 14, KImageConsumed = 15, KDetach = 16,
	};

	// 内嵌资源 id，与 Src/Res/Resource.rc 里的一一对应
	enum : int {
		ResProc = 1000, ResDet = 1001, ResRec = 1002, ResDict = 1003,
	};

	// 槽文件头：sequence(8) + state(4) + w(4) + h(4) + stride(4) + bytes(4) + magic(4) = 32
	constexpr uint32_t kSlotMagic = 0x544F4C53; // "SLOT"
	constexpr int kSlotHeader = 32;

	// 超时：首次 Prepare 要加载 30MB 模型，慢机器上 1~4 秒，给足 20 秒；
	// 单张图的识别（含检测 + 逐行 rec）实测 0.2~0.5 秒，30 秒是防呆上限
	constexpr DWORD kTimeoutSmall = 5000;
	constexpr DWORD kTimeoutPrepare = 20000;
	constexpr DWORD kTimeoutRecognize = 30000;

	constexpr unsigned long long kMaxPixels = 4096ull * 2304ull; // 超过这张图引擎会拒

	std::wstring exeDir()
	{
		wchar_t buf[MAX_PATH * 2]{};
		const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
		std::wstring p(buf, n);
		const size_t pos = p.find_last_of(L"\\/");
		return pos == std::wstring::npos ? std::wstring(L".") : p.substr(0, pos);
	}

	std::wstring joinPath(const std::wstring& dir, const std::wstring& name)
	{
		if (dir.empty()) return name;
		const wchar_t last = dir.back();
		return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
	}

	bool fileExists(const std::wstring& path)
	{
		const DWORD attr = GetFileAttributesW(path.c_str());
		return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
	}

	// 缓存目录：%LOCALAPPDATA%\<品牌>\ocr。取不到环境变量就落到 %TEMP%
	// 目录名按品牌分叉：两版各自的模型缓存互不覆盖，也方便单独清理
	std::wstring cacheDir()
	{
		wchar_t buf[MAX_PATH * 2]{};
		const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, (DWORD)std::size(buf));
		if (n > 0 && n < std::size(buf)) return joinPath(std::wstring(buf, n), SNAP_DIR_W L"\\ocr");
		const DWORD t = GetTempPathW((DWORD)std::size(buf), buf);
		if (t > 0) return joinPath(std::wstring(buf, t), SNAP_DIR_W L"_ocr");
		return SNAP_DIR_W L"_ocr";
	}

	bool writeFileBytes(const std::wstring& path, const void* data, size_t len)
	{
		HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;
		const BYTE* p = static_cast<const BYTE*>(data);
		size_t off = 0;
		bool ok = true;
		while (off < len) {
			const DWORD chunk = (DWORD)std::min<size_t>(len - off, 1u << 24);
			DWORD written = 0;
			if (!WriteFile(h, p + off, chunk, &written, nullptr) || written == 0) { ok = false; break; }
			off += written;
		}
		CloseHandle(h);
		return ok && off == len;
	}

	struct AssetPaths
	{
		std::wstring proc, det, rec, dict, dir;
		bool ok() const { return !proc.empty() && !det.empty() && !rec.empty() && !dict.empty(); }
	};

	AssetPaths pathsFromDir(const std::wstring& dir)
	{
		AssetPaths a;
		a.dir = dir;
		a.proc = joinPath(dir, L"ocr-process.exe");
		a.det = joinPath(dir, L"PP-OCRv6_det_small.onnx");
		a.rec = joinPath(dir, L"PP-OCRv6_rec_small.onnx");
		a.dict = joinPath(dir, L"ppocrv6_dict.txt");
		return a;
	}

	bool dirComplete(const AssetPaths& a)
	{
		return fileExists(a.proc) && fileExists(a.det) && fileExists(a.rec) && fileExists(a.dict);
	}

	// CreateDirectoryW 只建最后一级，%LOCALAPPDATA%\SnapCore 还不存在时直接建 ocr 会失败。
	// 首次运行（缓存目录整个不存在）走的正是这条路，所以必须逐级往上建
	bool ensureDir(const std::wstring& path)
	{
		if (path.empty()) return false;
		if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
		const size_t pos = path.find_last_of(L"\\/");
		if (pos != std::wstring::npos && pos > 2) ensureDir(path.substr(0, pos)); // pos>2 避开 "C:"
		CreateDirectoryW(path.c_str(), nullptr);
		const DWORD attr = GetFileAttributesW(path.c_str());
		return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
	}

	// 文件已存在且大小一致才算"释放过"。只看存在的话，上次写到一半被杀的残file
	// 会永久生效，之后每次识别都报模型损坏
	bool fileMatches(const std::wstring& path, DWORD expected)
	{
		WIN32_FILE_ATTRIBUTE_DATA fad{};
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
		if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
		return fad.nFileSizeHigh == 0 && fad.nFileSizeLow == expected;
	}

	// 内嵌资源是 SOCP 容器（tools/ocrcmp 压的）。格式：magic(4) + 算法(1) + 原始长度(4) + 载荷。
	// 算法 0=原样，1=XPRESS-HUFF，2=LZMS —— 都走系统 compressapi，不引入第三方解压库。
	// 认不出 magic 就当原样数据（兼容 SNAPCORE_OCR_PACK=0 时的裸资源）
	bool unpackResource(const void* data, DWORD size, std::vector<BYTE>& out)
	{
		const BYTE* p = static_cast<const BYTE*>(data);
		if (size < 9 || memcmp(p, "SOCP", 4) != 0) {
			out.assign(p, p + size);
			return true;
		}
		const BYTE algo = p[4];
		uint32_t orig = 0;
		memcpy(&orig, p + 5, 4);
		const BYTE* body = p + 9;
		const DWORD bodyLen = size - 9;
		if (algo == 0) { out.assign(body, body + bodyLen); return true; }
		if (orig == 0 || orig > (256u << 20)) return false;

		const DWORD algId = (algo == 2) ? COMPRESS_ALGORITHM_LZMS : COMPRESS_ALGORITHM_XPRESS_HUFF;
		DECOMPRESSOR_HANDLE dec = nullptr;
		if (!CreateDecompressor(algId, nullptr, &dec)) return false;
		out.resize(orig);
		SIZE_T got = 0;
		const BOOL ok = Decompress(dec, body, bodyLen, out.data(), out.size(), &got);
		CloseDecompressor(dec);
		if (!ok || got != orig) { out.clear(); return false; }
		return true;
	}

	// 把内嵌的四个文件写到 dir。返回是否全部落盘成功
	bool extractEmbedded(const std::wstring& dir)
	{
		if (!ensureDir(dir)) return false;

		struct Item { int id; const wchar_t* name; };
		// DirectML.dll 不在内嵌清单里（18.5MB，CPU 后端用不到）。它属于"可选加速"：
		// 用户把 dll 放进 ocr 目录，startSession 就会看到并切到 GPU 后端
		const Item items[] = {
			{ ResProc, L"ocr-process.exe" },
			{ ResDet,  L"PP-OCRv6_det_small.onnx" },
			{ ResRec,  L"PP-OCRv6_rec_small.onnx" },
			{ ResDict, L"ppocrv6_dict.txt" },
		};
		bool all = true;
		HINSTANCE inst = GetModuleHandleW(nullptr);
		std::vector<BYTE> plain;
		for (const auto& it : items) {
			const std::wstring path = joinPath(dir, it.name);
			HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(it.id), (LPCWSTR)RT_RCDATA);
			if (!res) { all = false; continue; }
			const DWORD size = SizeofResource(inst, res);
			if (size == 0) { all = false; continue; }

			HGLOBAL glob = LoadResource(inst, res);
			const void* data = glob ? LockResource(glob) : nullptr;
			if (!data) { all = false; continue; }
			plain.clear();
			if (!unpackResource(data, size, plain)) { all = false; continue; }
			if (fileMatches(path, (DWORD)plain.size())) continue;   // 已释放过且完整，跳过写盘

			// 先写 .part 再改名：中途被杀不会留下"看着存在、其实被截断"的模型
			const std::wstring tmp = path + L".part";
			if (!writeFileBytes(tmp, plain.data(), plain.size())) { all = false; continue; }
			MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
			// 引擎本体要给可执行权限（NTFS 上 CreateFile 写出来的默认不带 +x 也能起，
			// 但落到 FAT/网络盘时会失败，显式设一下更稳）
			if (it.id == ResProc) SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
		}
		return all;
	}

	// 找一套能用的模型。顺序：本地缓存 → exe 同目录 ocr\ → 释放内嵌资源再走缓存。
	// 缓存优先是刻意的：释放过一次之后就再也不碰那 66MB 资源了，启动零成本
	AssetPaths resolveAssets(bool& fromEmbedded)
	{
		fromEmbedded = false;
		const std::wstring cache = cacheDir();
		AssetPaths a = pathsFromDir(cache);
		if (dirComplete(a)) return a;

		const AssetPaths local = pathsFromDir(joinPath(exeDir(), L"ocr"));
		if (dirComplete(local)) return local;

		if (extractEmbedded(cache)) {
			fromEmbedded = true;
			AssetPaths b = pathsFromDir(cache);
			if (dirComplete(b)) return b;
		}
		return {};
	}

	std::wstring utf8ToWide(const std::string& s)
	{
		if (s.empty()) return {};
		const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
		if (n <= 0) return {};
		std::wstring out((size_t)n, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
		return out;
	}

	// ———————————————— 帧读写 ————————————————
	struct Frame
	{
		uint16_t kind{};
		uint64_t req{};
		std::vector<BYTE> payload;
	};

	void putU16(std::vector<BYTE>& b, uint16_t v) { b.push_back(BYTE(v & 0xFF)); b.push_back(BYTE(v >> 8)); }
	void putU32(std::vector<BYTE>& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(BYTE((v >> (8 * i)) & 0xFF)); }
	void putU64(std::vector<BYTE>& b, uint64_t v) { for (int i = 0; i < 8; i++) b.push_back(BYTE((v >> (8 * i)) & 0xFF)); }

	// 协议里的"字符串"= u32 长度 + UTF-8 字节（不含结尾 0）
	void putStr(std::vector<BYTE>& b, const std::wstring& s)
	{
		const int n = s.empty() ? 0 : WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
		std::string u8(n > 0 ? (size_t)n : 0, '\0');
		if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), u8.data(), n, nullptr, nullptr);
		putU32(b, (uint32_t)u8.size());
		b.insert(b.end(), u8.begin(), u8.end());
	}

	uint16_t getU16(const BYTE* p) { return uint16_t(p[0] | (p[1] << 8)); }
	uint32_t getU32(const BYTE* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
	uint64_t getU64(const BYTE* p) { return (uint64_t)getU32(p) | ((uint64_t)getU32(p + 4) << 32); }

	// ———————————————— 引擎进程 ————————————————
	// 两条管道 + 一个子进程。析构即收尸，所以上层不用管生命周期，异常路径也不会留下孤儿进程
	class EngineProc
	{
	public:
		~EngineProc() { close(); }

		bool start(const std::wstring& exe, const std::wstring& workDir)
		{
			SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };   // 可继承：子进程要拿到对端
			HANDLE inRd = nullptr, inWr = nullptr, outRd = nullptr, outWr = nullptr;
			if (!CreatePipe(&inRd, &inWr, &sa, 0)) return false;
			if (!CreatePipe(&outRd, &outWr, &sa, 0)) {
				CloseHandle(inRd); CloseHandle(inWr);
				return false;
			}
			// 父进程自己这两头不参与继承，免得以后起别的子进程时把它们漏下去
			SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
			SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);

			STARTUPINFOW si{};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
			si.wShowWindow = SW_HIDE;
			si.hStdInput = inRd;
			si.hStdOutput = outWr;
			si.hStdError = nullptr;

			PROCESS_INFORMATION pi{};
			std::wstring cmd = L"\"" + exe + L"\"";
			std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
			cmdBuf.push_back(0);
			// CREATE_NO_WINDOW：控制台程序，不抑制的话会闪一个黑框
			const BOOL ok = CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW, nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);

			// 子进程已经拿到自己的那两个句柄，父进程把"给子进程的那两头"关掉，
			// 否则读端永远等不到 EOF（管道还挂着自己一份）
			CloseHandle(inRd);
			CloseHandle(outWr);
			if (!ok) {
				CloseHandle(inWr);
				CloseHandle(outRd);
				return false;
			}
			hIn = inWr;
			hOut = outRd;
			hProc = pi.hProcess;
			if (pi.hThread) CloseHandle(pi.hThread);
			return true;
		}

		void close()
		{
			if (hIn) { CloseHandle(hIn); hIn = nullptr; }     // 关写端 = 给子进程发 EOF
			if (hProc) {
				// 先给它 1.5 秒自己退（DirectML 设备要在进程里正常释放，硬杀会留显存垃圾）
				if (WaitForSingleObject(hProc, 1500) != WAIT_OBJECT_0) {
					TerminateProcess(hProc, 0);
					WaitForSingleObject(hProc, 1000);
				}
				CloseHandle(hProc);
				hProc = nullptr;
			}
			if (hOut) { CloseHandle(hOut); hOut = nullptr; }
		}

		void kill()
		{
			if (hIn) { CloseHandle(hIn); hIn = nullptr; }
			if (hProc) {
				TerminateProcess(hProc, 1);
				WaitForSingleObject(hProc, 1000);
				CloseHandle(hProc);
				hProc = nullptr;
			}
			if (hOut) { CloseHandle(hOut); hOut = nullptr; }
		}

		bool alive() const { return hProc && WaitForSingleObject(hProc, 0) == WAIT_TIMEOUT; }

		bool writeFrame(uint16_t kind, uint64_t req, const std::vector<BYTE>& payload)
		{
			if (!hIn) return false;
			BYTE h[20]{};
			memcpy(h, "SOCR", 4);
			h[4] = BYTE(kProtoVer & 0xFF); h[5] = BYTE(kProtoVer >> 8);
			h[6] = BYTE(kind & 0xFF); h[7] = BYTE(kind >> 8);
			for (int i = 0; i < 8; i++) h[8 + i] = BYTE((req >> (8 * i)) & 0xFF);
			const uint32_t len = (uint32_t)payload.size();
			for (int i = 0; i < 4; i++) h[16 + i] = BYTE((len >> (8 * i)) & 0xFF);

			if (!writeAll(h, sizeof(h))) return false;
			return payload.empty() || writeAll(payload.data(), payload.size());
		}

		bool readFrame(Frame& out, DWORD timeoutMs)
		{
			BYTE h[20]{};
			if (!readAll(h, sizeof(h), timeoutMs)) return false;
			if (memcmp(h, "SOCR", 4) != 0) return false;
			out.kind = getU16(h + 6);
			out.req = getU64(h + 8);
			const uint32_t len = getU32(h + 16);
			out.payload.resize(len);
			if (len && !readAll(out.payload.data(), len, timeoutMs)) return false;
			return true;
		}

	private:
		bool writeAll(const void* data, size_t len)
		{
			const BYTE* p = static_cast<const BYTE*>(data);
			size_t off = 0;
			while (off < len) {
				DWORD written = 0;
				if (!WriteFile(hIn, p + off, (DWORD)(len - off), &written, nullptr) || written == 0) return false;
				off += written;
			}
			return true;
		}

		// 带超时的精确读：管道上的 ReadFile 是阻塞的，子进程卡死时会把调用线程一起拖住。
		// 用 PeekNamedPipe 轮询 + 进程存活检查，卡住就报错，上层收尸重来
		bool readAll(void* dst, size_t len, DWORD timeoutMs)
		{
			BYTE* p = static_cast<BYTE*>(dst);
			size_t got = 0;
			const ULONGLONG start = GetTickCount64();
			while (got < len) {
				DWORD avail = 0;
				if (!PeekNamedPipe(hOut, nullptr, 0, nullptr, &avail, nullptr)) return false;
				if (avail > 0) {
					DWORD want = (DWORD)std::min<size_t>(avail, len - got);
					DWORD read = 0;
					if (!ReadFile(hOut, p + got, want, &read, nullptr) || read == 0) return false;
					got += read;
					continue;
				}
				if (GetTickCount64() - start > timeoutMs) return false;
				if (hProc && WaitForSingleObject(hProc, 0) == WAIT_OBJECT_0) return false; // 进程没了
				Sleep(4);
			}
			return true;
		}

		HANDLE hIn{ nullptr };
		HANDLE hOut{ nullptr };
		HANDLE hProc{ nullptr };
	};

	// ———————————————— 常驻引擎 ————————————————
	// 引擎常驻省掉每次约 1~1.5 秒的模型加载；代价是进程占约 100MB。
	// 主界面空闲 3 分钟后由 WinQr 的定时器调 shutdown() 收掉（见 OcrIdleCheck）
	struct Session
	{
		EngineProc proc;
		bool ready{ false };
		std::wstring det, rec, dict;
	};
	std::mutex gLock;                       // 串行化整轮识别
	std::unique_ptr<Session> gSession;
	std::atomic<ULONGLONG> gLastUse{ 0 };

	bool startSession(Session& s, const AssetPaths& assets, std::wstring& err)
	{
		if (!s.proc.start(assets.proc, assets.dir)) { err = L"spawn"; return false; }

		// Hello → Ready
		std::vector<BYTE> hello;
		putStr(hello, L"");
		Frame f;
		if (!s.proc.writeFrame(KHello, 0, hello) || !s.proc.readFrame(f, kTimeoutSmall) || f.kind != KReady) {
			err = L"noResp";
			return false;
		}

		// Prepare → SessionReady：模型加载就发生在这一帧里（1~4 秒）
		// 后端：ocr 目录里有 DirectML.dll 就用 GPU（1），没有就走 CPU（0）。
		// 两条路跑的是同一份 fp32 模型，输出完全一致；实测短文本 CPU 反而略快，
		// 因为它省掉了 DML 会话的初始化（0.65s vs 1.2s 模型加载）
		const bool hasDml = fileExists(joinPath(assets.dir, L"DirectML.dll"));
		std::vector<BYTE> prep;
		prep.push_back(hasDml ? 1 : 0);
		prep.push_back(0);   // 检测输入尺寸策略：0 = max
		prep.push_back(0);   // 处理顺序：0 = 精度优先
		putStr(prep, assets.det);
		putStr(prep, assets.rec);
		putStr(prep, assets.dict);
		if (!s.proc.writeFrame(KPrepare, 1, prep) || !s.proc.readFrame(f, kTimeoutPrepare)
			|| f.kind != KSessionReady || f.payload.empty() || f.payload[0] != 1) {
			err = L"loadFail";
			return false;
		}
		s.ready = true;
		s.det = assets.det;
		s.rec = assets.rec;
		s.dict = assets.dict;
		return true;
	}

	// 单轮识别：Attach(槽文件) → Submit → Recognize → Detach。
	// 像素不进管道：写进临时槽文件，只把路径和长度告诉引擎，引擎 mmap 读
	bool recognizeRound(Session& s, const BYTE* bgra, int w, int h, std::wstring& text, std::wstring& err,
		std::vector<OcrLine>* lines)
	{
		const int stride = w * 4;
		const size_t size = (size_t)stride * (size_t)h;

		wchar_t tmp[MAX_PATH * 2]{};
		const DWORD tn = GetTempPathW((DWORD)std::size(tmp), tmp);
		std::wstring slot = std::wstring(tmp, tn);
		swprintf_s(tmp, L"snapcore_ocr_%lu_%llu.buf", GetCurrentProcessId(),
			(unsigned long long)GetTickCount64());
		slot += tmp;

		{
			HANDLE hf = CreateFileW(slot.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				FILE_ATTRIBUTE_TEMPORARY, nullptr);
			if (hf == INVALID_HANDLE_VALUE) { err = L"tmpfile"; return false; }
			BYTE head[kSlotHeader]{};
			const uint64_t seq = 1;
			memcpy(head, &seq, 8);
			const uint32_t state = 1, ww = (uint32_t)w, hh = (uint32_t)h, ss = (uint32_t)stride,
				bytes = (uint32_t)size, magic = kSlotMagic;
			memcpy(head + 8, &state, 4);
			memcpy(head + 12, &ww, 4);
			memcpy(head + 16, &hh, 4);
			memcpy(head + 20, &ss, 4);
			memcpy(head + 24, &bytes, 4);
			memcpy(head + 28, &magic, 4);
			DWORD written = 0;
			bool ok = WriteFile(hf, head, kSlotHeader, &written, nullptr) && written == kSlotHeader;
			ok = ok && WriteFile(hf, bgra, (DWORD)size, &written, nullptr) && written == size;
			CloseHandle(hf);
			if (!ok) { DeleteFileW(slot.c_str()); err = L"tmpfile"; return false; }
		}

		const auto cleanup = [&slot]() { DeleteFileW(slot.c_str()); };
		const uint64_t total = (uint64_t)kSlotHeader + (uint64_t)size;
		Frame f;

		// Attach：把槽文件挂给引擎
		std::vector<BYTE> pay;
		putStr(pay, slot);
		putU64(pay, total);
		if (!s.proc.writeFrame(KAttach, 2, pay) || !s.proc.readFrame(f, kTimeoutSmall) || f.kind != KBufferAttached) {
			cleanup(); err = L"buffer"; return false;
		}
		// Submit：告诉它这一帧的几何。generation 必须等于 Attach 的请求号
		pay.clear();
		putU64(pay, 2);
		putU32(pay, (uint32_t)w);
		putU32(pay, (uint32_t)h);
		putU32(pay, (uint32_t)stride);
		putU64(pay, 1);
		if (!s.proc.writeFrame(KSubmit, 3, pay) || !s.proc.readFrame(f, kTimeoutSmall) || f.kind != KImageConsumed) {
			cleanup(); err = L"image"; return false;
		}
		// Recognize → Complete
		std::vector<BYTE> none;
		if (!s.proc.writeFrame(KRecognize, 3, none) || !s.proc.readFrame(f, kTimeoutRecognize)) {
			cleanup(); err = L"timeout"; return false;
		}
		if (f.kind != KComplete || f.payload.empty() || f.payload[0] != 1) {
			cleanup(); err = L"failed"; return false;
		}

		// Complete 载荷：[ok:1] + 空字符串 + 行数 + 每行 { 文本, score:4, quad:32 }
		const BYTE* p = f.payload.data();
		size_t off = 1, len = f.payload.size();
		const auto readU32 = [&](uint32_t& v) -> bool {
			if (off + 4 > len) return false;
			v = getU32(p + off); off += 4; return true;
			};
		uint32_t skip = 0;
		if (readU32(skip)) off += skip;             // 那个空字符串
		uint32_t n = 0;
		if (!readU32(n)) { cleanup(); err = L"failed"; return false; }

		std::wstring out;
		for (uint32_t i = 0; i < n; i++) {
			uint32_t sl = 0;
			if (!readU32(sl) || off + sl + 36 > len) break;   // 36 = score(4) + quad(32)
			const std::wstring line = utf8ToWide(std::string((const char*)p + off, sl));
			// 紧接着的 4 字节是置信度，后面 32 字节是四个角点（8×f32）。
			// 旧代码 `off += sl + 36` 把这一整块跳过去了 —— 排版要用的坐标正好在这儿
			float score = 0.f;
			float quad[8]{};
			memcpy(&score, p + off + sl, 4);
			memcpy(quad, p + off + sl + 4, 32);
			off += sl + 36;
			if (!out.empty()) out += L"\r\n";   // 多行 EDIT 只认 \r\n，单独 \n 会连成一段
			out += line;
			if (lines) {
				OcrLine l;
				l.text = line;
				l.score = score;
				// 四点顺序不做假设，直接取包围盒 —— 排版只需要外接矩形
				l.x1 = l.x2 = quad[0];
				l.y1 = l.y2 = quad[1];
				for (int k = 1; k < 4; k++) {
					const float x = quad[k * 2], y = quad[k * 2 + 1];
					if (x < l.x1) l.x1 = x;
					if (x > l.x2) l.x2 = x;
					if (y < l.y1) l.y1 = y;
					if (y > l.y2) l.y2 = y;
				}
				lines->push_back(std::move(l));
			}
		}
		cleanup();

		// Detach：释放这一轮的槽位（失败不影响结果，下一轮会重新 Attach）
		s.proc.writeFrame(KDetach, 2, none);
		s.proc.readFrame(f, kTimeoutSmall);

		if (out.empty()) { err = L"empty"; return false; }
		text = out;
		return true;
	}
}

// ———————————————— 对外接口 ————————————————

bool OcrEngine::available()
{
	static const bool ok = [] {
		bool embedded = false;
		const AssetPaths a = resolveAssets(embedded);
		return a.ok();
		}();
	return ok;
}

ULONGLONG OcrEngine::lastUseTick()
{
	return gLastUse.load();
}

void OcrEngine::shutdown()
{
	std::lock_guard<std::mutex> guard(gLock);
	if (gSession) {
		gSession->proc.close();
		gSession.reset();
	}
}

bool OcrEngine::recognize(const BYTE* bgra, int w, int h, std::wstring& text, std::wstring& err,
	std::vector<OcrLine>* lines)
{
	text.clear();
	err.clear();
	if (lines) lines->clear();
	if (!bgra || w <= 0 || h <= 0) { err = L"invalid"; return false; }
	if ((unsigned long long)w * (unsigned long long)h > kMaxPixels) { err = L"tooBig"; return false; }

	bool embedded = false;
	const AssetPaths assets = resolveAssets(embedded);
	if (!assets.ok()) { err = L"noModel"; return false; }

	std::lock_guard<std::mutex> guard(gLock);
	if (!gSession) {
		auto s = std::make_unique<Session>();
		if (!startSession(*s, assets, err)) return false;
		gSession = std::move(s);
	}

	bool ok = recognizeRound(*gSession, bgra, w, h, text, err, lines);
	if (!ok) {
		// 协议走坏说明这条管道已经不能再用（子进程挂了 / 帧错位），当场收掉，
		// 下次识别重新拉起，免得一直卡在坏状态里
		gSession->proc.kill();
		gSession.reset();
	}
	gLastUse.store(GetTickCount64());
	return ok;
}
