use crate::prelude::*;
use crate::*;

// ===== 应用内轻提示=====
// 深蓝灰底白字的小条，贴在当前显示器工作区底边上方 72px，1.8 秒自己消失。
// 单窗口复用：连存两张图也不会叠成两条。TOOLWINDOW/NOACTIVATE/TRANSPARENT
// 让它不进任务栏、不抢焦点、不吃鼠标。
pub(crate) const TOAST_CLASS: &str = concat!(env!("APP_NAME"), "Toast");

pub(crate) unsafe fn toast_font() -> HFONT {
    CreateFontW(-sc(ui::font::SIZE_18), 0, 0, 0, ui::font::W_REGULAR, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, wide(ui::font::UI_ALT).as_ptr())
}

pub(crate) extern "system" fn toast_proc(hwnd: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_ERASEBKGND => 1,
        WM_PAINT => unsafe {
            let mut ps: PAINTSTRUCT = std::mem::zeroed();
            let hdc = BeginPaint(hwnd, &mut ps);
            let mut cr: RECT = std::mem::zeroed();
            GetClientRect(hwnd, &mut cr);
            // 深蓝灰底，整窗透明度由 SetLayeredWindowAttributes 控制
            let bg = CreateSolidBrush(ui::color::cref(ui::color::TEXT));
            FillRect(hdc, &cr, bg);
            DeleteObject(bg as *mut winapi::ctypes::c_void);
            SetBkMode(hdc, TRANSPARENT as i32);
            SetTextColor(hdc, ui::color::cref(ui::color::TEXT_INVERSE));
            let f = toast_font();
            let old = SelectObject(hdc, f as *mut winapi::ctypes::c_void);
            let mut buf = [0u16; 512];
            let n = GetWindowTextW(hwnd, buf.as_mut_ptr(), 512);
            let pad = sc(ui::layout::main_page::TOAST_PAD);
            let mut tr = RECT { left: pad, top: 0, right: cr.right - pad, bottom: cr.bottom };
            DrawTextW(hdc, buf.as_ptr(), n, &mut tr,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | 0x0000_8000 /*DT_END_ELLIPSIS*/);
            SelectObject(hdc, old);
            DeleteObject(f as *mut winapi::ctypes::c_void);
            EndPaint(hwnd, &ps);
            0
        },
        WM_TIMER => unsafe { DestroyWindow(hwnd); 0 },
        0x0082 /*WM_NCDESTROY*/ => {
            if TOAST_HWND.load(Ordering::Relaxed) == hwnd as isize {
                TOAST_HWND.store(0, Ordering::Relaxed);
            }
            0
        },
        _ => unsafe { DefWindowProcW(hwnd, msg, wp, lp) },
    }
}

/// 弹出轻提示（截图保存 / 录制完成等收尾提示都用它）
pub(crate) unsafe fn show_toast(msg: &str) {
    if msg.is_empty() { return; }
    let hinstance = GetModuleHandleW(std::ptr::null_mut());
    let cls = wide(TOAST_CLASS);
    reg_class(TOAST_CLASS, Some(toast_proc), 0, IDC_ARROW as usize, false);
    let mut hwnd = TOAST_HWND.load(Ordering::Relaxed) as HWND;
    if hwnd.is_null() || IsWindow(hwnd) == 0 {
        // WS_EX_TOOLWINDOW|NOACTIVATE|TOPMOST|LAYERED|TRANSPARENT
        hwnd = CreateWindowExW(
            0x00000080 | 0x08000000 | 0x00000008 | 0x00080000 | 0x00000020,
            cls.as_ptr(), wide("").as_ptr(), WS_POPUP, 0, 0, 10, 10,
            std::ptr::null_mut(), std::ptr::null_mut(), hinstance, std::ptr::null_mut());
        if hwnd.is_null() { return; }
        TOAST_HWND.store(hwnd as isize, Ordering::Relaxed);
        SetLayeredWindowAttributes(hwnd, 0, ui::alpha::TOAST, 0x2 /*LWA_ALPHA*/);
    }
    let text = wide(msg);
    SetWindowTextW(hwnd, text.as_ptr());

    // 按文字实测宽度定尺寸：长短句都能一眼读完
    let mut ts: SIZE = std::mem::zeroed();
    let dc = GetDC(hwnd);
    if !dc.is_null() {
        let f = toast_font();
        let old = SelectObject(dc, f as *mut winapi::ctypes::c_void);
        GetTextExtentPoint32W(dc, text.as_ptr(), text.len() as i32 - 1, &mut ts);
        SelectObject(dc, old);
        DeleteObject(f as *mut winapi::ctypes::c_void);
        ReleaseDC(hwnd, dc);
    }
    // 贴光标所在那块屏的工作区（任务栏之上）
    let mut pt: POINT = std::mem::zeroed();
    GetCursorPos(&mut pt);
    let mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    let mut mi: MONITORINFO = std::mem::zeroed();
    mi.cbSize = std::mem::size_of::<MONITORINFO>() as u32;
    let wa = if !mon.is_null() && GetMonitorInfoW(mon, &mut mi) != 0 {
        mi.rcWork
    } else {
        RECT { left: 0, top: 0, right: GetSystemMetrics(SM_CXSCREEN), bottom: GetSystemMetrics(SM_CYSCREEN) }
    };
    let pad = sc(ui::layout::main_page::TOAST_PAD);
    let w_min = sc(ui::layout::main_page::TOAST_MIN_W);
    let w_max = (wa.right - wa.left) * ui::layout::main_page::TOAST_MAX_PCT / 100;
    let mut w = ts.cx + pad * 2;
    if w < w_min { w = w_min; }
    if w > w_max { w = w_max; }
    let h = sc(ui::layout::main_page::TOAST_H);
    let x = wa.left + ((wa.right - wa.left) - w) / 2;
    let y = wa.bottom - h - sc(ui::layout::main_page::TOAST_BOTTOM_GAP);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | 0x0040 /*SWP_SHOWWINDOW*/);
    InvalidateRect(hwnd, std::ptr::null_mut(), 1);
    // 同一个 id 再 SetTimer = 重新计时
    SetTimer(hwnd, 1, 1800, None);
}

