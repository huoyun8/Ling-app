#include "pch.h"
#include <wincodec.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <filesystem>
#include <format>
#include <fstream>
#include "Util.h"
#include "Lang.h"

using Microsoft::WRL::ComPtr;

namespace {
	// 把 BGRA top-down 像素编码成 PNG 写进 stream。saveToClipboard 和 saveToFile 共用这段。
	bool encodePng(IStream* stream, const int w, const int h, BYTE* data)
	{
		UINT rowBytes = (UINT)w * 4;
		UINT imgBytes = rowBytes * (UINT)h;
		ComPtr<IWICImagingFactory> factory;
		auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
		if (FAILED(hr)) return false;
		ComPtr<IWICBitmapEncoder> encoder;
		hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf());
		if (FAILED(hr)) return false;
		hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
		if (FAILED(hr)) return false;
		ComPtr<IWICBitmapFrameEncode> frame;
		hr = encoder->CreateNewFrame(frame.GetAddressOf(), nullptr);
		if (FAILED(hr)) return false;
		hr = frame->Initialize(nullptr);
		if (FAILED(hr)) return false;
		hr = frame->SetSize((UINT)w, (UINT)h);
		if (FAILED(hr)) return false;
		WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
		hr = frame->SetPixelFormat(&fmt);
		if (FAILED(hr) || !IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) return false;
		hr = frame->WritePixels((UINT)h, rowBytes, imgBytes, data);
		if (FAILED(hr)) return false;
		hr = frame->Commit();
		if (FAILED(hr)) return false;
		return SUCCEEDED(encoder->Commit());
	}
}

