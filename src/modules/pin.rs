use crate::prelude::*;
use crate::*;

// 贴在屏幕上：独立图钉窗口（自带像素副本，不受后续编辑影响）
// 用列表而不是单个句柄：允许同时贴好几张，
// 单句柄只能记住最后一张，前一张就脱离了所有程序侧管理（关不掉、也再也找不到）
pub(crate) static ANNO_PINS: Mutex<Vec<isize>> = Mutex::new(Vec::new());

// ---- 贴在屏幕上：独立图钉窗（自带像素副本，可拖动 / Ctrl+滚轮缩放 / 双击复制 / Esc 关闭） ----
//
// GWLP_USERDATA 上（Box<PinState>），这样同时贴多张图互不影响 —— 之前用一组全局量，
// 第二张图会把第一张的像素顶掉。
//
// 外观：
//   · 窗口比图每边大 1px（PIN_PAD），那 1px 画一条 30% 黑的隔离线，把贴图和背后的界面压开
//   · 隔离线内侧再压一条 1px 蓝线（0x1677ff，也就是 22,119,255）
//   · 落屏时那圈线按 白→青→蓝→紫 各 90ms 走一轮（"到场提示"）
//   · Ctrl+滚轮缩放，停手 800ms 才收掉右上角的倍数提示
// 三条线都是半透明/纯色叠加，窗口用分层窗口（UpdateLayeredWindow）逐像素给 alpha，
// 否则 30% 黑只能画成实心，贴在深色界面上会显出一圈脏边。

pub(crate) const PIN_PAD: i32 = ui::metric::PIN_PAD;
// 取值是 0xRRGGBB，这里按 (r,g,b) 存
pub(crate) const PIN_FLASH: [(u8, u8, u8); 4] = [
    ui::color::rgb8(ui::color::PIN_FLASH_WHITE),  // 白
    ui::color::rgb8(ui::color::PIN_FLASH_CYAN),   // 青
    ui::color::rgb8(ui::color::PIN_FLASH_BLUE),   // 蓝（与常态边框同色，过渡不突兀）
    ui::color::rgb8(ui::color::PIN_FLASH_PURPLE), // 紫
];
pub(crate) const PIN_BORDER: (u8, u8, u8) = ui::color::rgb8(ui::color::PIN_OUTLINE);
pub(crate) const PIN_FLASH_MS: u32 = 90;
pub(crate) const PIN_TIP_MS: u32 = 800;
pub(crate) const PIN_T_FLASH: usize = 102;
pub(crate) const PIN_T_TIP: usize = 101;

pub(crate) struct PinState {
pub(crate) img: Vec<u8>,      // 底图像素（BGRA、top-down、行步长 = iw*4）
pub(crate) iw: i32,
pub(crate) ih: i32,
pub(crate) scale: f32,
pub(crate) flash: i32,        // 还剩几步闪动
pub(crate) flash_idx: i32,
pub(crate) tip: bool,         // 倍数提示是否还挂着
pub(crate) drag: bool,
pub(crate) press: (i32, i32), // 按下点在窗口客户区里的偏移
}

pub(crate) unsafe fn pin_state(hwnd: HWND) -> *mut PinState {
    GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *mut PinState
}

/// 窗口尺寸 = round(底图 × 倍数) + 两侧隔离带
pub(crate) fn pin_size(st: &PinState) -> (i32, i32) {
    let w = (st.iw as f32 * st.scale).round().max(1.0) as i32;
    let h = (st.ih as f32 * st.scale).round().max(1.0) as i32;
    (w + PIN_PAD * 2, h + PIN_PAD * 2)
}

/// 倍数上限跟着底图大小走：窗口再大显存和眼睛都吃不消，
/// 但至少得能回到 1 倍
pub(crate) fn pin_max_scale(st: &PinState) -> f32 {
    let m = st.iw.max(st.ih).max(1) as f32;
    (16000.0 / m).min(8.0).max(1.0)
}

