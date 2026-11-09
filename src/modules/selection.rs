use crate::prelude::*;
use crate::*;

// 截图框选：全屏遮罩窗口 + 选区状态 + 整屏截图
// 智能选区识别表（z 序，屏幕坐标）：整窗和它的子控件都在里面，框选时用来做
// 窗口识别 / Tab / 滚轮循环 / 边缘吸附。每一项带所属顶层窗口的 z 序，
// 同一窗口内子控件按面积升序排在父矩形前面 —— "包含光标的第一项"就是最具体的那块
static SEL_WINS: Mutex<Vec<SnapRect>> = Mutex::new(Vec::new());
// 光标处的候选（同属最上层那个窗口，面积升序）：Tab / 滚轮在它上面循环
static SEL_CANDS: Mutex<Vec<SnapRect>> = Mutex::new(Vec::new());
// 光标当前悬停的窗口矩形（还没开始拖的时候用来预览"要框哪个窗口"）
static SEL_HOVER: Mutex<Option<(i32, i32, i32, i32)>> = Mutex::new(None);
static SEL_HOVER_IDX: AtomicI32 = AtomicI32::new(-1);  // Tab 循环的当前下标
// 悬停断开后的"挂住"闸门：光标落进桌面图标格间 / 窗口边缘的空白时
// 不立刻收框，等 700ms 真停了才收 —— 否则一路扫过图标就是"暗-亮-暗-亮"地闪
static SEL_HOLD: AtomicBool = AtomicBool::new(false);
// 最近一次"悬停智选"落下的框。挂住到期只收这一份 —— 用户自己拖出来的框不能被抹掉
static SEL_PREVIEW: Mutex<Option<(i32, i32, i32, i32)>> = Mutex::new(None);
static SEL_FRAME_TICK: AtomicI32 = AtomicI32::new(0);  // 上一帧的时间戳（合帧闸门）
static SEL_FRAME_PEND: AtomicBool = AtomicBool::new(false); // 已排一颗一次性定时器等下一帧
pub(crate) static SEL_HWND: AtomicIsize = AtomicIsize::new(0);
// 选区流程正在跑（含 start_region_select 里那段嵌套消息泵）。
// 两个用途：① 防重入 —— 泵里再收到热键/托盘又调进来会无限递归、栈溢出；
//          ② 给 ESC 让路 —— 流程中按 ESC 只收遮罩，绝不去 DestroyWindow(主窗)。
pub(crate) static SEL_REENTRANT: AtomicBool = AtomicBool::new(false);
/// 进入 start_region_select 就置位，函数怎么退（含提前 return）都会自动复位
struct SelReentryGuard;
impl Drop for SelReentryGuard {
    fn drop(&mut self) { SEL_REENTRANT.store(false, Ordering::Relaxed); }
}
// 遮罩所铺的那块显示器：左上角（屏幕坐标）与尺寸。
// 遮罩内部一律用"局部坐标"（= 屏幕坐标 - 原点），只有跟系统对话的地方才加回去 ——
// 单显示器上原点就是 (0,0)、尺寸就是主屏，这些值退化成 SM_CXSCREEN 那套。
static SEL_OX: AtomicI32 = AtomicI32::new(0);
static SEL_OY: AtomicI32 = AtomicI32::new(0);
static SEL_VW: AtomicI32 = AtomicI32::new(0);
static SEL_VH: AtomicI32 = AtomicI32::new(0);
static SEL_START: Mutex<Option<(i32, i32)>> = Mutex::new(None);
static SEL_CUR: Mutex<Option<(i32, i32)>> = Mutex::new(None);
pub(crate) static SEL_BGRA: Mutex<Option<(u32, u32, Vec<u8>)>> = Mutex::new(None);
// 注意：不保留"全屏压暗副本"。
// 压暗在绘制期对选区外四个矩形做 32% 黑 alpha 混合
// （ColorF(0x000000, 0.32f) 四块 FillRectangle 一一对应）——省下一整份整屏缓冲。
// 智能选区：光标这一点上"眼睛真正看见的那个顶层窗口"。
// 识别表只有 z 序位次，表又是节流重枚举的；刚弹出的窗口不在表里时位次会滑到它后面那一层，
// 所以每个 mousemove 用系统 z 序现问一次，问到的窗口句柄存在这里
static SEL_HIT_ROOT: AtomicIsize = AtomicIsize::new(0);
// 光标停在界面空处时只"标待办"，真正重枚举放在鼠标停住 30ms 之后（定时器 101），
// 免得 EnumWindows + 每窗 EnumChildWindows 挂在 mousemove 上拖帧
static SEL_ENUM_TICK: AtomicI32 = AtomicI32::new(0);
static SEL_ENUM_WANT: AtomicBool = AtomicBool::new(false);
static SEL_DESK_WANT: AtomicBool = AtomicBool::new(false);
// 右键退回选区后、鼠标还没动的那一段不做悬停吸附
// 先记下右键那一刻的光标，真挪出 2px 才恢复识别，否则原地一个 mousemove 就把框又吸回来
static SEL_SNAP_ARMED: AtomicBool = AtomicBool::new(false);
static SEL_ARM_POS: Mutex<Option<(i32, i32)>> = Mutex::new(None);
// ESC 兜底轮询（定时器 104）的上一次键状态
static SEL_ESC_DOWN: AtomicBool = AtomicBool::new(false);
// 桌面图标层：explorer 的 SysListView32 里每一项的槽位（屏幕坐标）。
// 桌面不是窗口树能描述的（四十几个图标全挂在一个 SysListView32 里，没有一个 HWND），
// 几何层与像素层在桌面上都给不出正确的框，所以单开一层，直接向 explorer 要槽位
static SEL_DESK: Mutex<Vec<DeskIcon>> = Mutex::new(Vec::new());
static SEL_DESK_LIST: AtomicIsize = AtomicIsize::new(0);
static SEL_DESK_STAMP: AtomicI32 = AtomicI32::new(0);
pub(crate) static SEL_MODE: AtomicI32 = AtomicI32::new(4); // 0=二维码识别(带智选), 3=标注截图, 4=二维码识别(纯手动拖框)；1=纯文字OCR, 2=先二维码后文字
pub(crate) static SEL_QR_HIT: AtomicI32 = AtomicI32::new(0); // 本轮识别是否命中二维码（mode=2 时决定结果回填到二维码框还是文字框）

// 一条 X/Y/R/B/W/H、白字 12 号、底色 32% 黑、内边距 3，贴着选区上边再往上 3px；
// 顶出屏幕就折回选区内部（left + 3, top + 1）。
// 底色刻意不用实心灰
// 拿 #333 去凑在深色/浅色背景上都会偏 —— 所以这里走一次真 alpha 混合。
pub(crate) unsafe fn blend_black(dc: winapi::shared::windef::HDC, x: i32, y: i32, w: i32, h: i32, alpha: u8) {
    if w <= 0 || h <= 0 { return; }
    // 1×1 的预乘黑，AlphaBlend 会把它拉伸铺满目标矩形（比自建整块掩膜省得多）
    static TBITS: AtomicIsize = AtomicIsize::new(0);
    static TDC: AtomicIsize = AtomicIsize::new(0);
    let mut bits = TBITS.load(Ordering::Relaxed) as *mut u8;
    let mut src = TDC.load(Ordering::Relaxed) as winapi::shared::windef::HDC;
    if bits.is_null() || src.is_null() {
        let sdc = GetDC(std::ptr::null_mut());
        let mut bmi: BITMAPINFO = std::mem::zeroed();
        bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        bmi.bmiHeader.biWidth = 1;
        bmi.bmiHeader.biHeight = 1;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        let mut p: *mut winapi::ctypes::c_void = std::ptr::null_mut();
        let bmp = CreateDIBSection(sdc, &mut bmi, DIB_RGB_COLORS, &mut p, std::ptr::null_mut(), 0);
        ReleaseDC(std::ptr::null_mut(), sdc);
        if bmp.is_null() || p.is_null() { return; }
        src = CreateCompatibleDC(std::ptr::null_mut());
        SelectObject(src, bmp as *mut winapi::ctypes::c_void);
        bits = p as *mut u8;
        TBITS.store(bits as isize, Ordering::Relaxed);
        TDC.store(src as isize, Ordering::Relaxed);
    }
    // 预乘：黑的分量恒为 0，只改 alpha
    *bits.offset(0) = 0;
    *bits.offset(1) = 0;
    *bits.offset(2) = 0;
    *bits.offset(3) = alpha;
    let bf = BLENDFUNCTION {
        BlendOp: AC_SRC_OVER as u8,
        BlendFlags: 0,
        SourceConstantAlpha: 255,
        AlphaFormat: AC_SRC_ALPHA as u8,
    };
    AlphaBlend(dc, x, y, w, h, src, 0, 0, 1, 1, bf);
}


// 整屏合成缓冲：遮罩每一帧先在内存 DIB 上画完，最后一次性 BitBlt 上屏。
//
// 直接往窗口 DC 上画的时候，"贴整屏原图"（SetDIBitsToDevice 一整屏 8MB）与后面
// 四周的 AlphaBlend 压暗会分成两批落到屏幕上：鼠标一动、每秒几十帧的时候，
// 屏幕看到的中间那一瞬就是**没压暗的原图** —— 肉眼就是整屏"一亮一暗"地闪，
// 桌面图标这种高对比画面上尤其刺眼。合成到内存里再单次呈现，一帧就是一个完整画面。
/// 整屏合成 DC（尺寸不对就重建）。建不出来时返回 None，调用方退回直接画窗口 DC。
static SEL_BUF: BufDc = BufDc::new(true);

unsafe fn sel_buf_free() { SEL_BUF.free(); }

/// 闲置回收（由主窗口的 TRIM_TIMER 调用）：整屏 8MB DIB 不框选时不必常驻。
pub(crate) unsafe fn sel_buf_trim(idle_ms: i64) { SEL_BUF.trim(idle_ms); }

unsafe fn sel_buf_dc(w: i32, h: i32) -> Option<winapi::shared::windef::HDC> { SEL_BUF.get(w, h) }

/// 贴整屏原图（不压暗）。尺寸必须与目标客户区一致且缓冲够长，否则不画 —— 防越界。
unsafe fn sel_blit_full(hdc: winapi::shared::windef::HDC, w: i32, h: i32) {
    if w <= 0 || h <= 0 { return; }
    let sg = SEL_BGRA.lock_ok();
    let (iw, ih, buf) = match &*sg { Some(v) => v, None => return };
    if *iw != w as u32 || *ih != h as u32 { return; }
    if buf.len() < (w as usize) * (h as usize) * 4 { return; }
    let mut bmi: BITMAPINFO = std::mem::zeroed();
    bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down，与 BGRA 缓冲一致
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, 0, 0, w as u32, h as u32, 0, 0, 0, h as u32,
        buf.as_ptr() as *const winapi::ctypes::c_void, &bmi, DIB_RGB_COLORS);
}

/// 选区外四块压暗（32% 黑 alpha）。选区无效时整屏压暗。
pub(crate) unsafe fn sel_dim_outside(hdc: winapi::shared::windef::HDC, w: i32, h: i32,
                          sx: i32, sy: i32, ex: i32, ey: i32) {
    const A: u8 = ui::alpha::SCRIM; // 82/255 ≈ 0.3216
    if w <= 0 || h <= 0 { return; }
    if sx < 0 || sy < 0 || ex <= sx || ey <= sy {
        blend_black(hdc, 0, 0, w, h, A);
        return;
    }
    let (sx, sy) = (sx.max(0), sy.max(0));
    let (ex, ey) = (ex.min(w), ey.min(h));
    blend_black(hdc, 0, 0, w, sy, A);              // 上
    blend_black(hdc, 0, ey, w, h - ey, A);         // 下
    blend_black(hdc, 0, sy, sx, ey - sy, A);       // 左
    blend_black(hdc, ex, sy, w - ex, ey - sy, A);  // 右
}

