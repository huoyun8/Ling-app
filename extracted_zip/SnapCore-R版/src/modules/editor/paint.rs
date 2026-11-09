use crate::prelude::*;
use crate::*;

/// 文字烘焙用的常驻 DIB + DC。
///
/// 旧实现每画一行字都 CreateCompatibleDC + CreateDIBSection + DeleteObject：
/// 序号徽章每次落点都要走一遍，实测这一段固定 16~19ms（约一格 60Hz），
/// 连点十来个就明显"跟不上"。尺寸只增不减，够用就复用。
struct TextDib {
    hdc: winapi::shared::windef::HDC,
    bmp: *mut winapi::ctypes::c_void,
    bits: *mut u8,
    w: i32,
    h: i32,
}
unsafe impl Send for TextDib {}

static TEXT_DIB: Mutex<Option<TextDib>> = Mutex::new(None);

unsafe fn text_dib(pw: i32, ph: i32) -> Option<std::sync::MutexGuard<'static, Option<TextDib>>> {
    let mut g = TEXT_DIB.lock_ok();
    let need_new = match g.as_ref() {
        Some(d) => d.w < pw || d.h < ph,
        None => true,
    };
    if need_new {
        let (nw, nh) = match g.as_ref() {
            Some(d) => (d.w.max(pw), d.h.max(ph)),
            None => (pw, ph),
        };
        if let Some(d) = g.take() {
            DeleteObject(d.bmp as *mut winapi::ctypes::c_void);
            DeleteDC(d.hdc);
        }
        let hdc = CreateCompatibleDC(std::ptr::null_mut());
        if hdc.is_null() { return None; }
        let mut bmi: BITMAPINFO = std::mem::zeroed();
        bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        bmi.bmiHeader.biWidth = nw;
        bmi.bmiHeader.biHeight = -nh;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        let mut bits: *mut winapi::ctypes::c_void = std::ptr::null_mut();
        let hbmp = CreateDIBSection(hdc, &mut bmi, DIB_RGB_COLORS, &mut bits, std::ptr::null_mut(), 0);
        if hbmp.is_null() { DeleteDC(hdc); return None; }
        SelectObject(hdc, hbmp as *mut winapi::ctypes::c_void);
        *g = Some(TextDib {
            hdc, bmp: hbmp as *mut winapi::ctypes::c_void,
            bits: bits as *mut u8, w: nw, h: nh,
        });
    }
    Some(g)
}

#[inline]
unsafe fn anno_px(buf: &mut [u8], w: i32, h: i32, x: i32, y: i32, b: u8, g: u8, r: u8) {
    if x < 0 || y < 0 || x >= w || y >= h { return; }
    let i = ((y * w + x) * 4) as usize;
    if i + 3 < buf.len() { buf[i] = b; buf[i + 1] = g; buf[i + 2] = r; buf[i + 3] = 255; }
}
// ==================== 覆盖率抗锯齿 ====================
// 只按"整数格子上非 0 即 1"填充就是硬边
// 斜线/椭圆放大看就是楼梯。这里补上：先把形状的覆盖率算进一张浮点蒙版（取并集，
// 所以拐角重叠处不会被混两遍），最后一次性混进底图 —— 半透明线条也不会越描越黑。
pub(crate) struct CovMask {
    x0: i32,
    y0: i32,
    w: i32,
    h: i32,
    m: Vec<f32>,
}

impl CovMask {
    /// 建一张覆盖 (x0,y0)-(x1,y1) 的蒙版（自动夹到图内）
    fn new(x0: i32, y0: i32, x1: i32, y1: i32, iw: i32, ih: i32) -> CovMask {
        let x0 = x0.max(0);
        let y0 = y0.max(0);
        let x1 = x1.min(iw - 1);
        let y1 = y1.min(ih - 1);
        let w = (x1 - x0 + 1).max(0);
        let h = (y1 - y0 + 1).max(0);
        CovMask { x0, y0, w, h, m: vec![0.0; (w * h) as usize] }
    }
    #[inline]
    fn add(&mut self, x: i32, y: i32, c: f32) {
        let ix = x - self.x0;
        let iy = y - self.y0;
        if ix < 0 || iy < 0 || ix >= self.w || iy >= self.h { return; }
        let i = (iy * self.w + ix) as usize;
        if c > self.m[i] { self.m[i] = c; }
    }
    /// 累加式写入：扫描线按子扫描线逐条累加覆盖率时用它。
    /// 不能用 add —— 它是"取并集"，会把 4×0.25 压成 0.25，整块填色就变半透明
    #[inline]
    fn add_acc(&mut self, x: i32, y: i32, c: f32) {
        let ix = x - self.x0;
        let iy = y - self.y0;
        if ix < 0 || iy < 0 || ix >= self.w || iy >= self.h { return; }
        let i = (iy * self.w + ix) as usize;
        self.m[i] = (self.m[i] + c).min(1.0);
    }
}