bool Util::saveToClipboard(const int w, const int h, BYTE* data)
{
	if (w <= 0 || h <= 0 || !data) return false;
	DWORD rowBytes = (DWORD)w * 4;
	DWORD imgBytes = rowBytes * (DWORD)h;

	// ---------- 1) PNG 编码到内存流 ----------
	ComPtr<IStream> pngStream;
	if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, pngStream.GetAddressOf()))) return false;
	if (!encodePng(pngStream.Get(), w, h, data)) return false;
	// 流内部的 HGLOBAL 尺寸可能大于实际字节数，拷一份精确大小的出来给剪切板
	STATSTG stat{};
	if (FAILED(pngStream->Stat(&stat, STATFLAG_NONAME))) return false;
	SIZE_T pngSize = (SIZE_T)stat.cbSize.QuadPart;
	if (pngSize == 0) return false;
	HGLOBAL hPngSrc{ nullptr };
	if (FAILED(GetHGlobalFromStream(pngStream.Get(), &hPngSrc)) || !hPngSrc) return false;
	auto srcPtr = GlobalLock(hPngSrc);
	if (!srcPtr) return false;
	HGLOBAL hPng = GlobalAlloc(GMEM_MOVEABLE, pngSize);
	if (!hPng) { GlobalUnlock(hPngSrc); return false; }
	auto dstPtr = GlobalLock(hPng);
	if (!dstPtr) { GlobalUnlock(hPngSrc); GlobalFree(hPng); return false; }
	CopyMemory(dstPtr, srcPtr, pngSize);
	GlobalUnlock(hPng);
	GlobalUnlock(hPngSrc);

	// ---------- 2) 构造 CF_DIBV5（带 alpha） ----------
	HGLOBAL hDibV5 = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + imgBytes);
	if (!hDibV5) { GlobalFree(hPng); return false; }
	auto pv5 = static_cast<BYTE*>(GlobalLock(hDibV5));
	if (!pv5) { GlobalFree(hDibV5); GlobalFree(hPng); return false; }
	auto bv5 = reinterpret_cast<BITMAPV5HEADER*>(pv5);
	*bv5 = {};
	bv5->bV5Size = sizeof(BITMAPV5HEADER);
	bv5->bV5Width = w;
	bv5->bV5Height = -h;                  // 负 = top-down
	bv5->bV5Planes = 1;
	bv5->bV5BitCount = 32;
	bv5->bV5Compression = BI_BITFIELDS;   // 让接收端识别 alpha
	bv5->bV5SizeImage = imgBytes;
	bv5->bV5RedMask = 0x00FF0000;
	bv5->bV5GreenMask = 0x0000FF00;
	bv5->bV5BlueMask = 0x000000FF;
	bv5->bV5AlphaMask = 0xFF000000;
	bv5->bV5CSType = LCS_sRGB;
	bv5->bV5Intent = LCS_GM_GRAPHICS;
	CopyMemory(pv5 + sizeof(BITMAPV5HEADER), data, imgBytes);
	GlobalUnlock(hDibV5);

	// ---------- 3) 构造 CF_DIB（24bpp、BI_RGB、自下而上） ----------
	// 老软件（比如 Illustrator 2020）只认最传统的这一种 DIB：注册格式 PNG 它不查，
	// CF_DIBV5 它不认，32bpp + BI_BITFIELDS 和 top-down 也读不了。系统虽然能从 CF_DIBV5
	// 合成出 CF_DIB，合成出来的仍是那份带 alpha 的 32 位数据，一样不合它的口味。
	// 所以显式再放一份最保守的：丢掉 alpha 写成 24 位，行按 4 字节对齐，自下而上排列
	DWORD dibRowBytes = ((DWORD)w * 3 + 3) & ~3u;
	DWORD dibImgBytes = dibRowBytes * (DWORD)h;
	HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + dibImgBytes);
	if (!hDib) { GlobalFree(hDibV5); GlobalFree(hPng); return false; }
	auto pDib = static_cast<BYTE*>(GlobalLock(hDib));
	if (!pDib) { GlobalFree(hDib); GlobalFree(hDibV5); GlobalFree(hPng); return false; }
	auto bi = reinterpret_cast<BITMAPINFOHEADER*>(pDib);
	*bi = {};
	bi->biSize = sizeof(BITMAPINFOHEADER);
	bi->biWidth = w;
	bi->biHeight = h;                     // 正 = 自下而上
	bi->biPlanes = 1;
	bi->biBitCount = 24;
	bi->biCompression = BI_RGB;
	bi->biSizeImage = dibImgBytes;
	auto dibPixels = pDib + sizeof(BITMAPINFOHEADER);
	for (int row = 0; row < h; row++) {
		auto src = data + (size_t)row * rowBytes;                 //入参是 top-down
		auto dst = dibPixels + (size_t)(h - 1 - row) * dibRowBytes;
		for (int col = 0; col < w; col++) {
			dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];     //BGRA -> BGR
			src += 4;
			dst += 3;
		}
	}
	GlobalUnlock(hDib);

	// ---------- 4) 写入剪切板 ----------
	if (!OpenClipboard(nullptr)) {
		GlobalFree(hDib);
		GlobalFree(hDibV5);
		GlobalFree(hPng);
		return false;
	}
	EmptyClipboard();
	// SetClipboardData 成功后 HGLOBAL 归剪切板所有，不能再 GlobalFree；失败了才要自己释放
	// 判定成功的口径用主格式 CF_DIBV5：剩下的 CF_DIB / PNG 都是给兼容性兜底的，
	// 主格式进去了就说明粘贴出来是张图
	bool ok{ false };
	if (SetClipboardData(CF_DIBV5, hDibV5)) ok = true;
	else GlobalFree(hDibV5);
	if (!SetClipboardData(CF_DIB, hDib)) {
		GlobalFree(hDib);
	}
	UINT cfPng = RegisterClipboardFormatW(L"PNG");
	if (cfPng == 0 || !SetClipboardData(cfPng, hPng)) {
		GlobalFree(hPng);
	}
	CloseClipboard();
	return ok;
}

