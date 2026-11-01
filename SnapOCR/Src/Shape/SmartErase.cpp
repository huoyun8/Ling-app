#include "pch.h"
#include "SmartErase.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

// 独立实现（不引用 Snow Shot 代码），数学与判据对齐：
//   曲面拟合 → 周期平移 → 结构引导 + 金字塔 PatchMatch + 邻域投票
// 与 Snow Shot 的差异只在不做的部分：异步取消、分阶段计时、并行投票、导出快照、缓存键。
namespace {

constexpr int kPatchR = 3;                     // patch 半径 3 → 7×7
constexpr int kMaxPeriod = 64;                 // 周期探测上限
constexpr std::size_t kMaxWorkingPixels = 4u * 1024u * 1024u;   // 交互上限，超则失败保留占位
constexpr int kPassesCoarse = 5, kPassesMid = 3, kPassesFine = 2;

// ─────────────────────────── 基础容器 ───────────────────────────
struct Mask {
	int w = 0, h = 0;
	std::vector<BYTE> v;
	void reset(int W, int H) { w = W; h = H; v.assign((size_t)W * H, 0); }
	BYTE& at(int x, int y) { return v[(size_t)y * w + x]; }
	BYTE at(int x, int y) const { return v[(size_t)y * w + x]; }
	size_t count() const { size_t n = 0; for (BYTE b : v) n += b ? 1 : 0; return n; }
};

struct Plane {
	int w = 0, h = 0;
	std::vector<float> v;
	void reset(int W, int H, float fill = 0.f) { w = W; h = H; v.assign((size_t)W * H, fill); }
	float& at(int x, int y) { return v[(size_t)y * w + x]; }
	float at(int x, int y) const { return v[(size_t)y * w + x]; }
};

struct Image {                                   // 三通道（RGB 或 Lab 共用）
	int w = 0, h = 0;
	std::vector<float> v;
	void reset(int W, int H) { w = W; h = H; v.assign((size_t)W * H * 3, 0.f); }
	float* at(int x, int y) { return &v[((size_t)y * w + x) * 3]; }
	const float* at(int x, int y) const { return &v[((size_t)y * w + x) * 3]; }
};

inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// ─────────────────────────── 图像基础操作 ───────────────────────────
std::vector<float> gaussKernel(int radius)
{
	const double sigma = std::max(0.5, radius / 2.0);
	std::vector<float> k((size_t)radius * 2 + 1);
	double sum = 0;
	for (int i = -radius; i <= radius; ++i) {
		const double t = i / sigma;
		const float wgt = (float)std::exp(-0.5 * t * t);
		k[(size_t)(i + radius)] = wgt;
		sum += wgt;
	}
	for (auto& wgt : k) wgt = (float)(wgt / sum);
	return k;
}

void blurPlane(Plane& p, int radius)
{
	if (radius <= 0 || p.w <= 1 || p.h <= 1) return;
	const auto k = gaussKernel(radius);
	const int w = p.w, h = p.h;
	Plane tmp;
	tmp.reset(w, h);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			float s = 0;
			for (int i = -radius; i <= radius; ++i) s += k[(size_t)(i + radius)] * p.at(std::clamp(x + i, 0, w - 1), y);
			tmp.at(x, y) = s;
		}
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			float s = 0;
			for (int i = -radius; i <= radius; ++i) s += k[(size_t)(i + radius)] * tmp.at(x, std::clamp(y + i, 0, h - 1));
			p.at(x, y) = s;
		}
}

// 5 点高斯降采样：核 [1 4 6 4 1] / 16，等价 pyrDown
void pyrDownPlane(const Plane& src, Plane& dst)
{
	const int dw = (src.w + 1) / 2, dh = (src.h + 1) / 2;
	dst.reset(dw, dh);
	static const float k[5] = { 1.f / 16, 4.f / 16, 6.f / 16, 4.f / 16, 1.f / 16 };
	Plane tmp;
	tmp.reset(src.w, dh);
	for (int y = 0; y < dh; ++y) {
		const int sy = std::clamp(y * 2 - 1, 0, src.h - 1);
		for (int x = 0; x < src.w; ++x) {
			float s = 0;
			for (int i = -2; i <= 2; ++i) s += k[i + 2] * src.at(std::clamp(x + i, 0, src.w - 1), sy);
			tmp.at(x, y) = s;
		}
	}
	for (int y = 0; y < dh; ++y)
		for (int x = 0; x < dw; ++x) {
			float s = 0;
			for (int i = -2; i <= 2; ++i) s += k[i + 2] * tmp.at(std::clamp(x * 2 + i, 0, src.w - 1), y);
			dst.at(x, y) = s;
		}
}

void pyrDownImage(const Image& src, Image& dst)
{
	const int dw = (src.w + 1) / 2, dh = (src.h + 1) / 2;
	dst.reset(dw, dh);
	static const float k[5] = { 1.f / 16, 4.f / 16, 6.f / 16, 4.f / 16, 1.f / 16 };
	Image tmp;
	tmp.reset(src.w, dh);
	for (int y = 0; y < dh; ++y) {
		const int sy = std::clamp(y * 2 - 1, 0, src.h - 1);
		for (int x = 0; x < src.w; ++x) {
			float s0 = 0, s1 = 0, s2 = 0;
			for (int i = -2; i <= 2; ++i) {
				const float* pv = src.at(std::clamp(x + i, 0, src.w - 1), sy);
				const float kk = k[i + 2];
				s0 += kk * pv[0]; s1 += kk * pv[1]; s2 += kk * pv[2];
			}
			float* q = tmp.at(x, y);
			q[0] = s0; q[1] = s1; q[2] = s2;
		}
	}
	for (int y = 0; y < dh; ++y)
		for (int x = 0; x < dw; ++x) {
			float s0 = 0, s1 = 0, s2 = 0;
			for (int i = -2; i <= 2; ++i) {
				const float* pv = tmp.at(std::clamp(x * 2 + i, 0, src.w - 1), y);
				const float kk = k[i + 2];
				s0 += kk * pv[0]; s1 += kk * pv[1]; s2 += kk * pv[2];
			}
			float* q = dst.at(x, y);
			q[0] = s0; q[1] = s1; q[2] = s2;
		}
}

