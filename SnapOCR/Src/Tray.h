#pragma once
#include <Ui/Ui.h>

class Tray
{
public:
	~Tray();
	static void init();
private:
	Tray();
	void onTrayRightClick();
};
