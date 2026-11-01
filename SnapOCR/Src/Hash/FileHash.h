#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// 文件校验：算法实现与界面完全无关，WinQr 只负责把结果摆到控件上。
// MD5 / SHA256 走 bcrypt（系统自带，不必打包实现），CRC32 自己算 ——
// CryptoAPI 的 CALG_CRC32 在部分 CSP 上不提供，而这张表只有 256 项。
//
// SHA1 已下线：2005 年起碰撞已可构造（2008 MD5 碰撞、2017 SHA-1 SHAttered），
// 完整性校验上早就不该用它；发行方现在给的基本都是 SHA-256，
// 所以这一格原地换成 SHA-256，界面上的勾选位置不动。

// 勾选了哪几种算法。位掩码，便于一路传到工作线程
enum HashAlgo : unsigned {
	HashMd5 = 1u << 0,
	HashSha256 = 1u << 1,   // 原来是 SHA1 的那一位
	HashCrc32 = 1u << 2,
};

struct FileDigest
{
	std::wstring name;      // 文件名（不含路径）
	std::wstring path;      // 完整路径
	unsigned long long size{ 0 };
	std::wstring version;   // 文件版本，没有版本资源时为空
	std::wstring modified;  // 修改时间，yyyy-MM-dd HH:mm:ss
	std::wstring md5;       // 32 位小写十六进制
	std::wstring sha256;    // 64 位（顶替原来的 sha1：40 位）
	std::wstring crc32;     // 8 位
};

// 增量摘要器：喂多少算多少，最后一次性取结果。
// 一个文件一只实例（各算法并排累加）
class HashCalc
{
public:
	explicit HashCalc(unsigned mask);
	~HashCalc();
	HashCalc(const HashCalc&) = delete;
	HashCalc& operator=(const HashCalc&) = delete;

	bool usable() const;
	bool update(const void* data, size_t bytes);
	// 收尾并取十六进制结果。未勾选的算法返回空串。只能调用一次
	void finish(std::wstring& md5, std::wstring& sha256, std::wstring& crc32);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

// 进度计数：工作线程写、UI 线程读，全是原子的，不用锁。
// fileDone/fileTotal 是当前这一个文件，allDone/allTotal 是整批文件
struct HashProgress
{
	std::atomic<unsigned long long> fileDone{ 0 };
	std::atomic<unsigned long long> fileTotal{ 0 };
	std::atomic<unsigned long long> allDone{ 0 };
	std::atomic<unsigned long long> allTotal{ 0 };
};

// 算一个文件。prog 非空时按块累加进度；tick 每读完一块调一次，线程那边自己做限流，
// 免得每 1MB 就往 UI 线程 Post 一条消息。
// 返回 false 表示读失败或被取消，此时 out 里的内容不完整
bool hashFile(const std::wstring& path, unsigned mask, FileDigest& out,
	const std::atomic<bool>& cancel,
	HashProgress* prog = nullptr, const std::function<void()>& tick = {});

// 1234567 -> "1,234,567"
std::wstring groupDigits(unsigned long long v);
