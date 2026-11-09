use crate::prelude::*;
use crate::*;

/// 截屏用的常驻 DIB（尺寸不变就一直复用）。
///
/// 录屏是 30fps 连续截屏，每帧都 CreateDIBSection + CreateCompatibleDC
/// 再删掉：1080p 下每秒建/拆 240MB，4K 就是每秒近 1GB。这里按尺寸常驻一份。
/// 用一把锁串起来 —— 录屏线程和 UI 线程都可能截屏，同一时刻只允许一个人在
/// BitBlt 上（一张 DIB 不能被两处同时写）。
static SHOT_LOCK: Mutex<()> = Mutex::new(());
static SHOT_DC: AtomicIsize = AtomicIsize::new(0);
static SHOT_BMP: AtomicIsize = AtomicIsize::new(0);
static SHOT_BITS: AtomicIsize = AtomicIsize::new(0);
static SHOT_W: AtomicI32 = AtomicI32::new(0);
static SHOT_H: AtomicI32 = AtomicI32::new(0);

/// 调用方必须已经持有 SHOT_LOCK（供截屏内部复用尺寸切换时调用）
unsafe fn shot_free_locked() {
    let bmp = SHOT_BMP.swap(0, Ordering::Relaxed) as *mut winapi::ctypes::c_void;
    let dc = SHOT_DC.swap(0, Ordering::Relaxed) as winapi::shared::windef::HDC;
    // DC 只能用 DeleteDC 还；误写成 DeleteObject 等于每次截屏漏一个 DC，
    // 录屏 30fps 一分钟就是 1800 个，最后 GetDC 直接失败（画面黑掉）
    if !dc.is_null() { DeleteDC(dc); }
    if !bmp.is_null() { DeleteObject(bmp); }
    SHOT_BITS.store(0, Ordering::Relaxed);
    SHOT_W.store(0, Ordering::Relaxed);
    SHOT_H.store(0, Ordering::Relaxed);
}

pub(crate) unsafe fn shot_free() {
    // 录制线程可能正在 BitBlt / memcpy，这里必须和截屏共用同一把锁，
    // 否则会在别人使用 DIB 的中途把它删掉
    let _guard = SHOT_LOCK.lock_ok();
    shot_free_locked();
}

