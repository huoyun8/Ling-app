#include "pch.h"
#include "Tray.h"
#include "App.h"
#include "Lang.h"
#include "Win/WinCap.h"
#include "Win/WinQr.h"
#include "Setting.h"

namespace {
	static std::unique_ptr<Tray> trayIns;
	static constexpr UINT mainMsg = 162;
	static constexpr UINT settingMsg = 163;
	static constexpr UINT exitMsg = 164;
}

Tray::Tray()
{
	auto lingApp = ui::App::get();
	// 托盘悬停提示也走语言包：跟菜单项同一个来源，改名时只改一处
	lingApp->initTray(100, Lang::get(L"qr.title"));
	Setting::get()->initShortcutKeys();
	// 左键单击 / 双击 -> 主界面显隐切换（和"再双击一次桌面图标"同一套逻辑）。
	// 直接截图留给热键 F1
	lingApp->onTrayMouseEvent.add([this](bool isDown, bool isRight) {
		if (isDown && !isRight) {
			WinQr::toggleMainVisible();
		}
		else if (isDown && isRight) {
			this->onTrayRightClick();
		}
	});
}

Tray::~Tray()
{
}

void Tray::init()
{
	trayIns.reset(new Tray());
}

void Tray::onTrayRightClick()
{
	auto menu = CreatePopupMenu();
	AppendMenu(menu, MF_STRING, mainMsg, Lang::get(L"tray.main").data());
	AppendMenu(menu, MF_STRING, settingMsg, Lang::get(L"tray.setting").data());
	AppendMenu(menu, MF_STRING, exitMsg, Lang::get(L"tray.exit").data());
	auto menuId = ui::App::get()->popupMenu(menu);
	if (menuId == mainMsg)
	{
		WinQr::init();
	}
	else if (menuId == settingMsg)
	{
		WinQr::showSettingPage();
	}
	else if (menuId == exitMsg)
	{
		ui::App::get()->quit(0);
	}
}