/// 点到线段的距离（像素中心用 px+0.5 传进来）
#[inline]
fn cov_dist_seg(px: f32, py: f32, x0: f32, y0: f32, x1: f32, y1: f32) -> f32 {
    let (vx, vy) = (x1 - x0, y1 - y0);
    let (wx, wy) = (px - x0, py - y0);
    let len2 = vx * vx + vy * vy;
    let t = if len2 > 0.0 { ((wx * vx + wy * vy) / len2).clamp(0.0, 1.0) } else { 0.0 };
    let (dx, dy) = (wx - vx * t, wy - vy * t);
    (dx * dx + dy * dy).sqrt()
}

/// 圆头粗线段 = 胶囊（ROUND 线帽 + 圆角连接就是这个形状）。
/// 覆盖率按"到中心线距离"取 c = clamp(r + 0.5 - d, 0, 1)：半径 6 的水平线正好占 12 行，
/// 斜线则按距离平滑过渡，锯齿水平与逐图元抗锯齿相当
fn cov_add_seg(mask: &mut CovMask, x0: f32, y0: f32, x1: f32, y1: f32, r: f32) {
    let minx = (x0.min(x1) - r - 1.0).floor() as i32;
    let maxx = (x0.max(x1) + r + 1.0).ceil() as i32;
    let miny = (y0.min(y1) - r - 1.0).floor() as i32;
    let maxy = (y0.max(y1) + r + 1.0).ceil() as i32;
    for py in miny..=maxy {
        for px in minx..=maxx {
            let d = cov_dist_seg(px as f32 + 0.5, py as f32 + 0.5, x0, y0, x1, y1);
            let c = (r + 0.5 - d).clamp(0.0, 1.0);
            if c > 0.0 { mask.add(px, py, c); }
        }
    }
}

/// 轴对齐矩形边框。
/// 写法：上/下/左/右四条**有端点的实心细条**取并集（mask.add 取 max，转角不会被混两遍）。
///
/// 不能用「到四条无限长边线的距离取最小」——那条式子只在矩形**里面**成立：
/// 在矩形外面，边线仍旧无限延伸，于是四条边各自往外多涂半个线宽，
/// 四个角各多出一小块方像素（用户：矩形是短角的，4 角都有多余的像素）。
fn cov_add_rect_frame(mask: &mut CovMask, x0: f32, y0: f32, x1: f32, y1: f32, half: f32) {
    // 每条细条两端各长出 half，转角处正好补满一个方角（miter），且不再往外溢
    cov_add_box(mask, x0 - half, y0 - half, x1 + half, y0 + half);
    cov_add_box(mask, x0 - half, y1 - half, x1 + half, y1 + half);
    cov_add_box(mask, x0 - half, y0 - half, x0 + half, y1 + half);
    cov_add_box(mask, x1 - half, y0 - half, x1 + half, y1 + half);
}

/// 轴对齐实心矩形的覆盖率：有向距离场，0.5 像素宽的过渡带
fn cov_add_box(mask: &mut CovMask, x0: f32, y0: f32, x1: f32, y1: f32) {
    if x1 < x0 || y1 < y0 { return; }
    let (bcx, bcy) = ((x0 + x1) * 0.5, (y0 + y1) * 0.5);
    let (hx, hy) = ((x1 - x0) * 0.5, (y1 - y0) * 0.5);
    let minx = (x0 - 1.0).floor() as i32;
    let maxx = (x1 + 1.0).ceil() as i32;
    let miny = (y0 - 1.0).floor() as i32;
    let maxy = (y1 + 1.0).ceil() as i32;
    for py in miny..=maxy {
        for px in minx..=maxx {
            let qx = (px as f32 + 0.5 - bcx).abs() - hx;
            let qy = (py as f32 + 0.5 - bcy).abs() - hy;
            let ox = qx.max(0.0);
            let oy = qy.max(0.0);
            let sd = (ox * ox + oy * oy).sqrt() + qx.max(qy).min(0.0);
            let c = (0.5 - sd).clamp(0.0, 1.0);
            if c > 0.0 { mask.add(px, py, c); }
        }
    }
}

