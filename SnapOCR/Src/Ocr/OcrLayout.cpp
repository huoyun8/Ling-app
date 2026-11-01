#include "pch.h"
#include "OcrLayout.h"

#include <algorithm>
#include <cmath>

// 排版解析的实现。规则抄的是 Umi-OCR 的 tbpu 思路（只抄规则，不抄代码）：
//   投影切栏 → 栏内聚类成行 → 行内按 x 拼 → 跨行判段 → 竖排单独走
// 全部基于引擎给的 quad 坐标，没有模型、没有额外资源。

namespace
{
	struct Box
	{
		const OcrLine* src{};
		float x1{}, y1{}, x2{}, y2{};

		float cx() const { return (x1 + x2) * 0.5f; }
		float cy() const { return (y1 + y2) * 0.5f; }
		float w() const { return x2 - x1; }
		float h() const { return y2 - y1; }
	};

	struct Row
	{
		std::wstring text;
		float x1{}, x2{}, cy{};
	};

	float median(std::vector<float> v)
	{
		if (v.empty()) return 0.f;
		const size_t mid = v.size() / 2;
		std::nth_element(v.begin(), v.begin() + mid, v.end());
		return v[mid];
	}

	// 句末标点：见到它就该换段
	bool endsParagraph(wchar_t c)
	{
		switch (c)
		{
		case L'。': case L'！': case L'？': case L'；': case L'：':
		case L'.': case L'!': case L'?': case L';': case L':':
		case L'…': case L'」': case L'』': case L'》': case L'）':
			return true;
		default:
			return false;
		}
	}

	// 拉丁字母/数字：两侧都是它才补空格（中文词语之间补空格是错的）
	bool isWordChar(wchar_t c)
	{
		if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'))
			return true;
		return c >= 0xC0 && c < 0x2000;   // 带重音的西欧字母
	}

	// 把 box 投影到 x 轴，返回"有字的横带"。窄于 minGap 的缝隙并进相邻带，
	// 免得字距稍大的行被切成两栏。
	std::vector<std::pair<float, float>> splitBands(const std::vector<Box>& bs, float minGap)
	{
		std::vector<float> xs;
		xs.reserve(bs.size() * 2);
		for (const auto& b : bs) { xs.push_back(b.x1); xs.push_back(b.x2); }
		std::sort(xs.begin(), xs.end());
		xs.erase(std::unique(xs.begin(), xs.end()), xs.end());

		std::vector<std::pair<float, float>> raw;
		bool in = false;
		float start = 0.f;
		for (size_t i = 0; i + 1 < xs.size(); i++)
		{
			const float a = xs[i], b = xs[i + 1];
			bool covered = false;
			for (const auto& bx : bs)
			{
				if (bx.x1 <= a && bx.x2 >= b) { covered = true; break; }
			}
			if (covered && !in) { start = a; in = true; }
			if (!covered && in) { raw.push_back({ start, a }); in = false; }
		}
		if (in) raw.push_back({ start, xs.back() });

		std::vector<std::pair<float, float>> out;
		for (const auto& s : raw)
		{
			if (!out.empty() && s.first - out.back().second <= minGap) out.back().second = s.second;
			else out.push_back(s);
		}
		return out;
	}

	// 同一行：中线高度差不超过半个字高
	void clusterRows(std::vector<Box>& bs, float unit, std::vector<std::vector<Box>>& rows)
	{
		std::sort(bs.begin(), bs.end(), [](const Box& a, const Box& b) { return a.cy() < b.cy(); });
		const float tol = 0.5f * unit;
		for (const auto& b : bs)
		{
			if (!rows.empty())
			{
				float sum = 0.f;
				for (const auto& o : rows.back()) sum += o.cy();
				const float cy = sum / (float)rows.back().size();
				if (std::fabs(b.cy() - cy) <= tol) { rows.back().push_back(b); continue; }
			}
			rows.push_back({ b });
		}
	}

