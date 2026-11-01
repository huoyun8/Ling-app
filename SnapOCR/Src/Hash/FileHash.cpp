#include "pch.h"
#include "FileHash.h"

#include <bcrypt.h>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {
	// —— CRC32（IEEE 802.3，多项式 0xEDB88320 反转形式）——
	struct Crc32Table
	{
		unsigned table[256]{};
		Crc32Table()
		{
			for (unsigned i = 0; i < 256; i++) {
				unsigned c = i;
				for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
				table[i] = c;
			}
		}
	};
	const Crc32Table& crcTable()
	{
		static const Crc32Table t;
		return t;
	}

	std::wstring toHex(const BYTE* data, DWORD bytes)
	{
		static const wchar_t* kHex = L"0123456789abcdef";
		std::wstring s;
		s.resize((size_t)bytes * 2);
		for (DWORD i = 0; i < bytes; i++) {
			s[i * 2] = kHex[data[i] >> 4];
			s[i * 2 + 1] = kHex[data[i] & 0x0F];
		}
		return s;
	}

	// 文件版本号，形如 "1.0.0.0"。没有版本资源返回空串。
	// 只有本文件的 hashFile 用（结果摆进校验记录），不进头文件
	std::wstring fileVersion(const std::wstring& path)
	{
		DWORD handle = 0;
		const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
		if (size == 0) return {};
		std::vector<BYTE> buf(size);
		if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return {};

		VS_FIXEDFILEINFO* info = nullptr;
		UINT len = 0;
		if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) {
			return {};
		}
		if (info->dwSignature != 0xFEEF04BDu) return {};
		wchar_t text[64]{};
		swprintf_s(text, L"%u.%u.%u.%u",
			HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
			HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
		return text;
	}

	// 本地修改时间，yyyy-MM-dd HH:mm:ss
	std::wstring fileModifiedTime(const std::wstring& path)
	{
		WIN32_FILE_ATTRIBUTE_DATA fad{};
		if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return {};
		FILETIME local{};
		if (!FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local)) return {};
		SYSTEMTIME st{};
		if (!FileTimeToSystemTime(&local, &st)) return {};
		wchar_t text[64]{};
		swprintf_s(text, L"%04u-%02u-%02u %02u:%02u:%02u",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		return text;
	}

	// bcrypt 的一个哈希上下文。构造失败（系统太老 / 算法不可用）时 usable() 为 false，
	// 调用方据此跳过这种算法，不会把整个校验拖垮
	struct BcryptHash
	{
		BCRYPT_ALG_HANDLE alg{ nullptr };
		BCRYPT_HASH_HANDLE hash{ nullptr };
		std::vector<BYTE> obj;
		std::vector<BYTE> digest;
		DWORD digestLen{ 0 };
		bool ready{ false };

		explicit BcryptHash(LPCWSTR id)
		{
			if (BCryptOpenAlgorithmProvider(&alg, id, nullptr, 0) < 0) return;
			DWORD objLen = 0, got = 0;
			if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
				reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &got, 0) < 0) return;
			if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
				reinterpret_cast<PUCHAR>(&digestLen), sizeof(digestLen), &got, 0) < 0) return;
			obj.resize(objLen);
			digest.resize(digestLen);
			if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) < 0) return;
			ready = true;
		}

		~BcryptHash()
		{
			if (hash) BCryptDestroyHash(hash);
			if (alg) BCryptCloseAlgorithmProvider(alg, 0);
		}

		bool update(const void* data, size_t bytes)
		{
			return ready && BCryptHashData(hash,
				static_cast<PUCHAR>(const_cast<void*>(data)), (ULONG)bytes, 0) >= 0;
		}

		std::wstring finishHex()
		{
			if (!ready) return {};
			if (BCryptFinishHash(hash, digest.data(), digestLen, 0) < 0) return {};
			ready = false;   // 收尾后句柄不再可用，重复调用直接返回空
			return toHex(digest.data(), digestLen);
		}
	};
}

struct HashCalc::Impl
{
	unsigned mask{ 0 };
	std::unique_ptr<BcryptHash> md5;
	std::unique_ptr<BcryptHash> sha256;
	unsigned crc{ 0xFFFFFFFFu };