/// 椭圆边界到点的一阶距离估计：F = (x/rx)² + (y/ry)² - 1，|∇F| 归一化后 d ≈ F/|∇F|。
/// 曲率不过分大时误差远小于半像素，够做 AA
#[inline]
fn cov_ellipse_dist(cx: f32, cy: f32, rx: f32, ry: f32, px: f32, py: f32) -> f32 {
    let (ux, uy) = ((px - cx) / rx, (py - cy) / ry);
    let f = ux * ux + uy * uy - 1.0;
    let gx = ux / rx;
    let gy = uy / ry;
    let g = 2.0 * (gx * gx + gy * gy).sqrt();
    if g < 1e-6 { 0.0 } else { f / g }
}

/// 椭圆环（空心椭圆）
fn cov_add_ellipse_ring(mask: &mut CovMask, cx: f32, cy: f32, rx: f32, ry: f32, half: f32) {
    let minx = (cx - rx - half - 1.0).floor() as i32;
    let maxx = (cx + rx + half + 1.0).ceil() as i32;
    let miny = (cy - ry - half - 1.0).floor() as i32;
    let maxy = (cy + ry + half + 1.0).ceil() as i32;
    for py in miny..=maxy {
        for px in minx..=maxx {
            let d = cov_ellipse_dist(cx, cy, rx, ry, px as f32 + 0.5, py as f32 + 0.5).abs();
            let c = (half + 0.5 - d).clamp(0.0, 1.0);
            if c > 0.0 { mask.add(px, py, c); }
        }
    }
}

/// 椭圆内域（实心椭圆）
fn cov_add_ellipse_fill(mask: &mut CovMask, cx: f32, cy: f32, rx: f32, ry: f32) {
    let minx = (cx - rx - 1.0).floor() as i32;
    let maxx = (cx + rx + 1.0).ceil() as i32;
    let miny = (cy - ry - 1.0).floor() as i32;
    let maxy = (cy + ry + 1.0).ceil() as i32;
    for py in miny..=maxy {
        for px in minx..=maxx {
            let d = cov_ellipse_dist(cx, cy, rx, ry, px as f32 + 0.5, py as f32 + 0.5);
            let c = (0.5 - d).clamp(0.0, 1.0);
            if c > 0.0 { mask.add(px, py, c); }
        }
    }
}

/// 把蒙版一次性混进底图。alpha 是整条形状的不透明度（线条工具的"半透明"= 0.55）
unsafe fn cov_blend(mask: &CovMask, buf: &mut [u8], w: i32, h: i32, b: u8, g: u8, r: u8, alpha: f32) {
    for iy in 0..mask.h {
        for ix in 0..mask.w {
            let c = mask.m[(iy * mask.w + ix) as usize] * alpha;
            if c <= 0.002 { continue; }
            anno_px_blend(buf, w, h, mask.x0 + ix, mask.y0 + iy, b, g, r, if c > 1.0 { 1.0 } else { c });
        }
    }
}

/// 4×4 超采样求一块区域的覆盖率蒙版，锯齿水平与逐图元抗锯齿相当
/// （每个像素按 16 个子采样点算面积占比），比解析式省事、也够准 ——
/// 副条那条 1px 滑块线、6px 圆点、13px 圆角色块全靠它对齐
pub(crate) fn cov_supersample(x0: i32, y0: i32, w: i32, h: i32, inside: &dyn Fn(f32, f32) -> bool) -> Vec<u8> {
    let mut m = vec![0u8; (w * h) as usize];
    for py in 0..h {
        for px in 0..w {
            let mut c = 0u32;
            for sy in 0..4 {
                for sx in 0..4 {
                    let x = (x0 + px) as f32 + (sx as f32 + 0.5) / 4.0;
                    let y = (y0 + py) as f32 + (sy as f32 + 0.5) / 4.0;
                    if inside(x, y) { c += 1; }
                }
            }
            m[(py * w + px) as usize] = ((c * 255 + 8) / 16) as u8;
        }
    }
    m
}

