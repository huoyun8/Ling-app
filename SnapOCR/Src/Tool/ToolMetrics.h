#pragma once
#include <cmath>

// 主编辑器（ToolMain）与副编辑器（ToolSub）共用的一套尺寸 / 字号规则。
//
// 为什么要有这个文件：两条工具条本来就是同一套视觉，但之前尺寸是散着写的 ——
// ToolMain 的按钮 32 高（setHeightPercent(100) 撑满），ToolSub 的按钮 29.5 高
// （每处都写一遍 btnSize - 2.5，共五六处），色块 13、图标 15、对勾 8 又各写一个数。
// 结果是同屏两条工具条的按钮不一样大、上下留白不一样，改一次总尺寸还得满文件找数字，
// 漏掉一处就是"有的按钮对、有的不对"。
//
// 现在只留一条规则：**所有东西都排在一张 cell × cell 的网格上**。
//   · 工具按钮 / 颜色按钮 各占一格；滑块占 sliderW 宽，两侧各留 sliderPad
//   · 格子里放按钮本体，四边各留 pad —— 所以按钮边长 = cell - 2*pad，上下左右留白完全相同
//   · 字号跟着格子走（iconFont / swatch / swatchFont 都由 cell 推出），改 cell 全部一起缩
//   · 描边画在格子内侧，不额外占位
//
// 于是"间距"不再是一个个调出来的数字，而是同一个 pad；要调只改这一个值。
// 万一某个图标看着偏大偏小，那是**设计稿里那个图形的留白**问题（图标按 vb 等比铺满方盒），
// 去改设计稿或生成器，不要回来给单个按钮塞一个特例数字 —— 那正是这套规则要消灭的东西。
namespace ToolMetrics {

	// ——— 网格 ———
	inline constexpr float cell{ 32.f };          // 一格按钮的边长（逻辑像素）
	inline constexpr float pad{ 1.5f };           // 按钮相对格子的留白，四条边一样
	inline constexpr float btn{ cell - pad * 2.f };   // 按钮本体边长 = 29

	// 分隔条宽度。传 1.f 而不是 dpi：Node::setWidth 内部已经乘过一次
	inline constexpr float sep{ 1.f };
	// 描边宽度：画在格子内，不占布局位置（主条 body 的 setBorder、副条的 paintBorder 都用它）
	inline constexpr float border{ 1.f };

	// ——— 副编辑器专有 ———
	inline constexpr float sliderW{ 80.f };       // 滑块宽度
	// 滑块两侧留白。比 pad 大一档：滑块是长条，贴着按钮会比按钮之间的缝更显挤
	inline constexpr float sliderPad{ 3.f };
	// 顶部箭头区域高度（副条顶上那个指向主条按钮的小三角）。
	// 从 3 收到 2：副条的总高要跟主条对齐，这 1px 是唯一还能挤的地方
	inline constexpr float arrowZone{ 2.f };
	// 箭头尖端两侧各伸出的长度
	inline constexpr float arrowHalf{ 3.f };

	// 主条与副条之间的竖直间距。WinHost::layoutTools() 用它算整组高度
	inline constexpr float mainGap{ 2.f };

	// ——— 字号（全部由格子推出，不单独写数字）———
	// 工具图标字号。0.47 那档（≈15px）在 29px 的按钮里偏小：字形里的笔画是 80/1024 em，
	// 折到 15px 上只有 1.2 个物理像素，抗锯齿一抹就成了灰边，看着"发虚"。
	// 提到 0.58（≈18.6px）笔画约 1.5px，同一套字形明显更实，按钮里也还留得住留白
	inline constexpr float iconFont{ cell * 0.58f };      // 工具图标（≈18.6）
	inline constexpr float textFont{ cell * 0.40f };      // 条上的纯文字（计时）
	inline constexpr float swatch{ cell * 0.40f };        // 颜色块边长（≈12.8）
	inline constexpr float swatchFont{ swatch * 0.60f };  // 颜色块里的对勾（≈7.7）

	// 分隔线长度：早先按"比格子矮一截"的思路给过一条定长，但那样在 body 的
	// alignItems: Center 下要用高度去撑；现在两条工具条的分隔条都改成
	// setHeightPercent(100) 撑满整行（见 ToolMain::onCreated / ToolSub::showVideoTools），
	// 这个常量没有人再读了，一并删掉

	// ——— 吸附到整物理像素 ———
	// 布局最后会把每个节点的 left / width 各自四舍五入到整物理像素（Node::layout）。
	// cell 32 在任何缩放下都是整数（125% → 40、150% → 48），但 pad 1.5 和 btn 29 不是：
	// 125% 下是 1.875 / 36.25，四舍五入后左边缝 2、右边缝 2、按钮 36 —— 每颗按钮的
	// 小数相位不同时，缝与按钮的整数化结果就会差 1px，选中的色块看着"有的宽有的窄"。
	// 先把 pad 与 btn 吸附成"物理像素恰好是整数"的逻辑值，再交给布局，
	// 每格的三段（缝 / 按钮 / 缝）在物理像素上就都一样宽了。
	inline float snapPx(float v, float dpi)
	{
		return dpi > 0.f ? std::round(v * dpi) / dpi : v;
	}
	inline float padAt(float dpi) { return snapPx(pad, dpi); }
	// 按钮 = 整格 - 两条缝：三者相加严格等于一格，绝不会因为取整多出或少掉 1px
	inline float btnAt(float dpi) { return snapPx(cell, dpi) - padAt(dpi) * 2.f; }

	// 逻辑字号 → "物理像素恰为整数"的逻辑字号。
	//
	// 这是图标发虚的根因：cell*0.47=15.04 这种小数字号，在 125% 缩放下变成 18.8 物理像素，
	// 字形轮廓整条落在半像素上，DWrite 只能用灰阶抗锯齿去凑，笔画边缘就是一片糊。
	// 大号文字看不出来，15px 的小图标最明显 —— 所以必须在喂给 DWrite 之前就取整。
	//
	// 所有 setFontSize 都过这一道，别在别处再写死字号。
	inline float fontFor(float logical, float dpi)
	{
		if (dpi <= 0.f) return logical;
		const float phys = std::round(logical * dpi);
		return phys < 1.f ? logical : phys / dpi;
	}

}
