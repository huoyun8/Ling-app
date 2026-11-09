use crate::prelude::*;
use crate::*;

// 设置态：[MP4][GIF] ‖ [系统声][麦克风] ‖ [开始]
// 录制态：[00:00 / 02:00] ‖ [丢弃][存文件][存剪切板]
// 录制中编辑器只收主条 / 副条，标注宿主窗留着：选区挖空露出真实桌面、选区外压暗，
// 录制条 topmost 跟着选区走（录的就是那块选区）；到上限自动存盘（MP4 10 分钟 / GIF 2 分钟）
pub(crate) static REC_BAR: AtomicIsize = AtomicIsize::new(0);           // 录制条窗口
pub(crate) static REC_STOP: AtomicBool = AtomicBool::new(false);        // 通知采集线程收工
pub(crate) static REC_PHASE: AtomicI32 = AtomicI32::new(0);             // 0=设置态 1=录制态
pub(crate) static REC_MP4: AtomicBool = AtomicBool::new(true);
pub(crate) static REC_SPK: AtomicBool = AtomicBool::new(true);          // 系统声（默认开）
pub(crate) static REC_MIC: AtomicBool = AtomicBool::new(false);         // 麦克风（默认关）
pub(crate) static REC_SEC: AtomicI32 = AtomicI32::new(0);               // 已录秒数
pub(crate) static REC_HOVER: AtomicI32 = AtomicI32::new(-1);            // 悬停项
pub(crate) static REC_ARROW: AtomicI32 = AtomicI32::new(20);            // 设置态顶上小三角的 x（窗口坐标）
pub(crate) static REC_ESC: AtomicBool = AtomicBool::new(false);         // ESC 上一轮是否按下（边沿触发）
pub(crate) static REC_THREAD: Mutex<Option<std::thread::JoinHandle<()>>> = Mutex::new(None);
pub(crate) static REC_PATH: Mutex<Option<String>> = Mutex::new(None);   // 临时文件路径
pub(crate) static REC_FRAMES: AtomicI64 = AtomicI64::new(0);            // 真正编码进文件的帧数
// 采集原点（屏幕物理像素）。开录时一次算好、录制中不再变动
pub(crate) static REC_X: AtomicI32 = AtomicI32::new(0);
pub(crate) static REC_Y: AtomicI32 = AtomicI32::new(0);
// 真正采集的宽高：MP4 要对齐到 4/2，可能比选区小 1~3 像素
pub(crate) static REC_W: AtomicI32 = AtomicI32::new(0);
pub(crate) static REC_H: AtomicI32 = AtomicI32::new(0);

// 设置态五格 + 两根分隔（cell*5 + sep*2）。
// 两颗互斥格式键（MP4 / GIF），没有退出键
pub(crate) const REC_T_TIMER: usize = 96;                                 // 每秒 +1
pub(crate) const REC_T_ESC: usize = 97;
pub(crate) const REC_ESC_MS: u32 = 30;
// GIF 帧率 16。首帧 delay = max(2, round(100/fps)) = 6cs = 60ms
pub(crate) const REC_GIF_DELAY_CS: u16 = 6;

// 录制条 8 颗按钮的提示（zh-CN.json 的 video.*，下标与 rec_hit 完全对应）
pub(crate) const REC_TIPS: [&str; 8] = [
    "录制为 MP4（带声音）", "录制为 GIF（无声音）",
    "录系统声音", "录麦克风",
    "开始录制", "停止并丢弃", "停止并保存", "停止并复制到剪贴板",
];

// —— 录制条不进录像 ——
// WDA_EXCLUDEFROMCAPTURE：Win10 2004+ 才有，winapi 0.3.9 没导出，自己写死。
// 作用是"这个窗口显示在屏幕上、但不出现在任何截屏/录屏里"——录制条靠它不进 MP4 画面
pub(crate) const WDA_EXCLUDEFROMCAPTURE: u32 = 0x0000_0011;

/// 采集线程每帧调一次：拿当前该抓的屏幕原点（开录时固定，录制中不再变）
pub(crate) fn rec_origin() -> (i32, i32) {
    (REC_X.load(Ordering::Relaxed), REC_Y.load(Ordering::Relaxed))
}

/// 采集线程：按 16fps 抓选区，逐帧压进 GIF（image 的 GifEncoder 自带调色板量化）。
/// 之后每帧按实测间隔改写
/// （编码追不上目标帧率时按真实时长播）；与上一帧逐字节相同的画面直接跳过，
/// 收尾时把最后一段静止时长补成尾帧
pub(crate) fn rec_loop(path: String, _ox: i32, _oy: i32, w: i32, h: i32) -> bool {
    let f = match std::fs::File::create(&path) { Ok(f) => f, Err(_) => { return false; } };
    let mut enc = image::codecs::gif::GifEncoder::new(f);
    // attrFlags = 0，不写 NETSCAPE 循环扩展：GIF 播一遍就停，这里不调 set_repeat
    let frame_ms = REC_GIF_DELAY_CS as u64 * 10;
    // 上一帧真正编码的 RGBA：静止画面整幅逐字节相同，不必再过一遍量化
    let mut last: Vec<u8> = Vec::new();
    let mut last_tick = std::time::Instant::now();
    let mut buf: Vec<u8> = Vec::new();
    let mut rgba: Vec<u8> = Vec::new();
    let mut ok = true;
    while !REC_STOP.load(Ordering::Relaxed) {
        let t0 = std::time::Instant::now();
        // 每帧现取原点：录制范围开录时已定死，这里读到的始终是同一块
        let (ox, oy) = rec_origin();
        if screenshot_bgra_into(ox, oy, w, h, &mut buf) {
            // 鼠标指针已由 screenshot_bgra_into 画进这一帧（gif / mp4 同一条路径），
            // 这里再画一次会重复合成半透明光标，且每帧多建一张整屏 DIB
            // BGRA → RGBA，一次写四个字节
            rgba.clear();
            rgba.reserve(buf.len());
            for s in buf.chunks_exact(4) {
                rgba.extend_from_slice(&[s[2], s[1], s[0], 255]);
            }
            if last.as_slice() == rgba.as_slice() {
                let used = t0.elapsed().as_millis() as u64;
                if used < frame_ms { std::thread::sleep(Duration::from_millis(frame_ms - used)); }
                continue;
            }
            // 除首帧外，delay 用实测帧间隔（单位 10ms，最小 2）：编码慢时按真实时长播，不快进
            let delay_cs = if REC_FRAMES.load(Ordering::Relaxed) == 0 {
                REC_GIF_DELAY_CS
            } else {
                ((last_tick.elapsed().as_millis() / 10) as u16).clamp(2, 0xFFFF)
            };
            if let Some(img) = image::RgbaImage::from_raw(w as u32, h as u32, rgba.clone()) {
                let frame = image::Frame::from_parts(img, 0, 0,
                    image::Delay::from_numer_denom_ms(delay_cs as u32 * 10, 1));
                if enc.encode_frame(frame).is_err() { ok = false; break; }   // 编码器坏了（写盘失败），直接收尾
                REC_FRAMES.fetch_add(1, Ordering::Relaxed);
                // 换缓冲而不是整幅再拷一遍；rgba 在下一轮开头清空复用
                std::mem::swap(&mut last, &mut rgba);
                last_tick = std::time::Instant::now();
            }
        }
        let used = t0.elapsed().as_millis() as u64;
        if used < frame_ms { std::thread::sleep(Duration::from_millis(frame_ms - used)); }
    }
    // 收尾：最后一段静止画面没有产生新帧，这截时长还没写进任何帧，补一帧同内容
    if !last.is_empty() && REC_FRAMES.load(Ordering::Relaxed) > 0 {
        let tail_cs = (last_tick.elapsed().as_millis() / 10) as u16;
        if tail_cs >= 2 {
            if let Some(img) = image::RgbaImage::from_raw(w as u32, h as u32, last) {
                let frame = image::Frame::from_parts(img, 0, 0,
                    image::Delay::from_numer_denom_ms(tail_cs as u32 * 10, 1));
                let _ = enc.encode_frame(frame);
            }
        }
    }
    drop(enc);   // 关闭即写 GIF 尾巴
    ok
}