// 双线性放大
void upscaleImage(const Image& src, int w, int h, Image& dst)
{
	dst.reset(w, h);
	if (src.w <= 0 || src.h <= 0) return;
	for (int y = 0; y < h; ++y) {
		float fy = ((y + 0.5f) * src.h / h) - 0.5f;
		fy = std::clamp(fy, 0.f, (float)src.h - 1);
		const int y0 = (int)std::floor(fy), y1 = std::min(y0 + 1, src.h - 1);
		const float ty = fy - y0;
		for (int x = 0; x < w; ++x) {
			float fx = ((x + 0.5f) * src.w / w) - 0.5f;
			fx = std::clamp(fx, 0.f, (float)src.w - 1);
			const int x0 = (int)std::floor(fx), x1 = std::min(x0 + 1, src.w - 1);
			const float tx = fx - x0;
			const float* p00 = src.at(x0, y0);
			const float* p10 = src.at(x1, y0);
			const float* p01 = src.at(x0, y1);
			const float* p11 = src.at(x1, y1);
			float* q = dst.at(x, y);
			for (int c = 0; c < 3; ++c) {
				const float a = p00[c] * (1 - tx) + p10[c] * tx;
				const float b = p01[c] * (1 - tx) + p11[c] * tx;
				q[c] = a * (1 - ty) + b * ty;
			}
		}
	}
}

// 面积均值降采样（掩膜用，与原版 INTER_AREA 对应）
void areaDownPlane(const Plane& src, Plane& dst)
{
	const int dw = std::max(1, (src.w + 1) / 2), dh = std::max(1, (src.h + 1) / 2);
	dst.reset(dw, dh);
	for (int y = 0; y < dh; ++y)
		for (int x = 0; x < dw; ++x) {
			float s = 0;
			int n = 0;
			for (int dy = 0; dy < 2; ++dy)
				for (int dx = 0; dx < 2; ++dx) {
					const int sx = x * 2 + dx, sy = y * 2 + dy;
					if (sx >= src.w || sy >= src.h) continue;
					s += src.at(sx, sy);
					++n;
				}
			dst.at(x, y) = n ? s / (float)n : 0.f;
		}
}

// ─────────────────────────── 形态学 ───────────────────────────
// 腐蚀：出界一律按 0 处理（对应原版 BORDER_CONSTANT=0）。所以掩膜在图像边缘会被收缩，
// 由它筛出的 donor 中心必然能容纳 2r+1 的 patch，后面读源 patch 不会越界。
Mask erodeRect(const Mask& m, int k)
{
	Mask out;
	out.reset(m.w, m.h);
	if (k <= 1) { out.v = m.v; return out; }
	const int r = k / 2;
	Mask tmp;
	tmp.reset(m.w, m.h);
	for (int y = 0; y < m.h; ++y)
		for (int x = 0; x < m.w; ++x) {
			BYTE all = 1;
			for (int i = x - r; i <= x + r && all; ++i) all = (i >= 0 && i < m.w) ? m.at(i, y) : 0;
			tmp.at(x, y) = all;
		}
	for (int y = 0; y < m.h; ++y)
		for (int x = 0; x < m.w; ++x) {
			BYTE all = 1;
			for (int i = y - r; i <= y + r && all; ++i) all = (i >= 0 && i < m.h) ? tmp.at(x, i) : 0;
			out.at(x, y) = all;
		}
	return out;
}

Mask dilateRect(const Mask& m, int k)
{
	Mask out;
	out.reset(m.w, m.h);
	if (k <= 1) { out.v = m.v; return out; }
	const int r = k / 2;
	Mask tmp;
	tmp.reset(m.w, m.h);
	for (int y = 0; y < m.h; ++y)
		for (int x = 0; x < m.w; ++x) {
			BYTE any = 0;
			for (int i = 0; i <= 2 * r && !any; ++i) any = m.at(std::clamp(x + i - r, 0, m.w - 1), y) ? 1 : 0;
			tmp.at(x, y) = any;
		}
	for (int y = 0; y < m.h; ++y)
		for (int x = 0; x < m.w; ++x) {
			BYTE any = 0;
			for (int i = 0; i <= 2 * r && !any; ++i) any = tmp.at(x, std::clamp(y + i - r, 0, m.h - 1)) ? 1 : 0;
			out.at(x, y) = any;
		}
	return out;
}

// 椭圆核膨胀：只算掩膜包围盒外扩 r 的范围，省掉全图开销
Mask dilateEllipse(const Mask& m, int k)
{
	Mask out;
	out.reset(m.w, m.h);
	if (k <= 1) { out.v = m.v; return out; }
	const int r = k / 2;
	const float rr = (float)(r * r) + 0.5f;
	int bx0 = m.w, by0 = m.h, bx1 = -1, by1 = -1;
	for (int y = 0; y < m.h; ++y)
		for (int x = 0; x < m.w; ++x)
			if (m.at(x, y)) {
				bx0 = std::min(bx0, x); by0 = std::min(by0, y);
				bx1 = std::max(bx1, x); by1 = std::max(by1, y);
			}
	if (bx1 < 0) return out;
	bx0 = std::max(0, bx0 - r); by0 = std::max(0, by0 - r);
	bx1 = std::min(m.w - 1, bx1 + r); by1 = std::min(m.h - 1, by1 + r);
	for (int y = by0; y <= by1; ++y)
		for (int x = bx0; x <= bx1; ++x) {
			BYTE any = 0;
			for (int dy = -r; dy <= r && !any; ++dy)
				for (int dx = -r; dx <= r; ++dx) {
					if ((float)(dx * dx + dy * dy) > rr) continue;
					const int sx = x + dx, sy = y + dy;
					if (sx < 0 || sy < 0 || sx >= m.w || sy >= m.h) continue;
					if (m.at(sx, sy)) { any = 1; break; }
				}
			out.at(x, y) = any;
		}
	return out;
}

// 两趟 chamfer 距离变换 + 最近种子标签（种子 = 非零像素）
void chamferNearest(const Mask& seed, std::vector<int>& label, Plane* distOut = nullptr)
{
	const int w = seed.w, h = seed.h;
	const float kFar = FLT_MAX / 4;
	Plane dist;
	dist.reset(w, h, kFar);
	label.assign((size_t)w * h, -1);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			if (seed.at(x, y)) {
				dist.at(x, y) = 0.f;
				label[(size_t)y * w + x] = y * w + x;
			}
	struct Step { int dx, dy; float cost; };
	static const Step fwd[4] = { { -1, -1, 1.4142f }, { 0, -1, 1.f }, { 1, -1, 1.4142f }, { -1, 0, 1.f } };
	static const Step bwd[4] = { { 1, 1, 1.4142f }, { 0, 1, 1.f }, { -1, 1, 1.4142f }, { 1, 0, 1.f } };
	for (int pass = 0; pass < 2; ++pass) {
		const bool forward = pass == 0;
		for (int yi = 0; yi < h; ++yi) {
			const int y = forward ? yi : h - 1 - yi;
			for (int xi = 0; xi < w; ++xi) {
				const int x = forward ? xi : w - 1 - xi;
				const size_t i = (size_t)y * w + x;
				float best = dist.v[i];
				int bestLabel = label[i];
				const Step* steps = forward ? fwd : bwd;
				for (int s = 0; s < 4; ++s) {
					const int sx = x + steps[s].dx, sy = y + steps[s].dy;
					if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
					const size_t si = (size_t)sy * w + sx;
					if (label[si] < 0) continue;
					const float d = dist.v[si] + steps[s].cost;
					if (d < best) { best = d; bestLabel = label[si]; }
				}
				dist.v[i] = best;
				label[i] = bestLabel;
			}
		}
	}
	if (distOut) *distOut = std::move(dist);
}

