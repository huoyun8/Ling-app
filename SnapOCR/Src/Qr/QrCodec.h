#pragma once
#include <cstdint>
#include <string>
#include <vector>

// 二维码编解码。底层是 vendored 进来的两个库：
//   Qr/third_party/qrencode  —— 编码（libqrencode，LGPL，fukuchi 版）
//   Qr/third_party/quirc     —— 解码（quirc，ISC，dlbeer 版）
// 这一层把它们收敛成两个函数，并负责 UTF-16 <-> UTF-8、像素格式转换，
// 界面层不需要碰任何一个 third_party 头文件。

// 编码结果：size × size 个模块，modules[y*size+x]==1 表示该模块是暗色。
// 不含静默区（绘制方自己留白边），也不含缩放 —— 显示时按目标像素边长铺。
struct QrBitmap
{
	int size{ 0 };
	std::vector<uint8_t> modules;
};

// 容错级别：数值与 libqrencode 的 QR_ECLEVEL_* 对齐（L=0 M=1 Q=2 H=3）。
// QrEcAuto = 参照草料「优先保持码型紧凑」的自动规则：
// Q 不升版本 → Q(25%)；Q 升版本而 M 不升 → M(15%)；连 M 都得升版本 → 回 Q
enum QrEcLevel { QrEcAuto = -1, QrEcM = 1, QrEcQ = 2, QrEcH = 3 };

// 把内容编成二维码。utf8 为空或内容太长会失败返回 false。
// ec 传 QrEcH 即草料「加 Logo 默认 30% 容错」那一条：Logo 遮挡也能扫
bool qrEncode(const std::string& utf8, QrBitmap& out, int ec = QrEcAuto);

// 从一张 BGRA、top-down、行紧凑（stride = w*4）的图里找二维码并解出文本。
// 图里没有码、或码本身已损坏时返回空串。一张图里有多个码时返回第一个能解的那个。
// scaleHint 用于"图很小但码很大"的场景（例如截图下来的一小块）：>1 时先把图放大再解，
// 小图上的模块边缘常被插值糊掉，放大后再交给 quirc 命中率明显更高。
std::string qrDecode(const uint8_t* bgra, int w, int h, int scaleHint = 1);
