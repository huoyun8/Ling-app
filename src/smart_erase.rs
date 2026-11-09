//! 智能消除：曲面拟合 → 精确周期平移 → 结构引导 + 金字塔 PatchMatch + 邻域投票。
//!
//! 约定：
//! `bits` 是 BGRA8、`pitch` 字节/行
//! 即 `px+0=B, px+1=G, px+2=R, px+3=A`。
//! - `mask[i]` 非 0 表示该像素要修掉（选框内部），其余是已知像素。
//! 返回 `false` = 算法放弃（`fill` 返回 false / 上层建位图失败），调用方保留
//!   品红占位；此时 `bits` 可能已被部分修改，调用方应丢弃这次缓冲。
//! alpha 一律不写： 只写 R/G/B 三个字节，cov 里的 alpha 只参与"是否有内容"的判断
//!
//! 并行：搜索阶段把洞像素按连续区间分给多条线程，每段一条独立的 `mt19937`
//!   （种子 `0x534e4f57 + t * 7919`）。线程间会读邻居的匹配结果做传播，
//!   因此跨线程共享表统一用 relaxed 原子访问（避免数据竞争 UB），
//!   但同一输入在不同调度下结果可能不逐像素相同；投票阶段写 next、读 output，无竞争。

use std::sync::atomic::{AtomicI32, AtomicU32, Ordering as AtomicOrdering};

const K_PATCH_R: i32 = 3; // patch 半径 3 → 7×7
const K_MAX_PERIOD: i32 = 64; // 周期探测上限
const K_MAX_WORKING_PIXELS: usize = 4 * 1024 * 1024; // 交互上限，超则失败保留占位
const K_PASSES_COARSE: i32 = 5;
const K_PASSES_MID: i32 = 3;
const K_PASSES_FINE: i32 = 2;
/// 这里作为"搜索阶段的分段数"使用（串行执行，见文件头）
const K_SEARCH_SPANS: usize = 8;
const K_SURFACE_RING: i32 = 9; // surfaceFill 的椭圆膨胀半径
const K_SURFACE_ITERS: usize = 5; // 迭代重加权次数
const K_DONOR_HALF_MIN: i32 = 16; // donorDomain 的 reach 下限
const K_DONOR_HALF_MAX: i32 = 96; // donorDomain 的 reach 上限
const K_DONOR_REACH_CAP: i32 = 512; // reach 翻倍的天花板

// ─────────────────────────── RNG：std::mt19937 等价实现 ───────────────────────────
/// 与 `std::mt19937` 逐位一致（32 位种子初始化 + 624 状态扭转 + 标准 tempering），
/// 取模方式 的 `rng % n`（无偏置修改）
struct Mt19937 {
    mt: [u32; 624],
    idx: usize,
}

impl Mt19937 {
    fn new(seed: u32) -> Self {
        let mut mt = [0u32; 624];
        mt[0] = seed;
        for i in 1..624 {
            mt[i] = 1812433253u32
                .wrapping_mul(mt[i - 1] ^ (mt[i - 1] >> 30))
                .wrapping_add(i as u32);
        }
        Self { mt, idx: 624 }
    }

    fn twist(&mut self) {
        for i in 0..624 {
            let x = (self.mt[i] & 0x8000_0000) | (self.mt[(i + 1) % 624] & 0x7fff_ffff);
            let mut xa = x >> 1;
            if x & 1 != 0 {
                xa ^= 0x9908_b0df;
            }
            self.mt[i] = self.mt[(i + 397) % 624] ^ xa;
        }
        self.idx = 0;
    }

    fn next_u32(&mut self) -> u32 {
        if self.idx >= 624 {
            self.twist();
        }
        let mut y = self.mt[self.idx];
        self.idx += 1;
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c_5680;
        y ^= (y << 15) & 0xefc6_0000;
        y ^= y >> 18;
        y
    }
}

#[derive(Clone)]
struct Mask {
    w: usize,
    h: usize,
    v: Vec<u8>,
}

impl Mask {
    fn new(w: usize, h: usize) -> Self {
        Self {
            w,
            h,
            v: vec![0u8; w * h],
        }
    }
    #[inline]
    fn at(&self, x: usize, y: usize) -> u8 {
        self.v[y * self.w + x]
    }
    /// `Mask::count`：非零像素数
    fn count(&self) -> usize {
        self.v.iter().filter(|&&b| b != 0).count()
    }
}

#[derive(Clone)]
struct Plane {
    w: usize,
    h: usize,
    v: Vec<f32>,
}

impl Plane {
    fn new(w: usize, h: usize, fill: f32) -> Self {
        Self {
            w,
            h,
            v: vec![fill; w * h],
        }
    }
    #[inline]
    fn at(&self, x: usize, y: usize) -> f32 {
        self.v[y * self.w + x]
    }
}

/// 三通道（RGB 或 Lab 共用），逐像素连续存 3 个 f32
#[derive(Clone)]
struct Image {
    w: usize,
    h: usize,
    v: Vec<f32>,
}

impl Image {
    fn new(w: usize, h: usize) -> Self {
        Self {
            w,
            h,
            v: vec![0f32; w * h * 3],
        }
    }
    #[inline]
    fn at(&self, x: usize, y: usize) -> &[f32] {
        let i = (y * self.w + x) * 3;
        &self.v[i..i + 3]
    }
    #[inline]
    fn at_mut(&mut self, x: usize, y: usize) -> &mut [f32] {
        let i = (y * self.w + x) * 3;
        &mut self.v[i..i + 3]
    }
}

#[inline]
fn dot3(a: &[f32], b: &[f32]) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

// ─────────────────────────── 图像基础操作 ───────────────────────────
/// `gaussKernel`（50–63）
fn gauss_kernel(radius: i32) -> Vec<f32> {
    let sigma = 0.5f64.max(radius as f64 / 2.0);
    let mut k = vec![0f32; (radius * 2 + 1) as usize];
    let mut sum = 0f64;
    for i in -radius..=radius {
        let t = i as f64 / sigma;
        let wgt = (-0.5 * t * t).exp() as f32;
        k[(i + radius) as usize] = wgt;
        sum += wgt as f64;
    }
    for wgt in k.iter_mut() {
        *wgt = (*wgt as f64 / sum) as f32;
    }
    k
}

/// `blurPlane`（65–84）：可分离高斯，边缘 clamp
fn blur_plane(p: &mut Plane, radius: i32) {
    if radius <= 0 || p.w <= 1 || p.h <= 1 {
        return;
    }
    let k = gauss_kernel(radius);
    let (w, h) = (p.w, p.h);
    let mut tmp = Plane::new(w, h, 0.0);
    for y in 0..h {
        for x in 0..w {
            let mut s = 0f32;
            for i in -radius..=radius {
                let xx = (x as i32 + i).clamp(0, w as i32 - 1) as usize;
                s += k[(i + radius) as usize] * p.v[y * w + xx];
            }
            tmp.v[y * w + x] = s;
        }
    }
    for y in 0..h {
        for x in 0..w {
            let mut s = 0f32;
            for i in -radius..=radius {
                let yy = (y as i32 + i).clamp(0, h as i32 - 1) as usize;
                s += k[(i + radius) as usize] * tmp.v[yy * w + x];
            }
            p.v[y * w + x] = s;
        }
    }
}

/// `pyrDownPlane`（87–108）：5 点高斯降采样 [1 4 6 4 1]/16
fn pyr_down_plane(src: &Plane) -> Plane {
    const K: [f32; 5] = [1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0];
    let dw = (src.w + 1) / 2;
    let dh = (src.h + 1) / 2;
    let mut dst = Plane::new(dw, dh, 0.0);
    let mut tmp = Plane::new(src.w, dh, 0.0);
    for y in 0..dh {
        let sy = (y as i32 * 2 - 1).clamp(0, src.h as i32 - 1) as usize;
        for x in 0..src.w {
            let mut s = 0f32;
            for i in -2i32..=2 {
                let xx = (x as i32 + i).clamp(0, src.w as i32 - 1) as usize;
                s += K[(i + 2) as usize] * src.v[sy * src.w + xx];
            }
            tmp.v[y * src.w + x] = s;
        }
    }
    for y in 0..dh {
        for x in 0..dw {
            let mut s = 0f32;
            for i in -2i32..=2 {
                let xx = (x as i32 * 2 + i).clamp(0, src.w as i32 - 1) as usize;
                s += K[(i + 2) as usize] * tmp.v[y * src.w + xx];
            }
            dst.v[y * dw + x] = s;
        }
    }
    dst
}