// ==================== 悬停提示气泡（自绘分层窗）====================
//
// 系统 tooltip 控件（comctl32）这里不用，原因
// ① 系统控件在本进程实测 TTM_ADDTOOLW 全线返回失败，
//      W 消息等于废掉，中文提示根本加不进工具；
//   ② 就算能用，量气泡尺寸也得"透明窗偷偷显示一次反推 Padding"、显示后再挠一次
//      位置，全是绕着控件短板的补丁。
// 因此自绘一个分层窗口（与贴图窗同一套技术：32 位 DIB + 每像素 alpha），
// 气泡形状走本项目通用的覆盖率超采样（cov_supersample）——圆角、1px 描边、
// 抗锯齿一次到位，尺寸自己算得准，不存在"先显示再校正"。
// 文字用系统状态字体（NONCLIENTMETRICS.lfStatusFont，tooltip 用的就是它），
// 画成灰度蒙版后按覆盖率压暗气泡——和图标同一路子（矢量/覆盖率），
// 不碰任何字形图标字体。
//
//   · 悬停 260ms 后才弹（鼠标路过不弹），离开立刻收；
//   · 气泡底边中点贴在 owner 上边缘往里 4px（Tip::anchorInset）；
//   · 滑块上时提示跟着鼠标横向走，显示的是鼠标底下那个位置的值（getValueAt）；
//   · 切工具（副条内容整批重建）时收掉。
// 结构上：提示文字是 (条, 归属) 的**纯函数**，悬停时现算；
// 副条按钮本来就是画出来的区域而非子窗口，因此不需要
// bind/rebind/unbindAll 绑定表（以及随之而来的野指针生命周期管理）。

// 系统 tooltip 的 delayMs 是 1000ms。实际用下来，主条/副条这种"每个按钮都有一句
// 说明"的工具条，1 秒太慢：光标停在箭头上要等整整一秒才出字，中途扫过别的按钮还会
// 重新计时。260ms 与副条预览（ANNO_SHOW_DELAY）同一档：停下就出，扫过不弹。
pub(crate) const TIP_DELAY_MS: u32 = 260;
pub(crate) const TIP_TIMER: usize = 0x5100;  // Tip::timerId（避开宿主窗口其他定时器）
// 气泡尺寸/配色见 ui::layout::main_page::TIP_* 与 ui::color::TOOLTIP_*

// 归属 id（每个 Tip 一份）：主条 = 按钮位 0..13；
// 副条 = 100+k 第 k 颗开关 / 200+i 第 i 颗色板 / 300 滑块。
// 副条切工具时按钮整批重建，整数编码比指针稳
pub(crate) const TIP_TGL: i32 = 100;
pub(crate) const TIP_CLR: i32 = 200;
pub(crate) const TIP_SLD: i32 = 300;