// 录制条上每一项按钮本体的矩形（物理像素、客户区坐标）。idx 含义见 rec_hit。
pub(crate) unsafe fn rec_btn_rect(idx: i32) -> (i32, i32, i32, i32) {
    let by = sc(ui::layout::record::BTN_Y);
    let bs = sc(ui::metric::BTN);
    let cell = |x: i32| (sc(x) + sc(ui::metric::PAD), by, bs, bs);
    match idx {
        // 设置态：MP4 / GIF / 系统声 / 麦克风 / 开始（格子在分隔条两侧）
        0 => (sc(ui::metric::PAD), by, sc(ui::metric::CELL) - 2 * sc(ui::metric::PAD), bs),
        1 => (sc(ui::metric::CELL) + sc(ui::metric::PAD), by, sc(ui::metric::CELL) - 2 * sc(ui::metric::PAD), bs),
        2 => cell(ui::metric::CELL * 2 + ui::metric::SEP),
        3 => cell(ui::metric::CELL * 3 + ui::metric::SEP),
        4 => cell(ui::metric::CELL * 4 + ui::metric::SEP * 2),
        // 录制态三颗：丢弃 / 存文件 / 存剪切板
        5 => cell(ui::layout::record::TIM_W + ui::metric::SEP),
        6 => cell(ui::layout::record::TIM_W + ui::metric::SEP + ui::metric::CELL),
        7 => cell(ui::layout::record::TIM_W + ui::metric::SEP + ui::metric::CELL * 2),
        _ => (0, 0, 0, 0),
    }
}

/// 命中的项：设置态 0=MP4 1=GIF 2=系统声 3=麦克风 4=开始；
/// 录制态 5=丢弃 6=存文件 7=存剪切板
pub(crate) unsafe fn rec_hit(x: i32, _y: i32) -> i32 {
    if REC_PHASE.load(Ordering::Relaxed) == 0 {
        for (a, b, i) in [(0, ui::metric::CELL, 0),
                          (ui::metric::CELL, ui::metric::CELL * 2, 1),
                          (ui::metric::CELL * 2 + ui::metric::SEP, ui::metric::CELL * 3 + ui::metric::SEP, 2),
                          (ui::metric::CELL * 3 + ui::metric::SEP, ui::metric::CELL * 4 + ui::metric::SEP, 3),
                          (ui::metric::CELL * 4 + ui::metric::SEP * 2, ui::layout::record::SET_W, 4)] {
            if x >= sc(a) && x < sc(b) { return i; }
        }
    } else {
        let base = ui::layout::record::TIM_W + ui::metric::SEP;
        for k in 0..3 {
            let a = base + ui::metric::CELL * k;
            if x >= sc(a) && x < sc(a + ui::metric::CELL) { return 5 + k; }
        }
    }
    -1
}

/// 条上的纯文字（格式键、计时）：居中画一行
pub(crate) unsafe fn rec_draw_text(hdc: winapi::shared::windef::HDC, x: i32, y: i32, w: i32, h: i32,
                        text: &str, color: u32, font: HFONT) {
    let oldf = SelectObject(hdc, font as *mut winapi::ctypes::c_void);
    SetBkMode(hdc, TRANSPARENT as i32);
    SetTextColor(hdc, color);
    let mut r = RECT { left: x, top: y, right: x + w, bottom: y + h };
    DrawTextW(hdc, wide(text).as_ptr(), -1, &mut r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldf);
}

