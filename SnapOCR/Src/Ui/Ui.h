#pragma once
// ui 层总入口：应用层只 include 这一个头，就能拿到全部控件。
#include <Windows.h>

#include "Layout.h"
#include "Color.h"
#include "UiApp.h"
#include "D2D.h"
#include "Node.h"
#include "Button.h"
#include "Label.h"
#include "Text.h"
#include "Slider.h"
#include "Canvas.h"
#include "TextBox.h"
#include "UiUtil.h"
#include "WinBase.h"

namespace ui {
	bool init();
	void dispose();
}