pub(crate) unsafe fn draw_size_label(dc: winapi::shared::windef::HDC, x: i32, y: i32, w: i32, h: i32) {
    let pad = sc(ui::layout::cap::SIZE_LABEL_PAD).max(1);
    // 统一用 Microsoft YaHei 12：换族之后字宽/行高就不一致了
    // 标签每帧都画（拖框时），字体走全局缓存，不再一帧一建一删
    let lf = ui_font_px(sc(ui::font::SIZE_12), ui::font::W_REGULAR) as winapi::shared::windef::HFONT;
    let oldf = SelectObject(dc, lf as *mut winapi::ctypes::c_void);
    let txt = wide(&format!("X:{} Y:{} R:{} B:{} W:{} H:{}", x, y, x + w, y + h, w, h));
    let mut ext: winapi::shared::windef::SIZE = std::mem::zeroed();
    GetTextExtentPoint32W(dc, txt.as_ptr(), txt.len() as i32 - 1, &mut ext);
    let (tw, th) = (ext.cx, ext.cy);
    let (mut lx, mut ly) = (x, y - pad - th - pad * 2);
    if ly < 0 {
        lx = x + pad;
        ly = y + pad / 2;
    }
    blend_black(dc, lx, ly, tw + pad * 2, th + pad * 2, ui::alpha::SCRIM);
    SetBkMode(dc, TRANSPARENT as i32);
    SetTextColor(dc, ui::color::cref(ui::color::TEXT_INVERSE));
    TextOutW(dc, lx + pad, ly + pad, txt.as_ptr(), txt.len() as i32 - 1);
    SelectObject(dc, oldf);
}
// 尺寸/位置/配色：
// 取景 3.75 倍，源 50×30 → 图 188×113
//     光标那一像素固定落在图的正中
//   · 图下面再留 66 逻辑像素放四行信息（HEX / RGB / CMYK / POS），字号 10、内边距 5；
//     字带右侧贴边两行提示「复制信息 / 按C」，字号 12、微软雅黑 UI、右对齐并整体垂直居中
//   · 卡片底色 56% 黑，十字半宽 4、颜色 ColorF(0.1,0.5,1,0.5)
//   · 贴在光标右下 10 逻辑像素处；右边/下边放不下就翻到左上
// 只在"还没按下"时出现—— 一按下就收起，让位给选区
static PIX_BITS: AtomicIsize = AtomicIsize::new(0); // 常驻 DIB 的像素指针
static PIX_DC: AtomicIsize = AtomicIsize::new(0);   // 复用的内存 DC（只用来写字）
static PIX_FONT: AtomicIsize = AtomicIsize::new(0);
static PIX_FONT_HINT: AtomicIsize = AtomicIsize::new(0); // 字带右侧两行提示的字体（雅黑 UI 12）

/// 常驻 DIB + 内存 DC。和 anno 那边一样：自己算好像素一次性贴出，不再逐个 GDI 调用
unsafe fn sel_pix_card() -> Option<(*mut u8, winapi::shared::windef::HDC)> {
    let bits = PIX_BITS.load(Ordering::Relaxed) as *mut u8;
    if !bits.is_null() {
        return Some((bits, PIX_DC.load(Ordering::Relaxed) as winapi::shared::windef::HDC));
    }
    let sdc = GetDC(std::ptr::null_mut());
    let mut bmi: BITMAPINFO = std::mem::zeroed();
    bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
    bmi.bmiHeader.biWidth = ui::metric::PIX_W;
    bmi.bmiHeader.biHeight = -ui::metric::PIX_CARD_H_MAX;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    let mut p: *mut winapi::ctypes::c_void = std::ptr::null_mut();
    let bmp = CreateDIBSection(sdc, &mut bmi, DIB_RGB_COLORS, &mut p, std::ptr::null_mut(), 0);
    ReleaseDC(std::ptr::null_mut(), sdc);
    if bmp.is_null() || p.is_null() {
        if !bmp.is_null() { DeleteObject(bmp as *mut winapi::ctypes::c_void); }
        return None;
    }
    let mdc = CreateCompatibleDC(std::ptr::null_mut());
    if mdc.is_null() {
        DeleteObject(bmp as *mut winapi::ctypes::c_void);
        return None; // 不能把 PIX_DC=0 缓存下来，否则放大镜卡片永远画不出字
    }
    SelectObject(mdc, bmp as *mut winapi::ctypes::c_void);
    PIX_BITS.store(p as isize, Ordering::Relaxed);
    PIX_DC.store(mdc as isize, Ordering::Relaxed);
    Some((p as *mut u8, mdc))
}

/// RGB → CMYK 百分比（纯算术，无色彩管理）
fn cmyk_of(r: u8, g: u8, b: u8) -> (i32, i32, i32, i32) {
    let (rf, gf, bf) = (r as f64 / 255.0, g as f64 / 255.0, b as f64 / 255.0);
    let k = 1.0 - rf.max(gf).max(bf);
    let f = |c: f64| if k >= 1.0 { 0 } else { ((1.0 - c - k) / (1.0 - k) * 100.0).round() as i32 };
    (f(rf), f(gf), f(bf), (k * 100.0).round() as i32)
}

/// 放大镜底下那四行取色信息（HEX / RGB / CMYK / POS），按光标位置现算。
/// 绘制和"按 C 复制"共用这一份 —— 格式只写一处，日后改格式不会两边不一致
/// 读的是进场时留下的整屏像素副本，O(1)。
fn pix_info_lines(px: i32, py: i32) -> [String; 4] {
    // 收的是 GetCursorPos 的**屏幕坐标**
    // POS 行原样打印屏幕坐标，取色才换算成遮罩局部坐标
    let (ox, oy) = sel_org();
    let (mx, my) = (px - ox, py - oy);
    let (sr, sg, sb) = {
        let g = SEL_BGRA.lock_ok();
        match &*g {
            Some((iw, ih, d)) if mx >= 0 && my >= 0 && mx < *iw as i32 && my < *ih as i32 => {
                let k = ((my as usize) * (*iw as usize) + mx as usize) * 4;
                (d[k + 2], d[k + 1], d[k])
            }
            _ => (0, 0, 0),
        }
    };
    let (c, m, y, k) = cmyk_of(sr, sg, sb);
    [
        format!("HEX : #{:02X}{:02X}{:02X}", sr, sg, sb),
        format!("RGB : {},{},{}", sr, sg, sb),
        format!("CMYK : {},{},{},{}", c, m, y, k),
        format!("POS : X:{} Y:{}", px, py),
    ]
}

unsafe fn sel_paint_pix(hdc: winapi::shared::windef::HDC, sw: i32, sh: i32) {
    // 按下（拖动中）不画
    if SEL_START.lock_ok().is_some() { return; }
    // paintPix 的 pixReady：抓屏失败时没有底图，放大镜整块都不画
    // （画出来会是一张全黑卡，用户会以为截图坏了）
    if SEL_BGRA.lock_ok().is_none() { return; }
    let (card, mdc) = match sel_pix_card() { Some(v) => v, None => return };
    let f = dpi() as f32 / 96.0;
    let mut pt: POINT = std::mem::zeroed();
    GetCursorPos(&mut pt);
    // 底图/压暗副本都是遮罩局部坐标，屏幕光标要先减去原点（副屏时原点非 0）
    let (ox, oy) = sel_org();
    let (mx, my) = (pt.x - ox, pt.y - oy);
    let info_h = (ui::layout::cap::PIX_INFO_H as f32 * f).round() as i32;
    // 常驻 DIB 只有 PIX_CARD_H_MAX 行：高 DPI 下信息条再高也不能越过它
    let card_h = (ui::metric::PIX_IMG_H + info_h).min(ui::metric::PIX_CARD_H_MAX);
    let span = ui::layout::cap::PIX_GAP as f32 * f;
    // 取色框位置：右下优先，放不下翻到左上（x、y 各自独立判断）
    let mut x = (mx as f32 + span + f).trunc() as i32;
    if x + ui::metric::PIX_W > sw { x = (mx as f32 - span - ui::metric::PIX_W as f32 + f).trunc() as i32; }
    let mut y = (my as f32 + span + f).trunc() as i32;
    if y + card_h > sh { y = (my as f32 - span - card_h as f32 + f).trunc() as i32; }
    let buf = std::slice::from_raw_parts_mut(card, (ui::metric::PIX_W * ui::metric::PIX_CARD_H_MAX * 4) as usize);

    //    所以底色取决于卡片压在哪儿：压在高亮/选区里（未压暗）→ 原图 ×0.44；
    //    压在压暗区（×0.68）→ 原图 ×0.30。这里逐像素照这个口径算
    //    （不调 GdiAlphaBlend：底图本来就在我们手里，一次乘算即可，省一张掩膜位图）
    {
        let sg = SEL_BGRA.lock_ok();
        let hv = *SEL_HOVER.lock_ok();
        let ok = matches!(&*sg, Some((iw, ih, _)) if *iw == sw as u32 && *ih == sh as u32);
        for j in 0..card_h {
            let sy = y + j;
            let row = (j * ui::metric::PIX_W * 4) as usize;
            for i in 0..ui::metric::PIX_W {
                let sx = x + i;
                let o = row + (i * 4) as usize;
                let (b, g, r) = if ok && sx >= 0 && sy >= 0 && sx < sw && sy < sh {
                    let d = match &*sg { Some((_, _, d)) => d, None => { buf[o..o + 4].copy_from_slice(&[0, 0, 0, 255]); continue; } };
                    let k = ((sy as usize) * (sw as usize) + sx as usize) * 4;
                    let pct = match hv {
                        Some((hx1, hy1, hx2, hy2)) if sx >= hx1 && sx < hx2 && sy >= hy1 && sy < hy2 => ui::layout::cap::PIX_BG_KEEP_ON,
                        _ => ui::layout::cap::PIX_BG_KEEP_OFF,
                    };
                    (
                        (d[k] as u32 * pct / 100) as u8,
                        (d[k + 1] as u32 * pct / 100) as u8,
                        (d[k + 2] as u32 * pct / 100) as u8,
                    )
                } else { (0, 0, 0) };
                buf[o] = b; buf[o + 1] = g; buf[o + 2] = r; buf[o + 3] = 255;
            }
        }
    }

    // 2) 3.75 倍放大（最近邻）：光标像素 → 卡片正中
    {
        let sg = SEL_BGRA.lock_ok();
        if let Some((iw, ih, src)) = &*sg {
            if *iw == sw as u32 && *ih == sh as u32 {
                let hw = ui::metric::PIX_SRC_W / 2;
                let hh = ui::metric::PIX_SRC_H / 2;
                for j in 0..ui::metric::PIX_IMG_H {
                    // 目标像素 → 源像素：i/3.75 等价于 i*50/188（整数乘除，不要浮点）
                    let sy = my - hh + j * ui::metric::PIX_SRC_H / ui::metric::PIX_IMG_H;
                    if sy < 0 || sy >= sh { continue; }
                    let row = (j * ui::metric::PIX_W * 4) as usize;
                    for i in 0..ui::metric::PIX_W {
                        let sx = mx - hw + i * ui::metric::PIX_SRC_W / ui::metric::PIX_W;
                        if sx < 0 || sx >= sw { continue; }
                        let k = ((sy as usize) * (*iw as usize) + sx as usize) * 4;
                        let o = row + (i * 4) as usize;
                        buf[o] = src[k]; buf[o + 1] = src[k + 1]; buf[o + 2] = src[k + 2]; buf[o + 3] = 255;
                    }
                }
            }
        }
    }

    // 3) 十字辅助线：四段矩形，50% 不透明的 (0.1,0.5,1)
    {
        let hw = (ui::layout::cap::PIX_CROSS_HALF as f32 * f).round() as i32;
        let (cx, cy) = (ui::metric::PIX_W / 2, ui::metric::PIX_IMG_H / 2);
        let rects = [
            (0, cy - hw, cx - hw, cy + hw),
            (cx + hw, cy - hw, ui::metric::PIX_W, cy + hw),
            (cx - hw, 0, cx + hw, cy - hw),
            (cx - hw, cy + hw, cx + hw, ui::metric::PIX_IMG_H),
        ];
        for (x0, y0, x1, y1) in rects.iter() {
            for yy in (*y0).max(0)..(*y1).max(0) {
                let row = (yy * ui::metric::PIX_W * 4) as usize;
                for xx in (*x0).max(0)..(*x1).max(0) {
                    let o = row + (xx * 4) as usize;
                    buf[o] = (buf[o] as u32 / 2 + 255 / 2) as u8;
                    buf[o + 1] = (buf[o + 1] as u32 / 2 + 128 / 2) as u8;
                    buf[o + 2] = (buf[o + 2] as u32 / 2 + 26 / 2) as u8;
                }
            }
        }
    }

    // 4) 1px 边框（DrawRectangle(pixRect, brushBg, dpi)，颜色同底色）
    for i in 0..ui::metric::PIX_W {
        for o in [(i * 4) as usize, ((card_h - 1) * ui::metric::PIX_W + i) as usize * 4] {
            for c in 0..3 { buf[o + c] = (buf[o + c] as u32 * ui::layout::cap::PIX_EDGE_KEEP / 100) as u8; }
        }
    }
    for j in 0..card_h {
        for o in [(j * ui::metric::PIX_W) as usize * 4, ((j * ui::metric::PIX_W + ui::metric::PIX_W - 1) as usize) * 4] {
            for c in 0..3 { buf[o + c] = (buf[o + c] as u32 * ui::layout::cap::PIX_EDGE_KEEP / 100) as u8; }
        }
    }

    // 5) 四行信息（格式与"按 C 复制"共用同一份，见 pix_info_lines）
    let lines = pix_info_lines(pt.x, pt.y);
    let fh = PIX_FONT.load(Ordering::Relaxed) as *mut winapi::ctypes::c_void;
    let fh = if fh.is_null() {
        let nf = CreateFontW(-sc(ui::font::SIZE_10), 0, 0, 0, ui::font::W_REGULAR, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH | FF_SWISS, wide(ui::font::UI).as_ptr());
        PIX_FONT.store(nf as isize, Ordering::Relaxed);
        nf as *mut winapi::ctypes::c_void
    } else { fh };
    let oldf = SelectObject(mdc, fh);
    SetBkMode(mdc, TRANSPARENT as i32);
    SetTextColor(mdc, ui::color::cref(ui::color::TEXT_INVERSE));
    let pad = (ui::layout::cap::PIX_INFO_PAD as f32 * f).round() as i32;
    let fsz = (ui::font::SIZE_10 as f32 * f).round() as i32;
    for (i, s) in lines.iter().enumerate() {
        let ws = wide(s);
        // 行盒顶 = IMG_H + pad*(i+1) + 字号*i；字符格顶比行盒顶再低 2px，
        // 所以这里回退 2
        let ty = ui::metric::PIX_IMG_H + pad * (i as i32 + 1) + fsz * i as i32 - ui::layout::cap::PIX_TEXT_DY;
        TextOutW(mdc, pad, ty, ws.as_ptr(), ws.len() as i32 - 1);
    }
    // 字带右侧贴边两行提示「复制信息 / 按C」：字号 12、微软雅黑 UI、右对齐，
    // 两行在整条字带里整体垂直居中。纯提示
    // 真正复制走框选阶段的 C 键
    let hf = PIX_FONT_HINT.load(Ordering::Relaxed) as *mut winapi::ctypes::c_void;
    let hf = if hf.is_null() {
        let nf = CreateFontW(-sc(ui::font::SIZE_12), 0, 0, 0, ui::font::W_REGULAR, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH | FF_SWISS, wide(ui::font::UI_ALT).as_ptr());
        PIX_FONT_HINT.store(nf as isize, Ordering::Relaxed);
        nf as *mut winapi::ctypes::c_void
    } else { hf };
    SelectObject(mdc, hf);
    {
        let a = wide("复制信息");
        let b = wide("按C");
        let mut sa: SIZE = std::mem::zeroed();
        let mut sb: SIZE = std::mem::zeroed();
        GetTextExtentPoint32W(mdc, a.as_ptr(), a.len() as i32 - 1, &mut sa);
        GetTextExtentPoint32W(mdc, b.as_ptr(), b.len() as i32 - 1, &mut sb);
        // GDI 的行高比行盒略高，两行间距按各字高累加；整体在字带里居中
        let total = sa.cy + sb.cy;
        let top = ui::metric::PIX_IMG_H + (info_h - total) / 2;
        let right = ui::metric::PIX_W - pad;
        TextOutW(mdc, right - sa.cx, top, a.as_ptr(), a.len() as i32 - 1);
        TextOutW(mdc, right - sb.cx, top + sa.cy, b.as_ptr(), b.len() as i32 - 1);
    }
    SelectObject(mdc, oldf);

    // 6) 一次性贴到窗口
    winapi::um::wingdi::GdiFlush();
    let mut bmi: BITMAPINFO = std::mem::zeroed();
    bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
    bmi.bmiHeader.biWidth = ui::metric::PIX_W;
    bmi.bmiHeader.biHeight = -ui::metric::PIX_CARD_H_MAX;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, x, y, ui::metric::PIX_W as u32, card_h as u32,
        0, 0, 0, card_h as u32, card as *const winapi::ctypes::c_void,
        &bmi, DIB_RGB_COLORS);
}