/// 把一份覆盖率蒙版按指定颜色混到 DC 的 (x0,y0) 区域上。
/// GDI 只会画整像素，滑块那种"压在两个像素中间"的 1px 线和 6px 圆点必须靠这个补：
/// 先把底图读回来，按覆盖率混完再写回去（所以圆角色块的对勾能叠在色块上而不糊掉底）
pub(crate) unsafe fn anno_blend_cov(dst: winapi::shared::windef::HDC, x0: i32, y0: i32, m: &[u8], w: i32, h: i32,
                         r: u8, g: u8, b: u8) {
    if w <= 0 || h <= 0 { return; }
    let mut bmi: BITMAPINFO = std::mem::zeroed();
    bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    let sdc = GetDC(std::ptr::null_mut());
    let mdc = CreateCompatibleDC(sdc);
    let mut p: *mut winapi::ctypes::c_void = std::ptr::null_mut();
    let hbmp = CreateDIBSection(sdc, &mut bmi, DIB_RGB_COLORS, &mut p, std::ptr::null_mut(), 0);
    if hbmp.is_null() || p.is_null() {
        if !hbmp.is_null() { DeleteObject(hbmp as *mut winapi::ctypes::c_void); }
        DeleteDC(mdc);
        ReleaseDC(std::ptr::null_mut(), sdc);
        return;
    }
    let old = SelectObject(mdc, hbmp as *mut winapi::ctypes::c_void);
    BitBlt(mdc, 0, 0, w, h, dst, x0, y0, SRCCOPY);
    let buf = std::slice::from_raw_parts_mut(p as *mut u8, (w * h * 4) as usize);
    for i in 0..(w * h) as usize {
        let a = m[i] as u32;
        if a == 0 { continue; }
        let inv = 255 - a;
        buf[i * 4] = ((b as u32 * a + buf[i * 4] as u32 * inv) / 255) as u8;
        buf[i * 4 + 1] = ((g as u32 * a + buf[i * 4 + 1] as u32 * inv) / 255) as u8;
        buf[i * 4 + 2] = ((r as u32 * a + buf[i * 4 + 2] as u32 * inv) / 255) as u8;
    }
    BitBlt(dst, x0, y0, w, h, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old);
    DeleteObject(hbmp as *mut winapi::ctypes::c_void);
    DeleteDC(mdc);
    ReleaseDC(std::ptr::null_mut(), sdc);
}

/// 抗锯齿粗线段。t 是线宽
pub(crate) unsafe fn anno_line_aa(buf: &mut [u8], w: i32, h: i32, x0: i32, y0: i32, x1: i32, y1: i32,
                       b: u8, g: u8, r: u8, t: i32, alpha: f32) {
    let rad = t.max(1) as f32 * 0.5;
    let pad = rad.ceil() as i32 + 1;
    let mut mask = CovMask::new(x0.min(x1) - pad, y0.min(y1) - pad,
                                x0.max(x1) + pad, y0.max(y1) + pad, w, h);
    if mask.w <= 0 || mask.h <= 0 { return; }
    cov_add_seg(&mut mask, x0 as f32, y0 as f32, x1 as f32, y1 as f32, rad);
    cov_blend(&mask, buf, w, h, b, g, r, alpha);
}

/// 自由手绘折线：整条只在**一张**蒙版上取并集，最后一次混色。
/// 逐段各自混色（旧写法）在透明色下会在每一处拐点二次叠加、颜色明显变深，
/// 采样密的地方看着像一串深浅不一的点；一次并集 + 一次混色才是均匀的一笔。
pub(crate) unsafe fn anno_polyline_aa(buf: &mut [u8], w: i32, h: i32, pts: &[(i32, i32)],
                          b: u8, g: u8, r: u8, t: i32, alpha: f32) {
    if pts.len() < 2 { return; }
    let rad = t.max(1) as f32 * 0.5;
    let pad = rad.ceil() as i32 + 1;
    let (mut minx, mut miny) = (i32::MAX, i32::MAX);
    let (mut maxx, mut maxy) = (i32::MIN, i32::MIN);
    for p in pts {
        minx = minx.min(p.0); maxx = maxx.max(p.0);
        miny = miny.min(p.1); maxy = maxy.max(p.1);
    }
    let mut mask = CovMask::new(minx - pad, miny - pad, maxx + pad, maxy + pad, w, h);
    if mask.w <= 0 || mask.h <= 0 { return; }
    for k in 1..pts.len() {
        let a = pts[k - 1];
        let c = pts[k];
        cov_add_seg(&mut mask, a.0 as f32, a.1 as f32, c.0 as f32, c.1 as f32, rad);
    }
    cov_blend(&mask, buf, w, h, b, g, r, alpha);
}