// ─────────────────────────── 颜色空间 ───────────────────────────
inline float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline float linearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

void rgbToLab(const Image& rgb, Image& lab)
{
	lab.reset(rgb.w, rgb.h);
	const int n = rgb.w * rgb.h;
	for (int i = 0; i < n; ++i) {
		const float r = srgbToLinear(std::clamp(rgb.v[(size_t)i * 3 + 0], 0.f, 1.f));
		const float g = srgbToLinear(std::clamp(rgb.v[(size_t)i * 3 + 1], 0.f, 1.f));
		const float b = srgbToLinear(std::clamp(rgb.v[(size_t)i * 3 + 2], 0.f, 1.f));
		const float X = (0.4124f * r + 0.3576f * g + 0.1805f * b) / 0.95047f;
		const float Y = (0.2126f * r + 0.7152f * g + 0.0722f * b) / 1.00000f;
		const float Z = (0.0193f * r + 0.1192f * g + 0.9505f * b) / 1.08883f;
		auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
		const float fx = f(X), fy = f(Y), fz = f(Z);
		lab.v[(size_t)i * 3 + 0] = 116.f * fy - 16.f;
		lab.v[(size_t)i * 3 + 1] = 500.f * (fx - fy);
		lab.v[(size_t)i * 3 + 2] = 200.f * (fy - fz);
	}
}

void labToRgb(Image& lab)
{
	const int n = lab.w * lab.h;
	for (int i = 0; i < n; ++i) {
		const float L = lab.v[(size_t)i * 3 + 0];
		const float a = lab.v[(size_t)i * 3 + 1];
		const float b = lab.v[(size_t)i * 3 + 2];
		const float fy = (L + 16.f) / 116.f;
		const float fx = fy + a / 500.f;
		const float fz = fy - b / 200.f;
		auto finv = [](float t) { const float t3 = t * t * t; return t3 > 0.008856f ? t3 : (t - 16.f / 116.f) / 7.787f; };
		const float X = finv(fx) * 0.95047f;
		const float Y = finv(fy);
		const float Z = finv(fz) * 1.08883f;
		float r = 3.2406f * X - 1.5372f * Y - 0.4986f * Z;
		float g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
		float z = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;
		lab.v[(size_t)i * 3 + 0] = std::clamp(linearToSrgb(std::clamp(r, 0.f, 1.f)), 0.f, 1.f);
		lab.v[(size_t)i * 3 + 1] = std::clamp(linearToSrgb(std::clamp(g, 0.f, 1.f)), 0.f, 1.f);
		lab.v[(size_t)i * 3 + 2] = std::clamp(linearToSrgb(std::clamp(z, 0.f, 1.f)), 0.f, 1.f);
	}
}

// ─────────────────────────── 3×3 求解 ───────────────────────────
bool invert3(const double a[3][3], double inv[3][3])
{
	double m[3][6];
	for (int r = 0; r < 3; ++r) {
		for (int c = 0; c < 3; ++c) m[r][c] = a[r][c];
		for (int c = 0; c < 3; ++c) m[r][3 + c] = (r == c) ? 1.0 : 0.0;
	}
	for (int c = 0; c < 3; ++c) {
		int p = c;
		for (int r = c + 1; r < 3; ++r) if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
		if (std::fabs(m[p][c]) < 1e-12) return false;
		if (p != c) for (int k = 0; k < 6; ++k) std::swap(m[c][k], m[p][k]);
		const double d = m[c][c];
		for (int k = 0; k < 6; ++k) m[c][k] /= d;
		for (int r = 0; r < 3; ++r) {
			if (r == c) continue;
			const double f = m[r][c];
			for (int k = 0; k < 6; ++k) m[r][k] -= f * m[c][k];
		}
	}
	for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) inv[r][c] = m[r][3 + c];
	return true;
}