bool Util::saveToFile(const std::wstring& path, const int w, const int h, BYTE* data)
{
	if (path.empty() || w <= 0 || h <= 0 || !data) return false;
	ComPtr<IWICImagingFactory> factory;
	auto hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
	if (FAILED(hr)) return false;
	ComPtr<IWICStream> stream;
	hr = factory->CreateStream(stream.GetAddressOf());
	if (FAILED(hr)) return false;
	hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
	if (FAILED(hr)) return false;
	return encodePng(stream.Get(), w, h, data);
}

std::wstring Util::getSaveFilePath(HWND hwnd, const std::wstring& ext)
{
	std::wstring result;
	ComPtr<IFileSaveDialog> saveDialog;
	auto hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(saveDialog.GetAddressOf()));
	if (FAILED(hr)) return result;
	DWORD dwFlags{ 0 };
	saveDialog->GetOptions(&dwFlags);
	saveDialog->SetOptions(dwFlags | FOS_OVERWRITEPROMPT | FOS_STRICTFILETYPES);
	auto pattern = L"*." + ext;
	auto typeName = Lang::get(L"util.file");
	COMDLG_FILTERSPEC filterSpec[]{ { typeName.c_str(), pattern.c_str() } };
	saveDialog->SetFileTypes(_countof(filterSpec), filterSpec);
	saveDialog->SetFileTypeIndex(1);
	saveDialog->SetDefaultExtension(ext.c_str());
	auto fileName = createFileName(ext);
	saveDialog->SetFileName(fileName.c_str());
	// 用户取消时 Show 返回 HRESULT_FROM_WIN32(ERROR_CANCELLED)，一样走 FAILED 分支
	hr = saveDialog->Show(hwnd);
	if (FAILED(hr)) return result;
	ComPtr<IShellItem> item;
	hr = saveDialog->GetResult(item.GetAddressOf());
	if (FAILED(hr)) return result;
	PWSTR filePath{ nullptr };
	hr = item->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
	if (FAILED(hr)) return result;
	result = filePath;
	CoTaskMemFree(filePath);
	return result;
}

std::wstring Util::desktopDir()
{
	PWSTR raw{ nullptr };
	// 桌面跟随资源重定向/漫游，必须走 Known Folders API，不能拼 %USERPROFILE%
	if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &raw))) return L"";
	std::wstring dir = raw;
	CoTaskMemFree(raw);
	return dir;
}

std::wstring Util::resolveSaveDir(const std::wstring& dir)
{
	std::error_code ec;
	if (!dir.empty() && std::filesystem::is_directory(dir, ec)) return dir;
	return desktopDir();
}

bool Util::isDirWritable(const std::wstring& dir)
{
	// 目录存在不等于能写。实测法：就地建一个临时文件再删掉，不猜权限位 ——
	// 只读目录、写保护的 U 盘、断连的网络盘都能走到这里，而它们都会让"保存"变成无声失败
	std::error_code ec;
	if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return false;
	wchar_t tmp[MAX_PATH]{};
	if (!GetTempFileNameW(dir.c_str(), L"snap", 0, tmp)) return false;
	DeleteFileW(tmp);
	return true;
}

std::wstring Util::savePathIn(const std::wstring& dir, const std::wstring& ext)
{
	auto resolved = resolveSaveDir(dir);
	if (resolved.empty()) return L"";
	return resolved + L"\\" + createFileName(ext);
}

std::wstring Util::desktopSavePath(const std::wstring& ext)
{
	auto dir = desktopDir();
	if (dir.empty()) return L"";
	return dir + L"\\" + createFileName(ext);
}