/// `pyrDownImage`（110–141）
fn pyr_down_image(src: &Image) -> Image {
    const K: [f32; 5] = [1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0];
    let dw = (src.w + 1) / 2;
    let dh = (src.h + 1) / 2;
    let mut dst = Image::new(dw, dh);
    let mut tmp = Image::new(src.w, dh);
    for y in 0..dh {
        let sy = (y as i32 * 2 - 1).clamp(0, src.h as i32 - 1) as usize;
        for x in 0..src.w {
            let mut s = [0f32; 3];
            for i in -2i32..=2 {
                let xx = (x as i32 + i).clamp(0, src.w as i32 - 1) as usize;
                let kk = K[(i + 2) as usize];
                let pv = &src.v[(sy * src.w + xx) * 3..][..3];
                s[0] += kk * pv[0];
                s[1] += kk * pv[1];
                s[2] += kk * pv[2];
            }
            let q = &mut tmp.v[(y * src.w + x) * 3..][..3];
            q[0] = s[0];
            q[1] = s[1];
            q[2] = s[2];
        }
    }
    for y in 0..dh {
        for x in 0..dw {
            let mut s = [0f32; 3];
            for i in -2i32..=2 {
                let xx = (x as i32 * 2 + i).clamp(0, src.w as i32 - 1) as usize;
                let kk = K[(i + 2) as usize];
                let pv = &tmp.v[(y * src.w + xx) * 3..][..3];
                s[0] += kk * pv[0];
                s[1] += kk * pv[1];
                s[2] += kk * pv[2];
            }
            let q = &mut dst.v[(y * dw + x) * 3..][..3];
            q[0] = s[0];
            q[1] = s[1];
            q[2] = s[2];
        }
    }
    dst
}

/// `upscaleImage`（144–170）：双线性放大
fn upscale_image(src: &Image, w: usize, h: usize) -> Image {
    let mut dst = Image::new(w, h);
    if src.w == 0 || src.h == 0 {
        return dst;
    }
    for y in 0..h {
        let mut fy = (y as f32 + 0.5) * src.h as f32 / h as f32 - 0.5;
        fy = fy.clamp(0.0, src.h as f32 - 1.0);
        let y0 = fy.floor() as usize;
        let y1 = (y0 + 1).min(src.h - 1);
        let ty = fy - y0 as f32;
        for x in 0..w {
            let mut fx = (x as f32 + 0.5) * src.w as f32 / w as f32 - 0.5;
            fx = fx.clamp(0.0, src.w as f32 - 1.0);
            let x0 = fx.floor() as usize;
            let x1 = (x0 + 1).min(src.w - 1);
            let tx = fx - x0 as f32;
            let p00 = src.at(x0, y0);
            let p10 = src.at(x1, y0);
            let p01 = src.at(x0, y1);
            let p11 = src.at(x1, y1);
            let q = dst.at_mut(x, y);
            for c in 0..3 {
                let a = p00[c] * (1.0 - tx) + p10[c] * tx;
                let b = p01[c] * (1.0 - tx) + p11[c] * tx;
                q[c] = a * (1.0 - ty) + b * ty;
            }
        }
    }
    dst
}

/// `areaDownPlane`（173–190）：面积均值降采样（掩膜用）
fn area_down_plane(src: &Plane) -> Plane {
    let dw = ((src.w + 1) / 2).max(1);
    let dh = ((src.h + 1) / 2).max(1);
    let mut dst = Plane::new(dw, dh, 0.0);
    for y in 0..dh {
        for x in 0..dw {
            let mut s = 0f32;
            let mut n = 0i32;
            for dy in 0..2 {
                for dx in 0..2 {
                    let sx = x * 2 + dx;
                    let sy = y * 2 + dy;
                    if sx >= src.w || sy >= src.h {
                        continue;
                    }
                    s += src.at(sx, sy);
                    n += 1;
                }
            }
            dst.v[y * dw + x] = if n != 0 { s / n as f32 } else { 0.0 };
        }
    }
    dst
}

/// `erodeRect`（195–216）：出界按 0（BORDER_CONSTANT=0），可分离
fn erode_rect(m: &Mask, k: i32) -> Mask {
    let (w, h) = (m.w, m.h);
    let mut out = Mask::new(w, h);
    if k <= 1 {
        out.v.copy_from_slice(&m.v);
        return out;
    }
    let r = k / 2;
    let mut tmp = Mask::new(w, h);
    for y in 0..h {
        for x in 0..w {
            let mut all = 1u8;
            let mut i = x as i32 - r;
            while i <= x as i32 + r && all != 0 {
                all = if i >= 0 && i < w as i32 {
                    m.v[y * w + i as usize]
                } else {
                    0
                };
                i += 1;
            }
            tmp.v[y * w + x] = all;
        }
    }
    for y in 0..h {
        for x in 0..w {
            let mut all = 1u8;
            let mut i = y as i32 - r;
            while i <= y as i32 + r && all != 0 {
                all = if i >= 0 && i < h as i32 {
                    tmp.v[i as usize * w + x]
                } else {
                    0
                };
                i += 1;
            }
            out.v[y * w + x] = all;
        }
    }
    out
}

/// `dilateRect`（218–239）：出界按 clamp（不是 0）
fn dilate_rect(m: &Mask, k: i32) -> Mask {
    let (w, h) = (m.w, m.h);
    let mut out = Mask::new(w, h);
    if k <= 1 {
        out.v.copy_from_slice(&m.v);
        return out;
    }
    let r = k / 2;
    let mut tmp = Mask::new(w, h);
    for y in 0..h {
        for x in 0..w {
            let mut any = 0u8;
            for i in 0..=2 * r {
                let xx = (x as i32 + i - r).clamp(0, w as i32 - 1) as usize;
                if m.v[y * w + xx] != 0 {
                    any = 1;
                    break;
                }
            }
            tmp.v[y * w + x] = any;
        }
    }
    for y in 0..h {
        for x in 0..w {
            let mut any = 0u8;
            for i in 0..=2 * r {
                let yy = (y as i32 + i - r).clamp(0, h as i32 - 1) as usize;
                if tmp.v[yy * w + x] != 0 {
                    any = 1;
                    break;
                }
            }
            out.v[y * w + x] = any;
        }
    }
    out
}

/// `dilateEllipse`（242–272）：椭圆核膨胀，只扫掩膜包围盒外扩 r 的范围
fn dilate_ellipse(m: &Mask, k: i32) -> Mask {
    let (w, h) = (m.w, m.h);
    let mut out = Mask::new(w, h);
    if k <= 1 {
        out.v.copy_from_slice(&m.v);
        return out;
    }
    let r = k / 2;
    let rr = (r * r) as f32 + 0.5;
    let mut bx0 = w as i32;
    let mut by0 = h as i32;
    let mut bx1 = -1i32;
    let mut by1 = -1i32;
    for y in 0..h {
        for x in 0..w {
            if m.v[y * w + x] != 0 {
                bx0 = bx0.min(x as i32);
                by0 = by0.min(y as i32);
                bx1 = bx1.max(x as i32);
                by1 = by1.max(y as i32);
            }
        }
    }
    if bx1 < 0 {
        return out;
    }
    let bx0 = (bx0 - r).max(0);
    let by0 = (by0 - r).max(0);
    let bx1 = (bx1 + r).min(w as i32 - 1);
    let by1 = (by1 + r).min(h as i32 - 1);
    for y in by0..=by1 {
        for x in bx0..=bx1 {
            let mut any = 0u8;
            'outer: for dy in -r..=r {
                for dx in -r..=r {
                    if (dx * dx + dy * dy) as f32 > rr {
                        continue;
                    }
                    let sx = x + dx;
                    let sy = y + dy;
                    if sx < 0 || sy < 0 || sx >= w as i32 || sy >= h as i32 {
                        continue;
                    }
                    if m.v[sy as usize * w + sx as usize] != 0 {
                        any = 1;
                        break 'outer;
                    }
                }
            }
            out.v[y as usize * w + x as usize] = any;
        }
    }
    out
}

/// 两趟 chamfer 距离变换 + 最近种子标签（种子 = 非零像素）。
/// 返回 `(label, dist)`，的 `distOut` 是可选的，这里总是算出来（调用点按需取用）
fn chamfer_nearest(seed: &Mask) -> (Vec<i32>, Plane) {
    let (w, h) = (seed.w, seed.h);
    const K_FAR: f32 = f32::MAX / 4.0;
    let mut dist = Plane::new(w, h, K_FAR);
    let mut label = vec![-1i32; w * h];
    for y in 0..h {
        for x in 0..w {
            if seed.v[y * w + x] != 0 {
                dist.v[y * w + x] = 0.0;
                label[y * w + x] = (y * w + x) as i32;
            }
        }
    }
    const FWD: [(i32, i32, f32); 4] = [
        (-1, -1, std::f32::consts::SQRT_2),
        (0, -1, 1.0),
        (1, -1, std::f32::consts::SQRT_2),
        (-1, 0, 1.0),
    ];
    const BWD: [(i32, i32, f32); 4] = [
        (1, 1, std::f32::consts::SQRT_2),
        (0, 1, 1.0),
        (-1, 1, std::f32::consts::SQRT_2),
        (1, 0, 1.0),
    ];
    for pass in 0..2 {
        let forward = pass == 0;
        let steps: &[(i32, i32, f32); 4] = if forward { &FWD } else { &BWD };
        for yi in 0..h {
            let y = if forward { yi } else { h - 1 - yi };
            for xi in 0..w {
                let x = if forward { xi } else { w - 1 - xi };
                let i = y * w + x;
                let mut best = dist.v[i];
                let mut best_label = label[i];
                for s in 0..4 {
                    let sx = x as i32 + steps[s].0;
                    let sy = y as i32 + steps[s].1;
                    if sx < 0 || sy < 0 || sx >= w as i32 || sy >= h as i32 {
                        continue;
                    }
                    let si = sy as usize * w + sx as usize;
                    if label[si] < 0 {
                        continue;
                    }
                    let d = dist.v[si] + steps[s].2;
                    if d < best {
                        best = d;
                        best_label = label[si];
                    }
                }
                dist.v[i] = best;
                label[i] = best_label;
            }
        }
    }
    (label, dist)
}

