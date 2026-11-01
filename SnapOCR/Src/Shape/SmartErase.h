#pragma once
#include <windows.h>

// 智能消除（内容感知填充）：用洞周围的原图内容重建被遮住的像素。
// 三路策略，按代价从低到高：
//   1) 曲面拟合：纯色 / 线性渐变背景直接按平面预测填
//   2) 周期平移：横竖方向的精确重复纹理，整段平移验证通过后整块搬
//   3) PatchMatch：结构先验 + 金字塔 + 逐像素匹配场 + 邻域投票
namespace smarterase {

// bits: BGRA8，pitch: 行字节数；mask: 0 = 已知像素，非 0 = 待补的洞（按像素，长度 width*height）
// 只有洞内像素会被改写，alpha 保持原样。
// 返回 false 表示没补全 —— 调用方应保持占位色，别把原图露出来
bool fill(BYTE* bits, UINT32 pitch, int width, int height, const BYTE* mask);

}