// 全屏框选遮罩窗口
// 画一块"取景框"：先把选区从原图拷回来（回亮），再描 #1677FF 双线框，最后钉尺寸标签。
// 拖框时画当前选区，未拖动时画光标下的窗口 —— 两条路共用同一份绘制，观感才一致。
unsafe fn sel_paint_frame(hdc: winapi::shared::windef::HDC, w: i32, h: i32, sx: i32, sy: i32, ex: i32, ey: i32) {
            // 先裁进客户区：识别出来的窗口矩形可能有一部分在屏幕外（窗口被拖到边上），
            // 而下面按行算出来的源下标一旦是负数，转成 usize 就是天文数字 → 切片越界。
            // 半出屏的窗口会让按行算出的源下标变成负数，转 usize 就是天文数字 → 切片越界。
            let (sx, sy) = (sx.max(0), sy.max(0));
            let (ex, ey) = (ex.min(w), ey.min(h));
            if ex - sx < 1 || ey - sy < 1 { return; }
            let cw = ex - sx; let ch = ey - sy;
            // 选区内外都贴整屏原图：WM_PAINT 先贴原图，只对选区外做 alpha 压暗
            // 蓝框（2 逻辑像素粗线）
            //   DrawRectangle(rc 外扩 halfStroke, 2*dpi)
            // 描边以路径为中心 → 整条框落在选区**外侧**，正好压住压暗层最里面那一圈，
            // 选区里的像素一个都不被盖住（预览和最终截图完全一致）
            let blue = ui::color::cref(ui::color::SELECTION);
            let br = CreateSolidBrush(blue);
            let th = ((ui::metric::SELECT_STROKE as f32 * dpi() as f32 / 96.0).round() as i32).max(1);
            for (x1, y1, x2, y2) in [
                (sx - th, sy - th, ex + th, sy),         // 上
                (sx - th, ey, ex + th, ey + th),         // 下
                (sx - th, sy, sx, ey),                   // 左
                (ex, sy, ex + th, ey),                   // 右
            ] {
                let mut r = RECT { left: x1, top: y1, right: x2, bottom: y2 };
                FillRect(hdc, &mut r, br);
            }
            DeleteObject(br as *mut winapi::ctypes::c_void);
            // 尺寸标签（X/Y/R/B/W/H）—— 与标注编辑器里那一块共用同一份绘制
            draw_size_label(hdc, sx, sy, cw, ch);
}

// ---- 窗口识别：框选时"点一下就框住整个窗口" ----
// 识别表的一项
#[derive(Clone, Copy)]
struct SnapRect {
    l: i32,
    t: i32,
    r: i32,
    b: i32,
    root: i32, // 所属顶层窗口的 z 序位次（0 = 最上层）；子控件继承父窗口的位次
    top: bool, // 这一项本身是顶层窗口，不是子控件
    // 所属顶层窗口的句柄。用它对上"系统点名的那一个窗口"：光标压着的窗口
    // 才能参选，被它盖住的窗口哪怕正好有小控件压在同一点也不许参选
    hwnd: isize,
}

impl SnapRect {
    fn rect(&self) -> (i32, i32, i32, i32) { (self.l, self.t, self.r, self.b) }
    fn area(&self) -> i64 {
        ((self.r - self.l) as i64) * ((self.b - self.t) as i64)
    }
}

// 桌面图标层的一项：slot = explorer 给的图标槽位，hit = 整项包围盒（图标 + 文字标签）。
// 两个粒度是滚轮循环想要的语义（整格 / 图标本体），与"面板 → 窗口"同一套
#[derive(Clone, Copy)]
struct DeskIcon {
    slot: (i32, i32, i32, i32),
    hit: (i32, i32, i32, i32),
}

struct SnapCollect {
    out: Vec<SnapRect>,
    z: i32,
}

struct ChildCollect {
    out: Vec<(i32, i32, i32, i32)>,
}

// 遮罩要铺的那块屏：光标所在显示器的**整块**矩形（rcMonitor 而不是 rcWork ——
// 框选要能盖住任务栏、也能框任务栏上的窗口）。拿不到显示器信息就退回主屏
unsafe fn sel_monitor_rect() -> (i32, i32, i32, i32) {
    // 设置页"截图范围=所有屏幕"：直接取虚拟桌面外接矩形
    if load_dword(SEL_SCOPE_VALUE, 0) == 1 {
        let v = (GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                 GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
        if v.2 > 0 && v.3 > 0 { return v; }
    }
    let mut pt: POINT = std::mem::zeroed();
    GetCursorPos(&mut pt);
    let mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if !mon.is_null() {
        let mut mi: MONITORINFO = std::mem::zeroed();
        mi.cbSize = std::mem::size_of::<MONITORINFO>() as u32;
        if GetMonitorInfoW(mon, &mut mi) != 0 {
            let r = mi.rcMonitor;
            if r.right - r.left > 0 && r.bottom - r.top > 0 {
                return (r.left, r.top, r.right - r.left, r.bottom - r.top);
            }
        }
    }
    (0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN))
}

// 遮罩原点 / 尺寸（局部坐标系的基准）。没开会话时是 (0,0) / (0,0)
pub(crate) fn sel_org() -> (i32, i32) { (SEL_OX.load(Ordering::Relaxed), SEL_OY.load(Ordering::Relaxed)) }
pub(crate) fn sel_vp() -> (i32, i32) { (SEL_VW.load(Ordering::Relaxed), SEL_VH.load(Ordering::Relaxed)) }

// 把屏幕矩形裁进遮罩范围：完全在外面的丢掉
// 剩下的小于 6px 也丢掉 —— 6 是 的门槛，比它小的框选出来没有任何意义
unsafe fn sel_clip_rect(r: (i32, i32, i32, i32)) -> Option<(i32, i32, i32, i32)> {
    let (ox, oy) = sel_org();
    let (w, h) = sel_vp();
    let (l, t, rr, b) = (r.0 - ox, r.1 - oy, r.2 - ox, r.3 - oy);
    if rr <= 0 || b <= 0 || l >= w || t >= h { return None; }
    let (cl, ct) = (l.max(0), t.max(0));
    let (cr, cb) = (rr.min(w), b.min(h));
    if cr - cl <= 6 || cb - ct <= 6 { return None; }
    Some((cl, ct, cr, cb))
}

// 候选表在拉起遮罩前枚举一次（z 序：EnumWindows 就是从最上层往下走）
unsafe extern "system" fn sel_enum_cb(hwnd: HWND, lp: LPARAM) -> i32 {
    let ctx = &mut *(lp as *mut SnapCollect);
    if IsWindowVisible(hwnd) == 0 { return 1; }
    // 本进程的窗口：只有主界面（CLS_MAIN）算"看得见的内容"，
    // 遮罩 / 工具条 / 贴图窗都是截图界面自己的一部分，必须跳过
    let mut pid: u32 = 0;
    GetWindowThreadProcessId(hwnd, &mut pid);
    if pid == winapi::um::processthreadsapi::GetCurrentProcessId() {
        let mut cn = [0u16; 64];
        let n = winapi::um::winuser::GetClassNameW(hwnd, cn.as_mut_ptr(), 64).max(0) as usize;
        if String::from_utf16_lossy(&cn[..n]) != CLS_MAIN { return 1; }
    }
    // 被 DWM 隐藏的窗口（UWP 挂起、别的虚拟桌面）看不见，别当候选
    let mut cloaked: u32 = 0;
    DwmGetWindowAttribute(hwnd, 14, &mut cloaked as *mut u32 as *mut winapi::ctypes::c_void, 4);
    if cloaked != 0 { return 1; }
    // 用扩展边框：去掉 Win10 那圈看不见的窗口阴影，框出来才严丝合缝
    let mut r: RECT = std::mem::zeroed();
    if DwmGetWindowAttribute(hwnd, 9, &mut r as *mut RECT as *mut winapi::ctypes::c_void,
        std::mem::size_of::<RECT>() as u32) != 0
        || r.right <= r.left || r.bottom <= r.top
    {
        if GetWindowRect(hwnd, &mut r) == 0 { return 1; }
    }
    // initWinRect 的门槛：> 6px，并且要裁进遮罩范围（半出屏的窗口贴边）
    let top = match sel_clip_rect((r.left, r.top, r.right, r.bottom)) {
        Some(v) => v,
        None => return 1,
    };
    let z = ctx.z;
    ctx.z += 1;
    ctx.out.push(SnapRect { l: top.0, t: top.1, r: top.2, b: top.3, root: z, top: true,
                            hwnd: hwnd as isize });
    // 子控件：让识别细化到按钮 / 输入框 / 面板。它们的尺寸经常不到 24px，也要单独成候选。
    let mut kids = ChildCollect { out: Vec::new() };
    winapi::um::winuser::EnumChildWindows(hwnd, Some(sel_child_cb),
        &mut kids as *mut ChildCollect as LPARAM);
    kids.out.sort_by_key(|k| ((k.2 - k.0) as i64) * ((k.3 - k.1) as i64));
    for (cl, ct, cr, cb) in kids.out {
        ctx.out.push(SnapRect { l: cl, t: ct, r: cr, b: cb, root: z, top: false, hwnd: hwnd as isize });
    }
    1
}

unsafe extern "system" fn sel_child_cb(hwnd: HWND, lp: LPARAM) -> i32 {
    let ctx = &mut *(lp as *mut ChildCollect);
    if IsWindowVisible(hwnd) == 0 { return 1; }
    let mut rc: RECT = std::mem::zeroed();
    if GetWindowRect(hwnd, &mut rc) == 0 { return 1; }
    // collectChild：只要求 > 6px，然后和顶层窗口一样裁进遮罩范围
    if rc.right - rc.left <= 6 || rc.bottom - rc.top <= 6 { return 1; }
    if let Some(v) = sel_clip_rect((rc.left, rc.top, rc.right, rc.bottom)) {
        ctx.out.push(v);
    }
    1
}