#[inline]
fn srgb_to_linear(c: f32) -> f32 {
    if c <= 0.04045 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

#[inline]
fn linear_to_srgb(c: f32) -> f32 {
    if c <= 0.0031308 {
        c * 12.92
    } else {
        1.055 * c.powf(1.0 / 2.4) - 0.055
    }
}

/// `rgbToLab`（321–338），RGB 为 [0,1]
fn rgb_to_lab(rgb: &Image, lab: &mut Image) {
    *lab = Image::new(rgb.w, rgb.h);
    let n = rgb.w * rgb.h;
    for i in 0..n {
        let r = srgb_to_linear(rgb.v[i * 3].clamp(0.0, 1.0));
        let g = srgb_to_linear(rgb.v[i * 3 + 1].clamp(0.0, 1.0));
        let b = srgb_to_linear(rgb.v[i * 3 + 2].clamp(0.0, 1.0));
        let x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047;
        let y = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 1.0;
        let z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;
        let f = |t: f32| {
            if t > 0.008856 {
                t.cbrt()
            } else {
                7.787 * t + 16.0 / 116.0
            }
        };
        let fx = f(x);
        let fy = f(y);
        let fz = f(z);
        lab.v[i * 3] = 116.0 * fy - 16.0;
        lab.v[i * 3 + 1] = 500.0 * (fx - fy);
        lab.v[i * 3 + 2] = 200.0 * (fy - fz);
    }
}

/// `labToRgb`（340–361），原地改写
fn lab_to_rgb(lab: &mut Image) {
    let n = lab.w * lab.h;
    for i in 0..n {
        let l = lab.v[i * 3];
        let a = lab.v[i * 3 + 1];
        let b = lab.v[i * 3 + 2];
        let fy = (l + 16.0) / 116.0;
        let fx = fy + a / 500.0;
        let fz = fy - b / 200.0;
        let finv = |t: f32| {
            let t3 = t * t * t;
            if t3 > 0.008856 {
                t3
            } else {
                (t - 16.0 / 116.0) / 7.787
            }
        };
        let x = finv(fx) * 0.95047;
        let y = finv(fy);
        let z = finv(fz) * 1.08883;
        let r = 3.2406 * x - 1.5372 * y - 0.4986 * z;
        let g = -0.9689 * x + 1.8758 * y + 0.0415 * z;
        let zz = 0.0557 * x - 0.2040 * y + 1.0570 * z;
        lab.v[i * 3] = linear_to_srgb(r.clamp(0.0, 1.0)).clamp(0.0, 1.0);
        lab.v[i * 3 + 1] = linear_to_srgb(g.clamp(0.0, 1.0)).clamp(0.0, 1.0);
        lab.v[i * 3 + 2] = linear_to_srgb(zz.clamp(0.0, 1.0)).clamp(0.0, 1.0);
    }
}

fn invert3(a: &[[f64; 3]; 3]) -> Option<[[f64; 3]; 3]> {
    let mut m = [[0f64; 6]; 3];
    for r in 0..3 {
        for c in 0..3 {
            m[r][c] = a[r][c];
        }
        for c in 0..3 {
            m[r][3 + c] = if r == c { 1.0 } else { 0.0 };
        }
    }
    for c in 0..3 {
        let mut p = c;
        for r in c + 1..3 {
            if m[r][c].abs() > m[p][c].abs() {
                p = r;
            }
        }
        if m[p][c].abs() < 1e-12 {
            return None;
        }
        if p != c {
            for k in 0..6 {
                let t = m[c][k];
                m[c][k] = m[p][k];
                m[p][k] = t;
            }
        }
        let d = m[c][c];
        for k in 0..6 {
            m[c][k] /= d;
        }
        for r in 0..3 {
            if r == c {
                continue;
            }
            let f = m[r][c];
            for k in 0..6 {
                m[r][k] -= f * m[c][k];
            }
        }
    }
    let mut inv = [[0f64; 3]; 3];
    for r in 0..3 {
        for c in 0..3 {
            inv[r][c] = m[r][3 + c];
        }
    }
    Some(inv)
}

// ─────────────────────────── 路径一：曲面拟合 ───────────────────────────
/// 洞外环带（椭圆膨胀 9）上的已知像素拟合平面 `a + b·u + c·v`，u/v 按包围盒归一化。
/// 迭代重加权压离群点；内点率 ≥97% 且内点平方误差 ≤ 6/255² 才接受。
fn surface_fill(rgb: &mut Image, hole: &Mask, known: &Mask) -> bool {
    let (w, h) = (rgb.w, rgb.h);
    let mut bx0 = w as i32;
    let mut by0 = h as i32;
    let mut bx1 = -1i32;
    let mut by1 = -1i32;
    for y in 0..h {
        for x in 0..w {
            if hole.v[y * w + x] != 0 {
                bx0 = bx0.min(x as i32);
                by0 = by0.min(y as i32);
                bx1 = bx1.max(x as i32);
                by1 = by1.max(y as i32);
            }
        }
    }
    if bx1 < 0 {
        return false;
    }
    let bx0 = bx0 as usize;
    let by0 = by0 as usize;
    let bx1 = bx1 as usize;
    let by1 = by1 as usize;
    let bw = (bx1 - bx0 + 1).max(1);
    let bh = (by1 - by0 + 1).max(1);

    let ring0 = dilate_ellipse(hole, K_SURFACE_RING);
    let mut pts: Vec<usize> = Vec::with_capacity(4096);
    for y in 0..h {
        for x in 0..w {
            if ring0.v[y * w + x] != 0 && known.v[y * w + x] != 0 {
                pts.push(y * w + x);
            }
        }
    }
    if pts.len() < 12 {
        return false;
    }
    let stride = (pts.len() / 4096).max(1);

    let basis = |x: usize, y: usize| -> [f64; 3] {
        [
            1.0,
            (x as f64 - bx0 as f64) / bw as f64,
            (y as f64 - by0 as f64) / bh as f64,
        ]
    };

    let mut coef = [[0f64; 3]; 3]; // [channel][term]
    for iter in 0..K_SURFACE_ITERS {
        let mut normal = [[0f64; 3]; 3];
        let mut rhs = [[0f64; 3]; 3]; // [channel][term]
        let mut i = 0usize;
        while i < pts.len() {
            let p = pts[i];
            let x = p % w;
            let y = p / w;
            let b = basis(x, y);
            let c = &rgb.v[p * 3..p * 3 + 3];
            let mut pred = [0f64; 3];
            for ch in 0..3 {
                pred[ch] = coef[ch][0] * b[0] + coef[ch][1] * b[1] + coef[ch][2] * b[2];
            }
            let mut residual = 0f64;
            if iter > 0 {
                let mut s = 0f64;
                for ch in 0..3 {
                    let d = pred[ch] - c[ch] as f64;
                    s += d * d;
                }
                residual = s.sqrt();
            }
            let wgt = if iter == 0 {
                1.0
            } else {
                1.0f64.min(0.008 / residual.max(1e-8))
            };
            for r in 0..3 {
                for c2 in 0..3 {
                    normal[r][c2] += wgt * b[r] * b[c2];
                }
                for ch in 0..3 {
                    rhs[ch][r] += wgt * b[r] * c[ch] as f64;
                }
            }
            i += stride;
        }
        let inv = match invert3(&normal) {
            Some(inv) => inv,
            None => return false,
        };
        for ch in 0..3 {
            for r in 0..3 {
                coef[ch][r] = inv[r][0] * rhs[ch][0] + inv[r][1] * rhs[ch][1] + inv[r][2] * rhs[ch][2];
            }
        }
    }

    let mut inliers = 0usize;
    let mut count = 0usize;
    let mut squared_error = 0f64;
    let mut i = 0usize;
    while i < pts.len() {
        let p = pts[i];
        let x = p % w;
        let y = p / w;
        let b = basis(x, y);
        let c = &rgb.v[p * 3..p * 3 + 3];
        let mut s = 0f64;
        for ch in 0..3 {
            let pred = coef[ch][0] * b[0] + coef[ch][1] * b[1] + coef[ch][2] * b[2];
            let d = pred - c[ch] as f64;
            s += d * d;
        }
        let residual = s.sqrt();
        count += 1;
        if residual < 4.0 / 255.0 {
            inliers += 1;
            squared_error += residual * residual;
        }
        i += stride;
    }
    if inliers * 100 < count * 97 {
        return false;
    }
    if squared_error > inliers as f64 * 6.0 / (255.0 * 255.0) {
        return false;
    }

    for y in by0..=by1 {
        for x in bx0..=bx1 {
            if hole.v[y * w + x] == 0 {
                continue;
            }
            let b = basis(x, y);
            for ch in 0..3 {
                let pred = coef[ch][0] * b[0] + coef[ch][1] * b[1] + coef[ch][2] * b[2];
                rgb.v[(y * w + x) * 3 + ch] = pred.clamp(0.0, 1.0) as f32;
            }
        }
    }
    true
}

// ─────────────────────────── donor 域 ───────────────────────────
/// 先局部（到洞的距离 ≤ reach），patch 可用点不足 64 才把 reach 翻倍，直到 512
fn donor_domain(hole: &Mask, known: &Mask) -> Mask {
    let (w, h) = (hole.w, hole.h);
    let (_label, dist) = chamfer_nearest(hole);
    let mut bx0 = w as i32;
    let mut by0 = h as i32;
    let mut bx1 = -1i32;
    let mut by1 = -1i32;
    for y in 0..h {
        for x in 0..w {
            if hole.v[y * w + x] != 0 {
                bx0 = bx0.min(x as i32);
                by0 = by0.min(y as i32);
                bx1 = bx1.max(x as i32);
                by1 = by1.max(y as i32);
            }
        }
    }
    if bx1 < 0 {
        // 这里返回 `Mask`（0×0），后续 periodicFill / fillLevel 会用图像尺寸去索引
        // 这个空容器，属于越界读（UB）。这里返回同尺寸全零掩膜，行为确定为：
        // 找不到 donor → PatchMatch 的候选为空 → fill() 返回 false。
        return Mask::new(w, h);
    }
    let mut reach = ((bx1 - bx0 + 1).min(by1 - by0 + 1) / 2).clamp(K_DONOR_HALF_MIN, K_DONOR_HALF_MAX);
    loop {
        let mut domain = Mask::new(w, h);
        for y in 0..h {
            for x in 0..w {
                let i = y * w + x;
                domain.v[i] = if dist.v[i] <= reach as f32 && known.v[i] != 0 {
                    1
                } else {
                    0
                };
            }
        }
        let patches = erode_rect(&domain, 7);
        if patches.count() >= 64 || reach >= K_DONOR_REACH_CAP {
            return domain;
        }
        reach = K_DONOR_REACH_CAP.min(reach * 2);
    }
}

// ─────────────────────────── 路径二：周期平移 ───────────────────────────
/// 只在 donor 域内验证平移误差：单点误差 ≤0.5/255² 几乎等于"精确周期"
fn periodic_fill(rgb: &Image, hole: &Mask, domain: &Mask) -> Option<Image> {
    let (w, h) = (rgb.w, rgb.h);
    let mut pts: Vec<usize> = Vec::with_capacity(2048);
    for y in 0..h {
        for x in 0..w {
            if domain.v[y * w + x] != 0 {
                pts.push(y * w + x);
            }
        }
    }
    if pts.len() < 32 {
        return None;
    }
    let tol = 0.5 / (255.0 * 255.0);

    for dir in [(1i32, 0i32), (0, 1)] {
        for period in 2..=K_MAX_PERIOD {
            let ox = dir.0 * period;
            let oy = dir.1 * period;
            let mut count = 0i64;
            let mut error = 0f64;
            let mut abort = false;
            for &p in pts.iter() {
                let x = (p % w) as i32;
                let y = (p / w) as i32;
                let qx = x + ox;
                let qy = y + oy;
                if qx < 0 || qy < 0 || qx >= w as i32 || qy >= h as i32 {
                    continue;
                }
                if domain.v[qy as usize * w + qx as usize] == 0 {
                    continue;
                }
                let a = &rgb.v[p * 3..p * 3 + 3];
                let b = rgb.at(qx as usize, qy as usize);
                let mut s = 0f64;
                for c in 0..3 {
                    let d = a[c] as f64 - b[c] as f64;
                    s += d * d;
                }
                error += s;
                count += 1;
                // 提前否决按最终样本数上界（pts.size()）算，比 count 更宽松，不会误杀真周期
                if error > pts.len() as f64 * tol {
                    abort = true;
                    break;
                }
            }
            if abort || count < 64 || error > count as f64 * tol {
                continue;
            }

            let mut tmp = rgb.clone();
            let mut complete = true;
            let span = w.max(h) as i32;
            'outer: for y in 0..h {
                for x in 0..w {
                    if hole.v[y * w + x] == 0 {
                        continue;
                    }
                    let mut found = false;
                    let mut d = 1i32;
                    while d * period < span && !found {
                        for sign in [-1i32, 1] {
                            let qx = x as i32 + ox * d * sign;
                            let qy = y as i32 + oy * d * sign;
                            if qx < 0 || qy < 0 || qx >= w as i32 || qy >= h as i32 {
                                continue;
                            }
                            if domain.v[qy as usize * w + qx as usize] == 0 {
                                continue;
                            }
                            let s = rgb.at(qx as usize, qy as usize);
                            let t = tmp.at_mut(x, y);
                            t[0] = s[0];
                            t[1] = s[1];
                            t[2] = s[2];
                            found = true;
                            break;
                        }
                        d += 1;
                    }
                    if !found {
                        complete = false;
                        break 'outer;
                    }
                }
            }
            if complete {
                return Some(tmp);
            }
        }
    }
    None
}