// ─────────────────────────── 路径一：曲面拟合 ───────────────────────────
// 洞外环带（椭圆膨胀 9）上的已知像素拟合平面 a + b·u + c·v；u/v 按包围盒归一化。
// 迭代重加权把边缘的反锯齿和相邻物体压成离群点；内点率 ≥97% 且内点平方误差 ≤ 6/255² 才接受。
bool surfaceFill(Image& rgb, const Mask& hole, const Mask& known)
{
	int bx0 = rgb.w, by0 = rgb.h, bx1 = -1, by1 = -1;
	for (int y = 0; y < rgb.h; ++y)
		for (int x = 0; x < rgb.w; ++x)
			if (hole.at(x, y)) {
				bx0 = std::min(bx0, x); by0 = std::min(by0, y);
				bx1 = std::max(bx1, x); by1 = std::max(by1, y);
			}
	if (bx1 < 0) return false;
	const int bw = std::max(1, bx1 - bx0 + 1), bh = std::max(1, by1 - by0 + 1);
	const Mask ring0 = dilateEllipse(hole, 9);
	std::vector<int> pts;
	pts.reserve(4096);
	for (int y = 0; y < rgb.h; ++y)
		for (int x = 0; x < rgb.w; ++x)
			if (ring0.at(x, y) && known.at(x, y)) pts.push_back(y * rgb.w + x);
	if (pts.size() < 12) return false;
	const size_t stride = std::max<size_t>(1, pts.size() / 4096);

	auto basis = [&](int x, int y, double b[3]) {
		b[0] = 1.0;
		b[1] = (double)(x - bx0) / bw;
		b[2] = (double)(y - by0) / bh;
	};
	double coef[3][3] = {};        // [channel][term]
	for (int iter = 0; iter < 5; ++iter) {
		double normal[3][3] = {}, rhs[3][3] = {};
		for (size_t i = 0; i < pts.size(); i += stride) {
			const int p = pts[i];
			const int x = p % rgb.w, y = p / rgb.w;
			double b[3];
			basis(x, y, b);
			const float* c = rgb.at(x, y);
			double pred[3];
			for (int ch = 0; ch < 3; ++ch) pred[ch] = coef[ch][0] * b[0] + coef[ch][1] * b[1] + coef[ch][2] * b[2];
			double residual = 0;
			if (iter > 0) {
				double s = 0;
				for (int ch = 0; ch < 3; ++ch) { const double d = pred[ch] - c[ch]; s += d * d; }
				residual = std::sqrt(s);
			}
			const double wgt = iter == 0 ? 1.0 : std::min(1.0, 0.008 / std::max(1e-8, residual));
			for (int r = 0; r < 3; ++r) {
				for (int c2 = 0; c2 < 3; ++c2) normal[r][c2] += wgt * b[r] * b[c2];
				for (int ch = 0; ch < 3; ++ch) rhs[ch][r] += wgt * b[r] * c[ch];
			}
		}
		double inv[3][3];
		if (!invert3(normal, inv)) return false;
		for (int ch = 0; ch < 3; ++ch)
			for (int r = 0; r < 3; ++r)
				coef[ch][r] = inv[r][0] * rhs[ch][0] + inv[r][1] * rhs[ch][1] + inv[r][2] * rhs[ch][2];
	}
	size_t inliers = 0, count = 0;
	double squaredError = 0;
	for (size_t i = 0; i < pts.size(); i += stride) {
		const int p = pts[i];
		const int x = p % rgb.w, y = p / rgb.w;
		double b[3];
		basis(x, y, b);
		const float* c = rgb.at(x, y);
		double s = 0;
		for (int ch = 0; ch < 3; ++ch) {
			const double pred = coef[ch][0] * b[0] + coef[ch][1] * b[1] + coef[ch][2] * b[2];
			const double d = pred - c[ch];
			s += d * d;
		}
		const double residual = std::sqrt(s);
		++count;
		if (residual < 4.0 / 255.0) { ++inliers; squaredError += residual * residual; }
	}
	if (inliers * 100 < count * 97) return false;
	if (squaredError > (double)inliers * 6.0 / (255.0 * 255.0)) return false;

	for (int y = by0; y <= by1; ++y)
		for (int x = bx0; x <= bx1; ++x) {
			if (!hole.at(x, y)) continue;
			double b[3];
			basis(x, y, b);
			float* c = rgb.at(x, y);
			for (int ch = 0; ch < 3; ++ch) {
				const double pred = coef[ch][0] * b[0] + coef[ch][1] * b[1] + coef[ch][2] * b[2];
				c[ch] = (float)std::clamp(pred, 0.0, 1.0);
			}
		}
	return true;
}

// ─────────────────────────── donor 域 ───────────────────────────
// 先局部（到洞的距离 ≤ reach），patch 可用点不足 64 才把 reach 翻倍，直到 512
Mask donorDomain(const Mask& hole, const Mask& known)
{
	Plane dist;
	std::vector<int> label;
	chamferNearest(hole, label, &dist);
	int bx0 = hole.w, by0 = hole.h, bx1 = -1, by1 = -1;
	for (int y = 0; y < hole.h; ++y)
		for (int x = 0; x < hole.w; ++x)
			if (hole.at(x, y)) {
				bx0 = std::min(bx0, x); by0 = std::min(by0, y);
				bx1 = std::max(bx1, x); by1 = std::max(by1, y);
			}
	if (bx1 < 0) return Mask();
	int reach = std::clamp(std::min(bx1 - bx0 + 1, by1 - by0 + 1) / 2, 16, 96);
	for (;;) {
		Mask domain;
		domain.reset(hole.w, hole.h);
		for (int y = 0; y < hole.h; ++y)
			for (int x = 0; x < hole.w; ++x)
				domain.at(x, y) = (dist.at(x, y) <= (float)reach && known.at(x, y)) ? 1 : 0;
		const Mask patches = erodeRect(domain, 7);
		if (patches.count() >= 64 || reach >= 512) return domain;
		reach = std::min(512, reach * 2);
	}
}

// ─────────────────────────── 路径二：周期平移 ───────────────────────────
// 只在 donor 域内验证平移误差：单点误差 ≤0.5/255² 几乎等于"精确周期"，避免把无关矩形整块搬进洞
bool periodicFill(const Image& rgb, const Mask& hole, const Mask& domain, Image& out)
{
	std::vector<int> pts;
	pts.reserve(2048);
	for (int y = 0; y < rgb.h; ++y)
		for (int x = 0; x < rgb.w; ++x)
			if (domain.at(x, y)) pts.push_back(y * rgb.w + x);
	if (pts.size() < 32) return false;
	const double tol = 0.5 / (255.0 * 255.0);

	struct Dir { int dx, dy; };
	for (const Dir dir : { Dir{1, 0}, Dir{0, 1} }) {
		for (int period = 2; period <= kMaxPeriod; ++period) {
			const int ox = dir.dx * period, oy = dir.dy * period;
			int count = 0;
			double error = 0;
			bool abort = false;
			for (size_t i = 0; i < pts.size(); ++i) {
				const int p = pts[i];
				const int x = p % rgb.w, y = p / rgb.w;
				const int qx = x + ox, qy = y + oy;
				if (qx < 0 || qy < 0 || qx >= rgb.w || qy >= rgb.h) continue;
				if (!domain.at(qx, qy)) continue;
				const float* a = rgb.at(x, y);
				const float* b = rgb.at(qx, qy);
				double s = 0;
				for (int c = 0; c < 3; ++c) { const double d = (double)a[c] - b[c]; s += d * d; }
				error += s;
				++count;
				// 提前否决按最终样本数上界（pts.size()）算，比 count 更宽松，不会误杀真周期
				if (error > (double)pts.size() * tol) { abort = true; break; }
			}
			if (abort || count < 64 || error > (double)count * tol) continue;

			Image tmp;
			tmp = rgb;
			bool complete = true;
			const int span = std::max(rgb.w, rgb.h);
			for (int y = 0; y < rgb.h && complete; ++y)
				for (int x = 0; x < rgb.w; ++x) {
					if (!hole.at(x, y)) continue;
					bool found = false;
					for (int d = 1; d * period < span && !found; ++d) {
						for (int sign : { -1, 1 }) {
							const int qx = x + ox * d * sign, qy = y + oy * d * sign;
							if (qx < 0 || qy < 0 || qx >= rgb.w || qy >= rgb.h) continue;
							if (!domain.at(qx, qy)) continue;
							float* t = tmp.at(x, y);
							const float* s = rgb.at(qx, qy);
							t[0] = s[0]; t[1] = s[1]; t[2] = s[2];
							found = true;
							break;
						}
					}
					if (!found) { complete = false; break; }
				}
			if (complete) { out = std::move(tmp); return true; }
		}
	}
	return false;
}

// ─────────────────────────── 结构引导 ───────────────────────────
struct Guide {
	Image color;
	Plane variation;
};