std::wstring Util::pickFolder(HWND hwnd, const std::wstring& initial)
{
	std::wstring result;
	ComPtr<IFileOpenDialog> dialog;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(dialog.GetAddressOf())))) return result;
	DWORD flags{ 0 };
	if (FAILED(dialog->GetOptions(&flags))) return result;
	// 只让选文件夹（FOS_PICKFOLDERS），且必须是一个真实存在、文件系统里的路径
	dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
	if (!initial.empty()) {
		ComPtr<IShellItem> start;
		if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
			IID_PPV_ARGS(start.GetAddressOf())))) {
			dialog->SetFolder(start.Get());
		}
	}
	if (FAILED(dialog->Show(hwnd))) return result;   // 用户取消
	ComPtr<IShellItem> item;
	if (FAILED(dialog->GetResult(item.GetAddressOf()))) return result;
	PWSTR path{ nullptr };
	if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
		result = path;
		CoTaskMemFree(path);
	}
	return result;
}

std::wstring Util::createFileName(const std::wstring& ext)
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	return std::format(L"{:04d}{:02d}{:02d}{:02d}{:02d}{:02d}{:03d}.{}",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ext);
}

bool Util::copyText(const std::wstring& text)
{
	if (!OpenClipboard(nullptr)) return false;
	EmptyClipboard();
	const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (!hMem) {
		CloseClipboard();
		return false;
	}
	void* dst = GlobalLock(hMem);
	if (!dst) {
		GlobalFree(hMem);
		CloseClipboard();
		return false;
	}
	CopyMemory(dst, text.c_str(), bytes);
	GlobalUnlock(hMem);
	// 成功后所有权归系统，失败才自己释放
	const bool ok = SetClipboardData(CF_UNICODETEXT, hMem) != nullptr;
	if (!ok) GlobalFree(hMem);
	CloseClipboard();
	return ok;
}

// CF_HDROP：DROPFILES 头 + 一串双 \0 结尾的宽字符路径。
// 只放 CF_HDROP 就够 —— 资源管理器 / 聊天窗口都是按"拖进来看作文件"这条路收的，
// 它自己会再去问 CF_FILECONTENTS 把内容读走，所以这里不提供 CF_FILECONTENTS
bool Util::addFileToClipboard(const std::wstring& path)
{
	if (path.empty()) return false;
	const size_t listBytes = (path.size() + 2) * sizeof(wchar_t);   // 末尾两个 \0
	const size_t total = sizeof(DROPFILES) + listBytes;
	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
	if (!hMem) return false;
	auto base = static_cast<BYTE*>(GlobalLock(hMem));
	if (!base) {
		GlobalFree(hMem);
		return false;
	}
	auto drop = reinterpret_cast<DROPFILES*>(base);
	drop->pFiles = sizeof(DROPFILES);   // 路径列表相对结构体开头的偏移
	drop->fWide = TRUE;                 // 宽字符
	CopyMemory(base + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
	GlobalUnlock(hMem);
	if (!OpenClipboard(nullptr)) {
		GlobalFree(hMem);
		return false;
	}
	EmptyClipboard();
	// 成功后所有权归剪切板，不能再 GlobalFree
	const bool ok = SetClipboardData(CF_HDROP, hMem) != nullptr;
	if (!ok) GlobalFree(hMem);
	CloseClipboard();
	return ok;
}

bool Util::saveTextFile(const std::wstring& path, const std::wstring& text)
{
	if (path.empty()) return false;
	const int need = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
		nullptr, 0, nullptr, nullptr);
	// WideCharToMultiByte 失败返回的是 0，不是负数。原来判 "< 0" 等于没判：
	// 转换失败（含代理对损坏）时 need=0，最后会写出一份只有 BOM 的空文件还报"已保存"
	if (need <= 0 && !text.empty()) return false;
	std::string utf8((size_t)need, '\0');
	if (need > 0) {
		WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(),
			utf8.data(), need, nullptr, nullptr);
	}
	std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
	if (!out) return false;
	out.write("\xEF\xBB\xBF", 3);      // UTF-8 BOM
	out.write(utf8.data(), (std::streamsize)utf8.size());
	return out.good();
}