unsafe fn sel_collect_windows() {
    let mut ctx = SnapCollect { out: Vec::new(), z: 0 };
    EnumWindows(Some(sel_enum_cb), &mut ctx as *mut SnapCollect as LPARAM);
    *SEL_WINS.lock_ok() = ctx.out;
    // 只换识别表：不碰当前悬停框
    // 停定维护时清一下，屏幕会"框消失再出现"地闪一帧
    SEL_ENUM_TICK.store(winapi::um::sysinfoapi::GetTickCount() as i32, Ordering::Relaxed);
}

// ———————————————— 智能选区：三层识别 ————————————————
// 智能选区分三层识别，三层各管一段：
//   · 几何层：顶层窗口 + 它的子控件（有 HWND 的东西）
//   · 像素层：冻结画面里的颜色突变（Electron / 浏览器内容区这些"一个窗口装整块 UI"
//     的地方，几何层只给得出整窗，像素层才能贴着内容边走）
//   · 桌面图标层：桌面没有一个图标有 HWND，直接向 explorer 要槽位
// 跨窗口的竞争一律以 z 序为准（光标压着的那个窗口才参选）；窗口内部才按面积升序
// —— 面积只能回答"同一个窗口里谁更具体"。

// 光标这一点上"眼睛真能看见"的那个顶层窗口。不能用 WindowFromPoint：
// 遮罩自己是全屏置顶窗，问系统只会得到"你自己"。逐个矩形先判命中再问可见性，
// 省掉大部分系统调用（屏幕上通常只有个位数窗口盖住某一点）
unsafe fn sel_visible_root(x: i32, y: i32) -> HWND {
    // 入参是遮罩局部坐标，跟系统窗口矩形比之前先加回屏幕原点
    let (ox, oy) = sel_org();
    let (x, y) = (x + ox, y + oy);
    let me = winapi::um::processthreadsapi::GetCurrentProcessId();
    let mut h = GetTopWindow(std::ptr::null_mut());
    while !h.is_null() {
        let mut rc: RECT = std::mem::zeroed();
        if GetWindowRect(h, &mut rc) != 0
            && x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom
            && IsWindowVisible(h) != 0
            && winapi::um::winuser::IsIconic(h) == 0
        {
            let mut pid: u32 = 0;
            GetWindowThreadProcessId(h, &mut pid);
            let mut cloaked: u32 = 0;
            DwmGetWindowAttribute(h, 14, &mut cloaked as *mut u32 as *mut winapi::ctypes::c_void, 4);
            // 自己的窗口里只有主界面算"看得见的内容"
            // 截自己的主界面时，想吸的就是里面的输入框和按钮
            let mine_ok = if pid == me {
                let mut cn = [0u16; 64];
                let n = winapi::um::winuser::GetClassNameW(h, cn.as_mut_ptr(), 64).max(0) as usize;
                String::from_utf16_lossy(&cn[..n]) == CLS_MAIN
            } else { true };
            if mine_ok && cloaked == 0 {
                return h;
            }
        }
        h = GetWindow(h, winapi::um::winuser::GW_HWNDNEXT);
    }
    std::ptr::null_mut()
}

// 桌面有两只宿主：静态壁纸是 Progman 直接挂 SHELLDLL_DefView；幻灯片 / 动态壁纸
// 把 DefView 挪到某个 WorkerW 下面，Progman 只剩一张壁纸。两只都要认
unsafe fn sel_is_desktop(h: HWND) -> bool {
    if h.is_null() { return false; }
    let mut cn = [0u16; 32];
    let n = winapi::um::winuser::GetClassNameW(h, cn.as_mut_ptr(), 32).max(0) as usize;
    matches!(String::from_utf16_lossy(&cn[..n]).as_str(), "Progman" | "WorkerW")
}

// 桌面那个 SysListView32（图标全在里面，一个图标一个 item，没有一个 HWND）
unsafe fn sel_desk_list() -> HWND {
    let cached = SEL_DESK_LIST.load(Ordering::Relaxed) as HWND;
    // explorer 重启之后句柄就废了，每次都用 IsWindow 复核
    if !cached.is_null() && winapi::um::winuser::IsWindow(cached) != 0 { return cached; }
    SEL_DESK_LIST.store(0, Ordering::Relaxed);
    let defv = wide("SHELLDLL_DefView");
    let lv = wide("SysListView32");
    let pg = wide("Progman");
    let ww = wide("WorkerW");
    let mut def = FindWindowExW(FindWindowW(pg.as_ptr(), std::ptr::null()), std::ptr::null_mut(),
                                defv.as_ptr(), std::ptr::null());
    if def.is_null() {
        let mut w = FindWindowExW(std::ptr::null_mut(), std::ptr::null_mut(), ww.as_ptr(), std::ptr::null());
        while !w.is_null() && def.is_null() {
            def = FindWindowExW(w, std::ptr::null_mut(), defv.as_ptr(), std::ptr::null());
            if def.is_null() {
                w = FindWindowExW(std::ptr::null_mut(), w, ww.as_ptr(), std::ptr::null());
            }
        }
    }
    if def.is_null() { return std::ptr::null_mut(); }
    let lv_h = FindWindowExW(def, std::ptr::null_mut(), lv.as_ptr(), std::ptr::null());
    SEL_DESK_LIST.store(lv_h as isize, Ordering::Relaxed);
    lv_h
}