pub(crate) unsafe fn pin_put(buf: &mut [u8], w: i32, x: i32, y: i32, r: u8, g: u8, b: u8, a: u8) {
    if x < 0 || y < 0 || x >= w { return; }
    let i = ((y * w + x) * 4) as usize;
    if i + 3 >= buf.len() { return; }
    // 分层窗口要的是**预乘** alpha
    let f = |c: u8| ((c as u32 * a as u32 + 127) / 255) as u8;
    buf[i] = f(b); buf[i + 1] = f(g); buf[i + 2] = f(r); buf[i + 3] = a;
}

/// 32 位 DIB 上的 1px 矩形描边（GDI 的 Rectangle 带 NULL_BRUSH 会画在坐标线上，
/// 这里自己铺，省得跟"线段内缩半像素"的规则打架）
pub(crate) unsafe fn pin_stroke_rect(buf: &mut [u8], w: i32,
                          l: i32, t: i32, r: i32, b: i32, c: (u8, u8, u8), a: u8) {
    for x in l..=r { pin_put(buf, w, x, t, c.0, c.1, c.2, a); pin_put(buf, w, x, b, c.0, c.1, c.2, a); }
    for y in t..=b { pin_put(buf, w, l, y, c.0, c.1, c.2, a); pin_put(buf, w, r, y, c.0, c.1, c.2, a); }
}

