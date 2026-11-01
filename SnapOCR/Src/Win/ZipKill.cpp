#include "pch.h"
#include "ZipKill.h"
#include <tlhelp32.h>
#include <set>
#include <map>
#include <vector>
#include <atomic>
#include <thread>

namespace {

	// 认得的压缩软件主程序（全部小写，不带路径）。
	// 只收"有图形界面"的主程序：7z.exe / rar.exe 这类命令行工具常被别的软件当子进程调用，
	// 关掉它们等于破坏别人的工作，一律不碰。
	const wchar_t* const kZips[] = {
		L"winrar.exe",
		L"7zfm.exe",
		L"7zg.exe",
		L"360zip.exe",
		L"360zipmini.exe",
		L"bandizip.exe",
		L"winzip32.exe",
		L"winzip64.exe",
		L"winzip.exe",
		L"haozip.exe",          // 好压
		L"2345zip.exe",         // 2345 好压
		L"kuaizip.exe",         // 快压
		L"peazip.exe",
		L"izarc.exe",
		L"winarchiver.exe",
		L"zipware.exe",
	};

	// 已经处理过的 PID：同一次开启期间不重复骚扰
	std::set<DWORD> handled;

	bool isZipExe(const wchar_t* name)
	{
		if (!name || !*name) return false;
		wchar_t lower[MAX_PATH]{};
		size_t i = 0;
		for (; name[i] && i < MAX_PATH - 1; i++) {
			wchar_t c = name[i];
			lower[i] = (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
		}
		lower[i] = 0;
		for (auto t : kZips) {
			if (wcscmp(lower, t) == 0) return true;
		}
		return false;
	}

	struct EnumCtx
	{
		DWORD pid{ 0 };
		int windows{ 0 };
	};

	BOOL CALLBACK closeWindowProc(HWND h, LPARAM lp)
	{
		auto ctx = reinterpret_cast<EnumCtx*>(lp);
		DWORD pid = 0;
		GetWindowThreadProcessId(h, &pid);
		if (pid != ctx->pid) return TRUE;
		// 只要有可见的顶层窗口就发关闭消息；隐藏的托盘图标窗口不算
		if (!IsWindowVisible(h)) return TRUE;
		if (GetWindow(h, GW_OWNER) && !(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_APPWINDOW)) return TRUE;
		PostMessageW(h, WM_CLOSE, 0, 0);
		ctx->windows++;
		return TRUE;
	}

	// 后台看护已经催过的窗口：同一个窗口短时间内不重复发 WM_CLOSE，
	// 否则用户点"取消"之后每一轮都会再被问一次
	std::set<HWND> guardHandled;
	// 窗口 -> 我们第一次看到它的时刻（GetTickCount64）。同进程多窗口时
	// 进程创建时间完全一样，只能靠这个判"谁是新打开的那个"
	std::map<HWND, ULONGLONG> firstSeen;

	// 是不是"值得管的"顶层窗口：可见、非工具窗
	bool isArchiveWindow(HWND h)
	{
		if (!IsWindowVisible(h)) return false;
		if (GetWindow(h, GW_OWNER) && !(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_APPWINDOW)) return false;
		return true;
	}

	// 从窗口标题取"压缩包标识"。标题长这样：
	//   "demo.zip - Bandizip" / "C:\a b\demo.zip - WinRAR"
	// 取最后一个 " - " 之前的部分。用最后一个而不是第一个：路径本身可能带 " - "，
	// 按第一个切会把路径切碎，两个不同的包反而被判成同一个
	std::wstring archiveIdentity(const std::wstring& title)
	{
		static const std::wstring sep = L" - ";
		const size_t p = title.rfind(sep);
		std::wstring id = (p == std::wstring::npos) ? title : title.substr(0, p);
		const size_t b = id.find_first_not_of(L" \t");
		const size_t e = id.find_last_not_of(L" \t");
		if (b == std::wstring::npos) return L"";
		return id.substr(b, e - b + 1);
	}

	// 现在有没有压缩软件在跑（用于决定后台看护的轮询间隔）
	bool hasZipRunning()
	{
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE) return false;
		bool found = false;
		PROCESSENTRY32W pe{};
		pe.dwSize = sizeof(pe);
		if (Process32FirstW(snap, &pe)) {
			do {
				if (isZipExe(pe.szExeFile)) { found = true; break; }
			} while (Process32NextW(snap, &pe));
		}
		CloseHandle(snap);
		return found;
	}

