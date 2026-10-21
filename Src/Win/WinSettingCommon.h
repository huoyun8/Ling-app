#pragma once
#include <include/Ling.h>
class WinSettingCommon:public Ling::Node
{
public:
	WinSettingCommon(Ling::WinBase* parent);
	~WinSettingCommon();
private:
	void initAutoStartCtrls();
	void setAutoStartBtn(Ling::Button* btn);
};