	// 一行里多个框按 x 拼接；缝隙够大才补空格，且只在拉丁文之间补
	std::wstring joinRow(std::vector<Box>& row, float unit)
	{
		std::sort(row.begin(), row.end(), [](const Box& a, const Box& b) { return a.x1 < b.x1; });
		std::wstring out;
		for (size_t i = 0; i < row.size(); i++)
		{
			if (i && !out.empty())
			{
				const float gap = row[i].x1 - row[i - 1].x2;
				const wchar_t prev = out.back();
				const wchar_t nextc = row[i].src->text.empty() ? L' ' : row[i].src->text.front();
				if (gap > 1.8f * unit) out += L' ';                       // 明显是两列字段
				else if (gap > 0.4f * unit && (isWordChar(prev) || isWordChar(nextc))) out += L' ';
			}
			out += row[i].src->text;
		}
		return out;
	}

	// 横排：先聚行，再按需判段
	std::wstring formatHorizontal(std::vector<Box> col, float unit, bool mergeParagraph)
	{
		std::vector<std::vector<Box>> rows;
		clusterRows(col, unit, rows);

		float left = 0.f, right = 0.f;
		for (const auto& b : col)
		{
			if (left == 0.f && right == 0.f) { left = b.x1; right = b.x2; }
			left = std::min(left, b.x1);
			right = std::max(right, b.x2);
		}
		const float width = std::max(right - left, unit);

		std::wstring out;
		bool first = true;
		Row prev{};
		for (auto& r : rows)
		{
			Row cur;
			cur.text = joinRow(r, unit);
			if (cur.text.empty()) continue;
			cur.x1 = r.front().x1;
			cur.x2 = r.front().x2;
			for (const auto& b : r)
			{
				cur.x1 = std::min(cur.x1, b.x1);
				cur.x2 = std::max(cur.x2, b.x2);
			}
			cur.cy = r.front().cy();

			if (first) { out = cur.text; }
			else
			{
				bool brk = !mergeParagraph;
				if (!brk && endsParagraph(prev.text.back())) brk = true;
				if (!brk && cur.cy - prev.cy > 1.9f * unit) brk = true;                 // 段间距
				if (!brk && cur.x1 - left > 1.2f * unit) brk = true;                    // 首行缩进
				if (!brk && (prev.x2 - left) < 0.78f * width) brk = true;               // 上一行没写满
				if (brk) out += L"\r\n";
				else if (isWordChar(out.back()) && isWordChar(cur.text.front())) out += L' ';
				out += cur.text;
			}
			prev = cur;
			first = false;
		}
		return out;
	}

	// 竖排：列内从上到下，每框一行（竖排的段落判据和横排不是一回事，先不合并）
	std::wstring formatVertical(std::vector<Box> col)
	{
		std::sort(col.begin(), col.end(), [](const Box& a, const Box& b) { return a.y1 < b.y1; });
		std::wstring out;
		for (const auto& b : col)
		{
			if (b.src->text.empty()) continue;
			if (!out.empty()) out += L"\r\n";
			out += b.src->text;
		}
		return out;
	}

	// 旧行为：引擎给什么顺序就是什么顺序。Raw 档必须与改动前逐字节一致
	std::wstring rawJoin(const std::vector<OcrLine>& lines)
	{
		std::wstring out;
		for (const auto& l : lines)
		{
			if (!out.empty()) out += L"\r\n";
			out += l.text;
		}
		return out;
	}
}

Layout::Mode Layout::fromKey(const std::wstring& key)
{
	if (key == L"line") return Mode::Line;
	if (key == L"flat") return Mode::Flat;
	if (key == L"raw") return Mode::Raw;
	return Mode::Auto;
}

const wchar_t* Layout::keyOf(Mode m)
{
	switch (m)
	{
	case Mode::Line: return L"line";
	case Mode::Flat: return L"flat";
	case Mode::Raw: return L"raw";
	default: return L"auto";
	}
}

Layout::Mode Layout::next(Mode m)
{
	switch (m)
	{
	case Mode::Auto: return Mode::Line;
	case Mode::Line: return Mode::Flat;
	case Mode::Flat: return Mode::Raw;
	default: return Mode::Auto;
	}
}