// 文案来自 SnapCore/Lang/zh-CN.json 的 tool.* / color.*（原值）
pub(crate) const ANNO_BTN_TIP: [&str; 14] = [
    "矩形", "箭头", "标号", "画笔", "文本", "马赛克 / 高斯模糊", "橡皮擦",
    "录屏",
    "撤销", "重做", "关闭", "贴屏", "保存", "剪切板",
];
pub(crate) const ANNO_COLOR_TIP: [&str; 9] = ["红", "黄", "绿", "青", "蓝", "紫", "粉", "黑", "白"];

// 副条开关提示（按工具 + 当前状态），文案逐条对 Lang/zh-CN.json 的 tool.*：
// 实心/空心、选框/手绘 这类是"点了会变成什么"的对偶，跟着当前状态反过来
pub(crate) unsafe fn anno_sub_tip_text(tool: i32, k: i32) -> &'static str {
    let a = anno_sub_opt(4) != 0;
    let b = anno_sub_opt(5) != 0;
    let c = anno_sub_opt(6) != 0;
    match tool {
        0 => if k == 0 {
            if a { "实心矩形" } else { "空心矩形" }
        } else if c { "实心椭圆" } else { "空心椭圆" },
        2 => if k == 0 {
            if a { "实心箭头" } else { "空心箭头" }
        } else { "直线" },
        3 => if k == 0 {
            if a { "实心圆序号" } else { "空心圆序号" }
        } else if c { "实心方序号" } else { "空心方序号" },
        4 => if k == 0 {
            if a { "半透明" } else { "不透明" }
        } else if b { "选框" } else { "手绘" },
        5 => if k == 0 { "粗体" } else { "斜体" },
        6 => match k {
            0 => "马赛克",
            1 => "高斯模糊",
            2 => "智能消除",
            _ => if c || a { "选框" } else { "手绘" },
        },
        7 => if a { "选框" } else { "手绘" },
        _ => "",
    }
}

// 一条工具条配一个气泡。状态只有"归属 + 锚点 + 显示中"三样，文字一律现算
pub(crate) struct Tip {
pub(crate) hwnd: AtomicUsize,          // 气泡窗口（0 = 没建）
pub(crate) bar: AtomicUsize,           // 宿主工具条（定时器挂它身上；滑块值换算要用）
pub(crate) owner: AtomicI32,           // 当前归属，-1 = 无
pub(crate) anchor: Mutex<(i32, i32)>,  // 屏幕锚点 = 气泡底边中点
pub(crate) visible: AtomicBool,
}
unsafe impl Send for Tip {}
unsafe impl Sync for Tip {}

pub(crate) static TIP_MAIN: Tip = Tip {
    hwnd: AtomicUsize::new(0), bar: AtomicUsize::new(0), owner: AtomicI32::new(-1),
    anchor: Mutex::new((0, 0)), visible: AtomicBool::new(false),
};
pub(crate) static TIP_SUB: Tip = Tip {
    hwnd: AtomicUsize::new(0), bar: AtomicUsize::new(0), owner: AtomicI32::new(-1),
    anchor: Mutex::new((0, 0)), visible: AtomicBool::new(false),
};
// 录制条自己一条，两态共用同一批下标
pub(crate) static TIP_REC: Tip = Tip {
    hwnd: AtomicUsize::new(0), bar: AtomicUsize::new(0), owner: AtomicI32::new(-1),
    anchor: Mutex::new((0, 0)), visible: AtomicBool::new(false),
};

// 系统状态字体（tooltip 用的就是它），整个进程只建一次
pub(crate) static TIP_FONT: AtomicUsize = AtomicUsize::new(0);