/// 把当前状态整帧画进分层窗口
pub(crate) unsafe fn pin_paint(hwnd: HWND) {
    let stp = pin_state(hwnd);
    if stp.is_null() { return; }
    let st = &mut *stp;
    if st.img.is_empty() || st.iw <= 0 || st.ih <= 0 { return; }
    let (w, h) = pin_size(st);
    if w <= 2 || h <= 2 { return; }
    let mut rc: RECT = std::mem::zeroed();
    GetWindowRect(hwnd, &mut rc);
    let (winx, winy) = (rc.left, rc.top);

    let sdc = GetDC(std::ptr::null_mut());
    if sdc.is_null() { return; }
    let mdc = CreateCompatibleDC(sdc);
    let mut bmi: BITMAPINFO = std::mem::zeroed();
    bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    let mut bits: *mut winapi::ctypes::c_void = std::ptr::null_mut();
    let hbmp = CreateDIBSection(mdc, &mut bmi, DIB_RGB_COLORS, &mut bits, std::ptr::null_mut(), 0);
    if hbmp.is_null() { DeleteDC(mdc); ReleaseDC(std::ptr::null_mut(), sdc); return; }
    let oldb = SelectObject(mdc, hbmp as *mut winapi::ctypes::c_void);

    // 1) 底图：缩放到内区。HALFTONE 让放大走双线性，不糊成方块
    let dw = w - PIN_PAD * 2;
    let dh = h - PIN_PAD * 2;
    {
        let src = CreateCompatibleDC(sdc);
        let mut sb: BITMAPINFO = std::mem::zeroed();
        sb.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        sb.bmiHeader.biWidth = st.iw;
        sb.bmiHeader.biHeight = -st.ih;
        sb.bmiHeader.biPlanes = 1;
        sb.bmiHeader.biBitCount = 32;
        sb.bmiHeader.biCompression = BI_RGB;
        let mut sbits: *mut winapi::ctypes::c_void = std::ptr::null_mut();
        let sbmp = CreateDIBSection(src, &mut sb, DIB_RGB_COLORS, &mut sbits, std::ptr::null_mut(), 0);
        if !sbmp.is_null() {
            let olds = SelectObject(src, sbmp as *mut winapi::ctypes::c_void);
            std::ptr::copy_nonoverlapping(st.img.as_ptr(), sbits as *mut u8, (st.iw * st.ih * 4) as usize);
            SetStretchBltMode(mdc, HALFTONE);
            StretchBlt(mdc, PIN_PAD, PIN_PAD, dw, dh, src, 0, 0, st.iw, st.ih, SRCCOPY);
            SelectObject(src, olds);
            DeleteObject(sbmp as *mut winapi::ctypes::c_void);
        }
        DeleteDC(src);
    }
    // StretchBlt 是批处理的：不刷批就去读 DIB，可能读到旧像素（黑边/透明块）
    GdiFlush();

    let buf = std::slice::from_raw_parts_mut(bits as *mut u8, (w * h * 4) as usize);
    // 2) 隔离线：最外 1px 30% 黑（预乘后就是 0,0,0,77）
    for x in 0..w { pin_put(buf, w, x, 0, 0, 0, 0, ui::alpha::PIN_EDGE); pin_put(buf, w, x, h - 1, 0, 0, 0, ui::alpha::PIN_EDGE); }
    for y in 0..h { pin_put(buf, w, 0, y, 0, 0, 0, ui::alpha::PIN_EDGE); pin_put(buf, w, w - 1, y, 0, 0, 0, ui::alpha::PIN_EDGE); }
    // 3) 内侧 1px 蓝边（盖住底图最外一圈，内缩 1.5px）
    pin_stroke_rect(buf, w, PIN_PAD, PIN_PAD, w - 1 - PIN_PAD, h - 1 - PIN_PAD, PIN_BORDER, 255);
    // 4) 到场提示：同一位置换色覆盖隔离线
    if st.flash > 0 {
        let c = PIN_FLASH[st.flash_idx as usize % PIN_FLASH.len()];
        pin_stroke_rect(buf, w, 0, 0, w - 1, h - 1, c, 255);
    }
    // 5) 倍数提示：右上角半透明黑底 + 白字（窗口装饰，不跟图缩放）
    if st.tip {
        let txt = format!("{}%", (st.scale * 100.0).round() as i32);
        // paintScaleTip：字号 11×dpi、内边距 3×dpi、外边距 5×dpi
        let fs = sc(ui::font::SIZE_11);
        let tw = anno_tx_measure(fs, false, false, &txt);
        let th = anno_tx_spacing(fs).ceil() as i32;
        let pad = sc(ui::layout::pin::TIP_PAD);
        let margin = sc(ui::layout::pin::TIP_MARGIN);
        let mut bl = w - margin - tw - pad * 2;
        if bl < margin { bl = margin; }
        let bt = margin;
        let br = bl + tw + pad * 2;
        let bb = bt + th + pad * 2;
        // 46% 黑压在底图上：底色 ×0.54，alpha 保持 255（这里是纯装饰，不参与桌面混合）
        for y in bt..bb.min(h) { for x in bl..br.min(w) {
            let i = ((y * w + x) * 4) as usize;
            if i + 3 < buf.len() {
                buf[i] = (buf[i] as u32 * ui::layout::pin::TIP_KEEP / 100) as u8;
                buf[i + 1] = (buf[i + 1] as u32 * ui::layout::pin::TIP_KEEP / 100) as u8;
                buf[i + 2] = (buf[i + 2] as u32 * ui::layout::pin::TIP_KEEP / 100) as u8;
                buf[i + 3] = 255;
            }
        }}
        let f = anno_tx_font(fs, false, false);
        let oldf = SelectObject(mdc, f);
        SetBkMode(mdc, TRANSPARENT as i32);
        SetTextColor(mdc, ui::color::cref(ui::color::TEXT_INVERSE));
        let ws: Vec<u16> = txt.encode_utf16().collect();
        TextOutW(mdc, bl + pad, bt + pad, ws.as_ptr(), ws.len() as i32);
        SelectObject(mdc, oldf);
    }
    // TextOutW 同样走 GDI 批处理，先刷批再按 CPU 遍历收 alpha
    GdiFlush();
    // 6) alpha 收尾：GDI 画过的地方 alpha 会被写成 0，内区一律按不透明算；
    //    隔离线保持 30%（闪动时那条线是纯色，按不透明算）
    for y in 0..h { for x in 0..w {
        let i = ((y * w + x) * 4) as usize;
        if i + 3 >= buf.len() { continue; }
        let outer = x == 0 || y == 0 || x == w - 1 || y == h - 1;
        buf[i + 3] = if outer && st.flash <= 0 { 77 } else { 255 };
    }}

    let mut dst = POINT { x: winx, y: winy };
    let mut size = SIZE { cx: w, cy: h };
    let mut srcp = POINT { x: 0, y: 0 };
    let mut blend = BLENDFUNCTION {
        BlendOp: AC_SRC_OVER as u8,
        BlendFlags: 0,
        SourceConstantAlpha: 255,
        AlphaFormat: AC_SRC_ALPHA as u8,
    };
    UpdateLayeredWindow(hwnd, sdc, &mut dst, &mut size, mdc, &mut srcp, 0, &mut blend, ULW_ALPHA);

    SelectObject(mdc, oldb);
    DeleteObject(hbmp as *mut winapi::ctypes::c_void);
    DeleteDC(mdc);
    ReleaseDC(std::ptr::null_mut(), sdc);
}