// 给 explorer 发消息，20ms 封顶：它卡住（正在拷几十 G / 弹了模态框）时默认的
// SendMessage 会把我们自己的界面一起钉死。SMTO_ABORTIFHUNG：对方没在泵消息就立刻放弃
unsafe fn sel_list_msg(h: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> isize {
    let mut r: usize = 0;
    if SendMessageTimeoutW(h, msg, wp, lp,
        winapi::um::winuser::SMTO_ABORTIFHUNG | winapi::um::winuser::SMTO_BLOCK, 20, &mut r) == 0
    {
        return 0;
    }
    r as isize
}

// LVM_* 不在 winapi 的常量表里，按 commctrl.h 的数值写死
const LVM_GETITEMCOUNT: u32 = 0x1000 + 4;
const LVM_GETITEMRECT: u32 = 0x1000 + 14;
const LVIR_BOUNDS: i32 = 0;
const LVIR_ICON: i32 = 1;

// 刷新桌面图标表。800ms 节流：explorer 自己算槽位，不需要每帧问。
// 跨进程要一块远程缓冲（LVM_GETITEMRECT 的 lParam 是**对方进程里的指针**），
// 一块反复用，不要每项申请一次
unsafe fn sel_desk_refresh(force: bool) -> bool {
    let now = winapi::um::sysinfoapi::GetTickCount() as i32;
    let old = SEL_DESK_STAMP.load(Ordering::Relaxed);
    if !force {
        // deskIconAt 的节流：表建过一次之后，800ms 内直接吃上次的结果
        // 过了就只"标待办"（SEL_DESK_WANT），真扫描放到鼠标停住之后
        if old != 0 {
            if now.wrapping_sub(old) < 800 {
                return !SEL_DESK.lock_ok().is_empty();
            }
            SEL_DESK_WANT.store(true, Ordering::Relaxed);
            return !SEL_DESK.lock_ok().is_empty();
        }
    }
    // 先落时间戳：即便这次失败（explorer 被保护 / UIPI 拦了），也别每帧重试一遍
    SEL_DESK_STAMP.store(now, Ordering::Relaxed);
    SEL_DESK.lock_ok().clear();
    let lv = sel_desk_list();
    if lv.is_null() { return false; }
    let count = sel_list_msg(lv, LVM_GETITEMCOUNT, 0, 0);
    if count <= 0 || count > 4000 { return false; }
    let mut pid: u32 = 0;
    GetWindowThreadProcessId(lv, &mut pid);
    let proc_h = OpenProcess(winapi::um::winnt::PROCESS_VM_OPERATION
        | winapi::um::winnt::PROCESS_VM_READ | winapi::um::winnt::PROCESS_VM_WRITE, 0, pid);
    if proc_h.is_null() { return false; }
    let remote = winapi::um::memoryapi::VirtualAllocEx(proc_h, std::ptr::null_mut(),
        32, winapi::um::winnt::MEM_COMMIT | winapi::um::winnt::MEM_RESERVE,
        winapi::um::winnt::PAGE_READWRITE);
    if remote.is_null() { winapi::um::handleapi::CloseHandle(proc_h); return false; }
    // 列表客户区原点在屏幕上的位置（任务栏在左 / 多显示器时不是 0,0）
    let mut org: POINT = std::mem::zeroed();
    winapi::um::winuser::ClientToScreen(lv, &mut org);
    // 图标槽位最终要落在遮罩局部坐标里：客户端原点减掉遮罩原点即可
    let (sox, soy) = sel_org();
    let (gx, gy) = (org.x - sox, org.y - soy);

    let mut list: Vec<DeskIcon> = Vec::with_capacity(count as usize);
    for i in 0..count {
        // 两次 LVM_GETITEMRECT 复用同一块远程缓冲：kind 写在请求矩形的 left 里，
        // top 是项序号，这是 commctrl 定死的协议
        let mut slot: RECT = std::mem::zeroed();
        let mut got = false;
        let req: RECT = RECT { left: LVIR_ICON, top: i as i32, right: 0, bottom: 0 };
        if winapi::um::memoryapi::WriteProcessMemory(proc_h, remote,
            &req as *const RECT as *const winapi::ctypes::c_void, 16, std::ptr::null_mut()) != 0
            && sel_list_msg(lv, LVM_GETITEMRECT, i as usize, remote as isize) != 0
            && winapi::um::memoryapi::ReadProcessMemory(proc_h, remote,
                &mut slot as *mut RECT as *mut winapi::ctypes::c_void, 16, std::ptr::null_mut()) != 0
        {
            got = true;
        }
        if !got { continue; }
        if slot.right - slot.left < 8 || slot.bottom - slot.top < 8 { continue; }
        // 整项包围盒（图标 + 标签）：标签行数 explorer 自己算，一行两行都对 ——
        // 官方那条虚线框用的就是 LVIR_BOUNDS。只按"槽位 + 固定 45% 标签带"估，
        // 两行标题的第二行必然被切掉
        let mut bounds: RECT = std::mem::zeroed();
        let mut has_bounds = false;
        let req_b: RECT = RECT { left: LVIR_BOUNDS, top: i as i32, right: 0, bottom: 0 };
        if winapi::um::memoryapi::WriteProcessMemory(proc_h, remote,
            &req_b as *const RECT as *const winapi::ctypes::c_void, 16, std::ptr::null_mut()) != 0
            && sel_list_msg(lv, LVM_GETITEMRECT, i as usize, remote as isize) != 0
            && winapi::um::memoryapi::ReadProcessMemory(proc_h, remote,
                &mut bounds as *mut RECT as *mut winapi::ctypes::c_void, 16, std::ptr::null_mut()) != 0
        {
            has_bounds = bounds.right - bounds.left >= slot.right - slot.left
                && bounds.bottom - bounds.top >= slot.bottom - slot.top;
        }
        let slot = (slot.left + gx, slot.top + gy, slot.right + gx, slot.bottom + gy);
        let hit = if has_bounds {
            // 槽位与 BOUNDS 取并集（explorer 给的 BOUNDS 一般已经包住槽位，
            // 但"仅显示图标 / 紧凑布局"下不一定）：直接算 min/max，不借 API
            let bl = bounds.left + gx; let bt = bounds.top + gy;
            let br = bounds.right + gx; let bb = bounds.bottom + gy;
            (bl.min(slot.0), bt.min(slot.1), br.max(slot.2), bb.max(slot.3))
        } else {
            // 取不到 BOUNDS（旧版 explorer / 跨进程被拦）：槽位 + 45% 标签带兜底
            let lh = ((slot.3 - slot.1) * 45 / 100).max(12);
            (slot.0, slot.1, slot.2, slot.3 + lh)
        };
        list.push(DeskIcon { slot, hit });
    }
    winapi::um::memoryapi::VirtualFreeEx(proc_h, remote, 0, winapi::um::winnt::MEM_RELEASE);
    winapi::um::handleapi::CloseHandle(proc_h);
    *SEL_DESK.lock_ok() = list;
    !SEL_DESK.lock_ok().is_empty()
}

// 槽位 → 图标本体。explorer 给的槽位是"格"，图形居中放在里面（76×52 的格子里
// 图形只有 48×48）。在冻结画面里扫出图形的真实包围盒，框才贴得住抗锯齿边缘。
// 判定底色取槽位四角的主色：静态壁纸是纯色，渐变壁纸四角也只差几个色阶
unsafe fn sel_refine_glyph(slot: (i32, i32, i32, i32)) -> Option<(i32, i32, i32, i32)> {
    let g = SEL_BGRA.lock_ok();
    let (w, h, d) = match &*g {
        Some((w, h, d)) => (*w as i32, *h as i32, d.as_slice()),
        None => return None,
    };
    let at = |x: i32, y: i32| -> u32 {
        if x < 0 || y < 0 || x >= w || y >= h { return 0; }
        let k = ((y as usize) * (w as usize) + x as usize) * 4;
        d[k] as u32 | ((d[k + 1] as u32) << 8) | ((d[k + 2] as u32) << 16)
    };
    let cd = |a: u32, b: u32| -> i32 {
        ((a & 0xFF) as i32 - (b & 0xFF) as i32).abs()
            + (((a >> 8) & 0xFF) as i32 - ((b >> 8) & 0xFF) as i32).abs()
            + (((a >> 16) & 0xFF) as i32 - ((b >> 16) & 0xFF) as i32).abs()
    };
    let (l, t, r, b) = (slot.0.max(1), slot.1.max(1), slot.2.min(w - 1), slot.3.min(h - 1));
    if r - l < 8 || b - t < 8 { return None; }
    let corners = [at(l + 1, t + 1), at(r - 2, t + 1), at(l + 1, b - 2), at(r - 2, b - 2)];
    let mut bg = corners[0];
    let mut best_n = 0;
    for a in corners {
        let n = corners.iter().filter(|&&c| cd(a, c) < 30).count();
        if n > best_n { best_n = n; bg = a; }
    }
    let (mut minx, mut miny, mut maxx, mut maxy) = (i32::MAX, i32::MAX, -1, -1);
    for y in t..b {
        for x in l..r {
            // 60 = "明显不是底色"：抗锯齿过渡像素不参与，免得包围盒被羽化边撑到格子大小
            if cd(at(x, y), bg) <= 60 { continue; }
            if x < minx { minx = x; }
            if y < miny { miny = y; }
            if x > maxx { maxx = x; }
            if y > maxy { maxy = y; }
        }
    }
    if maxx < 0 { return None; }
    let (bw, bh) = (maxx - minx + 1, maxy - miny + 1);
    let (sw, sh) = (r - l, b - t);
    // 前景太小 = 底色估错了；前景铺满整格 = 壁纸太花，分不出图形与背景。两种都退回槽位
    if bw < sw * 2 / 5 && bh < sh * 2 / 5 { return None; }
    if bw >= sw - 2 && bh >= sh - 2 { return None; }
    Some((minx, miny, maxx + 1, maxy + 1))
}

// 光标落在哪个桌面图标上：给出两个粒度 —— 图标本体（滚轮往里切）与整格（默认）
unsafe fn sel_desk_at(x: i32, y: i32) -> Option<((i32, i32, i32, i32), (i32, i32, i32, i32))> {
    if !sel_desk_refresh(false) { return None; }
    let list = SEL_DESK.lock_ok();
    // 同一位置上偶尔叠着两个（图标拖得半叠 / 长名字伸进邻居的带里）：取槽位最小的
    let mut best: Option<&DeskIcon> = None;
    let mut best_area = i64::MAX;
    for d in list.iter() {
        if x < d.hit.0 || x >= d.hit.2 || y < d.hit.1 || y >= d.hit.3 { continue; }
        let a = ((d.slot.2 - d.slot.0) as i64) * ((d.slot.3 - d.slot.1) as i64);
        if a < best_area { best_area = a; best = Some(d); }
    }
    let d = *best?;
    drop(list);
    let glyph = sel_refine_glyph(d.slot).unwrap_or(d.slot);
    Some((glyph, d.hit))
}

// —— 像素层 ——
// 在冻结画面里量出"光标所在的那个框"：四个方向各找第一处"跳变 + 稳住"的边界。
// 判据两级：
//   · 颜色差超过 42 才算一次跳变；
//   · 跳过去之后那一侧还要连续 6 个像素保持同色 —— 用来滤文字笔画（只有两三像素宽，
//     跨过去马上又变回来），真正的边界后面是一整片同色区。
// 最后还要验边：沿每条边取样，边线本身与内侧一格必须明显不同；撞到屏幕边的直接不合格
// —— 四条方向线撞出来的"拼凑框"（桌面空白）不是任何一个元素，选上去等于吸了个寂寞。
// 桌面不走这一层：图标边缘是抗锯齿软边，第一跳会越过图形咬到格子网格线（框大两倍）
unsafe fn sel_pixel_box_at(px: i32, py: i32) -> Option<(i32, i32, i32, i32)> {
    let g = SEL_BGRA.lock_ok();
    let (w, h, d) = match &*g {
        Some((w, h, d)) => (*w as i32, *h as i32, d.as_slice()),
        None => return None,
    };
    if w <= 2 || h <= 2 || px < 1 || py < 1 || px >= w - 1 || py >= h - 1 { return None; }
    let at = |x: i32, y: i32| -> u32 {
        if x < 0 || y < 0 || x >= w || y >= h { return 0; }
        let k = ((y as usize) * (w as usize) + x as usize) * 4;
        d[k] as u32 | ((d[k + 1] as u32) << 8) | ((d[k + 2] as u32) << 16)
    };
    let cd = |a: u32, b: u32| -> i32 {
        ((a & 0xFF) as i32 - (b & 0xFF) as i32).abs()
            + (((a >> 8) & 0xFF) as i32 - ((b >> 8) & 0xFF) as i32).abs()
            + (((a >> 16) & 0xFF) as i32 - ((b >> 16) & 0xFF) as i32).abs()
    };
    // 阈值跟 DPI 走：200% 缩放下边框有 2~3 物理像素，稳住的距离也得放宽
    let s = (dpi() as f32 / 96.0).max(1.0);
    let k_stable = ((6.0 * s) as i32).max(4);
    const K_JUMP: i32 = 42;
    const K_STEADY_TOL: i32 = 14;
    let scan = |x0: i32, y0: i32, dx: i32, dy: i32, limit: i32| -> i32 {
        for i in 1..=limit {
            let x = x0 + dx * i;
            let y = y0 + dy * i;
            if x < 0 || y < 0 || x >= w || y >= h { return -1; }
            let c = at(x, y);
            if cd(c, at(x - dx, y - dy)) < K_JUMP { continue; }
            let mut steady = true;
            for k in 1..=k_stable {
                let nx = x + dx * k;
                let ny = y + dy * k;
                if nx < 0 || ny < 0 || nx >= w || ny >= h { break; }
                if cd(at(nx, ny), c) > K_STEADY_TOL { steady = false; break; }
            }
            if steady { return i; }
        }
        -1
    };
    let dl = scan(px, py, -1, 0, px);
    let dr = scan(px, py, 1, 0, w - 1 - px);
    let du = scan(px, py, 0, -1, py);
    let dd = scan(px, py, 0, 1, h - 1 - py);
    if dl < 0 || dr < 0 || du < 0 || dd < 0 { return None; }
    // 边界落在"跨过去的那一个像素"上，内侧那一边要让回 1 像素（pixelBoxAt 的）
    // r.right = pos.x + dr - 1 / r.bottom = pos.y + dd - 1）
    let (l, t, r, b) = (px - dl + 1, py - du + 1, px + dr - 1, py + dd - 1);
    let min_side = (8.0 * s) as i32;
    if r - l < min_side || b - t < min_side { return None; }
    // 铺满整屏的框没有意义：压暗层四块宽度为 0，看着像什么都没吸到
    if r - l >= w - 3 && b - t >= h - 3 { return None; }
    let (xl, yt, xr, yb) = (l, t, r - 1, b - 1);
    if xl < 1 || yt < 1 || xr >= w - 1 || yb >= h - 1 { return None; }
    let edge_score = |x0: i32, y0: i32, ux: i32, uy: i32, nx: i32, ny: i32, len: i32| -> i32 {
        // 沿这条边取样，拿"边上的像素"和它内侧一格比（法线指向框内）。
        // 边上那一圈是图形与背景的过渡像素，往框里退一格才是元素自己的颜色，
        // 两者差异明显才说明这条线真是元素的边 —— 不是四个方向随便撞出来的假框
        let step = (len / 24).max(4);
        let (mut hits, mut tries) = (0, 0);
        let mut k = 0;
        while k <= len {
            let (x, y) = (x0 + ux * k, y0 + uy * k);
            let (ix, iy) = (x + nx, y + ny);
            if x < 1 || y < 1 || x >= w - 1 || y >= h - 1 { return -1; }
            if ix < 0 || iy < 0 || ix >= w || iy >= h { return -1; }
            if cd(at(x, y), at(ix, iy)) > K_JUMP / 2 { hits += 1; }
            tries += 1;
            k += step;
        }
        if tries < 6 { return -1; }
        hits * 100 / tries
    };
    // 40%：允许抗锯齿 / 圆角 / 羽化边缘处看不出边界，但不能过半看不出来。
    // 实测真元素四边 44~55，拼凑框只有 20~24，中间留了足够的安全带
    const NEED: i32 = 40;
    let sc_l = edge_score(xl, yt, 0, 1, 1, 0, yb - yt);
    let sc_r = edge_score(xr, yt, 0, 1, -1, 0, yb - yt);
    let sc_t = edge_score(xl, yt, 1, 0, 0, 1, xr - xl);
    let sc_b = edge_score(xl, yb, 1, 0, 0, -1, xr - xl);
    if sc_l < NEED || sc_r < NEED || sc_t < NEED || sc_b < NEED { return None; }
    Some((l, t, r, b))
}

// 光标落在所有窗口之外（桌面空白、贴着窗口边缘往外一点）：退一步吸"最近的那个顶层窗口"。
// 没有这一条，用户把鼠标往窗口边上一靠就什么都不吸，必须精确进到窗口里面才算数
unsafe fn sel_nearest_window(x: i32, y: i32) -> Option<(i32, i32, i32, i32)> {
    let max_d = (24.0 * (dpi() as f32 / 96.0)) as i32;
    let list = SEL_WINS.lock_ok();
    let mut best: Option<(i32, i32, i32, i32)> = None;
    let mut best_d = max_d + 1;
    let mut best_rank = i32::MAX;
    for s in list.iter() {
        if !s.top { continue; }   // 子控件只在它自己窗口被命中后才有意义
        let dx = (s.l - x).max(0).max(x - s.r);
        let dy = (s.t - y).max(0).max(y - s.b);
        let d = ((dx * dx + dy * dy) as f64).sqrt() as i32;
        if d > max_d { continue; }
        if d < best_d || (d == best_d && s.root < best_rank) {
            best_d = d;
            best_rank = s.root;
            best = Some(s.rect());
        }
    }
    best
}

// 铺满截图区的矩形不当候选（桌面窗口、最大化的全屏窗口都是这一类）。
// 吸上去等于"框住整屏"：压暗层四块宽度为 0，屏幕看着一点没变，但选区数据确实变成了整屏，
// 下一帧像素层又把它细化回小框 —— 压暗层在"没有"和"盖满全屏"之间来回切，就是跳闪
unsafe fn sel_is_full(r: (i32, i32, i32, i32)) -> bool {
    let tol = (4.0 * (dpi() as f32 / 96.0)) as i32;
    // 用遮罩尺寸判"铺满"，副屏上就不会把主屏尺寸当基准误判
    let (w, h) = sel_vp();
    r.0 <= tol && r.1 <= tol && r.2 >= w - tol && r.3 >= h - tol
}

// 光标点落在哪一块：返回最具体的那个矩形（可能是子控件）。顺便把候选表挂起来，
// Tab / 滚轮往上切时不用再算一遍
unsafe fn sel_hover_at(x: i32, y: i32) -> Option<(i32, i32, i32, i32)> {
    let root = sel_visible_root(x, y);
    SEL_HIT_ROOT.store(root as isize, Ordering::Relaxed);
    let mut out: Vec<SnapRect> = Vec::new();
    let prev_idx = SEL_HOVER_IDX.load(Ordering::Relaxed);

    // ① 桌面图标：三层里只有这一层说得对，命中就直接收工（后面三层既不准也会把它盖掉）
    let on_desk = sel_is_desktop(root);
    if on_desk {
        if let Some((glyph, item)) = sel_desk_at(x, y) {
            // 插入顺序不是面积顺序：整格排在 0 号 → 默认框整格，
            // 滚轮往里走才切到图标本体
            out.push(SnapRect { l: item.0, t: item.1, r: item.2, b: item.3, root: 0, top: false,
                                hwnd: root as isize });
            out.push(SnapRect { l: glyph.0, t: glyph.1, r: glyph.2, b: glyph.3, root: 0, top: false,
                                hwnd: root as isize });
            return sel_pick(out, prev_idx);
        }
    }

    // 桌面空白（图标格之间 / 图标之外）：不做早退 —— 桌面分支只在"命中图标"
    // 时收工，没命中就照常往下走三层。几何层这时只剩桌面自己（整屏，被 sel_is_full 过滤），
    // 像素层能给出壁纸里的均匀块，兜底的 nearestWindow 最近的一定是桌面整屏、同样被过滤，
    // 所以这里不会吸到隔壁那个窗口，也不会把框吸成整屏

    // ② 几何层：只认"系统点名的那一个窗口"名下的矩形，被它盖住的一律不参选
    if !root.is_null() {
        let mut rc: RECT = std::mem::zeroed();
        if DwmGetWindowAttribute(root, 9, &mut rc as *mut RECT as *mut winapi::ctypes::c_void,
            std::mem::size_of::<RECT>() as u32) != 0
        {
            GetWindowRect(root, &mut rc);
        }
        if rc.right > rc.left && rc.bottom > rc.top {
            // 屏幕坐标 → 遮罩局部坐标（副屏时遮罩原点不是 0,0）
            let (ox, oy) = sel_org();
            let root_rc = (rc.left - ox, rc.top - oy, rc.right - ox, rc.bottom - oy);
            {
                let tb = SEL_WINS.lock_ok();
                for s in tb.iter() {
                    // inside：四边都是开区间（贴着边框那一像素不算"在里面"）
                    if s.hwnd != root as isize { continue; }
                    if !(x > s.l && y > s.t && x < s.r && y < s.b) { continue; }
                    if sel_is_full(s.rect()) { continue; }
                    out.push(*s);
                }
            }
            // ③ 像素层：框必须落在"眼睛看见的那个窗口"里面，越界就是扫到别的窗口上了
            if let Some((pl, pt, pr, pb)) = sel_pixel_box_at(x, y) {
                let (rl, rt_, rr, rb) = root_rc;
                let (pl, pt, pr, pb) = (pl.max(rl), pt.max(rt_), pr.min(rr), pb.min(rb));
                let min_side = (8.0 * (dpi() as f32 / 96.0)) as i32;
                // 不去重：像素框与整窗矩形重合时，两份候选同样留着，
                // 滚轮就在这两份之间切换（看起来没变化）
                if pr - pl >= min_side && pb - pt >= min_side
                    && !sel_is_full((pl, pt, pr, pb))
                {
                    out.push(SnapRect { l: pl, t: pt, r: pr, b: pb, root: 0, top: false,
                                        hwnd: root as isize });
                }
            }
            // 系统点名的窗口没进识别表（刚弹出来 / 枚举之后界面又变了）：
            // 直接用它自己的矩形，绝不能因为"表里没有"就往下层找
            if out.is_empty() && !sel_is_full(root_rc) {
                let (l, t, r, b) = root_rc;
                out.push(SnapRect { l, t, r, b, root: 0, top: true, hwnd: root as isize });
            }
        }
    }

    // ④ 兜底：最近的顶层窗口。只有光标落在所有顶层窗口之外（贴窗口边 / 空白）才允许。
    // root 非空 = 眼睛看见的就是它，候选只能来自它自己或它的子控件 ——
    // 这里跨窗口往下找，就是"多个界面叠加时吸到后面界面"的唯一来源
    if out.is_empty() && root.is_null() {
        if let Some((l, t, r, b)) = sel_nearest_window(x, y) {
            if !sel_is_full((l, t, r, b)) {
                out.push(SnapRect { l, t, r, b, root: 0, top: true, hwnd: root as isize });
            }
        }
    }

    // 面积升序：同一个窗口里越小的元素越具体（跨窗口的竞争已经由 z 序解决）
    out.sort_by_key(|s| s.area());
    sel_pick(out, prev_idx)
}

// 把这一轮候选落到 SELECT_HOVER：下标上一次
// 这样滚轮切到的层不会因为手抖 1px 就被打回 0 号候选
fn sel_pick(out: Vec<SnapRect>, prev_idx: i32) -> Option<(i32, i32, i32, i32)> {
    if out.is_empty() {
        SEL_HOVER_IDX.store(-1, Ordering::Relaxed);
        *SEL_CANDS.lock_ok() = out;
        return None;
    }
    let idx = if prev_idx < 0 || prev_idx as usize >= out.len() { 0 } else { prev_idx as usize };
    let rc = out[idx].rect();
    SEL_HOVER_IDX.store(idx as i32, Ordering::Relaxed);
    *SEL_CANDS.lock_ok() = out;
    Some(rc)
}

// 记一次重枚举请求（600ms 节流），真正干活在鼠标停住之后（定时器 101）
unsafe fn sel_mark_enum_want() {
    let now = winapi::um::sysinfoapi::GetTickCount() as i32;
    let fresh = now.wrapping_sub(SEL_ENUM_TICK.load(Ordering::Relaxed)) > 600;
    let empty = SEL_CANDS.lock_ok().is_empty();
    if empty {
        if fresh { SEL_ENUM_WANT.store(true, Ordering::Relaxed); }
        return;
    }
    let root = SEL_HIT_ROOT.load(Ordering::Relaxed) as HWND;
    if root.is_null() { return; }
    let known = SEL_WINS.lock_ok().iter().any(|s| s.hwnd == root as isize);
    if !known && fresh { SEL_ENUM_WANT.store(true, Ordering::Relaxed); }
}

// 启动参数 --enter=pin / --enter pin：松手即贴屏
fn enter_arg_pin() -> bool {
    let a: Vec<String> = std::env::args_os()
        .skip(1)
        .map(|s| s.to_string_lossy().to_ascii_lowercase())
        .collect();
    a.iter().any(|s| s == "--enter=pin")
        || a.windows(2).any(|w| w[0] == "--enter" && w[1] == "pin")
}

// Tab / 滚轮：在候选里换一层（最具体 → 整窗 → 循环）
unsafe fn sel_cycle_window(dir: i32) {
    let c = SEL_CANDS.lock_ok();
    // cycleHover：只有一个候选时滚轮没有意义，直接不换
    if c.len() < 2 { return; }
    let n = c.len() as i32;
    let cur = SEL_HOVER_IDX.load(Ordering::Relaxed);
    let cur = if cur < 0 || cur >= n { 0 } else { cur };
    let k = (cur + dir).rem_euclid(n) as usize;
    SEL_HOVER_IDX.store(k as i32, Ordering::Relaxed);
    *SEL_HOVER.lock_ok() = Some(c[k].rect());
}


// 合帧：放大镜要跟着光标走，而鼠标可以按 125~1000Hz 送 WM_MOUSEMOVE。
// 每个消息都整屏重绘一次（全屏 blit + 放大镜合成）的话输入队列必然积压，
// 表现就是"手已停、放大镜还在追"。这里按 16ms 封顶
// 到点了立刻画，没到点就排一个一次性定时器，把这一帧合并到最近的那次重绘上。
const SEL_FRAME_MS: i32 = 16;
unsafe fn sel_frame(hwnd: HWND) {
    let now = winapi::um::sysinfoapi::GetTickCount() as i32;
    if now.wrapping_sub(SEL_FRAME_TICK.load(Ordering::Relaxed)) >= SEL_FRAME_MS {
        SEL_FRAME_TICK.store(now, Ordering::Relaxed);
        InvalidateRect(hwnd, std::ptr::null_mut(), 0);
    } else if !SEL_FRAME_PEND.load(Ordering::Relaxed) {
        SEL_FRAME_PEND.store(true, Ordering::Relaxed);
        SetTimer(hwnd, 102, SEL_FRAME_MS as u32, None);
    }
}

extern "system" fn sel_wnd_proc(hwnd: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_TIMER => unsafe {
            if wp as usize == 102 {
                KillTimer(hwnd, 102);
                SEL_FRAME_PEND.store(false, Ordering::Relaxed);
                SEL_FRAME_TICK.store(winapi::um::sysinfoapi::GetTickCount() as i32, Ordering::Relaxed);
                InvalidateRect(hwnd, std::ptr::null_mut(), 0);
            } else if wp as usize == 101 {
                // 停定维护：鼠标已经停住 30ms，才做重枚举
                // 顺手把桌面图标表的过期刷新也落下来
                KillTimer(hwnd, 101);
                if SEL_ENUM_WANT.swap(false, Ordering::Relaxed) {
                    sel_collect_windows();
                    SEL_ENUM_TICK.store(winapi::um::sysinfoapi::GetTickCount() as i32,
                        Ordering::Relaxed);
                }
                if SEL_DESK_WANT.swap(false, Ordering::Relaxed) {
                    sel_desk_refresh(true);
                }
            } else if wp as usize == 104 {
                // ESC 兜底轮询：输入法组字会把 WM_KEYDOWN 吞掉，这里直接看键状态。
                // 低两位都认 —— 2600Hz 键盘在 25ms 采样下可能"按下又抬起"落在两次之间
                let st = GetAsyncKeyState(VK_ESCAPE);
                let down = (st as u16 & 0x8000) != 0;
                let was = SEL_ESC_DOWN.swap(down, Ordering::Relaxed);
                if (st as u16 & 0x0001) != 0 || (down && !was) {
                    KillTimer(hwnd, 104);
                    DestroyWindow(hwnd);
                    SEL_HWND.store(0, Ordering::Relaxed);
                    let qh = MAIN_HWND.load(Ordering::Relaxed) as HWND;
                    if !qh.is_null() {
                        ShowWindow(qh, SW_SHOW);
                        SetForegroundWindow(qh);
                        SetFocus(qh);
                    }
                }
            } else if wp as usize == 103 {
                // 悬停挂住到期：真停在没有目标的地方了，才收掉预览框。
                // 只收"悬停智选"落下的那一份（SEL_PREVIEW），用户拖出来的框留着
                KillTimer(hwnd, 103);
                SEL_HOLD.store(false, Ordering::Relaxed);
                let pv = *SEL_PREVIEW.lock_ok();
                let mut hv = SEL_HOVER.lock_ok();
                if *hv == pv {
                    *hv = None;
                    SEL_CANDS.lock_ok().clear();
                    SEL_HOVER_IDX.store(-1, Ordering::Relaxed);
                }
                drop(hv);
                InvalidateRect(hwnd, std::ptr::null_mut(), 0);
            }
            0
        },
        WM_PAINT => unsafe {
            let mut ps: PAINTSTRUCT = std::mem::zeroed();
            let hdc = BeginPaint(hwnd, &mut ps);
            let (w, h) = sel_vp();
            // 全部画到内存 DIB 上，最后一次性 BitBlt —— 否则"贴原图"和"压暗"分两批上屏，
            // 鼠标一动就是整屏一亮一暗地闪（见 sel_buf_dc 上面的那段说明）
            let target = match sel_buf_dc(w, h) { Some(mdc) => mdc, None => hdc };
            // 1) 贴整屏原图（只有这一份缓冲，没有压暗副本）
            sel_blit_full(target, w, h);
            // 2) 选区外压暗：32% 黑 alpha 的四个矩形
            let (sx, sy, ex, ey) = sel_rect();
            // 按下时只 startMakeRect，maskRect 还是刚才那个智选框，直到第一次
            // WM_MOUSEMOVE 才被 makeRect 顶掉，这里：按下但还没挪过 → 继续显示预览框
            // 否则按一下就会整屏黑一帧
            let pressed = *SEL_START.lock_ok();
            let moved = match (pressed, *SEL_CUR.lock_ok()) {
                (Some(a), Some(b)) => a != b,
                _ => false,
            };
            let hover = if !moved { *SEL_HOVER.lock_ok() } else { None };
            if moved {
                // makeRect：先夹进客户区，再按 minSize=4 向右/下补齐（补出界就回收）
                let (l, t, r, b) = sel_fit_drag(w, h, sx, sy, ex, ey);
                sel_dim_outside(target, w, h, l, t, r, b);
                sel_paint_frame(target, w, h, l, t, r, b);
            } else if let Some((hx1, hy1, hx2, hy2)) = hover {
                // 还没开始拖：光标悬停在哪个窗口就先框哪个
                sel_dim_outside(target, w, h, hx1, hy1, hx2, hy2);
                sel_paint_frame(target, w, h, hx1, hy1, hx2, hy2);
            } else {
                sel_dim_outside(target, w, h, -1, -1, -1, -1);
            }
            // 3) 放大镜 + 取色（画在选区边框和标签之上）
            sel_paint_pix(target, w, h);
            if target != hdc {
                BitBlt(hdc, 0, 0, w, h, target, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            0
        },
        WM_LBUTTONDOWN => unsafe {
            let x = (lp & 0xFFFF) as i16 as i32;
            let y = ((lp >> 16) & 0xFFFF) as i16 as i32;
            // 智选框只是"参考"：一旦按下左键，就一律按用户自己的拖动重新画框，
            // 选区不锁在智选框上（没有按住框内=整体平移、压边=拉边那套手势）。
            // 只有"按下不拖、直接松手"才截光标下那块智选框，见 WM_LBUTTONUP。
            *SEL_START.lock_ok() = Some((x, y));
            *SEL_CUR.lock_ok() = Some((x, y));
            // 用户下笔了：悬停预览的"挂住"立刻作废，别让它的定时器在拖框期间到点
            if SEL_HOLD.swap(false, Ordering::Relaxed) {
                KillTimer(hwnd, 103);
            }
            // 用鼠标自己画框时吸附一律恢复（右键留下的待命状态只对"没动过的光标"有效）
            SEL_SNAP_ARMED.store(false, Ordering::Relaxed);
            SetCapture(hwnd);
            // 不擦背景：窗口本来没有背景刷，擦一下只会黑闪一帧
            InvalidateRect(hwnd, std::ptr::null_mut(), 0);
            0
        },
        // 右键：退回"刚按快捷键那一帧"的待命状态重新框选
        winapi::um::winuser::WM_RBUTTONDOWN => unsafe {
            *SEL_START.lock_ok() = None;
            *SEL_CUR.lock_ok() = None;
            *SEL_HOVER.lock_ok() = None;
            SEL_CANDS.lock_ok().clear();
            SEL_HOVER_IDX.store(-1, Ordering::Relaxed);
            *SEL_PREVIEW.lock_ok() = None;
            SEL_HOLD.store(false, Ordering::Relaxed);
            KillTimer(hwnd, 103);
            ReleaseCapture();
            // restartSelect：吸附先关，等鼠标真挪出 2px 由 onMove 打开
            let mut pt: POINT = std::mem::zeroed();
            GetCursorPos(&mut pt);
            let (ox, oy) = sel_org();
            *SEL_ARM_POS.lock_ok() = Some((pt.x - ox, pt.y - oy));
            SEL_SNAP_ARMED.store(true, Ordering::Relaxed);
            InvalidateRect(hwnd, std::ptr::null_mut(), 0);
            0
        },
        WM_MOUSEMOVE => unsafe {
            let x = (lp & 0xFFFF) as i16 as i32;
            let y = ((lp >> 16) & 0xFFFF) as i16 as i32;
            if SEL_START.lock_ok().is_some() {
                // 手动拖框一律不吸附：
                // 用户已经在画自己的框，四条边再往窗口 / 控件边界上跳，就是"手往前拉、
                // 框自己往旁边蹦"，想框的边永远差几像素。吸附只在悬停预览里做
                *SEL_CUR.lock_ok() = Some((x, y));
                sel_frame(hwnd);
            } else if SEL_MODE.load(Ordering::Relaxed) != 3 {
                // 文字/二维码识别（mode 1/2，旧的 0/4 同理）只要最基础的手动拖框：
                // 不认光标下的窗口/控件，等用户自己拉框。智选预览只留给标注截图 mode 3
                sel_frame(hwnd);
            } else {
                // 右键退回待命后、鼠标还没真挪动
                // 先别吸附，只让放大镜跟手；挪出 2px 才恢复识别，否则原地一个
                // mousemove 又立刻把框吸回来，等于右键没用
                if SEL_SNAP_ARMED.load(Ordering::Relaxed) {
                    let moved = match *SEL_ARM_POS.lock_ok() {
                        Some((ax, ay)) => (x - ax).abs() > 2 || (y - ay).abs() > 2,
                        None => true,
                    };
                    if !moved {
                        sel_frame(hwnd);
                        return 0;
                    }
                    SEL_SNAP_ARMED.store(false, Ordering::Relaxed);
                }
                // 还没按下去：光标下的元素整块预览，点一下就能整块截取；
                // 同时放大镜每帧都要跟着动，所以这里不只在高亮目标变了才重绘
                let hv = sel_hover_at(x, y);
                sel_mark_enum_want();
                // 每次移动都重设同一颗一次性定时器 = 天然防抖：手一直动就一直不维护，
                // 停下 30ms 才做那件重活（highlight 末尾 setTimer(30, 101)）
                SetTimer(hwnd, 101, 30, None);
                let mut cur = SEL_HOVER.lock_ok();
                if let Some(rc) = hv {
                    // 又吸到了目标：挂住作废，别让 103 到期把刚落好的框收掉
                    if SEL_HOLD.swap(false, Ordering::Relaxed) {
                        KillTimer(hwnd, 103);
                    }
                    if *cur != Some(rc) {
                        *cur = Some(rc);
                        *SEL_PREVIEW.lock_ok() = Some(rc);
                    }
                } else if cur.is_some() {
                    // 粘滞：光标还在当前框里就不动；已经离开才挂住
                    let inside = match *cur {
                        Some((l, t, r, b)) => x > l && y > t && x < r && y < b,
                        None => false,
                    };
                    if !inside && !SEL_HOLD.swap(true, Ordering::Relaxed) {
                        // 落进图标格间 / 窗口边缘的空白：不立刻收框（压暗层会一亮一暗地闪），
                        // 挂住 700ms；期间落到新目标就作废，真停空才由 WM_TIMER 103 收掉
                        SetTimer(hwnd, 103, 700, None);
                    }
                }
                drop(cur);
                sel_frame(hwnd);
            }
            0
        },
        // 滚轮：在同一位置重叠的候选之间循环（每次都按当前光标位置重算）
        // 光标重算一遍候选，再按下标 ±1；方向跟着滚轮方向）
        WM_MOUSEWHEEL => unsafe {
            // 只有标注截图（mode 3）才有「智选候选」可循环；识别类只认手动框
            if SEL_START.lock_ok().is_none() && SEL_MODE.load(Ordering::Relaxed) == 3 {
                let mut pt: POINT = std::mem::zeroed();
                GetCursorPos(&mut pt);
                // 系统给屏幕坐标，识别/预览全用遮罩局部坐标
                let (ox, oy) = sel_org();
                sel_hover_at(pt.x - ox, pt.y - oy);
                let dir = if ((wp >> 16) & 0xFFFF) as u16 as i16 > 0 { 1 } else { -1 };
                sel_cycle_window(dir);
                sel_frame(hwnd);
            }
            0
        },
        WM_LBUTTONUP => unsafe {
            ReleaseCapture();
            let (mut sx, mut sy, mut ex, mut ey) = sel_rect();
            // 按下就松开（拖动幅度 < 4px）= 不自己选，采用光标下那块智选框（仅作参考落定）
            if ex - sx < 4 && ey - sy < 4 {
                match *SEL_HOVER.lock_ok() {
                    Some((l, t, r, b)) => { sx = l; sy = t; ex = r; ey = b; }
                    // 既没拖、光标下也没东西（桌面空白 / 刚右键清过）：什么都不截，
                    // onUp 里 `if (!cutMask->hasRect) return，` 同一条
                    None => {
                        *SEL_START.lock_ok() = None;
                        *SEL_CUR.lock_ok() = None;
                        return 0;
                    }
                }
            }
            let mode = SEL_MODE.load(Ordering::Relaxed);
            // 同样裁进遮罩范围：整块截取那条路用的是识别到的窗口矩形，可能探到屏幕外，
            // 直接按它算裁剪下标就会越界
            let (scrw, scrh) = sel_vp();
            let (sx, sy, ex, ey) = sel_fit_drag(scrw, scrh, sx, sy, ex, ey);
            // 像素副本先摘出来再关窗：WM_DESTROY 会把整屏副本释放掉，关完再读
            // 就成了"截图失败"。而且只取一次，不再整份 clone（4K 下一次 66MB）
            let cap = SEL_BGRA.lock_ok().take();
            // 截图标注（mode 3）：遮罩先留着，等编辑器把首帧画完再拆。
            // 先拆遮罩的话，「遮罩消失」和「编辑器第一次 WM_PAINT」之间会露一帧
            // 没压暗的桌面 —— 这就是手动框选松手时闪一下的来源
            let mut keep_mask = mode == 3;
            if !keep_mask {
                DestroyWindow(hwnd);
                SEL_HWND.store(0, Ordering::Relaxed);
            }
            // 截图模式(3)：打开标注编辑器，主界面暂不恢复（标注窗口关闭后再恢复）
            // 其它模式：立即恢复主界面（识别在后台进行，识别完自动切页/回填）
            if mode != 3 {
                let qh = MAIN_HWND.load(Ordering::Relaxed) as HWND;
                if !qh.is_null() {
                    ShowWindow(qh, SW_SHOW);
                    SetForegroundWindow(qh);
                    SetFocus(qh);
                }
            }
            if ex > sx && ey > sy {
                match cap {
                    Some((sw, sh, buf)) => {
                        // 缓冲实际尺寸可能与遮罩尺寸不一致（DPI 切换 / 多屏偏移）：
                        // 按缓冲再裁一次，杜绝 crop_region_* 越界读
                        // （panic="abort"，一次越界就是整个进程消失）
                        let sx2 = (sx as u32).min(sw);
                        let sy2 = (sy as u32).min(sh);
                        let ex2 = (ex as u32).min(sw);
                        let ey2 = (ey as u32).min(sh);
                        let w2 = ex2.saturating_sub(sx2);
                        let h2 = ey2.saturating_sub(sy2);
                        if w2 == 0 || h2 == 0 {
                            if keep_mask {
                                DestroyWindow(hwnd);
                                SEL_HWND.store(0, Ordering::Relaxed);
                            }
                            return 0;
                        }
                        // Ctrl+松手 或 启动参数 --enter=pin = 跳过标注编辑器，直接把这块钉到桌面上。
                        // onUp 的顺序是 grabMode → enterByArg → Ctrl：二维码取像（mode 4）
                        // 两条分支都不走；这里同样把 mode 4 排除在外
                        if mode != 4 && (enter_arg_pin()
                            || (GetAsyncKeyState(VK_CONTROL) as u16 & 0x8000) != 0) {
                            let bgra = crop_region_bgra(&buf, sw, sx2, sy2, w2, h2);
                            let (sox, soy) = sel_org();
                            pin_open_img(bgra, w2 as i32, h2 as i32,
                                sx2 as i32 + sox, sy2 as i32 + soy);
                            if keep_mask {
                                DestroyWindow(hwnd);
                                SEL_HWND.store(0, Ordering::Relaxed);
                            }
                            return 0;
                        }
                        if mode == 3 {
                            // 截图标注：截取区域 BGRA → 打开标注编辑器（原生 1:1，位置=截取位置）
                            let bgra = crop_region_bgra(&buf, sw, sx2, sy2, w2, h2);
                            // 编辑器背景是「整屏原图 + 选区外 32% 压暗」
                            // 里的 Clear + DrawBitmap + cutMask->paint）。整屏副本必须交回去，
                            // 否则 anno_img_proc 拿不到底图，选区外会退化成纯黑。
                            // 只在编辑期间占用（1080p 8MB / 4K 33MB），anno_close 里释放。
                            *SEL_BGRA.lock_ok() = Some((sw, sh, buf));
                            anno_open(bgra, w2 as i32, h2 as i32, sx2 as i32, sy2 as i32);
                            // 老遮罩和编辑器画的是同一幅「整屏 + 压暗」，先把编辑器首帧
                            // 同步画出来（UpdateWindow 会立刻派发 WM_PAINT），再拆遮罩，
                            // 两帧内容一致，中间不会露桌面
                            let ihw = ANNO_HWND.load(Ordering::Relaxed) as HWND;
                            if !ihw.is_null() {
                                // 首帧已经在遮罩下面画完整了（UpdateWindow 同步派发 WM_PAINT）
                                UpdateWindow(ihw);
                                // 再一步提到最上层，并让合成器立刻应用这一帧：
                                // 上一帧还是遮罩、下一帧就是编辑器，两帧内容一致，中间没有过渡帧
                                SetWindowPos(ihw, HWND_TOPMOST, 0, 0, 0, 0,
                                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                                DwmFlush();
                                // 工具条跟着一起亮（它建的时候故意没显示）
                                let mb = ANNO_MAIN_HWND.load(Ordering::Relaxed) as HWND;
                                if !mb.is_null() { ShowWindow(mb, SW_SHOWNA); }
                            }
                            if keep_mask {
                                DestroyWindow(hwnd);
                                DwmFlush();
                                SEL_HWND.store(0, Ordering::Relaxed);
                                keep_mask = false;
                            }
                        } else {
                            std::thread::spawn(move || {
                                let qh = MAIN_HWND.load(Ordering::Relaxed) as HWND;
                                if mode == SEL_MODE_OCR && cfg!(feature = "ocr") {
                                    // 纯文字 OCR：裁出 BGRA 直接送引擎
                                    let bgra = crop_region_bgra(&buf, sw, sx2, sy2, w2, h2);
                                    let text = run_ocr_bgra(w2, h2, &bgra);
                                    SEL_QR_HIT.store(0, Ordering::Relaxed);
                                    *RECOG_RESULT.lock_ok() = text.clone();
                                } else {
                                    // 二维码识别（手动拖框 / 智选共一条路径）；mode=2 没解到码再走文字兜底
                                    let bgra = crop_region_bgra(&buf, sw, sx2, sy2, w2, h2);
                                    match decode_qr(&bgra, w2, h2) {
                                        Some(t) => {
                                            SEL_QR_HIT.store(1, Ordering::Relaxed);
                                            *RECOG_RESULT.lock_ok() = t.clone();
                                        }
                                        None => {
                                            SEL_QR_HIT.store(0, Ordering::Relaxed);
                                            if mode == SEL_MODE_QR_THEN_OCR && cfg!(feature = "ocr") {
                                                if !qh.is_null() {
                                                    PostMessageW(qh, WM_QR_OCR_FALLBACK, 0, 0);
                                                }
                                                *RECOG_RESULT.lock_ok() = run_ocr_bgra(w2, h2, &bgra);
                                            } else {
                                                if !qh.is_null() {
                                                    PostMessageW(qh, WM_QR_NOT_FOUND, 0, 0);
                                                }
                                                // 空结果：fill_result 不动输入框，只保留状态提示
                                                *RECOG_RESULT.lock_ok() = String::new();
                                            }
                                        }
                                    }
                                }
                                if !qh.is_null() {
                                    PostMessageW(qh, WM_QR_RESULT, 0, 0);
                                }
                            });
                        }
                    }
                    None => {
                        if mode != 3 {
                            *RECOG_RESULT.lock_ok() = "截图失败".to_string();
                            let qh = MAIN_HWND.load(Ordering::Relaxed) as HWND;
                            if !qh.is_null() { PostMessageW(qh, WM_QR_RESULT, 0, 0); }
                        }
                    }
                }
            }
            // 编辑器之外的路径（裁剪区为空 / 没有像素副本）：遮罩得在这儿拆掉
            if keep_mask {
                DestroyWindow(hwnd);
                SEL_HWND.store(0, Ordering::Relaxed);
            }
            0
        },
        WM_KEYDOWN => unsafe {
            // Tab：在光标处重叠的候选之间切换（子控件 ↔ 整窗 ↔ 下一个元素）
            if wp as i32 == 0x09 {
                sel_cycle_window(1);
                InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                return 0;
            }
            // C：把放大镜那四行取色信息一起复制进剪贴板
            // 带 Ctrl 的组合键留给别的功能，不抢）
            if wp as i32 == 0x43 && SEL_START.lock_ok().is_none()
                && (GetAsyncKeyState(VK_CONTROL) as u16 & 0x8000) == 0 {
                let mut pt: POINT = std::mem::zeroed();
                GetCursorPos(&mut pt);
                copy_text_clipboard(&pix_info_lines(pt.x, pt.y).join("\r\n"));
                return 0;
            }
            if wp as i32 == VK_ESCAPE {
                DestroyWindow(hwnd);
                SEL_HWND.store(0, Ordering::Relaxed);
                // 取消选择：恢复主界面
                let qh = MAIN_HWND.load(Ordering::Relaxed) as HWND;
                if !qh.is_null() {
                    ShowWindow(qh, SW_SHOW);
                    SetForegroundWindow(qh);
                    SetFocus(qh);
                }
            }
            // 方向键 1 像素微调光标：真正挪的是系统
            // 光标，随后的 WM_MOUSEMOVE 会把悬停识别、放大镜、尺寸标签一起刷新
            if matches!(wp as i32, VK_UP | VK_DOWN | VK_LEFT | VK_RIGHT) {
                let mut pt: POINT = std::mem::zeroed();
                GetCursorPos(&mut pt);
                match wp as i32 {
                    VK_UP => pt.y -= 1,
                    VK_DOWN => pt.y += 1,
                    VK_LEFT => pt.x -= 1,
                    _ => pt.x += 1,
                }
                winapi::um::winuser::SetCursorPos(pt.x, pt.y);
            }
            0
        },
        WM_DESTROY => unsafe {
            KillTimer(hwnd, 103);
            KillTimer(hwnd, 101);
            KillTimer(hwnd, 104);
            SEL_HOLD.store(false, Ordering::Relaxed);
            SEL_ENUM_WANT.store(false, Ordering::Relaxed);
            SEL_DESK_WANT.store(false, Ordering::Relaxed);
            SEL_SNAP_ARMED.store(false, Ordering::Relaxed);
            SEL_ESC_DOWN.store(false, Ordering::Relaxed);
            *SEL_ARM_POS.lock_ok() = None;
            *SEL_PREVIEW.lock_ok() = None;
            *SEL_START.lock_ok() = None;
            *SEL_CUR.lock_ok() = None;
            *SEL_HOVER.lock_ok() = None;
            SEL_CANDS.lock_ok().clear();
            SEL_WINS.lock_ok().clear();
            SEL_DESK.lock_ok().clear();
            SEL_DESK_LIST.store(0, Ordering::Relaxed);
            SEL_DESK_STAMP.store(0, Ordering::Relaxed);
            // 整屏合成缓冲（1080p 8MB / 4K 33MB）跟着这次框选一起还回去
            sel_buf_free();
            // 整屏副本 4K 下约 33MB，取消/收尾之后必须还回去；
            // 成功截图那条路已经先把副本 take 走了，这里拿到的是 None。
            // 唯一例外：截图标注态 —— 编辑器每次重绘（打字、画图）都要用它当背景，
            // 这里收走的话选区以外会在下一次重绘时退化成一整块黑底。
            // 那条路由 anno_close 负责释放
            if ANNO_HWND.load(Ordering::Relaxed) == 0 {
                let _ = SEL_BGRA.lock_ok().take();
            }
            DefWindowProcW(hwnd, msg, wp, lp)
        },
        _ => unsafe { DefWindowProcW(hwnd, msg, wp, lp) },
    }
}

// 计算当前选区（归一化）
fn sel_rect() -> (i32, i32, i32, i32) {
    let s = *SEL_START.lock_ok();
    let c = *SEL_CUR.lock_ok();
    match (s, c) {
        (Some((x1, y1)), Some((x2, y2))) => (x1.min(x2), y1.min(y2), x1.max(x2), y1.max(y2)),
        _ => (-1, -1, -1, -1),
    }
}

// 先夹进客户区，再按 minSize=4 向右/下补齐
// 补到出界就把另一头往回推 —— 预览边框、尺寸标签、最终截取的矩形必须同一个
fn sel_fit_drag(w: i32, h: i32, sx: i32, sy: i32, ex: i32, ey: i32) -> (i32, i32, i32, i32) {
    let (mut l, mut t) = (sx.clamp(0, w), sy.clamp(0, h));
    let (mut r, mut b) = (ex.clamp(0, w), ey.clamp(0, h));
    if r < l { std::mem::swap(&mut l, &mut r); }
    if b < t { std::mem::swap(&mut t, &mut b); }
    if r - l < 4 { r = (l + 4).min(w); if r - l < 4 { l = (r - 4).max(0); } }
    if b - t < 4 { b = (t + 4).min(h); if b - t < 4 { t = (b - 4).max(0); } }
    (l, t, r, b)
}

// 遮罩窗口建不出来时的一次性提示
fn show_cap_fail_tip() {
    static TIP_SHOWN: AtomicBool = AtomicBool::new(false);
    if TIP_SHOWN.swap(true, Ordering::Relaxed) { return; }
    unsafe {
        MessageBoxW(std::ptr::null_mut(),
            wide("图形设备初始化失败，截图功能暂时不可用。可尝试更新显卡驱动，或在虚拟机/远程桌面中开启硬件加速后重试。").as_ptr(),
            wide(APP_NAME).as_ptr(), MB_OK | MB_ICONINFORMATION);
    }
}

// 开始截图框选：截屏 + 弹出全屏遮罩（mode=0仅二维码 / 1仅文字 / 2两者）
pub(crate) fn start_region_select(mode: i32) {
    // 防重入：本函数下面有嵌套消息泵（hide 分支里的 PeekMessage/Dispatch），
    // 泵里可能再收到热键、托盘、按钮消息 → 又调进来一次 → 递归到栈溢出
    // （0xC00000FD，windows_subsystem=windows 下同样没有任何日志，表现为"自己退了"）。
    if SEL_REENTRANT.swap(true, Ordering::Relaxed) { return; }
    let _guard = SelReentryGuard; // 任何退出路径都会复位
    unsafe {
        // 同一时刻只允许一个遮罩：
        // 连按两下热键不能叠出两个全屏窗 —— 它们共用一个选区状态，会互相踩，
        // 而且关掉一个另一个还盖在屏幕上，下一次截图会把这张遮罩也截进去
        let cur = SEL_HWND.load(Ordering::Relaxed) as HWND;
        if !cur.is_null() && IsWindow(cur) != 0 { return; }
        SEL_HWND.store(0, Ordering::Relaxed);
        SEL_MODE.store(mode, Ordering::Relaxed);
        // 截图(mode=3)：不主动隐藏主界面（用户要求“不需要主动隐藏界面”），直接出全屏框选遮罩；
        // 文字/二维码(mode=1/2)：按设置隐藏主界面，确保抓屏不带本程序界面、不挡待识别内容
        let hide = if mode == 3 { 0 } else { load_dword(HIDE_ON_REC_VALUE, 0) };
        let mh = MAIN_HWND.load(Ordering::Relaxed) as HWND;
        if hide != 0 && !mh.is_null() {
            ShowWindow(mh, SW_HIDE);
        }
        if hide != 0 {
            DwmFlush();
            let mut msg: MSG = std::mem::zeroed();
            let mut n = 0;
            while PeekMessageW(&mut msg, std::ptr::null_mut(), 0, 0, PM_REMOVE) != 0 {
                if msg.message == WM_QUIT {
                    // 退出消息不能在嵌套泵里被吞掉：原样回投，交给主消息循环
                    PostQuitMessage(msg.wParam as i32);
                    return;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                n += 1;
                if n > 400 { break; }
            }
            std::thread::sleep(std::time::Duration::from_millis(200));
            // 防御：若主界面又被显示回来，再隐藏一次
            if !mh.is_null() && IsWindowVisible(mh) != 0 {
                ShowWindow(mh, SW_HIDE);
            }
        }
        let hinstance = GetModuleHandleW(std::ptr::null_mut());
        let cls = wide(CLS_SELWND);
        reg_class(CLS_SELWND, Some(sel_wnd_proc), CS_HREDRAW | CS_VREDRAW,
            IDC_CROSS as usize, false);
        // 遮罩铺哪块屏 → 存进全局原点/尺寸，后面所有坐标换算都以它为准
        let (ox, oy, w, h) = sel_monitor_rect();
        SEL_OX.store(ox, Ordering::Relaxed);
        SEL_OY.store(oy, Ordering::Relaxed);
        SEL_VW.store(w, Ordering::Relaxed);
        SEL_VH.store(h, Ordering::Relaxed);
        if let Some((iw, ih, buf)) = screenshot_bgra(ox, oy, w, h) {
            // 只留一份整屏原图：选区外压暗在绘制期做 alpha 混合，
            // 不 clone 第二份并按像素烘焙 —— 1080p 省 8 MB、4K 省 33 MB
            *SEL_BGRA.lock_ok() = Some((iw, ih, buf));
        } else {
            // 抓屏失败（锁屏 / UAC 安全桌面 / DWM 正在切换）：别把上一次的整屏副本
            // 留在全局里 —— 放大镜和取色会读到一张过期画面
            let _ = SEL_BGRA.lock_ok().take();
        }
        // 先把桌面上的窗口摸一遍：后面悬停高亮、Tab 循环、边缘吸附都靠这张表。
        // 识别类（mode 1/2）走纯手动拖框，用不到这些候选，跳过两次全屏枚举，遮罩出来更快
        SEL_ENUM_WANT.store(false, Ordering::Relaxed);
        SEL_DESK_WANT.store(false, Ordering::Relaxed);
        SEL_SNAP_ARMED.store(false, Ordering::Relaxed);
        SEL_ESC_DOWN.store(false, Ordering::Relaxed);
        *SEL_ARM_POS.lock_ok() = None;
        SEL_HOLD.store(false, Ordering::Relaxed);
        if SEL_MODE.load(Ordering::Relaxed) == 3 {
            sel_collect_windows();
            // 第一次悬停就得能出框：桌面图标表首次当场扫
            sel_desk_refresh(true);
        } else {
            SEL_WINS.lock_ok().clear();
            SEL_DESK.lock_ok().clear();
        }
        *SEL_HOVER.lock_ok() = None;
        *SEL_PREVIEW.lock_ok() = None;
        *SEL_CANDS.lock_ok() = Vec::new();
        SEL_HOVER_IDX.store(-1, Ordering::Relaxed);
        let sh = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls.as_ptr(), std::ptr::null(),
            WS_POPUP | WS_VISIBLE, ox, oy, w, h,
            std::ptr::null_mut(), std::ptr::null_mut(), hinstance, std::ptr::null_mut());
        if sh.is_null() {
            // 遮罩创建失败 → 弹失败提示，并把主界面放回来
            sel_buf_free();
            let _ = SEL_BGRA.lock_ok().take();
            if !mh.is_null() {
                ShowWindow(mh, SW_SHOW);
                SetForegroundWindow(mh);
            }
            show_cap_fail_tip();
            return;
        }
        SEL_HWND.store(sh as isize, Ordering::Relaxed);
        *SEL_START.lock_ok() = None;
        *SEL_CUR.lock_ok() = None;
        // 输入法在这个窗口上必须摘掉：中文状态下按 C 是在组字，WM_KEYDOWN 永远收不到
        // （建窗后 ImmAssociateContext(hwnd, nullptr) 摘掉输入法上下文）
        ImmAssociateContext(sh, 0);
        ShowWindow(sh, SW_SHOW);
        UpdateWindow(sh);
        SetForegroundWindow(sh);
        // 焦点必须落在遮罩上：Esc / Tab / C 都靠 WM_KEYDOWN，
        // 前台化不等于拿到键盘焦点（隐藏主界面之后尤其如此）
        SetFocus(sh);
        // ESC 兜底轮询：先把当前键状态吃掉一次，避免上次残留的按键立刻触发退出
        let st = GetAsyncKeyState(VK_ESCAPE);
        SEL_ESC_DOWN.store((st as u16 & 0x8000) != 0, Ordering::Relaxed);
        SetTimer(sh, 104, 25, None);
    }
}