// 录制条：白底 + 1px #A8A8A8 外框，两态共用一条（宽度按状态换）
pub(crate) extern "system" fn rec_bar_proc(hwnd: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    // 录制条选中态配色：浅蓝底 + 蓝字
    const ON_BG: u32 = ui::color::RECORD_BG;
    const ON_FG: u32 = ui::color::cref(ui::color::RECORD_ACCENT);
    const OFF_FG: u32 = ui::color::cref(ui::color::ICON);
    const DIS_FG: u32 = ui::color::cref(ui::color::RECORD_META);
    unsafe {
        match msg {
            // 窗口类背景刷为空，再明确挡掉系统擦背景：免得显示/改大小时先擦出一块黑
            WM_ERASEBKGND => 1,
            WM_PAINT => {
                let mut ps: PAINTSTRUCT = std::mem::zeroed();
                let hdc = BeginPaint(hwnd, &mut ps);
                let mut rc: RECT = std::mem::zeroed();
                GetClientRect(hwnd, &mut rc);
                let cw = rc.right - rc.left; let ch = rc.bottom - rc.top;
                // 顶部箭头区：可见框从 top 开始
                let top = sc(ui::layout::anno::SUB_ARROW_ZONE);
                let ph = REC_PHASE.load(Ordering::Relaxed);
                // 双缓冲：一次画完再贴，免得计时每秒刷新时闪
                let buf = PaintBuf::new(hdc, cw, ch);
                let memdc = buf.as_ref().map_or(hdc, |b| b.dc());
                // 先把整块刷成背景色，窗口区域会把箭头区外的地方裁掉
                fill_solid(memdc, &rc, ui::color::cref(ui::color::TOOL_SURFACE));
                let bpen = CreatePen(PS_SOLID as i32, ui::metric::STROKE, ui::color::cref(ui::color::TOOL_BORDER));
                let oldbpen = SelectObject(memdc, bpen as *mut winapi::ctypes::c_void);
                let oldbbr = SelectObject(memdc, GetStockObject(NULL_BRUSH as i32));
                let sbrush = CreateSolidBrush(ui::color::cref(ui::color::TOOL_SURFACE));
                let oldsbr = SelectObject(memdc, sbrush as *mut winapi::ctypes::c_void);
                if ph == 0 {
                    // 设置态：矩形 + 顶上指向视频按钮的小三角
                    // 顶点 (ax±3, 2)(ax, 0)，用背景色填充、边框描一圈）
                    let ax = REC_ARROW.load(Ordering::Relaxed);
                    let half = sc(ui::layout::anno::SUB_ARROW_HALF);
                    let wb = cw as i64 - 1;
                    let hb = ch as i64 - 1;
                    let pts = [
                        POINT { x: (ax - half) as i32, y: top },
                        POINT { x: ax as i32, y: 0 },
                        POINT { x: (ax + half) as i32, y: top },
                        POINT { x: wb as i32, y: top },
                        POINT { x: wb as i32, y: hb as i32 },
                        POINT { x: 0, y: hb as i32 },
                        POINT { x: 0, y: top },
                    ];
                    Polygon(memdc, pts.as_ptr(), pts.len() as i32);
                } else {
                    // 录制态：没有箭头，可见框就是矩形
                    Rectangle(memdc, 0, top, cw, ch);
                    // GDI 的 Rectangle 不含右/下边，补 1px
                    let bp = CreateSolidBrush(ui::color::cref(ui::color::TOOL_BORDER));
                    let br = RECT { left: cw - ui::metric::STROKE, top: top, right: cw, bottom: ch };
                    FillRect(memdc, &br, bp);
                    let br = RECT { left: 0, top: ch - ui::metric::STROKE, right: cw, bottom: ch };
                    FillRect(memdc, &br, bp);
                    DeleteObject(bp as *mut winapi::ctypes::c_void);
                }
                SelectObject(memdc, oldsbr);
                DeleteObject(sbrush as *mut winapi::ctypes::c_void);
                SelectObject(memdc, oldbbr);
                SelectObject(memdc, oldbpen);
                DeleteObject(bpen as *mut winapi::ctypes::c_void);

                let font = ui_font_px(sc(ui::font::SIZE_13), ui::font::W_REGULAR) as HFONT;
                let hov = REC_HOVER.load(Ordering::Relaxed);

                if ph == 0 {
                    // 设置态：MP4 / GIF 两颗互斥格式键（选中那颗浅蓝底 + 蓝字）
                    let mp4 = REC_MP4.load(Ordering::Relaxed);
                    for (i, txt, on) in [(0, "MP4", mp4), (1, "GIF", !mp4)] {
                        let (fx, fy, fw, fh) = rec_btn_rect(i);
                        let fbg = if on { ON_BG } else if hov == i { ui::color::TOOL_HOVER } else { ui::color::TOOL_SURFACE };
                        let fr = RECT { left: fx, top: fy, right: fx + fw, bottom: fy + fh };
                        fill_solid(memdc, &fr, ((fbg & 0xFF) << 16) | (fbg & 0xFF00) | (fbg >> 16));
                        let fg = if on { ON_FG } else { OFF_FG };
                        rec_draw_text(memdc, fx, fy, fw, fh, txt, fg, font);
                    }
                    // 两根分隔条（整高，被 1px 描边各盖住上下 1px）
                    for sx in ui::layout::record::SET_SEP {
                        let sr = RECT { left: sc(sx), top: top + ui::metric::STROKE, right: sc(sx) + sc(ui::metric::SEP), bottom: ch - ui::metric::STROKE };
                        fill_solid(memdc, &sr, ui::color::TOOL_SEP);
                    }
                    // 系统声 / 麦克风：GIF 没有音轨，两颗一起压灰（点了也没反应）。
                    // 麦克风图标跟着开关走：开着是实心麦、关着是带斜杠的麦
                    let mic_on = REC_MIC.load(Ordering::Relaxed);
                    for (i, icon, on) in [(2, "video-speaker", REC_SPK.load(Ordering::Relaxed)),
                                          (3, if mic_on { "recording-microphone" } else { "recording-microphone-off" }, mic_on)] {
                        let (x, y, w, _h) = rec_btn_rect(i);
                        let (bg, fg) = if !mp4 { (ui::color::TOOL_SURFACE, DIS_FG) }
                            else if on { (ON_BG, ON_FG) }
                            else if hov == i { (ui::color::TOOL_HOVER, OFF_FG) }
                            else { (ui::color::TOOL_SURFACE, OFF_FG) };
                        anno_blit_icon(memdc, x, y, w, icon, bg, fg);
                    }
                    let (sx1, sy1, sw1, _sh1) = rec_btn_rect(4);
                    anno_blit_icon(memdc, sx1, sy1, sw1, "recording-resume",
                        if hov == 4 { ui::color::TOOL_HOVER } else { ui::color::TOOL_SURFACE }, OFF_FG);
                } else {
                    // 录制态：计时 {:02}:{:02} / {:02}:00（GIF 2 分钟、MP4 10 分钟）
                    let sec = REC_SEC.load(Ordering::Relaxed);
                    let maxm = if REC_MP4.load(Ordering::Relaxed) { 10 } else { 2 };
                    let txt = format!("{:02}:{:02} / {:02}:00", sec / 60, sec % 60, maxm);
                    rec_draw_text(memdc, 0, top, sc(ui::layout::record::TIM_W), ch - top, &txt, OFF_FG, font);
                    let sr = RECT { left: sc(ui::layout::record::TIM_W), top: top + ui::metric::STROKE, right: sc(ui::layout::record::TIM_W) + sc(ui::metric::SEP), bottom: ch - ui::metric::STROKE };
                    fill_solid(memdc, &sr, ui::color::TOOL_SEP);
                    for (i, icon, fg) in [(5, "trash", ui::color::ICON_DANGER), (6, "quick-save", OFF_FG),
                                          (7, "action-copy", OFF_FG)] {
                        let (x, y, w, _h) = rec_btn_rect(i);
                        anno_blit_icon(memdc, x, y, w, icon,
                            if hov == i { ui::color::TOOL_HOVER } else { ui::color::TOOL_SURFACE }, fg);
                    }
                }
                if let Some(b) = buf.as_ref() { b.present(hdc); }
                EndPaint(hwnd, &ps);
                0
            }
            WM_LBUTTONDOWN => 0,
            WM_LBUTTONUP => {
                let x = (lp & 0xFFFF) as i16 as i32;
                match rec_hit(x, 0) {
                    // 两颗格式键互斥：点已选中的那颗不做任何事
                    0 => { if !REC_MP4.load(Ordering::Relaxed) {
                        REC_MP4.store(true, Ordering::Relaxed);
                        // 切到 MP4：系统声默认开、麦克风默认关
                        REC_SPK.store(true, Ordering::Relaxed);
                        REC_MIC.store(false, Ordering::Relaxed);
                        rec_persist_opts();
                        InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                    } }
                    1 => { if REC_MP4.load(Ordering::Relaxed) {
                        REC_MP4.store(false, Ordering::Relaxed);
                        // 切到 GIF：没有音轨，两路一起关
                        REC_SPK.store(false, Ordering::Relaxed);
                        REC_MIC.store(false, Ordering::Relaxed);
                        rec_persist_opts();
                        InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                    } }
                    2 => { if REC_MP4.load(Ordering::Relaxed) {
                        let v = !REC_SPK.load(Ordering::Relaxed);
                        REC_SPK.store(v, Ordering::Relaxed);
                        rec_persist_opts();
                        InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                    } }
                    3 => { if REC_MP4.load(Ordering::Relaxed) {
                        let v = !REC_MIC.load(Ordering::Relaxed);
                        REC_MIC.store(v, Ordering::Relaxed);
                        rec_persist_opts();
                        InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                    } }
                    4 => anno_record_begin(),
                    5 => rec_finish(true, false),   // 丢弃：停录 + 删临时文件 + 收会话
                    6 => rec_finish(false, false),  // 存文件
                    7 => rec_finish(false, true),   // 存剪切板
                    _ => {}
                }
                0
            }
            WM_MOUSEMOVE => {
                let x = (lp & 0xFFFF) as i16 as i32; let y = ((lp >> 16) & 0xFFFF) as i16 as i32;
                track_leave(hwnd);
                let hov = rec_hit(x, y);
                if hov != REC_HOVER.load(Ordering::Relaxed) {
                    REC_HOVER.store(hov, Ordering::Relaxed);
                    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                }
                // 悬停提示：八颗按钮各有 video.* 文案
                if hov >= 0 {
                    let (bx, by, bw2, _bh2) = rec_btn_rect(hov);
                    tip_show_above(&TIP_REC, hov, bx as f32, by as f32, bw2 as f32);
                } else {
                    tip_hide(&TIP_REC);
                }
                0
            }
            WM_MOUSELEAVE => {
                tip_hide(&TIP_REC);
                if REC_HOVER.load(Ordering::Relaxed) != -1 {
                    REC_HOVER.store(-1, Ordering::Relaxed);
                    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                }
                0
            }
            WM_TIMER => {
                if wp == REC_T_TIMER {
                    if REC_PHASE.load(Ordering::Relaxed) != 1 { return 0; }
                    // 采集线程已提前退出：0 帧 = 编码器起不来（beginVideo 返回 false）
                    // 留在设置态）；有帧 = 中途写盘失败，把已录到的存下来再收尾。
                    // 不处理的话界面会一直停在"录制中"，要等 600 秒到点才收
                    if REC_THREAD.lock_ok().as_ref().map_or(false, |t| t.is_finished()) {
                        if REC_FRAMES.load(Ordering::Relaxed) == 0 { rec_failed_recover(); }
                        else { rec_finish(false, false); }
                        return 0;
                    }
                    let sec = REC_SEC.fetch_add(1, Ordering::Relaxed) + 1;
                    // MP4 最长 10 分钟、GIF 最长 2 分钟：到点自动存盘收工
                    let maxs = if REC_MP4.load(Ordering::Relaxed) { 600 } else { 120 };
                    // 每秒顺带保活一次：位置/置顶重落实一遍，录制条被谁压下去也能回来。
                    // 位置按选区重算：显示器被拔 / 分辨率或 DPI 变了
                    // 条也能被夹回还看得见的那块工作区里；SetWindowPos(HWND_TOPMOST) 同时保证
                    // 它压在录制中的宿主（分层压暗窗）之上，不会被压暗
                    rec_bar_follow_frame(sc(ui::layout::record::RUN_W), sc(ui::layout::record::H));
                    // 录制条是录制中唯一的入口：被别的程序藏掉就再也点不到"停止"了，
                    // 每秒无条件显示回来
                    if IsWindowVisible(hwnd) == 0 { ShowWindow(hwnd, SW_SHOWNA); }
                    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                    if sec >= maxs { rec_finish(false, false); }
                } else if wp == REC_T_ESC {
                    // 录制中宿主可能丢了焦点（用户点桌面里的应用），ESC 的 WM_KEYDOWN 收不到 ——
                    // 按边沿自己轮询。
                    // 低位 0x0001 = "上次调用之后按下过"，按得极快时高位会漏，这一位不会
                    let st = GetAsyncKeyState(VK_ESCAPE);
                    let down = (st & 0x8000u16 as i16) != 0;
                    let pressed = (st & 0x0001) != 0 || (down && !REC_ESC.load(Ordering::Relaxed));
                    REC_ESC.store(down, Ordering::Relaxed);
                    if pressed { rec_finish(true, false); }
                } else if wp == TIP_TIMER {
                    tip_timer(&TIP_REC);
                }
                0
            }
            WM_DESTROY => {
                KillTimer(hwnd, REC_T_TIMER); KillTimer(hwnd, REC_T_ESC);
                tip_destroy(&TIP_REC);
                REC_BAR.store(0, Ordering::Relaxed); 0
            }
            _ => DefWindowProcW(hwnd, msg, wp, lp),
        }
    }
}