// 加权观测均值 + 纹理强度；再沿 0°/90°/45°/135° 把两侧已知端点接进洞里
Guide buildGuide(const Image& lab, const Mask& hole, const Mask& known, int radius)
{
	const int w = lab.w, h = lab.h;
	Plane wgt, wr, wg, wb;
	wgt.reset(w, h); wr.reset(w, h); wg.reset(w, h); wb.reset(w, h);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			if (!known.at(x, y)) continue;
			const size_t i = (size_t)y * w + x;
			wgt.v[i] = 1.f;
			const float* c = lab.at(x, y);
			wr.v[i] = c[0]; wg.v[i] = c[1]; wb.v[i] = c[2];
		}
	blurPlane(wgt, radius);
	blurPlane(wr, radius);
	blurPlane(wg, radius);
	blurPlane(wb, radius);

	Guide g;
	g.color.reset(w, h);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			const size_t i = (size_t)y * w + x;
			const float denom = std::max(wgt.v[i], 1e-6f);
			float* c = g.color.at(x, y);
			c[0] = wr.v[i] / denom;
			c[1] = wg.v[i] / denom;
			c[2] = wb.v[i] / denom;
		}

	// 纹理强度：E[|c|²] − |mean|² 的开方，同样只用已知像素
	Plane sq;
	sq.reset(w, h);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			const size_t i = (size_t)y * w + x;
			if (!known.at(x, y)) continue;
			const float* c = lab.at(x, y);
			sq.v[i] = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
		}
	blurPlane(sq, radius);
	g.variation.reset(w, h);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			const size_t i = (size_t)y * w + x;
			const float* mean = g.color.at(x, y);
			const float v = sq.v[i] / std::max(wgt.v[i], 1e-6f) - dot3(mean, mean);
			g.variation.v[i] = std::sqrt(std::max(0.f, v));
		}

	// 方向延拓：给洞里的像素一个"结构上说得通"的颜色先验
	const float kFar = FLT_MAX / 4;
	Plane cost;
	cost.reset(w, h, kFar);
	struct Dir { int dx, dy; };
	for (const Dir dir : { Dir{1, 0}, Dir{0, 1}, Dir{1, 1}, Dir{1, -1} }) {
		for (int sy0 = 0; sy0 < h; ++sy0)
			for (int sx0 = 0; sx0 < w; ++sx0) {
				if (sx0 - dir.dx >= 0 && sy0 - dir.dy >= 0 && sy0 - dir.dy < h) continue;   // 不是这条线的起点
				int before = -1;
				std::vector<int> pending;
				auto flush = [&](int after) {
					if (pending.empty()) return;
					if (before < 0 && after < 0) return;
					const int a = before >= 0 ? before : after;
					const int b = after >= 0 ? after : before;
					const float* ca = g.color.at(a % w, a / w);
					const float* cb = g.color.at(b % w, b / w);
					float d0 = ca[0] - cb[0], d1 = ca[1] - cb[1], d2 = ca[2] - cb[2];
					const float difference = d0 * d0 + d1 * d1 + d2 * d2;
					const float span = (float)std::sqrt((double)((a % w - b % w) * (a % w - b % w) + (a / w - b / w) * (a / w - b / w)));
					const float score = difference + 0.02f * span + ((before < 0 || after < 0) ? 1000.f : 0.f);
					for (int p : pending) {
						if (score >= cost.v[(size_t)p]) continue;
						const float ta = g.variation.v[(size_t)a], tb = g.variation.v[(size_t)b];
						float t = 0;
						if (span > 0) {
							const float ddx = (float)(p % w - a % w), ddy = (float)(p / w - a / w);
							t = std::sqrt(ddx * ddx + ddy * ddy) / span;
						}
						t = std::clamp(t, 0.f, 1.f);
						float* c = g.color.at(p % w, p / w);
						if (difference < 900.f) {
							for (int k = 0; k < 3; ++k) c[k] = ca[k] * (1 - t) + cb[k] * t;
						}
						else {
							const float* src = (t < 0.5f) ? ca : cb;
							c[0] = src[0]; c[1] = src[1]; c[2] = src[2];
						}
						g.variation.v[(size_t)p] = ta * (1 - t) + tb * t;
						cost.v[(size_t)p] = score;
					}
					pending.clear();
				};
				int x = sx0, y = sy0;
				while (x >= 0 && y >= 0 && x < w && y < h) {
					const int p = y * w + x;
					if (known.at(x, y)) {
						flush(p);
						before = p;
					}
					else if (hole.at(x, y)) {
						pending.push_back(p);
					}
					else {
						flush(-1);
						before = -1;
					}
					x += dir.dx;
					y += dir.dy;
				}
				flush(-1);
			}
	}
	return g;
}

// ─────────────────────────── 路径三：PatchMatch ───────────────────────────
struct LevelState {
	Image image;
	std::vector<int> match;      // 每个像素的 donor 索引，-1 = 还没匹配
};