pub(crate) unsafe fn anno_rect(buf: &mut [u8], w: i32, h: i32, x0: i32, y0: i32, x1: i32, y1: i32, b: u8, g: u8, r: u8, t: i32) {
    let (lx, rx) = (x0.min(x1) as f32, x0.max(x1) as f32);
    let (ty, by) = (y0.min(y1) as f32, y0.max(y1) as f32);
    let half = t.max(1) as f32 * 0.5;
    let pad = half.ceil() as i32 + 1;
    let mut mask = CovMask::new(lx as i32 - pad, ty as i32 - pad, rx as i32 + pad, by as i32 + pad, w, h);
    if mask.w <= 0 || mask.h <= 0 { return; }
    cov_add_rect_frame(&mut mask, lx, ty, rx, by, half);
    cov_blend(&mask, buf, w, h, b, g, r, 1.0);
}

pub(crate) unsafe fn anno_fill_rect(buf: &mut [u8], w: i32, h: i32, x0: i32, y0: i32, x1: i32, y1: i32, b: u8, g: u8, r: u8) {
    // 填充矩形 (left, top, right, bottom) 是半开区间：像素中心落在 [left,right)
    // 里才填。整数坐标时右/下边界那一列不算，写成 ..= 会整体胖 1px
    let (x0, x1) = (x0.min(x1).max(0), x0.max(x1).min(w));
    let (y0, y1) = (y0.min(y1).max(0), y0.max(y1).min(h));
    for yy in y0..y1 { for xx in x0..x1 { anno_px(buf, w, h, xx, yy, b, g, r); } }
}

pub(crate) unsafe fn anno_ellipse(buf: &mut [u8], w: i32, h: i32, x0: i32, y0: i32, x1: i32, y1: i32, b: u8, g: u8, r: u8, t: i32) {
    let rx = (x1 - x0).abs() as f32 / 2.0;
    let ry = (y1 - y0).abs() as f32 / 2.0;
    let cx = (x0 + x1) as f32 / 2.0;
    let cy = (y0 + y1) as f32 / 2.0;
    if rx < 0.5 || ry < 0.5 { return; }
    let half = t.max(1) as f32 * 0.5;
    let pad = half.ceil() as i32 + 1;
    let mut mask = CovMask::new((cx - rx) as i32 - pad, (cy - ry) as i32 - pad,
                                (cx + rx) as i32 + pad, (cy + ry) as i32 + pad, w, h);
    if mask.w <= 0 || mask.h <= 0 { return; }
    cov_add_ellipse_ring(&mut mask, cx, cy, rx, ry, half);
    cov_blend(&mask, buf, w, h, b, g, r, 1.0);
}

/// 半透明填充矩形（画笔选框）
pub(crate) fn anno_rect_blend(buf: &mut [u8], w: i32, h: i32, x0: i32, y0: i32, x1: i32, y1: i32,
                   b: u8, g: u8, r: u8, a: f32) {
    let l = x0.min(x1).max(0);
    let rgt = x0.max(x1).min(w);
    let t = y0.min(y1).max(0);
    let bot = y0.max(y1).min(h);
    let (fa, ia) = (a.clamp(0.0, 1.0), 1.0 - a.clamp(0.0, 1.0));
    for y in t..bot {
        for x in l..rgt {
            let i = ((y * w + x) * 4) as usize;
            buf[i] = (b as f32 * fa + buf[i] as f32 * ia).round() as u8;
            buf[i + 1] = (g as f32 * fa + buf[i + 1] as f32 * ia).round() as u8;
            buf[i + 2] = (r as f32 * fa + buf[i + 2] as f32 * ia).round() as u8;
        }
    }
}

pub(crate) unsafe fn anno_fill_ellipse(buf: &mut [u8], w: i32, h: i32, x0: i32, y0: i32, x1: i32, y1: i32, b: u8, g: u8, r: u8) {
    let rx = (x1 - x0).abs() as f32 / 2.0;
    let ry = (y1 - y0).abs() as f32 / 2.0;
    let cx = (x0 + x1) as f32 / 2.0;
    let cy = (y0 + y1) as f32 / 2.0;
    if rx < 0.5 || ry < 0.5 {
        anno_fill_rect(buf, w, h, x0.min(x1), y0.min(y1), x0.max(x1), y0.max(y1), b, g, r);
        return;
    }
    let mut mask = CovMask::new((cx - rx) as i32 - 1, (cy - ry) as i32 - 1,
                                (cx + rx) as i32 + 1, (cy + ry) as i32 + 1, w, h);
    if mask.w <= 0 || mask.h <= 0 { return; }
    cov_add_ellipse_fill(&mut mask, cx, cy, rx, ry);
    cov_blend(&mask, buf, w, h, b, g, r, 1.0);
}