/// 建录制条窗口（只建一次，两态共用；宽度按状态用 SetWindowPos 换）
pub(crate) unsafe fn rec_bar_create() -> HWND {
    let hinstance = GetModuleHandleW(std::ptr::null_mut());
    let cls = wide(CLS_RECBAR);
    reg_class(CLS_RECBAR, Some(rec_bar_proc), 0, IDC_ARROW as usize, false);
    // WS_EX_NOACTIVATE：点这条不抢前台焦点 —— 录制中用户还在别的程序里打字，
    // 点一下"存文件"就把输入焦点抢走的话，这个功能就是"妨碍桌面操作"了
    let bh_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        cls.as_ptr(), wide("录屏").as_ptr(),
        WS_POPUP, 0, 0, sc(ui::layout::record::SET_W), sc(ui::layout::record::H),
        std::ptr::null_mut(), std::ptr::null_mut(), hinstance, std::ptr::null_mut());
    if bh_.is_null() { return std::ptr::null_mut(); }
    // 条也不能被录进视频：它挂着"存文件/丢弃"，录进画面就是穿帮。
    // 摆在选区外沿是主要手段，这里是双保险（老系统上这个调用会失败，无害）
    SetWindowDisplayAffinity(bh_, WDA_EXCLUDEFROMCAPTURE);
    REC_BAR.store(bh_ as isize, Ordering::Relaxed);
    tip_create(&TIP_REC, bh_);
    bh_
}

/// 摆放录制条。两种状态两套跟法：
///   · 设置态：它就是"录制工具的副条"，走与工具副条完全相同的 sub_bar_place ——
/// 居中到主条第 8 格（录制）正下方，与工具副条同一套摆法；
/// 否则同屏两条副条两种对齐方式，看着就是"录屏那条特别靠左"；
/// · 录制态：主条已经藏了，条跟着选区右下角外沿走 —— 而且贴着选区外面，
/// 不会被录进去。
pub(crate) unsafe fn rec_bar_place(bw_logical: i32) {
    let bar = REC_BAR.load(Ordering::Relaxed) as HWND;
    if bar.is_null() { return; }
    let bw = sc(bw_logical);
    let bh = sc(ui::layout::record::H);
    let top = sc(ui::layout::anno::SUB_ARROW_ZONE);
    let half = sc(ui::layout::anno::SUB_ARROW_HALF);
    let mut ax = 0;
    let mut arrow = false;
    if REC_PHASE.load(Ordering::Relaxed) == 1 {
        rec_bar_follow_frame(bw, bh);
    } else {
        // 第 8 格（索引 7）= 录制。副条居中到它下面，与马赛克/橡皮那条同一个规则
        let anchor = anno_btn_center_x(REC_BTN_IDX);
        let px = sub_bar_place(bar, bw, bh, anchor);
        let mut mr: RECT = std::mem::zeroed();
        let mhw = ANNO_MAIN_HWND.load(Ordering::Relaxed) as HWND;
        if !mhw.is_null() { GetWindowRect(mhw, &mut mr); }
        // 夹过之后箭头跟着修正，继续指着那颗按钮
        ax = (mr.left + anchor - px).clamp(half + 1, bw - half - 1);
        arrow = true;
    }
    REC_ARROW.store(ax, Ordering::Relaxed);
    // 把窗口裁成"矩形 + 顶上小三角"：箭头区两侧才不会露出白块
    let r1 = CreateRectRgn(0, top, bw, bh);
    if arrow {
        let tri = [
            POINT { x: ax - half, y: top },
            POINT { x: ax, y: 0 },
            POINT { x: ax + half, y: top },
        ];
        let r2 = CreatePolygonRgn(tri.as_ptr(), 3, WINDING);
        if !r2.is_null() {
            CombineRgn(r1, r1, r2, RGN_OR);
            DeleteObject(r2 as *mut winapi::ctypes::c_void);
        }
    }
    // 录制态每秒都会重算位置：只改形状、不在这里触发重绘（调用方会 Invalidate），
    // 避免"旧内容 + 新形状"的中间帧闪一下
    if SetWindowRgn(bar, r1, 0) == 0 {
        DeleteObject(r1 as *mut winapi::ctypes::c_void);
    }
}