// 单层：候选生成（最近点 / 随机 / 邻居传播 / 缩小窗口搜索）→ 加权投票
LevelState fillLevel(const Image& src, const Mask& hole, const Mask& cov, const Mask& dom,
	const Guide& guide, const LevelState& prev, int passes, bool& ok)
{
	const int w = src.w, h = src.h;
	// donor：整块 patch 都落在已知域的像素；半径从 3 递减，找不到才放宽
	int patchR = kPatchR;
	Mask donors;
	std::vector<int> candidates;
	for (; patchR >= 0; --patchR) {
		donors = erodeRect(dom, 2 * patchR + 1);
		candidates.clear();
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				if (donors.at(x, y)) candidates.push_back(y * w + x);
		if (!candidates.empty()) break;
	}
	if (candidates.empty()) { ok = false; return {}; }

	// 每个洞像素的 patch 覆盖统计（已知 / 待补 / 是否完全在画布内）
	std::vector<int> knownCnt((size_t)w * h, 0), missCnt((size_t)w * h, 0);
	std::vector<BYTE> interior((size_t)w * h, 0);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			if (!hole.at(x, y)) continue;
			int kn = 0, ms = 0;
			for (int dy = -patchR; dy <= patchR; ++dy) {
				const int ty = y + dy;
				if (ty < 0 || ty >= h) continue;
				for (int dx = -patchR; dx <= patchR; ++dx) {
					const int tx = x + dx;
					if (tx < 0 || tx >= w || !cov.at(tx, ty)) continue;
					if (hole.at(tx, ty)) ++ms; else ++kn;
				}
			}
			const size_t i = (size_t)y * w + x;
			knownCnt[i] = kn; missCnt[i] = ms;
			interior[i] = (kn + ms == (2 * patchR + 1) * (2 * patchR + 1)) ? 1 : 0;
		}

	// 上一层的结果（更粗）上采样后作为本层起点：图像插值填进洞，匹配场按最近邻搬过来当首个候选
	const bool hasPrev = prev.image.w > 0 && prev.image.h > 0 && !prev.match.empty();
	Image output = src;
	if (hasPrev) {
		Image up;
		upscaleImage(prev.image, w, h, up);
		for (int y = 0; y < h; ++y)
			for (int x = 0; x < w; ++x)
				if (hole.at(x, y)) {
					float* d = output.at(x, y);
					const float* s = up.at(x, y);
					d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
				}
	}

	// 最近 donor（距离变换标签），用作候选与兜底
	std::vector<int> nearestLabel;
	chamferNearest(donors, nearestLabel);

	std::vector<int> match((size_t)w * h, -1);
	std::vector<float> costs((size_t)w * h, FLT_MAX);
	std::vector<int> holes;
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			if (hole.at(x, y)) holes.push_back(y * w + x);

	// donor 像素 → candidates 下标，统计复用次数时直接查表
	std::vector<int> donorSlot((size_t)w * h, -1);
	for (size_t k = 0; k < candidates.size(); ++k) donorSlot[(size_t)candidates[k]] = (int)k;
	std::vector<int> candCount(candidates.size(), 0);
	std::vector<float> penalty((size_t)w * h, 0.f);

	std::mt19937 rng(0x534e4f57u);
	const float scale = (float)std::max(w, h);
	float synthW = 0.05f;                 // 迭代开始前的那次初始化，与原版同档

	auto targetNorm = [&](size_t i) { return std::max(1.f, (float)knownCnt[i] + (float)missCnt[i] * synthW); };
	auto structureWeight = [&](size_t i) { return 8.f / (4.f + guide.variation.v[i]); };

	// 候选评估：结构项先剪枝，再累加 patch 内逐像素 SSD（合成像素按 synthW 权重）
	auto tryCandidate = [&](int x, int y, size_t i, int sx, int sy) {
		if (sx < 0 || sy < 0 || sx >= w || sy >= h || !donors.at(sx, sy)) return;
		const float* gTarget = guide.color.at(x, y);
		const float* gSource = guide.color.at(sx, sy);
		float d0 = gTarget[0] - gSource[0], d1 = gTarget[1] - gSource[1], d2 = gTarget[2] - gSource[2];
		const float tDelta = guide.variation.v[i] - guide.variation.v[(size_t)sy * w + sx];
		const float distX = (float)(sx - x), distY = (float)(sy - y);
		const float fixed = structureWeight(i) * (d0 * d0 + d1 * d1 + d2 * d2) + 4.f * tDelta * tDelta
			+ penalty[(size_t)sy * w + sx] + 8.f * (distX * distX + distY * distY) / (scale * scale);
		const float best = costs[i];
		const float limit = best + 1e-5f * std::max(1.f, best);
		if (fixed > limit) return;
		const bool inside = interior[i] != 0;
		const float norm = targetNorm(i);
		float sum = 0;
		for (int py = -patchR; py <= patchR; ++py) {
			const int ty = y + py;
			if (ty < 0 || ty >= h) continue;
			const int sySample = sy + py;
			for (int px = -patchR; px <= patchR; ++px) {
				const int tx = x + px;
				if (tx < 0 || tx >= w) continue;
				if (!inside && !cov.at(tx, ty)) continue;
				const float* a = output.at(tx, ty);
				const float* b = src.at(sx + px, sySample);
				float e0 = a[0] - b[0], e1 = a[1] - b[1], e2 = a[2] - b[2];
				const float wgt = hole.at(tx, ty) ? synthW : 1.f;
				sum += (e0 * e0 + e1 * e1 + e2 * e2) * wgt;
			}
			if (sum / norm + fixed > limit) return;
		}
		const float cost = sum / norm + fixed;
		if (cost < best) {
			costs[i] = cost;
			match[i] = sy * w + sx;
		}
	};

	// 最粗层没有上层可用：最近 donor + 32 次随机抽样，代价只看引导图颜色与纹理；
	// 细化层：把上层的匹配场按比例搬过来，代价不足再用最近 donor 兜底
	for (int p : holes) {
		const int x = p % w, y = p / w;
		const size_t i = (size_t)p;
		if (hasPrev) {
			// 本层坐标 → 上层坐标（最近邻）
			auto toPrev = [](int v, int curN, int prevN) {
				return std::clamp((int)std::lround((v + 0.5f) * prevN / curN - 0.5f), 0, std::max(0, prevN - 1));
			};
			const int px = toPrev(x, w, prev.image.w), py = toPrev(y, h, prev.image.h);
			const int q = prev.match[(size_t)py * prev.image.w + px];
			if (q >= 0) {
				const int sx = toPrev(q % prev.image.w, prev.image.w, w);
				const int sy = toPrev(q / prev.image.w, prev.image.h, h);
				tryCandidate(x, y, i, sx, sy);
			}
			if (match[i] < 0 && nearestLabel[i] >= 0) {
				const int n = nearestLabel[i];
				tryCandidate(x, y, i, n % w, n / w);
			}
			continue;
		}
		int best = nearestLabel[i];
		if (best < 0) best = candidates[(size_t)rng() % candidates.size()];
		const float* gt = guide.color.at(x, y);
		const float* gs = guide.color.at(best % w, best / w);
		float d0 = gt[0] - gs[0], d1 = gt[1] - gs[1], d2 = gt[2] - gs[2];
		float tv = guide.variation.v[i] - guide.variation.v[(size_t)best];
		float bestCost = d0 * d0 + d1 * d1 + d2 * d2 + 4.f * tv * tv;
		for (int s = 0; s < 32; ++s) {
			const int c = candidates[(size_t)rng() % candidates.size()];
			gs = guide.color.at(c % w, c / w);
			d0 = gt[0] - gs[0]; d1 = gt[1] - gs[1]; d2 = gt[2] - gs[2];
			tv = guide.variation.v[i] - guide.variation.v[(size_t)c];
			const float cost = d0 * d0 + d1 * d1 + d2 * d2 + 4.f * tv * tv;
			if (cost < bestCost) { bestCost = cost; best = c; }
		}
		match[i] = best;
		// 初始像素直接取 donor 颜色（Lab 空间），后续迭代会重算
		const float* s = src.at(best % w, best / w);
		float* d = output.at(x, y);
		d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
	}

	const int threadCount = std::clamp((int)std::thread::hardware_concurrency(), 1, 8);
	for (int iter = 0; iter < passes; ++iter) {
		synthW = 0.15f + 0.20f * (float)iter / (float)std::max(1, passes - 1);
		// 复用惩罚：一个 donor 被越多次引用，越说明它在硬撑，代价抬高
		std::fill(candCount.begin(), candCount.end(), 0);
		for (int p : holes) {
			const int q = match[(size_t)p];
			if (q < 0) continue;
			const int slot = donorSlot[(size_t)q];
			if (slot >= 0) candCount[(size_t)slot]++;
		}
		const float expected = std::max(1.f, (float)holes.size() / (float)candidates.size());
		std::fill(penalty.begin(), penalty.end(), 0.f);
		for (size_t k = 0; k < candidates.size(); ++k)
			penalty[(size_t)candidates[k]] = 25.f * std::log1p((float)candCount[k] / expected);

		const int step = (iter % 2 == 0) ? 1 : -1;
		const int spanCount = (int)holes.size();
		// 每个洞像素的候选搜索互相独立（只用邻居上一轮的值做传播），按连续分段并行
		auto searchSpan = [&](size_t begin, size_t end, unsigned seed) {
			std::mt19937 local(seed);
			for (size_t si = begin; si < end; ++si) {
				const int p = holes[step > 0 ? si : spanCount - 1 - si];
				const int x = p % w, y = p / w;
				const size_t i = (size_t)p;
				costs[i] = FLT_MAX;
				const int old = match[i];
				if (old >= 0) tryCandidate(x, y, i, old % w, old / w);
				const int nearDonor = nearestLabel[i];
				if (nearDonor >= 0) tryCandidate(x, y, i, nearDonor % w, nearDonor / w);
				const int seedPixel = candidates[(size_t)local() % candidates.size()];
				tryCandidate(x, y, i, seedPixel % w, seedPixel / w);
				const int nx = x - step;
				if (nx >= 0 && nx < w) {
					const int nb = match[(size_t)y * w + nx];
					if (nb >= 0) tryCandidate(x, y, i, nb % w + step, nb / w);
				}
				const int ny = y - step;
				if (ny >= 0 && ny < h) {
					const int nb = match[(size_t)ny * w + x];
					if (nb >= 0) tryCandidate(x, y, i, nb % w, nb / w + step);
				}
				for (int window = (int)std::max(w, h); window > 0; window /= 2) {
					const int cur = match[i];
					if (cur < 0) break;
					const int span = window * 2 + 1;
					const int sx = cur % w + (int)(local() % (unsigned)span) - window;
					const int sy = cur / w + (int)(local() % (unsigned)span) - window;
					tryCandidate(x, y, i, sx, sy);
				}
			}
		};
		{
			std::vector<std::thread> workers;
			for (int t = 0; t < threadCount; ++t) {
				const size_t begin = (size_t)holes.size() * t / threadCount;
				const size_t end = (size_t)holes.size() * (t + 1) / threadCount;
				if (begin >= end) continue;
				workers.emplace_back(searchSpan, begin, end, 0x534e4f57u + (unsigned)t * 7919u);
			}
			for (auto& worker : workers) worker.join();
		}

		// 投票：7×7 内的洞像素互为担保，权重看代价、引导差和细节差
		Image next = output;
		auto voteSpan = [&](size_t begin, size_t end) {
			for (size_t hi = begin; hi < end; ++hi) {
				const int p = holes[hi];
				const int x = p % w, y = p / w;
				const size_t i = (size_t)p;
				const int center = match[i];
				if (center < 0) continue;
				const float* anchor = src.at(center % w, center / w);
				const float tolerance = std::clamp(guide.variation.v[i] * 0.25f, 1.5f, 10.f);
				const float* gTarget = guide.color.at(x, y);
				float sum0 = 0, sum1 = 0, sum2 = 0, wsum = 0;
				for (int dy = -patchR; dy <= patchR; ++dy) {
					const int ny = y + dy;
					if (ny < 0 || ny >= h) continue;
					for (int dx = -patchR; dx <= patchR; ++dx) {
						const int nx = x + dx;
						if (nx < 0 || nx >= w || !hole.at(nx, ny)) continue;
						const int donor = match[(size_t)ny * w + nx];
						if (donor < 0) continue;
						const int sx = donor % w - dx, sy = donor / w - dy;
						if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
						const float* value = src.at(sx, sy);
						const float* gDonor = guide.color.at(sx, sy);
						float g0 = gDonor[0] - gTarget[0], g1 = gDonor[1] - gTarget[1], g2 = gDonor[2] - gTarget[2];
						float d0 = value[0] - anchor[0], d1 = value[1] - anchor[1], d2 = value[2] - anchor[2];
						const float wgt = 1.f / ((1.f + costs[(size_t)ny * w + nx])
							* (1.f + (g0 * g0 + g1 * g1 + g2 * g2) / 100.f)
							* (1.f + (d0 * d0 + d1 * d1 + d2 * d2) / (tolerance * tolerance)));
						sum0 += value[0] * wgt;
						sum1 += value[1] * wgt;
						sum2 += value[2] * wgt;
						wsum += wgt;
					}
				}
				if (wsum > 0) {
					float* d = next.at(x, y);
					d[0] = sum0 / wsum;
					d[1] = sum1 / wsum;
					d[2] = sum2 / wsum;
				}
			}
		};
		{
			std::vector<std::thread> workers;
			for (int t = 0; t < threadCount; ++t) {
				const size_t begin = (size_t)holes.size() * t / threadCount;
				const size_t end = (size_t)holes.size() * (t + 1) / threadCount;
				if (begin >= end) continue;
				workers.emplace_back(voteSpan, begin, end);
			}
			for (auto& worker : workers) worker.join();
		}
		output = std::move(next);
	}
	return { output, match };
}

}