/// 任意简单多边形的覆盖率：按扫描线求交、按跨度累加。
///
/// 旧实现是"每个像素 4×4 个子采样点，每点都对全部边做一次射线法"，
/// 复杂度 O(面积 × 16 × 边数)。序号徽章是个 100 多边的圆盘，一次落点就要
/// 跑上千万次内层循环，实测固定 16~19ms —— 连点十来个当然跟不上。
/// 现在每个子扫描线只对每条边求一个交点、排序后按跨度填，复杂度降到
/// O(行数 × 4 × 边数 + 面积)，同一个徽章不到 1ms，而且覆盖率是连续的（不是 16 档）。
fn cov_add_poly(mask: &mut CovMask, pts: &[(f32, f32)]) {
    let n = pts.len();
    if n < 3 { return; }
    const SS: i32 = 4;
    let wgt = 1.0 / SS as f32;
    let mut miny = f32::MAX; let mut maxy = f32::MIN;
    for &(_, y) in pts.iter() {
        miny = miny.min(y); maxy = maxy.max(y);
    }
    let mut xs: Vec<f32> = Vec::with_capacity(16);
    for py in (miny.floor() as i32 - 1)..=(maxy.ceil() as i32 + 1) {
        for sy in 0..SS {
            let fy = py as f32 + (sy as f32 + 0.5) / SS as f32;
            // 射线法求这一行的全部交点（非零绕数由成对填充表达）
            xs.clear();
            for i in 0..n {
                let (ax, ay) = pts[i];
                let (bx, by) = pts[(i + 1) % n];
                if (ay <= fy) != (by <= fy) {
                    xs.push(ax + (fy - ay) / (by - ay) * (bx - ax));
                }
            }
            if xs.len() < 2 { continue; }
            xs.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
            let mut i = 0;
            while i + 1 < xs.len() {
                cov_add_span(mask, xs[i], xs[i + 1], py, wgt);
                i += 2;
            }
        }
    }
}

/// 把 [x0,x1) 这一行跨度按"每像素的横向覆盖比例"加进蒙版
#[inline]
fn cov_add_span(mask: &mut CovMask, x0: f32, x1: f32, py: i32, wgt: f32) {
    if x1 <= x0 { return; }
    let ix0 = x0.floor() as i32;
    let ix1 = x1.floor() as i32;
    for px in ix0..=ix1 {
        let a = (x0 - px as f32).clamp(0.0, 1.0);
        let b = (x1 - px as f32).clamp(0.0, 1.0);
        let c = b - a;
        if c > 0.0 { mask.add_acc(px, py, c * wgt); }
    }
}

/// 箭头 —— 按原几何绘制
///
/// 注意它**不是**"等宽线杆 + 三角头"：路径从起点（一个点）出发，直接连到箭杆在
/// 箭颈处的两侧（±v1），所以杆是**从尾尖张开的楔形**，宽度沿长度线性增长。
/// 截图里箭杆横向厚度 3→6→9 行正是这个原因，公式必须原样实现
///   arrowSize = 滑块值 × 4；v1 = arrowSize/4（杆半宽）；v2 = arrowSize*2/3（箭头半宽）
///   填实 = FillGeometry；空心 = 以 dpi(1px) 描边这条闭合路径
pub(crate) unsafe fn anno_arrow(buf: &mut [u8], w: i32, h: i32, x0: i32, y0: i32, x1: i32, y1: i32, b: u8, g: u8, r: u8, t: i32, fill: bool) {
    let (sx, sy, ex, ey) = (x0 as f32, y0 as f32, x1 as f32, y1 as f32);
    let dx = ex - sx;
    let dy = ey - sy;
    let len = (dx * dx + dy * dy).sqrt();
    if len < 1.0 { return; }
    let (ux, uy) = (dx / len, dy / len);
    let (vx, vy) = (-uy, ux);
    let asz = t.max(1) as f32 * 4.0;
    let v1 = asz / 4.0;        // 杆半宽
    let v2 = asz * 2.0 / 3.0;  // 箭头半宽
    let pts: [(f32, f32); 6] = [
        (sx, sy),
        (ex - asz * ux - v1 * vx, ey - asz * uy - v1 * vy),
        (ex - (asz + v1) * ux - v2 * vx, ey - (asz + v1) * uy - v2 * vy),
        (ex, ey),
        (ex - (asz + v1) * ux + v2 * vx, ey - (asz + v1) * uy + v2 * vy),
        (ex - asz * ux + v1 * vx, ey - asz * uy + v1 * vy),
    ];
    let pad = (asz + v2).ceil() as i32 + 2;
    let mut mask = CovMask::new(x0.min(x1) - pad, y0.min(y1) - pad,
                                x0.max(x1) + pad, y0.max(y1) + pad, w, h);
    if mask.w <= 0 || mask.h <= 0 { return; }
    if fill {
        cov_add_poly(&mut mask, &pts);
    } else {
        // 空心：按 1px 描边这条闭合路径（是 DrawGeometry(path, brush, win->dpi)）
        // 1px 的 miter/round 拐角差别不到半格，用圆头段拼即可
        for i in 0..6 {
            let (ax, ay) = pts[i];
            let (bx, by) = pts[(i + 1) % 6];
            cov_add_seg(&mut mask, ax, ay, bx, by, 0.5);
        }
    }
    cov_blend(&mask, buf, w, h, b, g, r, 1.0);
}