struct Guide {
    color: Image,
    variation: Plane,
}

/// 加权观测均值 + 纹理强度；再沿 0°/90°/45°/135° 把两侧已知端点接进洞里。
fn build_guide(lab: &Image, hole: &Mask, known: &Mask, radius: i32) -> Guide {
    let (w, h) = (lab.w, lab.h);
    let mut wgt = Plane::new(w, h, 0.0);
    let mut wr = Plane::new(w, h, 0.0);
    let mut wg = Plane::new(w, h, 0.0);
    let mut wb = Plane::new(w, h, 0.0);
    for y in 0..h {
        for x in 0..w {
            let i = y * w + x;
            if known.v[i] == 0 {
                continue;
            }
            wgt.v[i] = 1.0;
            wr.v[i] = lab.v[i * 3];
            wg.v[i] = lab.v[i * 3 + 1];
            wb.v[i] = lab.v[i * 3 + 2];
        }
    }
    blur_plane(&mut wgt, radius);
    blur_plane(&mut wr, radius);
    blur_plane(&mut wg, radius);
    blur_plane(&mut wb, radius);

    let mut g = Guide {
        color: Image::new(w, h),
        variation: Plane::new(w, h, 0.0),
    };
    for y in 0..h {
        for x in 0..w {
            let i = y * w + x;
            let denom = wgt.v[i].max(1e-6);
            let c = g.color.at_mut(x, y);
            c[0] = wr.v[i] / denom;
            c[1] = wg.v[i] / denom;
            c[2] = wb.v[i] / denom;
        }
    }

    // 纹理强度：E[|c|²] − |mean|² 的开方，同样只用已知像素
    let mut sq = Plane::new(w, h, 0.0);
    for y in 0..h {
        for x in 0..w {
            let i = y * w + x;
            if known.v[i] == 0 {
                continue;
            }
            let c = &lab.v[i * 3..i * 3 + 3];
            sq.v[i] = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
        }
    }
    blur_plane(&mut sq, radius);
    for y in 0..h {
        for x in 0..w {
            let i = y * w + x;
            let mean = &g.color.v[i * 3..i * 3 + 3];
            let v = sq.v[i] / wgt.v[i].max(1e-6) - dot3(mean, mean);
            g.variation.v[i] = v.max(0.0).sqrt();
        }
    }

    // 方向延拓：给洞里的像素一个"结构上说得通"的颜色先验
    let k_far = f32::MAX / 4.0;
    let mut cost = Plane::new(w, h, k_far);
    let mut pending: Vec<i32> = Vec::new();
    for dir in [(1i32, 0i32), (0, 1), (1, 1), (1, -1)] {
        for sy0 in 0..h as i32 {
            for sx0 in 0..w as i32 {
                // 前驱还在画布内 → 不是这条线的起点
                if sx0 - dir.0 >= 0 && sy0 - dir.1 >= 0 && sy0 - dir.1 < h as i32 {
                    continue;
                }
                let mut before = -1i32;
                pending.clear();
                let mut x = sx0;
                let mut y = sy0;
                while x >= 0 && y >= 0 && x < w as i32 && y < h as i32 {
                    let p = y * w as i32 + x;
                    let (xu, yu) = (x as usize, y as usize);
                    if known.at(xu, yu) != 0 {
                        guide_flush(
                            w,
                            before,
                            &mut pending,
                            p,
                            &mut g.color,
                            &mut g.variation,
                            &mut cost,
                        );
                        before = p;
                    } else if hole.at(xu, yu) != 0 {
                        pending.push(p);
                    } else {
                        guide_flush(
                            w,
                            before,
                            &mut pending,
                            -1,
                            &mut g.color,
                            &mut g.variation,
                            &mut cost,
                        );
                        before = -1;
                    }
                    x += dir.0;
                    y += dir.1;
                }
                guide_flush(
                    w,
                    before,
                    &mut pending,
                    -1,
                    &mut g.color,
                    &mut g.variation,
                    &mut cost,
                );
            }
        }
    }
    g
}

