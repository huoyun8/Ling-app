#include "pch.h"
#include "Lang.h"
#include "Util.h"

std::unique_ptr<Lang> lang;

Lang::Lang()
{
}

Lang::~Lang()
{
}

void Lang::init()
{
	auto ptr = new Lang();
	lang.reset(ptr);
	// 只保留简体中文：不再读设置里的旧语种，一律加载内置 zh-CN
	lang->initLang(L"zh-CN");
}

void Lang::dispose()
{
	lang.reset();
}

Lang* Lang::get()
{
	return lang.get();
}

std::wstring Lang::get(const std::wstring& keyPath)
{
	// 一路用带默认值的重载，缺键不抛异常：简体中文缺了某个键就把键名本身显示出来 ——
	// 界面上难看，但比崩掉好，也一眼能看出缺哪个键
	auto arr = Ling::Util::splitStr(keyPath, L'.');
	if (arr.size() < 2) return keyPath;
	auto& self = *lang;
	auto obj = self.langObj.GetNamedObject(arr[0], nullptr);
	if (!obj) return keyPath;
	auto text = obj.GetNamedString(arr[1], L"");
	if (text.empty()) return keyPath;
	return std::wstring{ text };
}

void Lang::initLang(const std::wstring& langCode)
{
	// 只保留简体中文：en-US 等其它语言资源已从 Lang/ 目录和 Resource.rc 移除，
	// 这里无条件加载内置 zh-CN，配置里存的旧语种也忽略
	(void)langCode;
	auto [pData, size] = Ling::Util::getRes(L"zh-CN");
	langObj = nullptr;
	if (pData && size > 0) {
		auto json = Ling::Util::readTextFromBytes(pData, size);
		if (!json.empty()) langObj = JsonObject::Parse(json);
	}
}