// 文本绘制：经 GDI 内存 DC + DIBSection 渲染后并回位图（TRANSPARENT 背景，非 0 像素覆盖）
// 只开"文字外接框"大小的小 DIB：旧实现每条文字都开整图 DIB、整图拷进、
// 再逐像素遍历整图回写，序号一多每次重放都是 O(整图 × 条数)，10 个标号就明显卡。
unsafe fn anno_gdi_text(buf: &mut [u8], w: i32, h: i32, x: i32, y: i32, s: &str, rgb: u32, px: i32, bold: bool, italic: bool) {
    if s.is_empty() || w <= 0 || h <= 0 { return; }
    let px = px.max(1);
    let (bw, bh, _) = anno_tx_box(s, px, bold, italic);
    let pad = (px as f32 * 0.45) as i32 + 3;   // 斜体出锋 / 抗锯齿留边
    let l = (x - pad).clamp(0, w);
    let tp = (y - pad).clamp(0, h);
    let r = (x + bw + pad).clamp(0, w);
    let b = (y + bh + pad).clamp(0, h);
    let (pw, ph) = (r - l, b - tp);
    if pw <= 0 || ph <= 0 { return; }
    let Some(mut g) = text_dib(pw, ph) else { return };
    let d = g.as_mut().unwrap();
    let (hdc, bits, stride) = (d.hdc, d.bits, (d.w as usize) * 4);
    // 先把对应区域灌进 DIB：GDI 的灰度 AA 是"跟已有底色混"，底色留黑会在字边挂一圈暗边
    let row = (pw as usize) * 4;
    {
        let dst = std::slice::from_raw_parts_mut(bits, stride * d.h as usize);
        for yy in 0..ph {
            let so = (((tp + yy) * w + l) * 4) as usize;
            let dof = (yy as usize) * stride;
            dst[dof..dof + row].copy_from_slice(&buf[so..so + row]);
        }
    }
    SetBkMode(hdc, TRANSPARENT as i32);
    let f = anno_tx_font(px, bold, italic);
    SelectObject(hdc, f);
    SetTextColor(hdc, rgb);
    let ws: Vec<u16> = s.encode_utf16().collect();
    // 落点用设计推进量算好的 dx（纯 TextOutW 会被 GDI 整数化）
    let pos = anno_tx_advances(px, bold, italic, s);
    let mut dx: Vec<i32> = Vec::with_capacity(ws.len());
    for k in 0..ws.len() { dx.push(pos[k + 1] - pos[k]); }
    // 画位按底图绝对坐标给，视口原点挪到小图左上角
    SetViewportOrgEx(hdc, -l, -tp, std::ptr::null_mut());
    ExtTextOutW(hdc, x, y, 0, std::ptr::null(), ws.as_ptr(), ws.len() as u32, dx.as_ptr());
    GdiFlush();
    let src = std::slice::from_raw_parts(bits as *const u8, stride * d.h as usize);
    // 只回写 BGR：alpha 由各自的调用方管（贴屏窗要自己铺，标注底图不用）
    for yy in 0..ph {
        let so = (yy as usize) * stride;
        let dof = (((tp + yy) * w + l) * 4) as usize;
        for xx in 0..pw as usize {
            let si = so + xx * 4;
            let di = dof + xx * 4;
            buf[di..di + 3].copy_from_slice(&src[si..si + 3]);
        }
    }
}