/// 颜色按距离线性分配给它中间的 pending 洞像素，代价更低才覆盖。
fn guide_flush(
    w: usize,
    before: i32,
    pending: &mut Vec<i32>,
    after: i32,
    color: &mut Image,
    variation: &mut Plane,
    cost: &mut Plane,
) {
    if pending.is_empty() {
        return;
    }
    if before < 0 && after < 0 {
        return;
    }
    let a = if before >= 0 { before } else { after };
    let b = if after >= 0 { after } else { before };
    let (ax, ay) = (a as usize % w, a as usize / w);
    let (bx, by) = (b as usize % w, b as usize / w);
    let ca = [
        color.at(ax, ay)[0],
        color.at(ax, ay)[1],
        color.at(ax, ay)[2],
    ];
    let cb = [
        color.at(bx, by)[0],
        color.at(bx, by)[1],
        color.at(bx, by)[2],
    ];
    let d0 = ca[0] - cb[0];
    let d1 = ca[1] - cb[1];
    let d2 = ca[2] - cb[2];
    let difference = d0 * d0 + d1 * d1 + d2 * d2;
    let dx = ax as i32 - bx as i32;
    let dy = ay as i32 - by as i32;
    let span = ((dx * dx + dy * dy) as f32).sqrt();
    let score = difference + 0.02 * span + if before < 0 || after < 0 { 1000.0 } else { 0.0 };
    for &p in pending.iter() {
        if score >= cost.v[p as usize] {
            continue;
        }
        let ta = variation.v[a as usize];
        let tb = variation.v[b as usize];
        let mut t = 0f32;
        if span > 0.0 {
            let px = p % w as i32;
            let py = p / w as i32;
            let ddx = (px - ax as i32) as f32;
            let ddy = (py - ay as i32) as f32;
            t = (ddx * ddx + ddy * ddy).sqrt() / span;
        }
        t = t.clamp(0.0, 1.0);
        let (px, py) = (p as usize % w, p as usize / w);
        let c = color.at_mut(px, py);
        if difference < 900.0 {
            for k in 0..3 {
                c[k] = ca[k] * (1.0 - t) + cb[k] * t;
            }
        } else {
            let src = if t < 0.5 { &ca } else { &cb };
            c[0] = src[0];
            c[1] = src[1];
            c[2] = src[2];
        }
        variation.v[p as usize] = ta * (1.0 - t) + tb * t;
        cost.v[p as usize] = score;
    }
    pending.clear();
}

// ─────────────────────────── 路径三：PatchMatch ───────────────────────────
struct LevelState {
    image: Image,
    /// 每个像素的 donor 索引（像素下标），-1 = 还没匹配
    matched: Vec<i32>,
}

impl LevelState {
    fn empty() -> Self {
        Self {
            image: Image::new(0, 0),
            matched: Vec::new(),
        }
    }
}

/// 单层候选评估所需的只读上下文（`synthW` 每轮会变，所以每轮重建一次）。
struct LevelCtx<'a> {
    w: usize,
    h: usize,
    patch_r: i32,
    scale: f32,
    synth_w: f32,
    src: &'a Image,
    output: &'a Image,
    hole: &'a Mask,
    cov: &'a Mask,
    donors: &'a Mask,
    guide: &'a Guide,
    known_cnt: &'a [i32],
    miss_cnt: &'a [i32],
    interior: &'a [u8],
    penalty: &'a [f32],
}

/// 裸指针跨线程传递（搜索阶段的共享表）。
#[derive(Clone, Copy)]
struct ErasePtr<T>(*mut T);
unsafe impl<T> Send for ErasePtr<T> {}
unsafe impl<T> Sync for ErasePtr<T> {}

// 搜索阶段多线程会同时读写 costs/matched 的同一块内存。普通裸指针并发读写
// 在 Rust 内存模型里是数据竞争（UB），这里统一用 relaxed 原子访问；
// 对齐的 f32/i32 与 AtomicU32/AtomicI32 布局一致，串行阶段照常按值访问。
#[inline]
unsafe fn cost_load(p: *mut f32) -> f32 {
    f32::from_bits((*(p as *const AtomicU32)).load(AtomicOrdering::Relaxed))
}
#[inline]
unsafe fn cost_store(p: *mut f32, v: f32) {
    (*(p as *const AtomicU32)).store(v.to_bits(), AtomicOrdering::Relaxed);
}
#[inline]
unsafe fn match_load(p: *mut i32) -> i32 {
    (*(p as *const AtomicI32)).load(AtomicOrdering::Relaxed)
}
#[inline]
unsafe fn match_store(p: *mut i32, v: i32) {
    (*(p as *const AtomicI32)).store(v, AtomicOrdering::Relaxed);
}

impl<T> ErasePtr<T> {
    #[inline]
    fn get(&self) -> *mut T {
        self.0
    }
}

impl<'a> LevelCtx<'a> {
    /// `tryCandidate` lambda（782–818）：结构项先剪枝，再累加 patch 内逐像素 SSD
    ///
    /// 收裸指针而不是 `&mut [..]`：搜索阶段按 C 版一样多线程分段跑，
    /// 每个线程只写自己分到的洞像素；读邻居（传播用）可能读到别的线程刚写进去的值，
    /// 与 C 版 8 线程实现的行为一致。i32/f32 都是对齐的自然字，不会撕裂。
    fn try_candidate(
        &self,
        x: i32,
        y: i32,
        i: usize,
        sx: i32,
        sy: i32,
        costs: *mut f32,
        matched: *mut i32,
    ) {
        let (w, h) = (self.w, self.h);
        if sx < 0 || sy < 0 || sx >= w as i32 || sy >= h as i32 {
            return;
        }
        let si = sy as usize * w + sx as usize;
        if self.donors.v[si] == 0 {
            return;
        }
        let g_target = self.guide.color.at(x as usize, y as usize);
        let g_source = self.guide.color.at(sx as usize, sy as usize);
        let d0 = g_target[0] - g_source[0];
        let d1 = g_target[1] - g_source[1];
        let d2 = g_target[2] - g_source[2];
        let t_delta = self.guide.variation.v[i] - self.guide.variation.v[si];
        let dist_x = (sx - x) as f32;
        let dist_y = (sy - y) as f32;
        let structure_weight = 8.0 / (4.0 + self.guide.variation.v[i]);
        let fixed = structure_weight * (d0 * d0 + d1 * d1 + d2 * d2)
            + 4.0 * t_delta * t_delta
            + self.penalty[si]
            + 8.0 * (dist_x * dist_x + dist_y * dist_y) / (self.scale * self.scale);
        let best = unsafe { cost_load(costs.add(i)) };
        let limit = best + 1e-5 * best.max(1.0);
        if fixed > limit {
            return;
        }
        let inside = self.interior[i] != 0;
        let norm = (self.known_cnt[i] as f32 + self.miss_cnt[i] as f32 * self.synth_w).max(1.0);
        let mut sum = 0f32;
        for py in -self.patch_r..=self.patch_r {
            let ty = y + py;
            if ty < 0 || ty >= h as i32 {
                continue;
            }
            let sy_sample = sy + py;
            for px in -self.patch_r..=self.patch_r {
                let tx = x + px;
                if tx < 0 || tx >= w as i32 {
                    continue;
                }
                let ti = ty as usize * w + tx as usize;
                if !inside && self.cov.v[ti] == 0 {
                    continue;
                }
                let a = self.output.at(tx as usize, ty as usize);
                let b = self.src.at((sx + px) as usize, sy_sample as usize);
                let e0 = a[0] - b[0];
                let e1 = a[1] - b[1];
                let e2 = a[2] - b[2];
                let wgt = if self.hole.v[ti] != 0 { self.synth_w } else { 1.0 };
                sum += (e0 * e0 + e1 * e1 + e2 * e2) * wgt;
            }
            if sum / norm + fixed > limit {
                return;
            }
        }
        let cost = sum / norm + fixed;
        if cost < best {
            unsafe {
                cost_store(costs.add(i), cost);
                match_store(matched.add(i), sy * w as i32 + sx);
            }
        }
    }
}