	std::atomic<bool> guardRun{ false };
	std::thread guardThread;
}

int ZipKill::closeAll()
{
	// 每次调用都是一次独立的"扫一遍"：先清掉上一轮记过的 PID。
	// 现在是点击按钮触发（不再是常驻开关），所以"上次没关掉的"这次要重试；
	// 而且 PID 会被系统复用，留着旧记录反而会误伤
	handled.clear();
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return 0;

	const DWORD self = GetCurrentProcessId();
	PROCESSENTRY32W pe{};
	pe.dwSize = sizeof(pe);
	int count = 0;
	std::set<DWORD> seen;

	if (Process32FirstW(snap, &pe)) {
		do {
			if (pe.th32ProcessID == 0 || pe.th32ProcessID == self) continue;
			if (!isZipExe(pe.szExeFile)) continue;
			if (handled.count(pe.th32ProcessID)) continue;
			if (!seen.insert(pe.th32ProcessID).second) continue;

			EnumCtx ctx{ pe.th32ProcessID, 0 };
			EnumWindows(closeWindowProc, reinterpret_cast<LPARAM>(&ctx));
			// 没有窗口的（后台预加载 / 命令行包装）不去动它
			if (ctx.windows == 0) continue;

			handled.insert(pe.th32ProcessID);
			count++;
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	return count;
}

// ———————————————— 后台防多开 ————————————————
//
// 语义：只处理"同一个压缩包被开出多个窗口"，不同压缩包各开一个时一律不动。
// 分组维度是「窗口」而不是「进程」—— Bandizip / NanaZip 这类是单进程多窗口，
// 按 PID 分组时永远只有 1 个元素，逻辑根本不触发（原版工具就栽在这）。
// 组键 = (exe 名, 标题里最后一个 " - " 之前那段)，同一组留最上面那个。
int ZipKill::dedupSameArchive()
{
	// 1. PID -> exe 名（小写）
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return 0;
	std::map<DWORD, std::wstring> zipPids;
	{
		const DWORD self = GetCurrentProcessId();
		PROCESSENTRY32W pe{};
		pe.dwSize = sizeof(pe);
		if (Process32FirstW(snap, &pe)) {
			do {
				if (pe.th32ProcessID == 0 || pe.th32ProcessID == self) continue;
				if (!isZipExe(pe.szExeFile)) continue;
				std::wstring n = pe.szExeFile;
				for (auto& c : n) c = (wchar_t)towlower(c);
				zipPids[pe.th32ProcessID] = n;
			} while (Process32NextW(snap, &pe));
		}
	}
	CloseHandle(snap);
	if (zipPids.empty()) return 0;

	// 2. 收压缩软件的可见顶层窗口，按 (exe, 包) 归组
	// 同时记住每个窗口"我们第一次看到它"的时刻，用来判新旧
	const ULONGLONG now = GetTickCount64();
	struct Item { HWND hwnd; std::wstring key; int z; };
	std::vector<Item> items;
	struct Ctx { const std::map<DWORD, std::wstring>* pids; std::vector<Item>* out; int z; };
	Ctx ctx{ &zipPids, &items, 0 };
	EnumWindows([](HWND h, LPARAM lp) -> BOOL {
		auto c = reinterpret_cast<Ctx*>(lp);
		DWORD pid = 0;
		GetWindowThreadProcessId(h, &pid);
		auto it = c->pids->find(pid);
		if (it == c->pids->end()) return TRUE;
		if (!isArchiveWindow(h)) return TRUE;
		wchar_t buf[512]{};
		if (GetWindowTextW(h, buf, 512) <= 0) return TRUE;
		const std::wstring id = archiveIdentity(buf);
		// 认不出是哪个包的（标题为空 / 只剩软件名）直接跳过：宁可不关，也不能误关。
		// 光判空不够：7-Zip、Bandizip、WinRAR 在没打开任何包时，主窗口标题就是
		// "7-Zip" / "Bandizip" 这种纯软件名，判空会把它当成"包名"。于是用户开了
		// 两个主界面（比如一个在下载、一个在解压），两个都归到 "7-Zip" 这一组，
		// 直接被关掉一个 —— 而他手上根本没有重复打开的包。
		// 压缩包的窗口标题一定带文件路径或扩展名，所以要求含 '.' 才认
		if (id.empty() || id.find(L'.') == std::wstring::npos) return TRUE;
		// z 从 0 起递增：EnumWindows 的顺序就是 z 序，越靠前越在上面
		c->out->push_back(Item{ h, it->second + L"|" + id, c->z++ });
		return TRUE;
	}, reinterpret_cast<LPARAM>(&ctx));
	if (items.size() < 2) return 0;

	// 窗口 -> 首次出现时刻。同一进程开多个窗口时，进程创建时间完全相同，
	// 只能靠"谁先出现在我们视野里"来判新旧（轮询 1 秒一轮，够用）
	std::map<HWND, ULONGLONG> seen;
	for (auto it = firstSeen.begin(); it != firstSeen.end();) {
		if (!IsWindow(it->first)) it = firstSeen.erase(it);
		else { seen[it->first] = it->second; ++it; }
	}
	for (const auto& it : items) {
		if (!seen.count(it.hwnd)) seen[it.hwnd] = now;
	}
	firstSeen = seen;

	// 3. 同组多于一个窗口：留"最新打开的那个"，其余发 WM_CLOSE
	// 新旧按 (进程创建时间, 首次出现时刻, z 序靠上) 依次比较 ——
	// 跨进程时创建时间说了算，同进程多窗口时退到首次出现时刻
	std::map<std::wstring, std::vector<const Item*>> groups;
	for (const auto& it : items) groups[it.key].push_back(&it);

	std::map<DWORD, ULONGLONG> ctCache;
	auto creatTime = [&ctCache](DWORD pid) -> ULONGLONG {
		auto it = ctCache.find(pid);
		if (it != ctCache.end()) return it->second;
		ULONGLONG v = 0;
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (h) {
			FILETIME c{}, e{}, k{}, u{};
			if (GetProcessTimes(h, &c, &e, &k, &u)) {
				v = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
			}
			CloseHandle(h);
		}
		ctCache[pid] = v;
		return v;
	};

	int closed = 0;
	std::set<HWND> alive;
	for (auto& g : groups) {
		if (g.second.size() < 2) continue;

		const Item* keep = g.second.front();
		ULONGLONG keepCt = creatTime([&] {
			DWORD pid = 0; GetWindowThreadProcessId(keep->hwnd, &pid); return pid; }());
		ULONGLONG keepSeen = seen[keep->hwnd];
		for (size_t i = 1; i < g.second.size(); i++) {
			const Item* c = g.second[i];
			DWORD pid = 0;
			GetWindowThreadProcessId(c->hwnd, &pid);
			const ULONGLONG ct = creatTime(pid);
			const ULONGLONG sn = seen[c->hwnd];
			const bool newer = (ct != keepCt) ? (ct > keepCt)
				: (sn != keepSeen) ? (sn > keepSeen)
				: (c->z < keep->z);   // 完全并列时留更靠上的那个
			if (newer) { keep = c; keepCt = ct; keepSeen = sn; }
		}

		for (const Item* c : g.second) {
			if (c == keep) continue;
			alive.insert(c->hwnd);
			if (guardHandled.count(c->hwnd)) continue;   // 已经催过一次，等它自己关
			PostMessageW(c->hwnd, WM_CLOSE, 0, 0);
			guardHandled.insert(c->hwnd);
			closed++;
		}
	}
	// 清掉已经不存在的记录：HWND 会被系统复用，留着旧记录会误判
	for (auto it = guardHandled.begin(); it != guardHandled.end();) {
		if (!alive.count(*it) || !IsWindow(*it)) it = guardHandled.erase(it);
		else ++it;
	}
	return closed;
}

void ZipKill::startGuard()
{
	bool expected = false;
	if (!guardRun.compare_exchange_strong(expected, true)) return;   // 已经在跑
	guardHandled.clear();
	firstSeen.clear();
	try {
		guardThread = std::thread([] {
			while (guardRun.load()) {
				const int n = dedupSameArchive();
				// 刚关过东西就勤一点，手上还有压缩软件在跑就是 1 秒，全空闲时退到 3 秒
				const int ms = n > 0 ? 600 : (hasZipRunning() ? 1000 : 3000);
				// 分片睡：stopGuard 时最多 100ms 就能醒，不必等满一个周期
				for (int waited = 0; waited < ms && guardRun.load(); waited += 100) {
					Sleep(100);
				}
			}
		});
	}
	catch (...) {
		// 线程创建失败（系统资源不足）：复位标志，否则 guardRun 永远 true，之后再也起不来
		guardRun = false;
	}
}

void ZipKill::stopGuard()
{
	if (!guardRun.exchange(false)) return;
	if (guardThread.joinable()) guardThread.join();
}