namespace smarterase {

bool fill(BYTE* bits, UINT32 pitch, int width, int height, const BYTE* maskIn)
{
	if (!bits || !maskIn || width < 8 || height < 8) return false;

	// 先按洞的包围盒裁剪工作区：4K 截图也只算局部，省内存也省时间
	int hx0 = width, hy0 = height, hx1 = -1, hy1 = -1;
	size_t holeCount = 0;
	for (int y = 0; y < height; ++y)
		for (int x = 0; x < width; ++x)
			if (maskIn[(size_t)y * width + x]) {
				++holeCount;
				hx0 = std::min(hx0, x); hy0 = std::min(hy0, y);
				hx1 = std::max(hx1, x); hy1 = std::max(hy1, y);
			}
	if (hx1 < 0) return false;
	// 已知像素不到一半时 donor 不够，补出来只会是瞎猜
	if (holeCount * 2 > (size_t)width * height) return false;

	const int holeW = hx1 - hx0 + 1, holeH = hy1 - hy0 + 1;
	const int margin = std::clamp(std::min(holeW, holeH) / 2, 16, 96) + 24;
	const int cw = std::min(width, holeW + margin * 2), ch = std::min(height, holeH + margin * 2);
	if ((size_t)cw * ch > kMaxWorkingPixels) return false;
	const int cx0 = std::max(0, std::min(width - cw, (hx0 + hx1 - cw + 1) / 2));
	const int cy0 = std::max(0, std::min(height - ch, (hy0 + hy1 - ch + 1) / 2));

	Mask cov, hole;
	cov.reset(cw, ch);
	hole.reset(cw, ch);
	Image rgb;
	rgb.reset(cw, ch);
	size_t knownCount = 0;
	for (int y = 0; y < ch; ++y) {
		const BYTE* row = bits + (size_t)(cy0 + y) * pitch;
		for (int x = 0; x < cw; ++x) {
			const BYTE a = row[(size_t)(cx0 + x) * 4 + 3];
			const size_t i = (size_t)y * cw + x;
			cov.v[i] = a > 0 ? 1 : 0;
			const bool isHole = maskIn[(size_t)(cy0 + y) * width + cx0 + x] != 0 && cov.v[i];
			hole.v[i] = isHole ? 1 : 0;
			if (!isHole && cov.v[i]) ++knownCount;
			float* c = rgb.at(x, y);
			c[0] = (float)row[(size_t)(cx0 + x) * 4 + 2] / 255.f;
			c[1] = (float)row[(size_t)(cx0 + x) * 4 + 1] / 255.f;
			c[2] = (float)row[(size_t)(cx0 + x) * 4 + 0] / 255.f;
		}
	}
	if (knownCount == 0) return false;

	Mask known;
	known.reset(cw, ch);
	for (size_t i = 0; i < hole.v.size(); ++i) known.v[i] = (cov.v[i] && !hole.v[i]) ? 1 : 0;

	// 捷径一：纯色 / 线性渐变
	if (!surfaceFill(rgb, hole, known)) {
		// 捷径二：精确周期
		const Mask domain = donorDomain(hole, known);
		Image periodic;
		if (periodicFill(rgb, hole, domain, periodic)) {
			rgb = std::move(periodic);
		}
		else {
			// 主路径：结构引导 + 金字塔 PatchMatch。入口已按洞的包围盒裁好工作区，
			// 洞的尺寸在入口已经算过，这里直接用，不再重复扫一遍
			const int guideRadius = std::clamp(std::min(holeW, holeH) / 12, 4, 16);
			Image lab;
			rgbToLab(rgb, lab);
			// 引导图必须只由 known 生成：洞内像素一旦参与，均值会把待擦掉的内容带回来，
			// 四方向延拓也会因洞像素被判成 known 而整段失效
			const Guide guide = buildGuide(lab, hole, known, guideRadius);

			// 金字塔：每层洞先 5×5 膨胀再面积缩放（任一点为洞即算洞），
			// 已知 / 域先 5×5 腐蚀再缩放、要求几乎全为 1（整块都在），可用点 <16 就停
			std::vector<Image> images{ lab };
			std::vector<Guide> guides{ guide };
			std::vector<Mask> holes{ hole }, covs{ cov }, doms{ domain };
			while (std::max(images.back().w, images.back().h) > 64 && std::min(images.back().w, images.back().h) > 8) {
				Image reduced;
				pyrDownImage(images.back(), reduced);

				const Mask dilatedHole = dilateRect(holes.back(), 5);
				Plane ph, phDown;
				ph.reset(dilatedHole.w, dilatedHole.h);
				for (size_t i = 0; i < ph.v.size(); ++i) ph.v[i] = dilatedHole.v[i] ? 1.f : 0.f;
				areaDownPlane(ph, phDown);
				Mask reducedHole;
				reducedHole.reset(phDown.w, phDown.h);
				for (size_t i = 0; i < phDown.v.size(); ++i) reducedHole.v[i] = phDown.v[i] > 0.f ? 1 : 0;

				Plane pc, pd, rc, rd;
				const Mask erodedCov = erodeRect(covs.back(), 5);
				const Mask erodedDom = erodeRect(doms.back(), 5);
				pc.reset(erodedCov.w, erodedCov.h);
				pd.reset(erodedDom.w, erodedDom.h);
				for (size_t i = 0; i < pc.v.size(); ++i) pc.v[i] = erodedCov.v[i] ? 1.f : 0.f;
				for (size_t i = 0; i < pd.v.size(); ++i) pd.v[i] = erodedDom.v[i] ? 1.f : 0.f;
				areaDownPlane(pc, rc);
				areaDownPlane(pd, rd);
				Mask reducedCov, reducedDom;
				reducedCov.reset(rc.w, rc.h);
				reducedDom.reset(rd.w, rd.h);
				for (size_t i = 0; i < rc.v.size(); ++i) {
					const bool eligible = !reducedHole.v[i];
					reducedCov.v[i] = (eligible && rc.v[i] > 0.996f) ? 1 : 0;
					reducedDom.v[i] = (eligible && rd.v[i] > 0.996f) ? 1 : 0;
				}
				if (erodeRect(reducedDom, 7).count() < 16) break;

				Guide reducedGuide;
				pyrDownImage(guides.back().color, reducedGuide.color);
				pyrDownPlane(guides.back().variation, reducedGuide.variation);

				images.push_back(std::move(reduced));
				guides.push_back(std::move(reducedGuide));
				holes.push_back(std::move(reducedHole));
				covs.push_back(std::move(reducedCov));
				doms.push_back(std::move(reducedDom));
			}

			LevelState filled;
			bool ok = true;
			for (size_t i = images.size(); i-- > 0;) {
				const int passes = (i == images.size() - 1) ? kPassesCoarse
					: (i == 0 ? kPassesFine : kPassesMid);
				filled = fillLevel(images[i], holes[i], covs[i], doms[i], guides[i], filled, passes, ok);
				if (!ok) break;
			}
			if (!ok) return false;

			labToRgb(filled.image);
			rgb = std::move(filled.image);
		}
	}

	// 只写洞里像素，alpha 原样保留
	for (int y = 0; y < ch; ++y) {
		BYTE* row = bits + (size_t)(cy0 + y) * pitch;
		for (int x = 0; x < cw; ++x) {
			if (!hole.at(x, y)) continue;
			const float* c = rgb.at(x, y);
			const size_t px = (size_t)(cx0 + x) * 4;
			row[px + 2] = (BYTE)std::clamp((int)std::lround(c[0] * 255.f), 0, 255);
			row[px + 1] = (BYTE)std::clamp((int)std::lround(c[1] * 255.f), 0, 255);
			row[px + 0] = (BYTE)std::clamp((int)std::lround(c[2] * 255.f), 0, 255);
		}
	}
	return true;
}

}
