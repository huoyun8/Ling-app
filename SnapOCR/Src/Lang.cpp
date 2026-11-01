#include "pch.h"
#include "Lang.h"
#include "Util.h"

std::unique_ptr<Lang> lang;

void Lang::init()
{
	lang.reset(new Lang());
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
	// 软件名不走语言包：同一个键要按构建版本返回 SnapCore 或 SnapOCR，
	// 而语言包是编进资源的固定文本。在这里特判一处，托盘提示、主窗口标题、
	// 各条消息框标题就全部跟着品牌走，语言包里也不用再维护两份
	if (keyPath == L"qr.title") return SNAP_BRAND_W;
	// 一路用带默认值的重载，缺键不抛异常：简体中文缺了某个键就把键名本身显示出来 ——
	// 界面上难看，但比崩掉好，也一眼能看出缺哪个键
	if (!lang) return keyPath; // 还没 init 就被问到（启动早期 / 退出后期），同样退回键名
	auto arr = ui::Util::splitStr(keyPath, L'.');
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
	auto [pData, size] = ui::Util::getRes(L"zh-CN");
	langObj = nullptr;
	if (pData && size > 0) {
		auto json = ui::Util::readTextFromBytes(pData, size);
		if (!json.empty()) langObj = JsonObject::Parse(json);
	}
}