/// 从 10° 逆时针绕到 350°（340° 圆弧），再连到 (r + r/3, 0) 那个尖，最后回到起点。
/// 尖角方向由 angle 决定（初值 270 = 朝下）。数字字号取 r，在 2r×2r 的框里居中。
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn anno_number(buf: &mut [u8], w: i32, h: i32, cx: i32, cy: i32, rad: i32,
                      ang: f32, val: i32, b: u8, g: u8, r: u8, fill: bool, square: bool) {
    let rf = rad as f32;
    if rf < 1.0 || w <= 0 || h <= 0 { return; }
    let a = (-ang) * std::f32::consts::PI / 180.0;
    let (ca, sa) = (a.cos(), a.sin());
    let tp = |px: f32, py: f32| -> (f32, f32) {
        (cx as f32 + px * ca - py * sa, cy as f32 + px * sa + py * ca)
    };
    let mut pts: Vec<(f32, f32)>;
    if square {
        // 方徽章：半边长 r 的方框，右缘中段开一个 ±tan(10°) 的口，尖角从口里伸出去（全直线）
        let lip = 0.17633f32;
        pts = vec![
            tp(rf, -rf * lip),
            tp(rf, -rf),
            tp(-rf, -rf),
            tp(-rf, rf),
            tp(rf, rf),
            tp(rf, rf * lip),
            tp(rf + rf / 3.0, 0.0),
        ];
    } else {
        // 圆弧采样：localPoint(θ) = (r·cosθ, −r·sinθ)，θ 从 10 递增到 350 正好是绕过顶部的长弧
        let n = ((rf * 1.2) as i32).clamp(24, 240);
        pts = Vec::with_capacity(n as usize + 2);
        for i in 0..=n {
            let th = (10.0 + 340.0 * i as f32 / n as f32) * std::f32::consts::PI / 180.0;
            pts.push(tp(rf * th.cos(), -rf * th.sin()));
        }
        pts.push(tp(rf + rf / 3.0, 0.0));
    }
    let pad = rf + rf / 3.0 + 2.0;
    let mut mask = CovMask::new((cx as f32 - pad) as i32, (cy as f32 - pad) as i32,
                                (cx as f32 + pad) as i32, (cy as f32 + pad) as i32, w, h);
    if mask.w <= 0 || mask.h <= 0 { return; }
    if fill {
        cov_add_poly(&mut mask, &pts);
    } else {
        for i in 0..pts.len() {
            let p = pts[i];
            let q = pts[(i + 1) % pts.len()];
            cov_add_seg(&mut mask, p.0, p.1, q.0, q.1, 0.5);
        }
    }
    cov_blend(&mask, buf, w, h, b, g, r, 1.0);
    // 数字：字号 = r，2r×2r 框里双向居中
    let s = val.to_string();
    let sp = anno_tx_spacing(rad);
    let tw = anno_tx_measure(rad, false, false, &s);
    let tx = cx - (tw as f32 / 2.0).round() as i32;
    let ty = cy - (sp / 2.0).round() as i32;
    let clr = if fill { ui::color::cref(ui::color::TEXT_INVERSE) } else { rgb(r as u32, g as u32, b as u32) };
    anno_gdi_text(buf, w, h, tx, ty, &s, clr, rad, false, false);
}
#[inline]
unsafe fn anno_px_blend(buf: &mut [u8], w: i32, h: i32, x: i32, y: i32, b: u8, g: u8, r: u8, a: f32) {
    if x < 0 || y < 0 || x >= w || y >= h { return; }
    let i = ((y * w + x) * 4) as usize;
    if i + 3 >= buf.len() { return; }
    // 四舍五入而不是截断：截断会让每一层 AA 都暗 1/255，叠几层就看得出来
    let ob = buf[i] as f32; let og = buf[i + 1] as f32; let orr = buf[i + 2] as f32;
    buf[i] = (b as f32 * a + ob * (1.0 - a)).round().min(255.0) as u8;
    buf[i + 1] = (g as f32 * a + og * (1.0 - a)).round().min(255.0) as u8;
    buf[i + 2] = (r as f32 * a + orr * (1.0 - a)).round().min(255.0) as u8;
    buf[i + 3] = 255;
}