/// `fillLevel`（697–985）：单层候选生成（最近点 / 随机 / 邻居传播 / 缩小窗口搜索）→ 加权投票
fn fill_level(
    src: &Image,
    hole: &Mask,
    cov: &Mask,
    dom: &Mask,
    guide: &Guide,
    prev: &LevelState,
    passes: i32,
    ok: &mut bool,
) -> LevelState {
    let (w, h) = (src.w, src.h);

    // donor：整块 patch 都落在已知域的像素；半径从 3 递减，找不到才放宽
    let mut patch_r = K_PATCH_R;
    let mut donors = Mask::new(w, h);
    let mut candidates: Vec<i32> = Vec::new();
    while patch_r >= 0 {
        donors = erode_rect(dom, 2 * patch_r + 1);
        candidates.clear();
        for y in 0..h {
            for x in 0..w {
                if donors.v[y * w + x] != 0 {
                    candidates.push((y * w + x) as i32);
                }
            }
        }
        if !candidates.is_empty() {
            break;
        }
        patch_r -= 1;
    }
    if candidates.is_empty() {
        *ok = false;
        return LevelState::empty();
    }

    // 每个洞像素的 patch 覆盖统计（已知 / 待补 / 是否完全在画布内）
    let mut known_cnt = vec![0i32; w * h];
    let mut miss_cnt = vec![0i32; w * h];
    let mut interior = vec![0u8; w * h];
    for y in 0..h {
        for x in 0..w {
            if hole.v[y * w + x] == 0 {
                continue;
            }
            let mut kn = 0i32;
            let mut ms = 0i32;
            for dy in -patch_r..=patch_r {
                let ty = y as i32 + dy;
                if ty < 0 || ty >= h as i32 {
                    continue;
                }
                for dx in -patch_r..=patch_r {
                    let tx = x as i32 + dx;
                    if tx < 0 || tx >= w as i32 {
                        continue;
                    }
                    let ti = ty as usize * w + tx as usize;
                    if cov.v[ti] == 0 {
                        continue;
                    }
                    if hole.v[ti] != 0 {
                        ms += 1;
                    } else {
                        kn += 1;
                    }
                }
            }
            let i = y * w + x;
            known_cnt[i] = kn;
            miss_cnt[i] = ms;
            interior[i] = if kn + ms == (2 * patch_r + 1) * (2 * patch_r + 1) {
                1
            } else {
                0
            };
        }
    }

    // 上一层（更粗）的结果上采样后作为本层起点：图像插值填进洞，匹配场按最近邻搬过来
    let has_prev = prev.image.w > 0 && prev.image.h > 0 && !prev.matched.is_empty();
    let mut output = src.clone();
    if has_prev {
        let up = upscale_image(&prev.image, w, h);
        for y in 0..h {
            for x in 0..w {
                if hole.v[y * w + x] != 0 {
                    let s = up.at(x, y);
                    let d = output.at_mut(x, y);
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                }
            }
        }
    }

    // 最近 donor（距离变换标签），用作候选与兜底
    let (nearest, _dist) = chamfer_nearest(&donors);

    let mut matched = vec![-1i32; w * h];
    let mut costs = vec![f32::MAX; w * h];
    let mut holes: Vec<i32> = Vec::new();
    for y in 0..h {
        for x in 0..w {
            if hole.v[y * w + x] != 0 {
                holes.push((y * w + x) as i32);
            }
        }
    }

    // donor 像素 → candidates 下标，统计复用次数时直接查表
    let mut donor_slot = vec![-1i32; w * h];
    for (k, &cand) in candidates.iter().enumerate() {
        donor_slot[cand as usize] = k as i32;
    }
    let mut cand_count = vec![0i32; candidates.len()];
    let mut penalty = vec![0f32; w * h];

    let mut rng = Mt19937::new(0x534e_4f57);
    let scale = w.max(h) as f32;
    let mut synth_w = 0.05f32; // 迭代开始前的那次初始化，与原版同档

    // 初始化：最粗层没有上层可用 → 最近 donor + 32 次随机抽样，代价只看引导图
    for idx in 0..holes.len() {
        let p = holes[idx];
        let x = p % w as i32;
        let y = p / w as i32;
        let i = p as usize;
        if has_prev {
            let init_ctx = LevelCtx {
                w,
                h,
                patch_r,
                scale,
                synth_w,
                src,
                output: &output,
                hole,
                cov,
                donors: &donors,
                guide,
                known_cnt: &known_cnt,
                miss_cnt: &miss_cnt,
                interior: &interior,
                penalty: &penalty,
            };
            // 本层坐标 → 上层坐标（最近邻）
            let to_prev = |v: i32, cur_n: i32, prev_n: i32| -> i32 {
                ((v as f32 + 0.5) * prev_n as f32 / cur_n as f32 - 0.5)
                    .round()
                    .clamp(0.0, (prev_n - 1).max(0) as f32) as i32
            };
            let px = to_prev(x, w as i32, prev.image.w as i32);
            let py = to_prev(y, h as i32, prev.image.h as i32);
            let q = prev.matched[py as usize * prev.image.w + px as usize];
            if q >= 0 {
                let qx = (q as usize % prev.image.w) as i32;
                let qy = (q as usize / prev.image.w) as i32;
                let sx = to_prev(qx, prev.image.w as i32, w as i32);
                let sy = to_prev(qy, prev.image.h as i32, h as i32);
                init_ctx.try_candidate(x, y, i, sx, sy, costs.as_mut_ptr(), matched.as_mut_ptr());
            }
            if matched[i] < 0 && nearest[i] >= 0 {
                let n = nearest[i];
                init_ctx.try_candidate(
                    x,
                    y,
                    i,
                    n % w as i32,
                    n / w as i32,
                    costs.as_mut_ptr(),
                    matched.as_mut_ptr(),
                );
            }
            continue;
        }
        let mut best = nearest[i];
        if best < 0 {
            best = candidates[(rng.next_u32() as usize) % candidates.len()];
        }
        let gt = guide.color.at(x as usize, y as usize);
        let gs = guide.color.at(best as usize % w, best as usize / w);
        let mut d0 = gt[0] - gs[0];
        let mut d1 = gt[1] - gs[1];
        let mut d2 = gt[2] - gs[2];
        let mut tv = guide.variation.v[i] - guide.variation.v[best as usize];
        let mut best_cost = d0 * d0 + d1 * d1 + d2 * d2 + 4.0 * tv * tv;
        for _ in 0..32 {
            let c = candidates[(rng.next_u32() as usize) % candidates.len()];
            let gs = guide.color.at(c as usize % w, c as usize / w);
            d0 = gt[0] - gs[0];
            d1 = gt[1] - gs[1];
            d2 = gt[2] - gs[2];
            tv = guide.variation.v[i] - guide.variation.v[c as usize];
            let cost = d0 * d0 + d1 * d1 + d2 * d2 + 4.0 * tv * tv;
            if cost < best_cost {
                best_cost = cost;
                best = c;
            }
        }
        matched[i] = best;
        // 初始像素直接取 donor 颜色（Lab 空间），后续迭代会重算
        let s = src.at(best as usize % w, best as usize / w);
        let d = output.at_mut(x as usize, y as usize);
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
    }

    let span_count = holes.len();
    for iter in 0..passes {
        synth_w = 0.15 + 0.20 * iter as f32 / (passes - 1).max(1) as f32;
        // 复用惩罚：一个 donor 被越多次引用，越说明它在硬撑，代价抬高
        for c in cand_count.iter_mut() {
            *c = 0;
        }
        for &p in holes.iter() {
            let q = matched[p as usize];
            if q < 0 {
                continue;
            }
            let slot = donor_slot[q as usize];
            if slot >= 0 {
                cand_count[slot as usize] += 1;
            }
        }
        let expected = (holes.len() as f32 / candidates.len() as f32).max(1.0);
        for v in penalty.iter_mut() {
            *v = 0.0;
        }
        for (k, &cand) in candidates.iter().enumerate() {
            penalty[cand as usize] = 25.0 * (cand_count[k] as f32 / expected).ln_1p();
        }

        let step: i32 = if iter % 2 == 0 { 1 } else { -1 };
        let ctx = LevelCtx {
            w,
            h,
            patch_r,
            scale,
            synth_w,
            src,
            output: &output,
            hole,
            cov,
            donors: &donors,
            guide,
            known_cnt: &known_cnt,
            miss_cnt: &miss_cnt,
            interior: &interior,
            penalty: &penalty,
        };
        // 每个洞像素的候选搜索互相独立（只用邻居上一轮的值做传播）。
        // 和 C 版一样按连续分段开多线程、各自一条 mt19937 —— 原来串行执行，
        // 大选框（工作区 500×400 以上）要 0.4~2 秒，用户看到的就是"点了没反应"。
        let ncpu = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1);
        let threads = ncpu.clamp(1, K_SEARCH_SPANS);
        {
            let costs_p = ErasePtr(costs.as_mut_ptr());
            let match_p = ErasePtr(matched.as_mut_ptr());
            let ctx_ref = &ctx;
            let holes_ref = &holes[..];
            let nearest_ref = &nearest;
            let cand_ref = &candidates;
            std::thread::scope(|sc| {
                for t in 0..threads {
                    let begin = span_count * t / threads;
                    let end = span_count * (t + 1) / threads;
                    if begin >= end {
                        continue;
                    }
                    let costs_p = costs_p;
                    let match_p = match_p;
                    sc.spawn(move || {
                        let costs_p = costs_p.get();
                        let match_p = match_p.get();
                        let mut local = Mt19937::new(
                            0x534e_4f57u32.wrapping_add((t as u32).wrapping_mul(7919)),
                        );
                        for si in begin..end {
                            let p = holes_ref[if step > 0 {
                                si
                            } else {
                                span_count - 1 - si
                            }];
                            let x = p % w as i32;
                            let y = p / w as i32;
                            let i = p as usize;
                            unsafe { cost_store(costs_p.add(i), f32::MAX); }
                            let old = unsafe { match_load(match_p.add(i)) };
                            if old >= 0 {
                                ctx_ref.try_candidate(
                                    x, y, i, old % w as i32, old / w as i32, costs_p, match_p,
                                );
                            }
                            let near_donor = nearest_ref[i];
                            if near_donor >= 0 {
                                ctx_ref.try_candidate(
                                    x, y, i, near_donor % w as i32, near_donor / w as i32,
                                    costs_p, match_p,
                                );
                            }
                            let seed_pixel =
                                cand_ref[(local.next_u32() as usize) % cand_ref.len()];
                            ctx_ref.try_candidate(
                                x, y, i, seed_pixel % w as i32, seed_pixel / w as i32,
                                costs_p, match_p,
                            );
                            let nx = x - step;
                            if nx >= 0 && nx < w as i32 {
                                let nb = unsafe { match_load(match_p.add(y as usize * w + nx as usize)) };
                                if nb >= 0 {
                                    ctx_ref.try_candidate(
                                        x, y, i, nb % w as i32 + step, nb / w as i32,
                                        costs_p, match_p,
                                    );
                                }
                            }
                            let ny = y - step;
                            if ny >= 0 && ny < h as i32 {
                                let nb = unsafe { match_load(match_p.add(ny as usize * w + x as usize)) };
                                if nb >= 0 {
                                    ctx_ref.try_candidate(
                                        x, y, i, nb % w as i32, nb / w as i32 + step,
                                        costs_p, match_p,
                                    );
                                }
                            }
                            let mut window = w.max(h) as i32;
                            while window > 0 {
                                let cur = unsafe { match_load(match_p.add(i)) };
                                if cur < 0 {
                                    break;
                                }
                                let span = window * 2 + 1;
                                let sx = cur % w as i32 + (local.next_u32() % span as u32) as i32 - window;
                                let sy = cur / w as i32 + (local.next_u32() % span as u32) as i32 - window;
                                ctx_ref.try_candidate(x, y, i, sx, sy, costs_p, match_p);
                                window /= 2;
                            }
                        }
                    });
                }
            });
        }

        // 投票：7×7 内的洞像素互为担保，权重看代价、引导差和细节差
        let mut next = output.clone();
        {
            // 投票同样按连续分段并行：每个线程只写自己那几个洞像素的颜色
            let next_p = ErasePtr(next.v.as_mut_ptr());
            let holes_ref = &holes[..];
            std::thread::scope(|sc| {
                for t in 0..threads {
                    let begin = span_count * t / threads;
                    let end = span_count * (t + 1) / threads;
                    if begin >= end {
                        continue;
                    }
                    let next_p = next_p;
                    let matched_ref = &matched;
                    let costs_ref = &costs;
                    sc.spawn(move || {
                        let next_p = next_p.get();
                        for hi in begin..end {
                            let p = holes_ref[hi];
                            let x = p % w as i32;
                            let y = p / w as i32;
                            let i = p as usize;
                            let center = matched_ref[i];
                            if center < 0 {
                                continue;
                            }
                            let anchor = src.at(center as usize % w, center as usize / w);
                            let tolerance = (guide.variation.v[i] * 0.25).clamp(1.5, 10.0);
                            let g_target = guide.color.at(x as usize, y as usize);
                            let mut sum = [0f32; 3];
                            let mut wsum = 0f32;
                            for dy in -patch_r..=patch_r {
                                let ny = y + dy;
                                if ny < 0 || ny >= h as i32 {
                                    continue;
                                }
                                for dx in -patch_r..=patch_r {
                                    let nx = x + dx;
                                    if nx < 0 || nx >= w as i32 {
                                        continue;
                                    }
                                    let ni = ny as usize * w + nx as usize;
                                    if hole.v[ni] == 0 {
                                        continue;
                                    }
                                    let donor = matched_ref[ni];
                                    if donor < 0 {
                                        continue;
                                    }
                                    let sx = donor % w as i32 - dx;
                                    let sy = donor / w as i32 - dy;
                                    if sx < 0 || sy < 0 || sx >= w as i32 || sy >= h as i32 {
                                        continue;
                                    }
                                    let value = src.at(sx as usize, sy as usize);
                                    let g_donor = guide.color.at(sx as usize, sy as usize);
                                    let g0 = g_donor[0] - g_target[0];
                                    let g1 = g_donor[1] - g_target[1];
                                    let g2 = g_donor[2] - g_target[2];
                                    let d0 = value[0] - anchor[0];
                                    let d1 = value[1] - anchor[1];
                                    let d2 = value[2] - anchor[2];
                                    let wgt = 1.0
                                        / ((1.0 + costs_ref[ni])
                                            * (1.0 + (g0 * g0 + g1 * g1 + g2 * g2) / 100.0)
                                            * (1.0
                                                + (d0 * d0 + d1 * d1 + d2 * d2)
                                                    / (tolerance * tolerance)));
                                    sum[0] += value[0] * wgt;
                                    sum[1] += value[1] * wgt;
                                    sum[2] += value[2] * wgt;
                                    wsum += wgt;
                                }
                            }
                            if wsum > 0.0 {
                                let o = (y as usize * w + x as usize) * 3;
                                unsafe {
                                    *next_p.add(o) = sum[0] / wsum;
                                    *next_p.add(o + 1) = sum[1] / wsum;
                                    *next_p.add(o + 2) = sum[2] / wsum;
                                }
                            }
                        }
                    });
                }
            });
        }
        output = next;
    }

    LevelState {
        image: output,
        matched,
    }
}