	explicit Impl(unsigned m) : mask(m)
	{
		// 算法打不开（FIPS 模式下 MD5 就被系统禁用）就把这个实例丢掉：
		// 留着它会让 update() 每次都返回失败，而 finish() 又只能给出空串，
		// 结果是界面上一行空哈希 —— 宁可按"算不了"处理，也不要交出看起来算过的空值
		if (m & HashMd5) {
			md5 = std::make_unique<BcryptHash>(BCRYPT_MD5_ALGORITHM);
			if (!md5->ready) md5.reset();
		}
		if (m & HashSha256) {
			sha256 = std::make_unique<BcryptHash>(BCRYPT_SHA256_ALGORITHM);
			if (!sha256->ready) sha256.reset();
		}
	}
};

HashCalc::HashCalc(unsigned mask) : impl(std::make_unique<Impl>(mask)) {}
HashCalc::~HashCalc() = default;

bool HashCalc::usable() const
{
	if (!impl) return false;
	if (impl->md5 && impl->md5->ready) return true;
	if (impl->sha256 && impl->sha256->ready) return true;
	return (impl->mask & HashCrc32) != 0;
}

bool HashCalc::update(const void* data, size_t bytes)
{
	if (!impl || !data || bytes == 0) return true;
	bool ok = true;
	if (impl->md5) ok = impl->md5->update(data, bytes) && ok;
	if (impl->sha256) ok = impl->sha256->update(data, bytes) && ok;
	if (impl->mask & HashCrc32) {
		const auto& t = crcTable().table;
		const BYTE* p = static_cast<const BYTE*>(data);
		unsigned c = impl->crc;
		for (size_t i = 0; i < bytes; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
		impl->crc = c;
	}
	return ok;
}

void HashCalc::finish(std::wstring& md5, std::wstring& sha256, std::wstring& crc32)
{
	md5.clear();
	sha256.clear();
	crc32.clear();
	if (!impl) return;
	if (impl->md5) md5 = impl->md5->finishHex();
	if (impl->sha256) sha256 = impl->sha256->finishHex();
	if (impl->mask & HashCrc32) {
		wchar_t buf[16]{};
		// CRC32 的惯例是最终异或 0xFFFFFFFF 再输出；空文件（没喂过数据）也一样
		swprintf_s(buf, L"%08x", impl->crc ^ 0xFFFFFFFFu);
		crc32 = buf;
	}
}

std::wstring groupDigits(unsigned long long v)
{
	std::wstring digits = std::to_wstring(v);
	std::wstring out;
	out.reserve(digits.size() + digits.size() / 3);
	int count = 0;
	for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
		if (count && count % 3 == 0) out.push_back(L',');
		out.push_back(*it);
		count++;
	}
	return std::wstring(out.rbegin(), out.rend());
}

bool hashFile(const std::wstring& path, unsigned mask, FileDigest& out,
	const std::atomic<bool>& cancel,
	HashProgress* prog, const std::function<void()>& tick)
{
	out.path = path;
	const size_t slash = path.find_last_of(L"\\/");
	out.name = slash == std::wstring::npos ? path : path.substr(slash + 1);
	out.version = fileVersion(path);
	out.modified = fileModifiedTime(path);

	// 勾选的算法一个都跑不起来就直接失败，别开文件（开了还得记着关）
	HashCalc calc(mask);
	if (!calc.usable()) return false;

	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
		FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;

	LARGE_INTEGER size{};
	if (GetFileSizeEx(file, &size)) out.size = (unsigned long long)size.QuadPart;
	if (prog) {
		prog->fileDone = 0;
		prog->fileTotal = out.size;
	}

	std::vector<BYTE> buf(1 << 20);   // 1MB 一块：足够摊薄系统调用，又不至于占内存
	bool ok = true;
	unsigned long long done = 0;
	for (;;) {
		if (cancel) { ok = false; break; }
		DWORD got = 0;
		if (!ReadFile(file, buf.data(), (DWORD)buf.size(), &got, nullptr)) { ok = false; break; }
		if (got == 0) break;
		// 摘要算失败（BCryptHashData 出错）时不能当成功继续读：交出去的就是一个错值
		if (!calc.update(buf.data(), got)) { ok = false; break; }
		done += got;
		if (prog) {
			prog->fileDone = done;
			prog->allDone.fetch_add(got);
		}
		if (tick) tick();
	}
	CloseHandle(file);
	if (!ok) return false;

	calc.finish(out.md5, out.sha256, out.crc32);
	// 文件实际大小以读到的字节为准：GetFileSizeEx 在校验途中被改写时会不一致
	out.size = done;
	return true;
}