/// 主条上"录制"那颗按钮的下标。0..6 是七个绘制工具，第 8 格才是录制
pub(crate) const REC_BTN_IDX: i32 = 7;

/// 录制态：把条挂在选区的右下角外沿（选区下方放不下就翻到上方），夹进工作区。
pub(crate) unsafe fn rec_bar_follow_frame(bw: i32, bh: i32) {
    let bar = REC_BAR.load(Ordering::Relaxed) as HWND;
    if bar.is_null() { return; }
    let gap = sc(ui::metric::SPACE_1);
    // 录制区域开录即定死，条跟着这块不动
    let (sox, soy) = sel_org();
    let fl = ANNO_OX.load(Ordering::Relaxed) + sox;
    let ft = ANNO_OY.load(Ordering::Relaxed) + soy;
    let fr = fl + ANNO_W.load(Ordering::Relaxed);
    let fb = ft + ANNO_H.load(Ordering::Relaxed);
    let (vw, vh) = sel_vp();
    let wa = mon_work_area((fl + fr) / 2, (ft + fb) / 2,
        RECT { left: sox, top: soy, right: sox + vw, bottom: soy + vh });
    let mut x = fr - bw;
    let mut y = fb + gap;
    if y + bh > wa.bottom { y = ft - gap - bh; }      // 下面放不下 → 翻到框上面
    if y < wa.top { y = wa.top; }
    x = x.max(wa.left).min((wa.right - bw).max(wa.left));
    // GIF 走的是 GDI BitBlt，WDA_EXCLUDEFROMCAPTURE 对它无效：条一旦压在采集区里，
    // 每一帧都会带着这条条。上下都放不下时再试左右外侧
    if !REC_MP4.load(Ordering::Relaxed) && x < fr && x + bw > fl && y < fb && y + bh > ft {
        let cy = (ft + ((fb - ft) - bh) / 2).max(wa.top).min(wa.bottom - bh);
        let left_x = fl - gap - bw;
        let right_x = fr + gap;
        if left_x >= wa.left { x = left_x; y = cy; }
        else if right_x + bw <= wa.right { x = right_x; y = cy; }
        // 左右也放不下（选区几乎铺满整屏）：没有任何屏内位置能避开，维持原样
    }
    SetWindowPos(bar, HWND_TOPMOST, x, y, bw, bh, SWP_NOACTIVATE);
}

/// 设置态录制条是否正显示。录制中（REC_PHASE=1）那条是录制控制条，不参与悬停收起
pub(crate) unsafe fn rec_settings_visible() -> bool {
    let b = REC_BAR.load(Ordering::Relaxed) as HWND;
    !b.is_null() && REC_PHASE.load(Ordering::Relaxed) == 0 && IsWindowVisible(b) != 0
}

/// 收起设置态录制条（录制中不动）
pub(crate) unsafe fn rec_settings_hide() {
    if rec_settings_visible() {
        let b = REC_BAR.load(Ordering::Relaxed) as HWND;
        tip_hide(&TIP_REC);
        ShowWindow(b, SW_HIDE);
    }
}

/// 录制条设置态的三颗开关按工具名 "video" 落盘。
/// 视频选项按 curToolId = video 持久化，同一条口径
pub(crate) fn rec_persist_opts() {
    cfg_set_tool_flag("video", "mp4", REC_MP4.load(Ordering::Relaxed));
    cfg_set_tool_flag("video", "speaker", REC_SPK.load(Ordering::Relaxed));
    cfg_set_tool_flag("video", "mic", REC_MIC.load(Ordering::Relaxed));
}

/// 清掉数据目录里上一次录制留下的临时文件（录制中被强杀 / 收尾超时都会留下半截文件）。
/// 只在开录前调用；还在被占用的那份删不掉也没关系，忽略返回值。
fn strip_stale_temp(dir: &std::path::Path) {
    let mut rd = match std::fs::read_dir(dir) { Ok(x) => x, Err(_) => return };
    while let Some(Ok(e)) = rd.next() {
        let name = e.file_name().to_string_lossy().into_owned();
        if name.starts_with("temp_") || name == "temp.mp4" || name == "temp.gif" {
            // 刚失败、还没来得及落盘的录制别当垃圾清掉（后台收尾失败时会保留源文件）
            let fresh = e.metadata().ok()
                .and_then(|m| m.modified().ok())
                .and_then(|t| t.elapsed().ok())
                .is_some_and(|d| d < Duration::from_secs(3600));
            if fresh { continue; }
            let _ = std::fs::remove_file(e.path());
        }
    }
}

/// 进入录制外观
/// 宿主窗**全程不藏**，整窗退成"选区外压暗"的一层遮罩 ——
/// 选区那块从窗口区域里挖掉（RGN_DIFF），露出的就是真实桌面；外侧由 WM_PAINT 画
/// 「整屏原图 + 一层 32% 黑」，与截图选框态同一个压暗值。
///
/// 压暗是画出来的，窗口不透明度恒为 255。之前这里"先藏、改完再显"，藏与显之间整屏桌面
/// 亮出来一帧，就是点录制时那一下闪。窗口不动，只换内容。
/// 另加 WS_EX_TRANSPARENT：压暗区也不挡鼠标，桌面照常能点。
pub(crate) unsafe fn rec_host_enter(hwnd: HWND) {
    if hwnd.is_null() { return; }
    let ox = ANNO_OX.load(Ordering::Relaxed);
    let oy = ANNO_OY.load(Ordering::Relaxed);
    let w = ANNO_W.load(Ordering::Relaxed);
    let h = ANNO_H.load(Ordering::Relaxed);
    let mut rc: RECT = std::mem::zeroed();
    GetClientRect(hwnd, &mut rc);
    let full = CreateRectRgn(rc.left, rc.top, rc.right, rc.bottom);
    let hole = CreateRectRgn(ox, oy, ox + w, oy + h);
    // SetWindowRgn 成功后区域归系统所有，不要再 DeleteObject。
    // bRedraw=0：这一步只改形状不立刻画，等下面统一次重绘，避免"旧内容 + 新形状"的中间帧。
    if !full.is_null() && !hole.is_null() && CombineRgn(full, full, hole, RGN_DIFF) != 0 {
        if SetWindowRgn(hwnd, full, 0) == 0 {
            DeleteObject(full as *mut winapi::ctypes::c_void);
        }
    } else if !full.is_null() {
        DeleteObject(full as *mut winapi::ctypes::c_void);
    }
    if !hole.is_null() { DeleteObject(hole as *mut winapi::ctypes::c_void); }
    let ex = GetWindowLongPtrW(hwnd, winapi::um::winuser::GWL_EXSTYLE) as u32;
    SetWindowLongPtrW(hwnd, winapi::um::winuser::GWL_EXSTYLE,
        (ex | WS_EX_LAYERED | WS_EX_TRANSPARENT) as isize);
    // 不透明度恒 255：只把"分层"这条件补上（跨进程鼠标穿透要 LAYERED+TRANSPARENT 组合）。
    // 压暗由 WM_PAINT 画出来，不能靠改窗口 alpha —— 改一次就是一次可见闪。
    SetLayeredWindowAttributes(hwnd, 0, 255, 0x2 /*LWA_ALPHA*/);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
    UpdateWindow(hwnd);
}

