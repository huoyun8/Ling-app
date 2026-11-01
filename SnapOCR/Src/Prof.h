#pragma once
// 性能埋点（第 17 轮性能线专用）。
// 环境变量门控：SNAPCORE_PROF=1 时才计时，默认完全关闭 ——
// 关闭态下 PROF_SCOPE 只留下一个函数内 static 索引的读取 + 两次空分支，
// 热路径上约 1~2ns，可以常驻。
//
// 打开后：每积累 4096 个埋点段就往 %TEMP%\snapcore_prof.txt 追加一份快照
// （ append + 每份快照后清零，长时间跑也只会得到周期性切片 ）。
// 用法：
//   1. set SNAPCORE_PROF=1 && SnapCore.exe     （ Git Bash: SNAPCORE_PROF=1 ./SnapCore.exe ）
//   2. 照常截图、移动鼠标、滚轮、停悬停
//   3. 关掉截图窗（ Esc 或完成截图 ），读 %TEMP%\snapcore_prof.txt
#include <windows.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace prof {

struct Stat { const char* name; long long count; long long totalUs; long long maxUs; };

inline Stat* g_stats = nullptr;
inline int g_count = 0;
inline int g_cap = 0;
inline long long g_sinceDump = 0;
inline bool g_tried = false;
inline bool g_enabled = false;

inline bool enabled()
{
	if (!g_tried) {
		g_tried = true;
		const char* e = std::getenv("SNAPCORE_PROF");
		g_enabled = (e && e[0] == '1');
		if (g_enabled) {
			g_cap = 96;
			g_stats = static_cast<Stat*>(std::calloc(g_cap, sizeof(Stat)));
		}
	}
	return g_enabled && g_stats;
}

// 段名 -> 固定索引。只在启用时发生，调用次数有限（每个埋点一行代码），
// 线性扫 64~96 个名字足够快，且只在第一次执行到该埋点时跑一次
inline int regIndex(const char* name)
{
	for (int i = 0; i < g_count; ++i) {
		if (std::strcmp(g_stats[i].name, name) == 0) return i;
	}
	if (g_count < g_cap) {
		g_stats[g_count].name = name;
		return g_count++;
	}
	return -1;   // 段表满了：这个埋点静默失效，不影响主流程
}

inline long long nowUs()
{
	LARGE_INTEGER c{}, f{};
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	if (f.QuadPart <= 0) return 0;
	return c.QuadPart * 1000000LL / f.QuadPart;
}

// 累计清零前把当前快照追加进文件。文件名带绝对路径，避免调试时找不到
inline void dump()
{
	if (!enabled() || !g_count) return;
	char dir[MAX_PATH]{};
	const DWORD n = GetTempPathA(MAX_PATH, dir);
	if (n == 0 || n >= MAX_PATH) return;
	char path[MAX_PATH + 32]{};
	std::snprintf(path, sizeof(path), "%s%s", dir, "snapcore_prof.txt");
	FILE* f = nullptr;
	if (fopen_s(&f, path, "a") != 0 || !f) return;
	std::fprintf(f, "==== tick=%llu ====\n", (unsigned long long)GetTickCount64());
	std::fprintf(f, "%-22s %8s %12s %10s %10s\n", "section", "count", "total_ms", "avg_us", "max_us");
	for (int i = 0; i < g_count; ++i) {
		const auto& s = g_stats[i];
		if (!s.count) continue;
		std::fprintf(f, "%-22s %8lld %12.2f %10.1f %10lld\n", s.name, s.count,
			s.totalUs / 1000.0, (double)s.totalUs / s.count, s.maxUs);
	}
	std::fclose(f);
	for (int i = 0; i < g_count; ++i) { g_stats[i].count = 0; g_stats[i].totalUs = 0; g_stats[i].maxUs = 0; }
	g_sinceDump = 0;
}

// 会话级清零：一次截图会话开始时调，让文件和会话一一对应
inline void reset()
{
	if (!enabled()) return;
	for (int i = 0; i < g_count; ++i) { g_stats[i].count = 0; g_stats[i].totalUs = 0; g_stats[i].maxUs = 0; }
	g_sinceDump = 0;
}

struct Scope
{
	int idx;
	long long t0;
	explicit Scope(int i) : idx(i)
	{
		if (idx >= 0) t0 = nowUs();
	}
	~Scope()
	{
		if (idx < 0) return;
		const long long dt = nowUs() - t0;
		Stat& s = g_stats[idx];
		++s.count;
		s.totalUs += dt;
		if (dt > s.maxUs) s.maxUs = dt;
		if (++g_sinceDump >= 4096) dump();
	}
};

} // namespace prof

// 每个埋点一行代码。函数内 static 索引带一次性初始化守卫（只跑一次），
// 之后每次进入只是读一个 int；prof 关闭时 Scope 全程走空分支。
// 行号经两级宏展开再粘贴：`##` 旁边不展开宏是非标准预处理器（/Zc:preprocessor、clang-cl）
// 的行为，直接 _idx_##__LINE__ 在那边会全部变成同一个名字、同函数内即重定义
#define PROF_CAT2(a, b) a##b
#define PROF_CAT(a, b) PROF_CAT2(a, b)

#define PROF_SCOPE(name) \
	static int PROF_CAT(_profIdx_, __LINE__) = ::prof::enabled() ? ::prof::regIndex(name) : -1; \
	::prof::Scope PROF_CAT(_profSc_, __LINE__)(PROF_CAT(_profIdx_, __LINE__))

// 手动累加一个已知耗时（单位 μs）：给"已有自己的计时器、不想被 Scope 重复包"的场合
#define PROF_ADD(name, us) \
	do { \
		static int PROF_CAT(_profIdxA_, __LINE__) = ::prof::enabled() ? ::prof::regIndex(name) : -1; \
		if (PROF_CAT(_profIdxA_, __LINE__) >= 0) { \
			::prof::Stat& _s = ::prof::g_stats[PROF_CAT(_profIdxA_, __LINE__)]; \
			++_s.count; _s.totalUs += (us); if ((us) > _s.maxUs) _s.maxUs = (us); \
			if (++::prof::g_sinceDump >= 4096) ::prof::dump(); \
		} \
	} while (0)
