#pragma once
#include <string>
#include <wrl.h>
#include <d2d1_1.h>

namespace ui {
    // 描边的端头 / 拐角。与 SVG 的 stroke-linecap / stroke-linejoin 一一对应，
    // 生成器把设计稿里的声明原样带过来 —— 图标线条是圆头还是方头，由设计稿决定，
    // 不是渲染时统一凑一个。枚举顺序刻意不跟 D2D 的值走，映射在 .cpp 里显式 switch。
    enum class Cap : unsigned char { Butt, Round, Square };
    enum class Join : unsigned char { Miter, Round, Bevel };

    // 图标路径：把生成器产出的 SVG path 数据解析成 D2D 几何。
    //
    // 为什么不用字体：字形光栅化受 DWrite 的 hinting / ClearType / 小数 ppem 影响，
    // 小尺寸下边缘不可控（透明表面上还会有彩边）。路径几何则完全由我们决定：
    // 画在什么尺寸、缩放几倍、是否落在整像素上，都能精确控制。
    //
    // 同一个 d 串只解析一次，之后复用同一份几何（几十条路径，常驻内存可忽略）。
    // 几何是独立的 COM 对象、不绑定 render target，设备销毁重建后依然可用。
    class SvgPath
    {
    public:
        // evenOdd：奇偶填充（SVG 的 fill-rule="evenodd"），否则非零环绕。
        // 失败返回 nullptr（路径数据错误时不该让整个界面崩掉）。
        static ID2D1PathGeometry* get(const std::wstring& d, bool evenOdd);
        // 描边样式。组合只有 3×3 种，建一次就缓存住，绘制时按 op 取用。
        // 设备没就绪时返回 nullptr，调用方按默认样式画（仍有描边，只是端头是方头）。
        static ID2D1StrokeStyle* stroke(Cap cap, Join join);
        // 设备整体销毁时调用：几何与描边样式都由工厂产出，跟着一起释放，
        // 避免下次重建后仍持有旧对象
        static void clear();
    };
}