/// 退出录制外观：撤掉挖洞 / 分层 / 鼠标穿透，宿主回到正常编辑态。
/// 停止 / 丢弃 / 失败三条路都要先走这里，桌面上不能留下压暗层。
/// 同样不藏窗口：区域和样式一次性撤掉再重画，避免"黑屏一帧 → 重现"。
pub(crate) unsafe fn rec_host_exit(hwnd: HWND) {
    if hwnd.is_null() || IsWindow(hwnd) == 0 { return; }
    SetWindowRgn(hwnd, std::ptr::null_mut(), 0);
    let ex = GetWindowLongPtrW(hwnd, winapi::um::winuser::GWL_EXSTYLE) as u32;
    SetWindowLongPtrW(hwnd, winapi::um::winuser::GWL_EXSTYLE,
        (ex & !(WS_EX_LAYERED | WS_EX_TRANSPARENT)) as isize);
    SetWindowPos(hwnd, std::ptr::null_mut(), 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
    UpdateWindow(hwnd);
}

/// 点主条第 8 格：直接开录。
/// 设置项（GIF/MP4、系统声、麦克风）在悬停时已经由 anno_record_open 读入，
/// 这里再读一遍配置作兜底 —— 没悬停过直接点（触屏 / 键盘触发）也能用当前的设置开录。
pub(crate) unsafe fn anno_record_start() {
    if REC_PHASE.load(Ordering::Relaxed) == 1 { return; }
    if REC_BAR.load(Ordering::Relaxed) == 0 && rec_bar_create().is_null() { return; }
    REC_MP4.store(cfg_tool_flag("video", "mp4", true), Ordering::Relaxed);
    REC_SPK.store(cfg_tool_flag("video", "speaker", true), Ordering::Relaxed);
    REC_MIC.store(cfg_tool_flag("video", "mic", false), Ordering::Relaxed);
    REC_HOVER.store(-1, Ordering::Relaxed);
    anno_record_begin();
}

/// 打开设置态录制条（悬停主条第 8 格时预览）
pub(crate) unsafe fn anno_record_open() {
    if REC_PHASE.load(Ordering::Relaxed) == 1 { return; }
    if REC_BAR.load(Ordering::Relaxed) == 0 && rec_bar_create().is_null() { return; }
    // 上次的格式 / 音源开关
    REC_MP4.store(cfg_tool_flag("video", "mp4", true), Ordering::Relaxed);
    REC_SPK.store(cfg_tool_flag("video", "speaker", true), Ordering::Relaxed);
    REC_MIC.store(cfg_tool_flag("video", "mic", false), Ordering::Relaxed);
    // 的录制条就是副条本身（换内容），所以开录制条时把别的工具的副条收掉
    // 不然两片同名区域的窗口会叠在同一个位置上
    let sub = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
    if !sub.is_null() { ShowWindow(sub, SW_HIDE); }
    tip_hide(&TIP_SUB);
    REC_PHASE.store(0, Ordering::Relaxed);
    REC_HOVER.store(-1, Ordering::Relaxed);
    rec_bar_place(ui::layout::record::SET_W);
    let bar = REC_BAR.load(Ordering::Relaxed) as HWND;
    ShowWindow(bar, SW_SHOWNA);
    UpdateWindow(bar);
}

/// 采集线程：mp4 走 Media Foundation（开音源时顺带抓声音），gif 走 image 的编码器
pub(crate) fn rec_engine(path: String, ox: i32, oy: i32, w: i32, h: i32, mp4: bool, spk: bool, mic: bool) {
    if mp4 {
        // 帧数由编码端带回：0 帧 = 空壳，收尾时按它判"没录到画面"
        match recvideo::record_mp4(&path, ox, oy, w, h, spk, mic) {
            Some(n) => REC_FRAMES.store(n as i64, Ordering::Relaxed),
            None => rec_record_failed(),
        }
    } else {
        // 帧数在 rec_loop 里边编码边累加；false = 临时文件都建不出来，与 MP4 起不来同口径报错
        if !rec_loop(path, ox, oy, w, h) { rec_record_failed(); }
    }
}

/// 编码器起不来 / 写到一半失败： CapVideo::showRecordError 同一句话同一个样式
/// 录制条是 TOPMOST 的，普通弹框会被压在后面，必须带 MB_TOPMOST | MB_SETFOREGROUND
pub(crate) fn rec_record_failed() {
    unsafe {
        MessageBoxW(std::ptr::null_mut(), wide("录制启动失败，请重试").as_ptr(),
            wide("系统提示").as_ptr(),
            MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    }
}

/// 编码器起不来（0 帧）：把界面从"录制中"退回设置态，编辑器不关。
/// beginVideo 返回 false 时就是这个结果（录制停在设置态，用户可换格式重试）
/// 采集线程已经结束，这里 join 不会卡；临时空壳文件一起删掉。
pub(crate) fn rec_failed_recover() {
    REC_PHASE.store(0, Ordering::Relaxed);
    REC_STOP.store(true, Ordering::Relaxed);
    let bar = REC_BAR.load(Ordering::Relaxed) as HWND;
    unsafe {
        if !bar.is_null() {
            KillTimer(bar, REC_T_TIMER);
            KillTimer(bar, REC_T_ESC);
        }
        rec_host_exit(ANNO_HWND.load(Ordering::Relaxed) as HWND);
    }
    if let Some(t) = REC_THREAD.lock_ok().take() { let _ = t.join(); }
    unsafe { shot_free(); }
    if let Some(p) = REC_PATH.lock_ok().take() { let _ = std::fs::remove_file(&p); }
    REC_HOVER.store(-1, Ordering::Relaxed);
    unsafe {
        rec_bar_place(ui::layout::record::SET_W);
        if !bar.is_null() {
            InvalidateRect(bar, std::ptr::null_mut(), 0);
            ShowWindow(bar, SW_SHOWNA);
        }
    }
}

/// 录制条的「开始」：藏掉编辑器、起采集线程、切成录制态。
pub(crate) unsafe fn anno_record_begin() {
    if REC_PHASE.load(Ordering::Relaxed) == 1 { return; }
    anno_tx_finish();   // 编辑中的文字先收笔，别让它挡住画面
    // 鼠标捕获先放开：带进录制的话，WS_EX_TRANSPARENT 也拦不住"消息还归本窗"，
    // 桌面上的应用就点不动了
    let host = ANNO_HWND.load(Ordering::Relaxed) as HWND;
    if !host.is_null() && GetCapture() == host { ReleaseCapture(); }
    let (sox, soy) = sel_org();
    let mut ox = ANNO_OX.load(Ordering::Relaxed) + sox;
    let mut oy = ANNO_OY.load(Ordering::Relaxed) + soy;
    let mut w = ANNO_W.load(Ordering::Relaxed);
    let mut h = ANNO_H.load(Ordering::Relaxed);
    // beginVideo 只要求 hasRect（宽高非 0），没有额外的最小尺寸门槛
    if w <= 0 || h <= 0 { return; }
    let mp4 = REC_MP4.load(Ordering::Relaxed);
    let spk = REC_SPK.load(Ordering::Relaxed);
    let mic = REC_MIC.load(Ordering::Relaxed);
    if mp4 {
        // 宽向下取整到 4、高向下取整到 2（至少 4x2）。代价是最多少录右侧 3px、底部 1px。
        // 宽不对齐 4 时 RGB32→NV12 会按对齐后的 stride 读缓冲，有的驱动编码器直接读越界。
        ox = (ox + 3) & !3;
        oy = (oy + 1) & !1;
        w = (w & !3).max(4);
        h = (h & !1).max(2);
    }
    let ext = if mp4 { "mp4" } else { "gif" };
    // 采集期间写数据目录（%APPDATA%\<品牌>\temp_<时间戳>.mp4）
    // CapVideo::startMp4/startGif 的 videoTempPath。不放桌面：那是成品位置，
    // 录制中被强行关机 / 崩溃就会在桌面上留一个看不出是什么的半截文件。
    let dir = match std::env::var("APPDATA") {
        Ok(app) => std::path::Path::new(&app).join(APP_NAME),
        Err(_) => std::env::temp_dir(),
    };
    let _ = std::fs::create_dir_all(&dir);
    strip_stale_temp(&dir);
    // 每一次录制用独立文件名：上一段的编码线程万一收尾慢（Finalize 卡住），它手里的
    // 临时文件还开着，同名再建会被共享冲突挡下来 —— 用户看到的就是"录一次、ESC、再录，
    // 提示录制启动失败"。换名字 + 开录前清旧的，这条路彻底堵死。
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis()).unwrap_or(0);
    let path = dir.join(format!("temp_{}.{}", stamp, ext)).to_string_lossy().into_owned();
    if path.is_empty() { return; }
    REC_STOP.store(false, Ordering::Relaxed);
    REC_SEC.store(0, Ordering::Relaxed);
    REC_FRAMES.store(0, Ordering::Relaxed);
    // 点击即进运行态：先把条从「设置态」切成「运行态」并当场显出来（宽条 / 计时 / 停止 / 丢弃），
    // 再去做整屏压暗。这一步不能排在 rec_host_enter 之后：它要建全屏区域、
    // 改分层样式、同步画一整屏压暗层 —— 那几十到几百毫秒里用户看到的就是「设置态停着不动」。
    // 设置态在按下的一瞬间就消失，运行态先到位，压暗层随后补上。
    let bar_now = REC_BAR.load(Ordering::Relaxed) as HWND;
    REC_PHASE.store(1, Ordering::Relaxed);
    if !bar_now.is_null() {
        rec_bar_place(ui::layout::record::RUN_W);
        InvalidateRect(bar_now, std::ptr::null_mut(), 0);
        ShowWindow(bar_now, SW_SHOWNA);
        UpdateWindow(bar_now);
    }
    // 采集原点/尺寸落到原子量上：开录即定死，录制中不再变
    REC_X.store(ox, Ordering::Relaxed);
    REC_Y.store(oy, Ordering::Relaxed);
    REC_W.store(w, Ordering::Relaxed);
    REC_H.store(h, Ordering::Relaxed);
    *REC_PATH.lock_ok() = Some(path.clone());
    REC_HOVER.store(-1, Ordering::Relaxed);
    let p = path.clone();
    *REC_THREAD.lock_ok() = Some(std::thread::spawn(move || rec_engine(p, ox, oy, w, h, mp4, spk, mic)));
    // 宿主切成录制外观：选区挖空露出真实桌面、选区外 32% 压暗、鼠标穿透。
    // 不画红框 —— 录制范围靠这层压暗的边界表达
    rec_host_enter(ANNO_HWND.load(Ordering::Relaxed) as HWND);
    // 主条 / 副条在**运行态条已经显示之后**再收：反过来的话，副条先消失、录制条还没出来，
    // 中间那几十毫秒桌面上一条都没有，看起来就是"闪一下"。
    // 宿主窗必须留着：选区外的压暗由它画，Ctrl+S / Ctrl+C 这些键也归它收。
    for hw in [ANNO_MAIN_HWND.load(Ordering::Relaxed), ANNO_SUB_HWND.load(Ordering::Relaxed)] {
        let hw = hw as HWND;
        if !hw.is_null() { ShowWindow(hw, SW_HIDE); }
    }
    tip_hide(&TIP_MAIN);
    tip_hide(&TIP_SUB);
    // 录制中气泡一起收：提示文字跟着按钮换了一批，留着旧文字就是错位
    tip_hide(&TIP_REC);
    let bar = REC_BAR.load(Ordering::Relaxed) as HWND;
    if !bar.is_null() {
        rec_bar_place(ui::layout::record::RUN_W);
        SetTimer(bar, REC_T_TIMER, 1000, None);
        SetTimer(bar, REC_T_ESC, REC_ESC_MS, None);
        GetAsyncKeyState(VK_ESCAPE);   // 先吃掉上一次操作留下的"按下过"标记
        REC_ESC.store(false, Ordering::Relaxed);
        // 压暗层是后建的 TOPMOST 窗口，同组里它排在条前面，会把条蒙上一层灰。
        // 条再提一次 TOPMOST 最前 —— 与编辑态主条同一层级（anno_raise_bars 的口径）。
        SetWindowPos(bar, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(bar, SW_SHOWNA);
        UpdateWindow(bar);
    }
}

/// 把临时文件落到设置页的视频目录（默认桌面）。成功返回目标路径；
/// 拷不走（目标盘满 / 目录只读 / 被占用）返回 None，且源文件原样留在临时目录 ——
/// 那是用户刚录的全部内容，删了就彻底没了
pub(crate) fn rec_save_temp(src: &str, mp4: bool) -> Option<String> {
    let dst = unsafe { desktop_media_path(if mp4 { "mp4" } else { "gif" }) };
    if dst.is_empty() { return None; }
    // 先试改名：临时目录和目标通常在同一卷，改名零拷贝；
    // 只有跨卷（桌面被重定向到别的盘）才真去复制一遍
    if std::fs::rename(src, &dst).is_ok() { return Some(dst); }
    if std::fs::copy(src, &dst).is_ok() {
        let _ = std::fs::remove_file(src);
        return Some(dst);
    }
    None
}

/// 收尾（存文件 / 存剪切板 / 丢弃 / ESC 中止 / 到点自动存）：停采集线程 → 落盘 → 收掉整个会话。
/// discard=true 连临时文件一起删；to_clipboard=true 落盘后顺带放进剪切板（CF_HDROP）
pub(crate) unsafe fn rec_finish(discard: bool, to_clipboard: bool) {
    // 录制已经结束（ESC 与按钮几乎同时打进来）就别再走一遍
    if REC_PHASE.swap(0, Ordering::Relaxed) != 1 { return; }
    // 宿主先退回正常编辑外观（撤掉选区挖空 / 压暗 / 鼠标穿透），后面不管走哪条收尾路径
    // 都不会在桌面上留下半透明的压暗层或空白窗
    rec_host_exit(ANNO_HWND.load(Ordering::Relaxed) as HWND);
    let bar = REC_BAR.load(Ordering::Relaxed) as HWND;
    if !bar.is_null() {
        KillTimer(bar, REC_T_TIMER);
        KillTimer(bar, REC_T_ESC);
    }
    REC_STOP.store(true, Ordering::Relaxed);
    // 有界等待 4 秒：收尾卡住也绝不把 UI 冻上
    // 到点先返回，线程与临时文件交给后台收口
    let th = REC_THREAD.lock_ok().take();
    let mut waited = 0u32;
    while th.as_ref().map_or(false, |t| !t.is_finished()) && waited < 4000 {
        std::thread::sleep(Duration::from_millis(50));
        waited += 50;
    }
    let mp4 = REC_MP4.load(Ordering::Relaxed);
    if th.as_ref().map_or(false, |t| !t.is_finished()) {
        // 仍在收尾：条先收掉，线程由后台 join 后补落盘
        let tmp = REC_PATH.lock_ok().take();
        if !discard { show_toast("视频仍在收尾，完成后会自动保存到目标目录"); }
        rec_bar_close();
        std::thread::spawn(move || {
            if let Some(t) = th { let _ = t.join(); }
            unsafe { shot_free(); }
            if let Some(src) = tmp {
                // 帧数要等线程真的收完才准（join 之前读到的还是 0），按它判"空壳"
                let n = REC_FRAMES.load(Ordering::Relaxed);
                if !discard && n > 0 {
                    if let Some(dst) = rec_save_temp(&src, mp4) {
                        if to_clipboard { unsafe { clipboard_put_file(&dst); } }
                    }
                    // 保存失败就保留临时文件：这里没有 UI 线程可以提示，
                    // 但绝不能把用户刚录到的东西删掉（下轮清理也留了 1 小时宽限）
                    return;
                }
                let _ = std::fs::remove_file(&src);
            }
        });
        anno_close();
        return;
    }
    if let Some(t) = th { let _ = t.join(); }
    // 采集线程已经 join 掉了，那张录制尺寸的截屏 DIB（1080p 8MB / 4K 33MB）
    // 立刻还给系统，别让"只录过一次屏"变成常驻占用
    shot_free();
    let frames = REC_FRAMES.load(Ordering::Relaxed);
    let tmp = REC_PATH.lock_ok().take();
    rec_bar_close();
    // 一帧都没录到：编码器只留了个空壳，别拿去存盘
    let mut saved: Option<String> = None;
    let mut copy_failed = false;
    if let Some(src) = tmp {
        if discard || frames == 0 {
            let _ = std::fs::remove_file(&src);
            if !discard { show_toast("没有录到画面，本次录制未保存"); }
        } else {
            match rec_save_temp(&src, mp4) {
                Some(dst) => saved = Some(dst),
                None => copy_failed = true,
            }
        }
    }
    if let Some(p) = saved {
        if to_clipboard { clipboard_put_file(&p); }
        show_toast("已保存");
    } else if copy_failed {
        show_toast("保存失败：保存目录不可写或磁盘已满，请在「设置」里换一个目录");
    }
    anno_close();
}

// CF_HDROP 的文件头（DROPFILES，winapi 0.3.9 没导出这个结构）
#[repr(C)]
pub(crate) struct DropFilesHdr { p_files: u32, pt: POINT, f_nc: i32, f_wide: i32 }

/// 把文件放进剪切板（CF_HDROP）
/// DROPFILES 头 + 双 0 结尾的宽字符路径，粘到资源管理器/聊天窗口里就是那个文件
pub(crate) unsafe fn clipboard_put_file(path: &str) -> bool {
    let mut wpath: Vec<u16> = OsStr::new(path).encode_wide().collect();
    wpath.push(0);
    wpath.push(0);
    let hdr = std::mem::size_of::<DropFilesHdr>();
    let bytes = hdr + wpath.len() * 2;
    let h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if h.is_null() { return false; }
    let p = GlobalLock(h) as *mut u8;
    if p.is_null() { GlobalFree(h); return false; }
    let mut df: DropFilesHdr = std::mem::zeroed();
    df.p_files = hdr as u32;
    df.f_wide = 1;
    std::ptr::copy_nonoverlapping(&df as *const _ as *const u8, p, hdr);
    std::ptr::copy_nonoverlapping(wpath.as_ptr() as *const u8, p.add(hdr), wpath.len() * 2);
    GlobalUnlock(h);
    if OpenClipboard(std::ptr::null_mut()) == 0 { GlobalFree(h); return false; }
    EmptyClipboard();
    let ok = !SetClipboardData(winapi::um::winuser::CF_HDROP, h as *mut winapi::ctypes::c_void).is_null();
    CloseClipboard();
    if !ok { GlobalFree(h); }
    ok
}

pub(crate) unsafe fn rec_bar_close() {
    let b = REC_BAR.load(Ordering::Relaxed) as HWND;
    if !b.is_null() { DestroyWindow(b); REC_BAR.store(0, Ordering::Relaxed); }
    REC_PHASE.store(0, Ordering::Relaxed);
    REC_HOVER.store(-1, Ordering::Relaxed);
}

/// 丢掉正在进行的这一段录制：停采集线程 → 删临时文件 → 状态复位。
/// 只管线程和文件，录制条窗口由调用方接着调 rec_bar_close 收掉。
/// 用于"编辑器被关掉时录制还没收尾"的路径。
pub(crate) fn rec_abort() {
    REC_PHASE.store(0, Ordering::Relaxed);
    REC_STOP.store(true, Ordering::Relaxed);
    // 宿主还活着就先撤掉录制外观；否则压暗层会跟着会话一起留在桌面上
    unsafe { rec_host_exit(ANNO_HWND.load(Ordering::Relaxed) as HWND); }
    // join 与删文件扔后台：SinkWriter 的 Finalize 可能卡上几秒，关窗不能跟着一起冻住
    let th = REC_THREAD.lock_ok().take();
    let tmp = REC_PATH.lock_ok().take();
    std::thread::spawn(move || {
        if let Some(t) = th { let _ = t.join(); }
        unsafe { shot_free(); }
        if let Some(p) = tmp { let _ = std::fs::remove_file(&p); }
    });
}