std::wstring Layout::format(const std::vector<OcrLine>& lines, Mode mode)
{
	std::vector<Box> bs;
	bs.reserve(lines.size());
	for (const auto& l : lines)
	{
		if (l.text.empty()) continue;
		bs.push_back({ &l, l.x1, l.y1, l.x2, l.y2 });
	}
	// 没坐标（老引擎/字段缺失）或明确要旧行为，都退回原始顺序
	if (bs.empty() || mode == Mode::Raw) return rawJoin(lines);

	// 字高用 min(w,h) 的中位数：横排的 min 是行高，竖排的 min 是列宽，两边都成立
	std::vector<float> units;
	units.reserve(bs.size());
	for (const auto& b : bs) units.push_back(std::min(b.w(), b.h()));
	float unit = median(units);
	if (unit < 2.f) unit = 2.f;

	int tall = 0;
	for (const auto& b : bs) if (b.h() > 1.5f * b.w()) tall++;
	const bool globalVertical = (tall * 2 >= (int)bs.size());

	if (mode == Mode::Line)
	{
		if (globalVertical)
		{
			std::sort(bs.begin(), bs.end(), [](const Box& a, const Box& b) {
				if (std::fabs(a.cx() - b.cx()) > 2.f) return a.cx() > b.cx();   // 竖排从右往左
				return a.y1 < b.y1;
				});
			std::wstring out;
			for (const auto& b : bs)
			{
				if (!out.empty()) out += L"\r\n";
				out += b.src->text;
			}
			return out;
		}
		std::vector<std::vector<Box>> rows;
		clusterRows(bs, unit, rows);
		std::wstring out;
		for (auto& r : rows)
		{
			std::sort(r.begin(), r.end(), [](const Box& a, const Box& b) { return a.x1 < b.x1; });
			for (const auto& b : r)
			{
				if (!out.empty()) out += L"\r\n";
				out += b.src->text;
			}
		}
		return out;
	}

	if (mode == Mode::Flat)
	{
		if (globalVertical)
		{
			// 竖排拼成一段要按竖排顺序来，否则读出来是乱的
			std::vector<Box> v = bs;
			std::sort(v.begin(), v.end(), [](const Box& a, const Box& b) {
				if (std::fabs(a.cx() - b.cx()) > 2.f) return a.cx() > b.cx();
				return a.y1 < b.y1;
				});
			std::wstring out;
			for (const auto& b : v) out += b.src->text;
			return out;
		}
		std::vector<std::vector<Box>> rows;
		clusterRows(bs, unit, rows);
		std::wstring out;
		for (auto& r : rows)
		{
			const std::wstring t = joinRow(r, unit);
			if (t.empty()) continue;
			if (!out.empty() && (isWordChar(out.back()) || isWordChar(t.front()))) out += L' ';
			out += t;
		}
		return out;
	}

	// Auto：先切栏，栏间按方向排序，栏内再聚行/判段
	// 切栏阈值取 0.9 字高：词间距通常不到 0.5 字高，栏间距一般在 1 字高以上。
	// 投影里还能活下来的空白带，基本都是整栏贯通的（词间空档会被别的行盖住）
	const auto bands = splitBands(bs, 0.9f * unit);
	if (bands.size() <= 1)
	{
		return globalVertical ? formatVertical(bs) : formatHorizontal(bs, unit, true);
	}

	std::vector<std::pair<std::pair<float, float>, std::vector<Box>>> cols;
	for (const auto& band : bands)
	{
		std::vector<Box> col;
		for (const auto& b : bs)
		{
			const float cx = b.cx();
			if (cx >= band.first && cx <= band.second) col.push_back(b);
		}
		if (!col.empty()) cols.push_back({ band, std::move(col) });
	}
	if (cols.empty()) return formatHorizontal(bs, unit, true);

	const bool verticalLayout = globalVertical;

	if (verticalLayout) std::sort(cols.begin(), cols.end(), [](const auto& a, const auto& b) {
		return a.first.first > b.first.first;      // 中文竖排：从右往左读
		});
	else std::sort(cols.begin(), cols.end(), [](const auto& a, const auto& b) {
		return a.first.first < b.first.first;
		});

	std::wstring out;
	for (auto& c : cols)
	{
		int t = 0;
		for (const auto& b : c.second) if (b.h() > 1.5f * b.w()) t++;
		const bool vertical = (t * 2 >= (int)c.second.size());
		const std::wstring piece = vertical ? formatVertical(c.second) : formatHorizontal(c.second, unit, true);
		if (piece.empty()) continue;
		if (!out.empty()) out += L"\r\n";
		out += piece;
	}
	return out.empty() ? rawJoin(lines) : out;
}