/// 截取屏幕区域写进调用方给的缓冲（复用内存，录屏时不再每帧新申请一张整屏）。
/// 成功返回 true，dst 大小恒为 w*h*4（BGRA、top-down）。
pub(crate) fn screenshot_bgra_into(ox: i32, oy: i32, w: i32, h: i32, dst: &mut Vec<u8>) -> bool {
    unsafe {
        if w <= 0 || h <= 0 { return false; }
        let _guard = SHOT_LOCK.lock_ok();
        let hdc_screen = GetDC(std::ptr::null_mut());
        if hdc_screen.is_null() { return false; }
        if SHOT_DC.load(Ordering::Relaxed) == 0
            || SHOT_W.load(Ordering::Relaxed) != w
            || SHOT_H.load(Ordering::Relaxed) != h
        {
            // 直接建 DIB 段让 BitBlt 落进去，再整块 memcpy 出来：
            // 老写法是 CreateCompatibleBitmap → BitBlt → GetDIBits，GetDIBits 本身又做一遍
            // 全帧格式转换/搬运。录屏 30fps、1080p 时这就是每秒多搬 240MB。
            let mut bmi: BITMAPINFO = std::mem::zeroed();
            bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h;   // 负数 = top-down，与下游 BGRA 约定一致
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            let mut bits: *mut winapi::ctypes::c_void = std::ptr::null_mut();
            let hbmp = CreateDIBSection(hdc_screen, &mut bmi, DIB_RGB_COLORS, &mut bits,
                std::ptr::null_mut(), 0);
            let hdc_mem = if hbmp.is_null() {
                std::ptr::null_mut()
            } else {
                CreateCompatibleDC(hdc_screen)
            };
            if hbmp.is_null() || bits.is_null() || hdc_mem.is_null() {
                if !hbmp.is_null() { DeleteObject(hbmp as *mut winapi::ctypes::c_void); }
                if !hdc_mem.is_null() { DeleteDC(hdc_mem); }
                ReleaseDC(std::ptr::null_mut(), hdc_screen);
                return false;
            }
            SelectObject(hdc_mem, hbmp as *mut winapi::ctypes::c_void);
            // 尺寸变了才走到这里：旧的那套先还掉，再挂上新的
            shot_free_locked();
            SHOT_DC.store(hdc_mem as isize, Ordering::Relaxed);
            SHOT_BMP.store(hbmp as isize, Ordering::Relaxed);
            SHOT_BITS.store(bits as isize, Ordering::Relaxed);
            SHOT_W.store(w, Ordering::Relaxed);
            SHOT_H.store(h, Ordering::Relaxed);
        }
        let hdc_mem = SHOT_DC.load(Ordering::Relaxed) as winapi::shared::windef::HDC;
        let bits = SHOT_BITS.load(Ordering::Relaxed) as *const u8;
        if hdc_mem.is_null() || bits.is_null() { 
            ReleaseDC(std::ptr::null_mut(), hdc_screen);
            return false;
        }
        // CAPTUREBLT：分层窗口（工具提示、部分菜单/浮层）也一起抓进来，
        // 少了它这些区域会留成空洞。起点 (ox,oy) = 遮罩原点，副屏时不是 0,0
        if BitBlt(hdc_mem, 0, 0, w, h, hdc_screen, ox, oy, SRCCOPY | 0x4000_0000) == 0 {
            // 锁屏/安全桌面等情况下 BitBlt 会失败，不能把旧画面当成功返回
            ReleaseDC(std::ptr::null_mut(), hdc_screen);
            return false;
        }
        // 录制中把鼠标指针也画进画面
        // 光标是独立合成层，GDI 的 BitBlt 抓不到，不补这一下视频里就看不到鼠标
        if REC_PHASE.load(Ordering::Relaxed) == 1 {
            use winapi::um::winuser::{GetCursorInfo, GetIconInfo, DrawIconEx,
                CURSORINFO, ICONINFO, CURSOR_SHOWING};
            // DI_NORMAL。不能带 DI_DEFAULTSIZE：那会无视光标实际尺寸，
            // 一律按 32×32 画，高 DPI/大光标会被拉伸变形
            const DI_NORMAL: u32 = 3;
            let mut ci: CURSORINFO = std::mem::zeroed();
            ci.cbSize = std::mem::size_of::<CURSORINFO>() as u32;
            if GetCursorInfo(&mut ci) != 0 && ci.flags == CURSOR_SHOWING {
                // GetIconInfo 失败时不会填 iconInfo，拿去 DeleteObject 就是把垃圾交给 GDI
                let mut ii: ICONINFO = std::mem::zeroed();
                if GetIconInfo(ci.hCursor, &mut ii) != 0 {
                    let lx = ci.ptScreenPos.x - ox - ii.xHotspot as i32;
                    let ly = ci.ptScreenPos.y - oy - ii.yHotspot as i32;
                    if !ii.hbmMask.is_null() { DeleteObject(ii.hbmMask as *mut winapi::ctypes::c_void); }
                    if !ii.hbmColor.is_null() { DeleteObject(ii.hbmColor as *mut winapi::ctypes::c_void); }
                    // 指针不在录制范围里就别画（DrawIconEx 也会自己裁，省一次合成）
                    if lx >= 0 && lx < w && ly >= 0 && ly < h {
                        DrawIconEx(hdc_mem, lx, ly, ci.hCursor, 0, 0, 0,
                            std::ptr::null_mut(), DI_NORMAL);
                    }
                }
            }
        }
        // GDI 是批处理的：不清刷就直接读 DIB 位，可能读到还没落盘的旧像素（半张黑）
        GdiFlush();
        let n = w as usize * h as usize * 4;
        dst.resize(n, 0);
        std::ptr::copy_nonoverlapping(bits, dst.as_mut_ptr(), n);
        ReleaseDC(std::ptr::null_mut(), hdc_screen);
        true
    }
}

// 一次性截屏（选区/贴图等）：拷完立刻把常驻 DIB 还掉。
// 这张 DIB 是整屏尺寸（1080p 约 8MB），只有录屏那条 30fps 路径需要按尺寸常驻复用；
// 一次性用完后如果留着，进标注后它会一直陪着，关掉标注也退不回来。
pub(crate) fn screenshot_bgra(ox: i32, oy: i32, w: i32, h: i32) -> Option<(u32, u32, Vec<u8>)> {
    let mut buf: Vec<u8> = Vec::new();
    let ok = screenshot_bgra_into(ox, oy, w, h, &mut buf);
    unsafe { shot_free(); }
    if ok {
        Some((w as u32, h as u32, buf))
    } else {
        None
    }
}

// 从整屏 BGRA 裁剪区域 → BGRA（供 OCR 共享槽）
pub(crate) fn crop_region_bgra(buf: &[u8], w_full: u32, x0: u32, y0: u32, w: u32, h: u32) -> Vec<u8> {
    let mut out = vec![0u8; (w as usize) * (h as usize) * 4];
    if w_full == 0 || w == 0 || h == 0 { return out; }
    let full_h = (buf.len() / 4 / w_full as usize) as u32;
    let cw = w.min(w_full.saturating_sub(x0)) as usize;
    if cw == 0 { return out; }
    // 行内是连续的，整行 memcpy 比逐像素 push 快得多
    for yy in 0..h {
        let sy = y0 + yy;
        if sy >= full_h { break; }
        let si = (sy as usize * w_full as usize + x0 as usize) * 4;
        if si + cw * 4 > buf.len() { break; }
        let di = yy as usize * w as usize * 4;
        out[di..di + cw * 4].copy_from_slice(&buf[si..si + cw * 4]);
    }
    out
}
