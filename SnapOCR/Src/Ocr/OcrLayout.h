#pragma once
#include <string>
#include <vector>

// 一行识别结果：文本 + 置信度 + 外接矩形。
// 引擎在 COMPLETE 载荷里本来就把每个文本框的四角坐标（quad: 8×f32）吐出来了，
// 以前解析时被 `off += 36` 直接跳过丢掉 —— 等于花算力拿到排版信息又扔了。
// 这里把它接住，坐标单位就是送进去那张图的像素。
struct OcrLine
{
	std::wstring text;
	float score = 0.f;
	float x1 = 0.f, y1 = 0.f, x2 = 0.f, y2 = 0.f;
};

// 排版解析：把引擎"按检测顺序"吐出来的行，重排成人能读的文本。
//
// 纯算法 —— 不碰模型、不动协议、不进资源，exe 体积增量 0。
// 四档行为见 Mode；默认 Auto，判错了能一档退回 Raw（= 旧行为）。
namespace Layout
{
	enum class Mode
	{
		Auto,   // 多栏切分 + 自然段合并：文章、网页、文档截图
		Line,   // 每框一行，只做阅读顺序排序：表格、列表、代码
		Flat,   // 全部拼成一段：只取一个短语
		Raw,    // 引擎原始顺序（旧行为，回归保底）
	};

	// 配置里存的是字符串键（ocr.layout），认不出来一律回 Auto
	Mode fromKey(const std::wstring& key);
	const wchar_t* keyOf(Mode m);
	// 结果页那颗"排版"按钮的循环顺序：Auto → Line → Flat → Raw → Auto
	Mode next(Mode m);

	// lines 为引擎原始顺序；返回带 \r\n 的成品文本（多行 EDIT 只认 \r\n）
	std::wstring format(const std::vector<OcrLine>& lines, Mode mode);
}