pub(crate) unsafe fn tip_font() -> HFONT {
    let f = TIP_FONT.load(Ordering::Relaxed) as HFONT;
    if !f.is_null() { return f; }
    let mut lf: LOGFONTW = std::mem::zeroed();
    let mut ncm: NONCLIENTMETRICSW = std::mem::zeroed();
    ncm.cbSize = std::mem::size_of::<NONCLIENTMETRICSW>() as u32;
    let ok = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,
        std::mem::size_of::<NONCLIENTMETRICSW>() as u32,
        &mut ncm as *mut _ as *mut winapi::ctypes::c_void, 0) != 0
        && ncm.lfStatusFont.lfFaceName[0] != 0
        && ncm.lfStatusFont.lfHeight != 0;
    if ok {
        // 原生 tooltip 用的就是系统状态字体（族/字号/字重原样搬）
        lf = ncm.lfStatusFont;
    } else {
        // SPI 拿不到时按 baseTextFormat 的口径落（雅黑 12 / 常规）
        // 空 face 交给 CreateFontW 会静悄悄退成系统默认字体，字形就"变样"了
        lf.lfHeight = -ui::dpi::font_px(ui::font::SIZE_12 as f32) as i32;
        lf.lfWeight = ui::font::W_REGULAR;
        lf.lfCharSet = DEFAULT_CHARSET as u8;
        for (i, c) in wide(ui::font::UI).iter().take(31).enumerate() { lf.lfFaceName[i] = *c; }
    }
    // 宽度交给 GDI 按字高自己算，lfWidth 跟着 SPI 传会把中文压扁
    lf.lfWidth = 0;
    // ClearType：文字只是当覆盖率蒙版用（下面取最亮通道），12px 的灰阶 AA
    // 在气泡上是糊的，而原生 tooltip 本来就是 ClearType，这里跟随 ClearType
    lf.lfQuality = CLEARTYPE_QUALITY as u8;
    let mut f = CreateFontW(lf.lfHeight, 0, lf.lfEscapement, lf.lfOrientation, lf.lfWeight,
        lf.lfItalic as u32, lf.lfUnderline as u32, lf.lfStrikeOut as u32, lf.lfCharSet as u32,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        lf.lfPitchAndFamily as u32, lf.lfFaceName.as_ptr());
    if f.is_null() {
        let face = wide(ui::font::UI);
        f = CreateFontW(-ui::dpi::font_px(ui::font::SIZE_12 as f32) as i32, 0, 0, 0,
            ui::font::W_REGULAR, 0, 0, 0, DEFAULT_CHARSET as u32, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            (VARIABLE_PITCH | FF_SWISS) as u32, face.as_ptr());
    }
    TIP_FONT.store(f as usize, Ordering::Relaxed);
    f
}

// 气泡窗口类只注册一次（进程内可多次截图，类不能重复注册）
pub(crate) static TIP_CLS_OK: AtomicBool = AtomicBool::new(false);

pub(crate) unsafe fn tip_create(tip: &'static Tip, bar: HWND) {
    if tip.hwnd.load(Ordering::Relaxed) != 0 { return; }
    let hinstance = GetModuleHandleW(std::ptr::null_mut());
    if !TIP_CLS_OK.swap(true, Ordering::Relaxed) {
        reg_class(CLS_TIP, Some(DefWindowProcW), 0, 0, false);
    }
    // WS_EX_TRANSPARENT：鼠标事件穿透，悬停判定照旧归工具条；
    // WS_EX_NOACTIVATE：不抢焦点；WS_EX_LAYERED：每像素 alpha
    let h = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        wide(CLS_TIP).as_ptr(), wide("").as_ptr(),
        WS_POPUP, 0, 0, 0, 0,
        std::ptr::null_mut(), std::ptr::null_mut(), hinstance, std::ptr::null_mut());
    if !h.is_null() {
        tip.hwnd.store(h as usize, Ordering::Relaxed);
        tip.bar.store(bar as usize, Ordering::Relaxed);
    }
}

pub(crate) unsafe fn tip_destroy(tip: &'static Tip) {
    let h = tip.hwnd.swap(0, Ordering::Relaxed) as HWND;
    if !h.is_null() && IsWindow(h) != 0 { DestroyWindow(h); }
    tip.bar.store(0, Ordering::Relaxed);
    tip.owner.store(-1, Ordering::Relaxed);
    tip.visible.store(false, Ordering::Relaxed);
    // 提示字体是进程级缓存：三条气泡都收掉之后一起释放（下次截图再按需重建）
    if TIP_MAIN.hwnd.load(Ordering::Relaxed) == 0
        && TIP_SUB.hwnd.load(Ordering::Relaxed) == 0
        && TIP_REC.hwnd.load(Ordering::Relaxed) == 0 {
        let f = TIP_FONT.swap(0, Ordering::Relaxed) as HFONT;
        if !f.is_null() { DeleteObject(f as *mut winapi::ctypes::c_void); }
    }
}

