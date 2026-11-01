#pragma once
#include <Ui/Ui.h>
#include <vector>
#include <winrt/Windows.Data.Json.h>
using namespace winrt::Windows::Data::Json;

class Lang
{
public:
	static void init();
	// 同 Setting::dispose：langObj 也是 WinRT 对象，得趁 COM 还活着放掉
	static void dispose();
	static Lang* get();
	static std::wstring get(const std::wstring& keyPath);
	void initLang(const std::wstring& langCode);
private:
	Lang() = default;
private:
	JsonObject langObj;
};
