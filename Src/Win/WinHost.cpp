#include "pch.h"
#include "WinHost.h"
#include "../Tool/ToolMain.h"
#include "../Tool/ToolSub.h"
#include "../History.h"

// unique_ptr<ToolMain/ToolSub/History> 成员要求这些类型在析构处是完整的，
// 而头文件里只有前置声明，所以析构函数放到这里定义。
WinHost::~WinHost() = default;