// 当前提示文字：纯函数现算。滑块那颗特殊——显示鼠标底下的值，跟着锚点变
pub(crate) unsafe fn tip_text(tip: &'static Tip) -> String {
    let owner = tip.owner.load(Ordering::Relaxed);
    if owner < 0 { return String::new(); }
    if std::ptr::eq(tip, &TIP_MAIN) {
        return ANNO_BTN_TIP[owner as usize].to_string();
    }
    if std::ptr::eq(tip, &TIP_REC) {
        return REC_TIPS.get(owner as usize).map(|s| s.to_string()).unwrap_or_default();
    }
    if owner >= TIP_SLD {
        // Slider::posToValue：轨道两端各让开一个圆点半径 r=3
        let bar = tip.bar.load(Ordering::Relaxed) as HWND;
        let (ax, _) = *tip.anchor.lock_ok();
        let mut wr: RECT = std::mem::zeroed();
        GetWindowRect(bar, &mut wr);
        let (cw, _) = anno_sub_client();
        let (_, slider, _) = anno_sub_geom(cw);
        let (sx, _, sw, _) = slider;
        let mn = ANNO_OPT_MIN.load(Ordering::Relaxed).max(0);
        let mx = ANNO_OPT_MAX.load(Ordering::Relaxed).max(mn + 1);
        let r = ui::layout::anno::SLIDER_DOT_R;
        let f = ((ax - wr.left) as f32 - sx as f32 - r) / (sw as f32 - 2.0 * r);
        let f = f.clamp(0.0, 1.0);
        let v = (mn as f32 + f * (mx - mn) as f32).round().clamp(mn as f32, mx as f32) as i32;
        return v.to_string();
    }
    if owner >= TIP_CLR {
        return ANNO_COLOR_TIP[(owner - TIP_CLR) as usize].to_string();
    }
    anno_sub_tip_text(anno_sub_tool(), owner - TIP_TGL).to_string()
}