/// 就地修复 `bits` 中被 `mask` 标记的像素，只写洞内像素的 RGB（alpha 原样保留）。
///
/// 返回 `false` 时 `bits` 不保证未被修改，调用方应丢弃这次修复
/// `buildMosaicBitmap` 拿到空指针后继续画占位符的行为一致）。
pub(crate) fn fill(bits: &mut [u8], pitch: usize, width: usize, height: usize, mask: &[u8]) -> bool {
    if width < 8 || height < 8 {
        return false;
    }
    // 调用方契约检查
    if mask.len() < width * height || pitch < width * 4 {
        return false;
    }
    if bits.len() < (height - 1) * pitch + width * 4 {
        return false;
    }

    // 先按洞的包围盒裁剪工作区：4K 截图也只算局部，省内存也省时间
    let mut hx0 = width as i32;
    let mut hy0 = height as i32;
    let mut hx1 = -1i32;
    let mut hy1 = -1i32;
    let mut hole_count = 0usize;
    for y in 0..height {
        for x in 0..width {
            if mask[y * width + x] != 0 {
                hole_count += 1;
                hx0 = hx0.min(x as i32);
                hy0 = hy0.min(y as i32);
                hx1 = hx1.max(x as i32);
                hy1 = hy1.max(y as i32);
            }
        }
    }
    if hx1 < 0 {
        return false;
    }
    // 已知像素不到一半时 donor 不够，补出来只会是瞎猜
    if hole_count * 2 > width * height {
        return false;
    }

    let hole_w = (hx1 - hx0 + 1) as usize;
    let hole_h = (hy1 - hy0 + 1) as usize;
    let margin = ((hole_w.min(hole_h) / 2).clamp(16, 96) + 24) as i32;
    let cw = width.min(hole_w + margin as usize * 2);
    let ch = height.min(hole_h + margin as usize * 2);
    if cw * ch > K_MAX_WORKING_PIXELS {
        return false;
    }
    let cx0 = 0.max((width as i32 - cw as i32).min((hx0 + hx1 - cw as i32 + 1) / 2)) as usize;
    let cy0 = 0.max((height as i32 - ch as i32).min((hy0 + hy1 - ch as i32 + 1) / 2)) as usize;

    let mut cov = Mask::new(cw, ch);
    let mut hole = Mask::new(cw, ch);
    let mut rgb = Image::new(cw, ch);
    let mut known_count = 0usize;
    for y in 0..ch {
        let row = (cy0 + y) * pitch;
        for x in 0..cw {
            let px = row + (cx0 + x) * 4;
            let a = bits[px + 3];
            let i = y * cw + x;
            let covered = a > 0;
            cov.v[i] = if covered { 1 } else { 0 };
            let is_hole = mask[(cy0 + y) * width + cx0 + x] != 0 && covered;
            hole.v[i] = if is_hole { 1 } else { 0 };
            if !is_hole && covered {
                known_count += 1;
            }
            // DIB 字节序 B,G,R,A → RGB
            let c = &mut rgb.v[i * 3..i * 3 + 3];
            c[0] = bits[px + 2] as f32 / 255.0;
            c[1] = bits[px + 1] as f32 / 255.0;
            c[2] = bits[px] as f32 / 255.0;
        }
    }
    if known_count == 0 {
        return false;
    }

    let mut known = Mask::new(cw, ch);
    for i in 0..hole.v.len() {
        known.v[i] = if cov.v[i] != 0 && hole.v[i] == 0 { 1 } else { 0 };
    }

    // 捷径一：纯色 / 线性渐变
    if !surface_fill(&mut rgb, &hole, &known) {
        // 捷径二：精确周期
        let domain = donor_domain(&hole, &known);
        if let Some(periodic) = periodic_fill(&rgb, &hole, &domain) {
            rgb = periodic;
        } else {
            // 主路径：结构引导 + 金字塔 PatchMatch
            let guide_radius = (hole_w.min(hole_h) / 12).clamp(4, 16) as i32;
            let mut lab = Image::new(cw, ch);
            rgb_to_lab(&rgb, &mut lab);
            // 引导图必须只由 known 生成：洞内像素一旦参与，均值会把待擦掉的内容带回来，
            // 四方向延拓也会因洞像素被判成 known 而整段失效
            let guide = build_guide(&lab, &hole, &known, guide_radius);

            // 金字塔：每层洞先 5×5 膨胀再面积缩放（任一点为洞即算洞），
            // 已知 / 域先 5×5 腐蚀再缩放、要求几乎全为 1（整块都在），可用点 <16 就停
            let mut images = vec![lab];
            let mut guides = vec![guide];
            let mut hole_levels = vec![hole.clone()];
            let mut cov_levels = vec![cov];
            let mut dom_levels = vec![domain];
            loop {
                let (lw, lh) = {
                    let last = images.last().unwrap();
                    (last.w, last.h)
                };
                if !(lw.max(lh) > 64 && lw.min(lh) > 8) {
                    break;
                }
                let reduced = pyr_down_image(images.last().unwrap());

                let dilated_hole = dilate_rect(hole_levels.last().unwrap(), 5);
                let mut ph = Plane::new(dilated_hole.w, dilated_hole.h, 0.0);
                for i in 0..ph.v.len() {
                    ph.v[i] = if dilated_hole.v[i] != 0 { 1.0 } else { 0.0 };
                }
                let ph_down = area_down_plane(&ph);
                let mut reduced_hole = Mask::new(ph_down.w, ph_down.h);
                for i in 0..ph_down.v.len() {
                    reduced_hole.v[i] = if ph_down.v[i] > 0.0 { 1 } else { 0 };
                }

                let eroded_cov = erode_rect(cov_levels.last().unwrap(), 5);
                let eroded_dom = erode_rect(dom_levels.last().unwrap(), 5);
                let mut pc = Plane::new(eroded_cov.w, eroded_cov.h, 0.0);
                let mut pd = Plane::new(eroded_dom.w, eroded_dom.h, 0.0);
                for i in 0..pc.v.len() {
                    pc.v[i] = if eroded_cov.v[i] != 0 { 1.0 } else { 0.0 };
                }
                for i in 0..pd.v.len() {
                    pd.v[i] = if eroded_dom.v[i] != 0 { 1.0 } else { 0.0 };
                }
                let rc = area_down_plane(&pc);
                let rd = area_down_plane(&pd);
                let mut reduced_cov = Mask::new(rc.w, rc.h);
                let mut reduced_dom = Mask::new(rd.w, rd.h);
                for i in 0..rc.v.len() {
                    let eligible = reduced_hole.v[i] == 0;
                    reduced_cov.v[i] = if eligible && rc.v[i] > 0.996 { 1 } else { 0 };
                    reduced_dom.v[i] = if eligible && rd.v[i] > 0.996 { 1 } else { 0 };
                }
                if erode_rect(&reduced_dom, 7).count() < 16 {
                    break;
                }

                let last_guide = guides.last().unwrap();
                let reduced_color = pyr_down_image(&last_guide.color);
                let reduced_variation = pyr_down_plane(&last_guide.variation);

                images.push(reduced);
                guides.push(Guide {
                    color: reduced_color,
                    variation: reduced_variation,
                });
                hole_levels.push(reduced_hole);
                cov_levels.push(reduced_cov);
                dom_levels.push(reduced_dom);
            }

            let mut filled = LevelState::empty();
            let mut ok = true;
            for i in (0..images.len()).rev() {
                let passes = if i == images.len() - 1 {
                    K_PASSES_COARSE
                } else if i == 0 {
                    K_PASSES_FINE
                } else {
                    K_PASSES_MID
                };
                filled = fill_level(
                    &images[i],
                    &hole_levels[i],
                    &cov_levels[i],
                    &dom_levels[i],
                    &guides[i],
                    &filled,
                    passes,
                    &mut ok,
                );
                if !ok {
                    break;
                }
            }
            if !ok {
                return false;
            }

            lab_to_rgb(&mut filled.image);
            rgb = filled.image;
        }
    }

    // 只写洞里像素，alpha 原样保留
    for y in 0..ch {
        let row = (cy0 + y) * pitch;
        for x in 0..cw {
            if hole.v[y * cw + x] == 0 {
                continue;
            }
            let c = &rgb.v[(y * cw + x) * 3..(y * cw + x) * 3 + 3];
            let px = row + (cx0 + x) * 4;
            bit_write(&mut bits[px + 2], c[0]);
            bit_write(&mut bits[px + 1], c[1]);
            bit_write(&mut bits[px], c[2]);
        }
    }
    true
}