/// 贴屏复制收尾（双击 / 回车 / Ctrl+C 共用）：
/// 剪切板拿不到时**不关窗**，否则贴着的这张图就白白丢了
pub(crate) unsafe fn pin_copy_close(hwnd: HWND) {
    let stp = pin_state(hwnd);
    if stp.is_null() { return; }
    let ok = { let st = &*stp; anno_copy_bgra(st.iw, st.ih, &st.img) };
    if ok { DestroyWindow(hwnd); }
    else { show_toast("复制到剪切板失败，请重试"); }
}

/// Ctrl+S 收尾：成功给提示并关窗，写盘失败只提示不关窗
pub(crate) unsafe fn pin_save_close(hwnd: HWND) {
    let stp = pin_state(hwnd);
    if stp.is_null() { return; }
    let ok = { let st = &*stp; save_bgra_desktop(st.iw, st.ih, &st.img).is_some() };
    if ok {
        show_toast("已保存");
        DestroyWindow(hwnd);
    } else {
        // 目录拿不到 / 只读 / U 盘拔了 / 磁盘满：说出原因，窗口留着让用户重试
        show_toast("保存失败：保存目录不可写或磁盘已满，请在「设置」里换一个目录");
    }
}

pub(crate) extern "system" fn anno_pin_proc(hwnd: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    unsafe {
        match msg {
            WM_PAINT => {
                let mut ps: PAINTSTRUCT = std::mem::zeroed();
                BeginPaint(hwnd, &mut ps);
                EndPaint(hwnd, &mut ps);
                pin_paint(hwnd);
                0
            }
            WM_LBUTTONDOWN => {
                let x = (lp & 0xFFFF) as i16 as i32; let y = ((lp >> 16) & 0xFFFF) as i16 as i32;
                let stp = pin_state(hwnd);
                if !stp.is_null() {
                    (*stp).press = (x, y);
                    (*stp).drag = true;
                }
                SetCapture(hwnd);
                // 去掉 WS_EX_NOACTIVATE 之后点击本来就会激活，这里显式再要一次 ——
                // Esc / Enter 要落在本窗口才收得到
                SetForegroundWindow(hwnd);
                SetFocus(hwnd);
                0
            }
            WM_MOUSEMOVE => {
                let stp = pin_state(hwnd);
                if stp.is_null() { return DefWindowProcW(hwnd, msg, wp, lp); }
                let st = &mut *stp;
                if !st.drag { return DefWindowProcW(hwnd, msg, wp, lp); }
                let x = (lp & 0xFFFF) as i16 as i32; let y = ((lp >> 16) & 0xFFFF) as i16 as i32;
                let mut rc: RECT = std::mem::zeroed();
                GetWindowRect(hwnd, &mut rc);
                SetWindowPos(hwnd, std::ptr::null_mut(),
                    rc.left + x - st.press.0, rc.top + y - st.press.1, 0, 0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                0
            }
            WM_LBUTTONUP => {
                let stp = pin_state(hwnd);
                if !stp.is_null() { (*stp).drag = false; }
                ReleaseCapture();
                0
            }
            WM_LBUTTONDBLCLK => {
                // 双击 = 复制到剪贴板并关窗
                pin_copy_close(hwnd);
                0
            }
            // Ctrl+滚轮放大后窗口可以比屏幕大，系统默认的最大
            // 跟踪尺寸只有主屏那么大，不放开 SetWindowPos 会被按住，放大就到此为止
            WM_GETMINMAXINFO => {
                let mmi = lp as *mut winapi::um::winuser::MINMAXINFO;
                if !mmi.is_null() {
                    (*mmi).ptMinTrackSize.x = 1;
                    (*mmi).ptMinTrackSize.y = 1;
                    (*mmi).ptMaxTrackSize.x = 20000;
                    (*mmi).ptMaxTrackSize.y = 20000;
                }
                0
            }
            WM_KEYDOWN => {
                // Enter/Ctrl+C 复制，Ctrl+S 存盘，Esc 关闭
                let ctrl = GetKeyState(VK_CONTROL) < 0;
                match wp as i32 {
                    // Enter = 复制
                    0x0D => { pin_copy_close(hwnd); }
                    // Ctrl+C = 复制
                    0x43 if ctrl => { pin_copy_close(hwnd); }
                    // Ctrl+S = 存到设置的截图目录
                    0x53 if ctrl => { pin_save_close(hwnd); }
                    VK_ESCAPE => { DestroyWindow(hwnd); }
                    _ => {}
                }
                0
            }
            WM_MOUSEWHEEL => {
                // 滚轮缩放：按住 Ctrl 才放大/缩小（一格 10%）。
                // 修饰键状态优先读 wParam 低字（MK_CONTROL）—— 真实事件里系统会给准；
                // GetKeyState 只在"本线程刚处理过键盘消息"时才对，纯粹靠它容易漏
                if (wp & 0x0008) != 0 || GetKeyState(VK_CONTROL) < 0 {
                    let stp = pin_state(hwnd);
                    if stp.is_null() { return 0; }
                    let st = &mut *stp;
                    let delta = ((wp >> 16) & 0xFFFF) as u16 as i16 as i32;
                    let step = if delta > 0 { 1.1f32 } else { 1.0 / 1.1 };
                    let ns = (st.scale * step).clamp(0.1, pin_max_scale(st));
                    if (ns - st.scale).abs() < 0.0001 { return 0; }
                    // 光标底下的那个图上点，缩放前后都要停在光标下。
                    // WM_MOUSEWHEEL 的 lParam 是**屏幕**坐标（不是客户区），先换算回来
                    let mut ax = (lp & 0xFFFF) as i16 as i32;
                    let mut ay = ((lp >> 16) & 0xFFFF) as i16 as i32;
                    let mut pt = POINT { x: ax, y: ay };
                    ScreenToClient(hwnd, &mut pt);
                    ax = pt.x; ay = pt.y;
                    let mut rc: RECT = std::mem::zeroed();
                    GetWindowRect(hwnd, &mut rc);
                    let ix = (ax - PIN_PAD) as f32 / st.scale;
                    let iy = (ay - PIN_PAD) as f32 / st.scale;
                    st.scale = ns;
                    let (nw, nh) = pin_size(st);
                    let nx = rc.left + ax - PIN_PAD - (ix * ns).round() as i32;
                    let ny = rc.top + ay - PIN_PAD - (iy * ns).round() as i32;
                    SetWindowPos(hwnd, std::ptr::null_mut(), nx, ny, nw, nh,
                                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
                    st.tip = true;
                    // 同一个 id 再 SetTimer 就是重新计时 —— 连续滚动期间提示一直不收
                    SetTimer(hwnd, PIN_T_TIP, PIN_TIP_MS, None);
                    pin_paint(hwnd);
                }
                0
            }
            WM_TIMER => {
                let id = wp;
                let stp = pin_state(hwnd);
                if stp.is_null() { return 0; }
                let st = &mut *stp;
                if id == PIN_T_FLASH {
                    if st.flash <= 0 { KillTimer(hwnd, PIN_T_FLASH); return 0; }
                    st.flash_idx = (st.flash_idx + 1) % PIN_FLASH.len() as i32;
                    st.flash -= 1;
                    if st.flash <= 0 { KillTimer(hwnd, PIN_T_FLASH); }
                    pin_paint(hwnd);
                } else if id == PIN_T_TIP {
                    KillTimer(hwnd, PIN_T_TIP);
                    st.tip = false;
                    pin_paint(hwnd);
                }
                0
            }
            WM_RBUTTONUP => {
                // 钉屏态右键不做事
                // 贴出去的图不带编辑器，右键既请不出工具条、也不关窗。
                // 收尾只有三条路：Esc、双击（=Ctrl+C 复制）、Ctrl+S 存盘，都成功即关
                0
            }
            WM_DESTROY => {
                let stp = pin_state(hwnd);
                if !stp.is_null() {
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    drop(Box::from_raw(stp));
                }
                ANNO_PINS.lock_ok().retain(|&h| h != hwnd as isize);
                0
            }
            _ => DefWindowProcW(hwnd, msg, wp, lp),
        }
    }
}

// 贴在屏幕上：取当前编辑图建立独立图钉窗，然后解除整个截图编辑器界面
pub(crate) unsafe fn anno_pin() {
    let w = ANNO_W.load(Ordering::Relaxed);
    let h = ANNO_H.load(Ordering::Relaxed);
    // 文字是图元，贴屏前先烘进副本 —— 图钉窗拿的是死像素
    anno_tx_finish();
    let pix = {
        if w <= 0 || h <= 0 { return; }
        if ANNO_IMG.lock_ok().is_empty() { return; }
        anno_tx_flat()
    };
    // 图钉位置 = 亮区(选区)在屏幕上的左上角；OX/OY 是遮罩局部坐标，要加回遮罩原点
    let (sox, soy) = sel_org();
    let x = ANNO_OX.load(Ordering::Relaxed) + sox;
    let y = ANNO_OY.load(Ordering::Relaxed) + soy;
    pin_open_img(pix, w, h, x, y);
    anno_close(); // 解除截图编辑器界面（图钉窗自带像素副本，不受后续编辑影响）
}

/// 用一张现成的 BGRA 像素开图钉窗（屏幕坐标 x,y = 图的左上角）。
/// 截图编辑器里的「贴屏」和框选阶段按住 Ctrl 松手直贴共用这一条路。
pub(crate) unsafe fn pin_open_img(pix: Vec<u8>, w: i32, h: i32, x: i32, y: i32) {
    if w <= 0 || h <= 0 || pix.is_empty() { return; }
    let hinstance = GetModuleHandleW(std::ptr::null_mut());
    let cls = wide(CLS_PIN);
    // CS_DBLCLKS：没有它系统根本不会发 WM_LBUTTONDBLCLK，双击复制就是死的。
    // 光标用四向箭头：贴出来的图可以拖着走
    reg_class(CLS_PIN, Some(anno_pin_proc), CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
        IDC_SIZEALL as usize, false);
    // 先建窗（尺寸按 1 倍算），再把状态挂上去、画第一帧。分层窗口 + 工具窗 + 置顶，
    // 不加 WS_EX_NOACTIVATE —— 那样点上去拿不到焦点，Esc / Enter 全收不到
    let phw = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, cls.as_ptr(), wide("").as_ptr(),
        WS_POPUP | WS_VISIBLE, x - PIN_PAD, y - PIN_PAD, w + PIN_PAD * 2, h + PIN_PAD * 2,
        std::ptr::null_mut(), std::ptr::null_mut(), hinstance, std::ptr::null_mut());
    if phw.is_null() { return; }
    let st = Box::new(PinState {
        img: pix,
        iw: w,
        ih: h,
        scale: 1.0,
        flash: PIN_FLASH.len() as i32,
        flash_idx: 0,
        tip: false,
        drag: false,
        press: (0, 0),
    });
    SetWindowLongPtrW(phw, GWLP_USERDATA, Box::into_raw(st) as isize);
    ANNO_PINS.lock_ok().push(phw as isize);
    ShowWindow(phw, SW_SHOW);
    pin_paint(phw);
    // 到场提示：那圈 1px 细线按 白→青→蓝→紫 各走 90ms
    SetTimer(phw, PIN_T_FLASH, PIN_FLASH_MS, None);
}