// 量一段文字在提示字体下的尺寸（DrawText DT_CALCRECT）
pub(crate) unsafe fn tip_measure(text: &str) -> (i32, i32) {
    let t = wide(text);
    let sdc = GetDC(std::ptr::null_mut());
    let mdc = CreateCompatibleDC(sdc);
    let hbmp = CreateCompatibleBitmap(sdc, 1, 1);
    let old = SelectObject(mdc, hbmp as *mut winapi::ctypes::c_void);
    let font = tip_font();
    let oldf = if !font.is_null() { SelectObject(mdc, font as *mut winapi::ctypes::c_void) } else { std::ptr::null_mut() };
    let mut rc = RECT { left: 0, top: 0, right: 0, bottom: 0 };
    DrawTextW(mdc, t.as_ptr(), -1, &mut rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    if !oldf.is_null() { SelectObject(mdc, oldf); }
    SelectObject(mdc, old);
    DeleteObject(hbmp as *mut winapi::ctypes::c_void);
    DeleteDC(mdc);
    ReleaseDC(std::ptr::null_mut(), sdc);
    (rc.right - rc.left, rc.bottom - rc.top)
}

// 把当前提示画出来并摆到位。尺寸 = 文字 + 内边距 + 描边，先算后显示，不挠第二次
pub(crate) unsafe fn tip_show_now(tip: &'static Tip) {
    let h = tip.hwnd.load(Ordering::Relaxed) as HWND;
    if h.is_null() { return; }
    let text = tip_text(tip);
    if text.is_empty() { tip_hide(tip); return; }
    let (tw, th) = tip_measure(&text);
    let bw = ui::layout::main_page::TIP_STROKE as i32;
    let w = (tw + ui::layout::main_page::TIP_PAD_X * 2 + bw * 2).max(1);
    let hh = (th + ui::layout::main_page::TIP_PAD_Y * 2 + bw * 2).max(1);

    let sdc = GetDC(std::ptr::null_mut());
    if sdc.is_null() { return; }
    // ---- 气泡 DIB：top-down 32 位，每像素 alpha ----
    let mut bmi: BITMAPINFO = std::mem::zeroed();
    bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -hh;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    let mut bits: *mut winapi::ctypes::c_void = std::ptr::null_mut();
    let hbmp = CreateDIBSection(sdc, &mut bmi, DIB_RGB_COLORS, &mut bits, std::ptr::null_mut(), 0);
    let mdc = CreateCompatibleDC(sdc);
    let oldbmp = SelectObject(mdc, hbmp as *mut winapi::ctypes::c_void);
    if !hbmp.is_null() && !bits.is_null() && !mdc.is_null() {
        let buf = std::slice::from_raw_parts_mut(bits as *mut u8, (w * hh * 4) as usize);
        // 圆角矩形：外沿贴窗口边界，内沿再收一个描边宽——中间那一圈就是 1px 描边，
        // 外缘顶格、内缘抗锯齿（与副条色块白边同一套算法）
        let (fw, fh) = (w as f32, hh as f32);
        let inside = |x: f32, y: f32, t: f32| -> bool {
            let rr = ui::layout::main_page::TIP_RAD - t;
            let ix = x.clamp(t + rr, fw - t - rr);
            let iy = y.clamp(t + rr, fh - t - rr);
            (x - ix).hypot(y - iy) <= rr
        };
        let m_out = cov_supersample(0, 0, w, hh, &|x, y| inside(x, y, 0.0));
        let m_in = cov_supersample(0, 0, w, hh,
            &|x, y| inside(x, y, ui::layout::main_page::TIP_STROKE));
        let n = (w * hh) as usize;
        for i in 0..n {
            let a = m_out[i] as u32;
            let ci = m_in[i] as u32;
            // 描边带 = 外覆盖 - 内覆盖；颜色按内覆盖在 填充↔描边 之间过渡
            let fill = ui::color::TOOLTIP_BG;
            let bord = ui::color::TOOLTIP_BORDER;
            let r = (((fill >> 16) & 0xFF) * ci + ((bord >> 16) & 0xFF) * (255 - ci)) / 255;
            let g = (((fill >> 8) & 0xFF) * ci + ((bord >> 8) & 0xFF) * (255 - ci)) / 255;
            let b = ((fill & 0xFF) * ci + (bord & 0xFF) * (255 - ci)) / 255;
            buf[i * 4] = (b * a / 255) as u8;
            buf[i * 4 + 1] = (g * a / 255) as u8;
            buf[i * 4 + 2] = (r * a / 255) as u8;
            buf[i * 4 + 3] = a as u8;
        }
        // ---- 文字蒙版：白字黑底画进同尺寸 DIB，取红通道当覆盖率 ----
        let mut tbmi: BITMAPINFO = std::mem::zeroed();
        tbmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        tbmi.bmiHeader.biWidth = w;
        tbmi.bmiHeader.biHeight = -hh;
        tbmi.bmiHeader.biPlanes = 1;
        tbmi.bmiHeader.biBitCount = 32;
        tbmi.bmiHeader.biCompression = BI_RGB;
        let mut tbits: *mut winapi::ctypes::c_void = std::ptr::null_mut();
        let thbmp = CreateDIBSection(sdc, &mut tbmi, DIB_RGB_COLORS, &mut tbits, std::ptr::null_mut(), 0);
        let tdc = CreateCompatibleDC(sdc);
        let told = SelectObject(tdc, thbmp as *mut winapi::ctypes::c_void);
        if !thbmp.is_null() && !tbits.is_null() && !tdc.is_null() {
            // 黑底（覆盖率 0）
            let tbuf = std::slice::from_raw_parts_mut(tbits as *mut u8, (w * hh * 4) as usize);
            for c in tbuf.chunks_exact_mut(4) { c[0] = 0; c[1] = 0; c[2] = 0; c[3] = 0; }
            let font = tip_font();
            let toldf = if !font.is_null() { SelectObject(tdc, font as *mut winapi::ctypes::c_void) } else { std::ptr::null_mut() };
            SetBkMode(tdc, TRANSPARENT as i32);
            SetTextColor(tdc, ui::color::cref(ui::color::TEXT_INVERSE));
            let ws = wide(&text);
            let mut trc = RECT {
                left: bw + ui::layout::main_page::TIP_PAD_X,
                top: bw + ui::layout::main_page::TIP_PAD_Y,
                right: w - bw - ui::layout::main_page::TIP_PAD_X,
                bottom: hh - bw - ui::layout::main_page::TIP_PAD_Y,
            };
            DrawTextW(tdc, ws.as_ptr(), -1, &mut trc, DT_SINGLELINE | DT_NOPREFIX);
            if !toldf.is_null() { SelectObject(tdc, toldf); }
            GdiFlush(); // DrawTextW 走 GDI 批处理，读蒙版前必须刷批
            // 按覆盖率把气泡压暗（文字是黑的）。预乘关系不变：alpha 不动，只乘 RGB
            for i in 0..n {
                // 白字黑底；开了 ClearType 后 RGB 三通道各带一点次像素权重，
                // 取最亮的那一路当覆盖率，笔画边缘才不会被平均糊掉
                let cov = (tbuf[i * 4] as u32)
                    .max(tbuf[i * 4 + 1] as u32)
                    .max(tbuf[i * 4 + 2] as u32);
                if cov == 0 { continue; }
                let k = 255 - cov;
                buf[i * 4] = (buf[i * 4] as u32 * k / 255) as u8;
                buf[i * 4 + 1] = (buf[i * 4 + 1] as u32 * k / 255) as u8;
                buf[i * 4 + 2] = (buf[i * 4 + 2] as u32 * k / 255) as u8;
            }
        }
        if !tdc.is_null() {
            if !told.is_null() { SelectObject(tdc, told); }
            DeleteDC(tdc);
        }
        if !thbmp.is_null() { DeleteObject(thbmp as *mut winapi::ctypes::c_void); }

        // ---- 上屏：位置 = 锚点 - 半宽 / 锚点 - 高（底边中点贴锚点）----
        let (ax, ay) = *tip.anchor.lock_ok();
        let mut dst = POINT { x: ax - w / 2, y: ay - hh };
        let mut size = SIZE { cx: w, cy: hh };
        let mut srcp = POINT { x: 0, y: 0 };
        let mut blend = BLENDFUNCTION {
            BlendOp: AC_SRC_OVER as u8,
            BlendFlags: 0,
            SourceConstantAlpha: 255,
            AlphaFormat: AC_SRC_ALPHA as u8,
        };
        UpdateLayeredWindow(h, sdc, &mut dst, &mut size, mdc, &mut srcp, 0, &mut blend, ULW_ALPHA);
        if !tip.visible.load(Ordering::Relaxed) {
            // 同组置顶里再抢一次最前（工具条个个都是置顶），NOACTIVATE 不抢焦点
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ShowWindow(h, SW_SHOWNA);
            tip.visible.store(true, Ordering::Relaxed);
        }
    }
    if !mdc.is_null() {
        if !oldbmp.is_null() { SelectObject(mdc, oldbmp); }
        DeleteDC(mdc);
    }
    if !hbmp.is_null() { DeleteObject(hbmp as *mut winapi::ctypes::c_void); }
    ReleaseDC(std::ptr::null_mut(), sdc);
}

// Tip::showAt：同一归属只更新锚点（已显示的立刻跟上——滑块跟鼠标靠这条）
// 换归属则旧气泡立刻收掉、1000ms 重新计时
pub(crate) unsafe fn tip_show_at(tip: &'static Tip, owner: i32, sx: i32, sy: i32) {
    if tip.hwnd.load(Ordering::Relaxed) == 0 { return; }
    if tip.owner.load(Ordering::Relaxed) == owner {
        *tip.anchor.lock_ok() = (sx, sy);
        if tip.visible.load(Ordering::Relaxed) {
            // 归属和文本都没变，只是跟着鼠标走：挪窗口即可。
            // 原来每次都重建两张 DIB + DC 并做 16× 超采样，拖动滑块时按 mousemove 频率空转
            let h = tip.hwnd.load(Ordering::Relaxed) as HWND;
            let mut r: RECT = std::mem::zeroed();
            if GetWindowRect(h, &mut r) != 0 {
                SetWindowPos(h, HWND_TOPMOST, sx - (r.right - r.left) / 2,
                             sy - (r.bottom - r.top), 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        return;
    }
    tip_hide(tip);
    tip.owner.store(owner, Ordering::Relaxed);
    *tip.anchor.lock_ok() = (sx, sy);
    let bar = tip.bar.load(Ordering::Relaxed) as HWND;
    if !bar.is_null() { SetTimer(bar, TIP_TIMER, TIP_DELAY_MS, None); }
}

// Tip::showAbove：气泡底边中点贴 owner 上沿，rx/ry/rw 是 owner 的窗口内布局矩形
pub(crate) unsafe fn tip_show_above(tip: &'static Tip, owner: i32, rx: f32, ry: f32, rw: f32) {
    let bar = tip.bar.load(Ordering::Relaxed) as HWND;
    if bar.is_null() { return; }
    let mut wr: RECT = std::mem::zeroed();
    GetWindowRect(bar, &mut wr);
    // 是浮点算完再 static_cast<int> 截断（中心 = x + w/2，上沿 = y + 4）
    let sx = (wr.left as f32 + rx + rw * 0.5) as i32;
    let sy = (wr.top as f32 + ry + ui::layout::main_page::TIP_INSET as f32) as i32;
    tip_show_at(tip, owner, sx, sy);
}

pub(crate) unsafe fn tip_hide(tip: &'static Tip) {
    let bar = tip.bar.load(Ordering::Relaxed) as HWND;
    if tip.owner.load(Ordering::Relaxed) != -1 {
        if !bar.is_null() { KillTimer(bar, TIP_TIMER); }
        tip.owner.store(-1, Ordering::Relaxed);
    }
    let h = tip.hwnd.load(Ordering::Relaxed) as HWND;
    if tip.visible.load(Ordering::Relaxed) && !h.is_null() {
        ShowWindow(h, SW_HIDE);
        tip.visible.store(false, Ordering::Relaxed);
    }
}

pub(crate) unsafe fn tip_timer(tip: &'static Tip) {
    let bar = tip.bar.load(Ordering::Relaxed) as HWND;
    if !bar.is_null() { KillTimer(bar, TIP_TIMER); }
    if tip.owner.load(Ordering::Relaxed) == -1 { return; }
    tip_show_now(tip);
}

// 主条第 i 颗按钮的提示
pub(crate) unsafe fn tip_main_hover(idx: i32) {
    if !(0..14).contains(&idx) { return; }
    // 布局值：按钮左上 = (cellX + 2, 3)，宽 28
    // Tip::showAbove 取 owner->x + owner->w/2，正好是格子中心，所以这里只把 2 加回来
    let cx = anno_btn_center_x(idx) as f32 - ui::metric::CELL as f32 * 0.5 + ui::metric::PAD as f32;
    tip_show_above(&TIP_MAIN, idx, cx, ui::layout::anno::MAIN_TIP_Y, ui::metric::BTN as f32);
}

// 副条悬停目标命中：开关 > 滑块 > 色板（与点击命中同序）。
// 返回 100+k / 200+i / 300，未命中 -1
pub(crate) unsafe fn anno_sub_hit_hover(x: i32, y: i32, cw: i32) -> i32 {
    let (toggles, slider, colors) = anno_sub_geom(cw);
    for (i, (tx, ty, tw, th, _)) in toggles.iter().enumerate() {
        if x >= *tx && x < tx + tw && y >= *ty && y < ty + th { return TIP_TGL + i as i32; }
    }
    let (hx, hy, hw2, hh) = slider;
    if x >= hx && x <= hx + hw2 && y >= hy && y < hy + hh { return TIP_SLD; }
    for (cx, cy, cw2, ch2, idx) in colors.iter() {
        if x >= *cx && x < cx + cw2 && y >= *cy && y < cy + ch2 { return TIP_CLR + *idx as i32; }
    }
    -1
}

// 副条悬停提示
pub(crate) unsafe fn tip_sub_hover(hov: i32, mx: i32, _my: i32) {
    let shw = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
    if shw.is_null() { return; }
    if hov >= TIP_SLD {
        // 滑块：锚点跟着鼠标（showAt(slider, x + pos.x, ...)）
        // slider->y = 内容区顶 = 2，加 anchorInset 就是 6
        let mut wr: RECT = std::mem::zeroed();
        GetWindowRect(shw, &mut wr);
        tip_show_at(&TIP_SUB, TIP_SLD, wr.left + mx,
            wr.top + ui::layout::main_page::TIP_ANCHOR_DY + ui::layout::main_page::TIP_INSET);
        return;
    }
    if hov < TIP_TGL { return; } // -1 或脏值
    let (cw, _) = anno_sub_client();
    let (toggles, _, colors) = anno_sub_geom(cw);
    // 工具切换的那一瞬间，hover 号可能还对不上新的格子数（录制/马赛克的格子数会变）。
    // 这里一律走 get()，取不到就当"没命中"直接放弃 —— 不能裸下标，
    // 越界 panic 在 panic=abort 下就是进程静默消失（鼠标划过副条即触发）。
    let tx = if hov >= TIP_CLR {
        colors.get((hov - TIP_CLR) as usize).map(|c| c.0)
    } else {
        toggles.get((hov - TIP_TGL) as usize).map(|t| t.0)
    };
    let Some(tx) = tx else { return; };
    // 布局值：按钮左上 = (cellX + 2, 5)（内容区顶 2 + 居中余量 3），宽 28
    tip_show_above(&TIP_SUB, hov, tx as f32 + ui::metric::PAD as f32, ui::layout::anno::SUB_TIP_Y, ui::metric::BTN as f32);
}

pub(crate) static TOAST_HWND: AtomicIsize = AtomicIsize::new(0); // 应用内轻提示窗口（单例复用）