/// `(BYTE)std::clamp((int)std::lround(c * 255.f), 0, 255)`
#[inline]
fn bit_write(dst: &mut u8, c: f32) {
    let v = (c * 255.0).round();
    *dst = if v.is_nan() { 0 } else { v.clamp(0.0, 255.0) as u8 };
}

// ─────────────────────────── 自检（仅测试构建） ───────────────────────────
#[cfg(test)]
mod tests {
    use super::fill;

    fn solid_bgra(w: usize, h: usize, b: u8, g: u8, r: u8, a: u8) -> Vec<u8> {
        let mut v = vec![0u8; w * h * 4];
        for px in v.chunks_exact_mut(4) {
            px[0] = b;
            px[1] = g;
            px[2] = r;
            px[3] = a;
        }
        v
    }

    /// 纯色图挖矩形洞：应走 surfaceFill，洞内接近原色、洞外一字不改
    #[test]
    fn solid_color_hole_is_filled() {
        let (w, h) = (160usize, 120usize);
        let mut bits = solid_bgra(w, h, 0, 0, 255, 255); // 纯红（BGRA）
        let before = bits.clone();
        let mut mask = vec![0u8; w * h];
        for y in 40..80 {
            for x in 60..100 {
                mask[y * w + x] = 1;
            }
        }
        assert!(fill(&mut bits, w * 4, w, h, &mask), "纯色图应当修复成功");
        for y in 40..80 {
            for x in 60..100 {
                let i = (y * w + x) * 4;
                assert!(bits[i] <= 4, "B 通道偏差过大: {}", bits[i]);
                assert!(bits[i + 1] <= 4, "G 通道偏差过大: {}", bits[i + 1]);
                assert!(bits[i + 2] >= 251, "R 通道偏差过大: {}", bits[i + 2]);
                assert_eq!(bits[i + 3], 255, "alpha 必须原样保留");
            }
        }
        for i in 0..w * h {
            if mask[i] != 0 {
                continue;
            }
            assert_eq!(
                &bits[i * 4..i * 4 + 4],
                &before[i * 4..i * 4 + 4],
                "洞外像素被改动: ({}, {})",
                i % w,
                i / w
            );
        }
    }

    #[test]
    fn empty_mask_returns_false() {
        let (w, h) = (32usize, 32usize);
        let mut bits = solid_bgra(w, h, 10, 20, 30, 255);
        let before = bits.clone();
        let mask = vec![0u8; w * h];
        assert!(!fill(&mut bits, w * 4, w, h, &mask));
        assert_eq!(bits, before, "无洞时不应修改任何像素");
    }
}
