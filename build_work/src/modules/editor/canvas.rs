use crate::prelude::*;
use crate::*;

/// 标注窗的背景：整屏原图（截图编辑器里是"原图 + 选区外压暗"的底）。
///
/// 不能直接用 sel_blit_full：它在"缓冲缺失"或"缓冲尺寸与客户区不等"时静默早退，
/// 内存位图就保持未初始化，随后只叠了 32% 黑 —— 选区以外退化成一片纯黑
/// （用户看到的就是"输入文字时四周是黑色的"）。这里把三种情况都兜住：
///   · 尺寸吻合：1:1 贴（老路径，零开销）；
///   · 尺寸不一致：缩放铺满（DPI 切换 / 多屏偏移时缓冲可能差几个像素）；
///   · 缓冲缺失：中性深灰打底 + 选区原图贴回原位 —— 宁可是灰的，也不要纯黑。
pub(crate) unsafe fn anno_paint_backdrop(hdc: winapi::shared::windef::HDC, cw: i32, ch: i32) {
    if cw <= 0 || ch <= 0 { return; }
    let mut bmi: BITMAPINFO = std::mem::zeroed();
    bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    {
        let sg = SEL_BGRA.lock_ok();
        if let Some((iw, ih, buf)) = &*sg {
            let need = (*iw as usize) * (*ih as usize) * 4;
            if *iw > 0 && *ih > 0 && buf.len() >= need {
                bmi.bmiHeader.biWidth = *iw as i32;
                bmi.bmiHeader.biHeight = -(*ih as i32); // top-down，与整屏 BGRA 副本同朝向
                if *iw == cw as u32 && *ih == ch as u32 {
                    SetDIBitsToDevice(hdc, 0, 0, *iw, *ih, 0, 0, 0, *ih,
                        buf.as_ptr() as *const winapi::ctypes::c_void, &bmi, DIB_RGB_COLORS);
                } else {
                    SetStretchBltMode(hdc, HALFTONE as i32);
                    StretchDIBits(hdc, 0, 0, cw, ch, 0, 0, *iw as i32, *ih as i32,
                        buf.as_ptr() as *const winapi::ctypes::c_void, &bmi, DIB_RGB_COLORS, SRCCOPY);
                }
                return;
            }
        }
    }
    // 整屏副本没了（理论上不该发生：anno_open 期间一直占着）：至少不要整块黑
    let mut rc = RECT { left: 0, top: 0, right: cw, bottom: ch };
    let br = CreateSolidBrush(ui::color::cref(ui::color::CANVAS));
    FillRect(hdc, &mut rc, br);
    DeleteObject(br as *mut winapi::ctypes::c_void);
    let (w, h) = (ANNO_W.load(Ordering::Relaxed), ANNO_H.load(Ordering::Relaxed));
    let (ox, oy) = (ANNO_OX.load(Ordering::Relaxed), ANNO_OY.load(Ordering::Relaxed));
    let orig = ANNO_ORIG.lock_ok();
    if w > 0 && h > 0 && orig.len() >= (w as usize) * (h as usize) * 4 {
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        SetDIBitsToDevice(hdc, ox, oy, w as u32, h as u32, 0, 0, 0, h as u32,
            orig.as_ptr() as *const winapi::ctypes::c_void, &bmi, DIB_RGB_COLORS);
    }
}



/// 上一次绘制用的合成缓冲 DC。换了 DC（第一次 / 被闲置回收后重建 / 尺寸变了）
/// 缓冲里的像素就是不可信的，必须整窗重铺一次底图与压暗；否则增量刷新沿用旧像素。
static ANNO_LAST_DC: AtomicIsize = AtomicIsize::new(0);


/// 取窗口 DC 上这次要画的**真实**矩形列表（更新区域可能是不相连的两三块）。
///
/// 落一个序号会同时脏两块：上一条的手柄要擦掉、新标号要画上，两块常隔了大半屏。
/// 只按 `rcPaint` 外接矩形去拷贝/贴屏，等于每次落号都把中间那一整片搬一遍 ——
/// 4K 下这就是每点一下固定十几毫秒，序号越多、点得越分散越明显。
pub(crate) unsafe fn paint_regions(hdc: winapi::shared::windef::HDC, dx0: i32, dy0: i32, dx1: i32, dy1: i32, out: &mut Vec<RECT>) {
    out.clear();
    let bbox = RECT { left: dx0, top: dy0, right: dx1, bottom: dy1 };
    let rgn = CreateRectRgn(0, 0, 0, 0);
    if rgn.is_null() { out.push(bbox); return; }
    if GetClipRgn(hdc, rgn) > 0 {
        let bytes = winapi::um::wingdi::GetRegionData(rgn, 0, std::ptr::null_mut());
        if bytes >= 32 {
            // 用 u32 缓冲保证 4 字节对齐（RECT 的成员都是 4 字节）
            let mut data = vec![0u32; (bytes as usize + 3) / 4];
            if winapi::um::wingdi::GetRegionData(rgn, bytes, data.as_mut_ptr() as *mut _) > 0 {
                let n = (*data.as_ptr().add(2)) as usize; // RGNDATAHEADER.nCount
                let base = data.as_ptr() as *const u8;
                // 区域矩形紧跟在 RGNDATAHEADER（32 字节）之后
                let rects = base.add(32) as *const RECT;
                for i in 0..n {
                    let r = *rects.add(i);
                    if r.right > r.left && r.bottom > r.top { out.push(r); }
                }
            }
        }
    }
    DeleteObject(rgn as *mut winapi::ctypes::c_void);
    if out.is_empty() { out.push(bbox); }
}

/// 只刷新画布上的一个矩形（选区图像坐标）。越界由系统裁掉。
pub(crate) unsafe fn anno_invalidate_rect(hwnd: HWND, r: (i32, i32, i32, i32)) {
    if hwnd.is_null() { return; }
    let (ox, oy) = (ANNO_OX.load(Ordering::Relaxed), ANNO_OY.load(Ordering::Relaxed));
    let (l, t, rr, b) = r;
    let rc = RECT { left: ox + l, top: oy + t, right: ox + rr, bottom: oy + b };
    InvalidateRect(hwnd, &rc, 0);
}

/// 只刷新最后一条标注那一块（悬停亮柄、落序号）。没有最后一条时退回整窗。
pub(crate) unsafe fn anno_invalidate_last(hwnd: HWND) {
    if hwnd.is_null() { return; }
    match anno_last_rect() {
        Some(r) => anno_invalidate_rect(hwnd, r),
        None => { InvalidateRect(hwnd, std::ptr::null_mut(), 0); }
    }
}

/// 把刚失效的那一小块立刻画出来。
///
/// WM_PAINT 是低优先级消息：只有消息队列空了系统才发。快速连点时点击消息
/// 一条接一条排在前面，中间一次都不会刷 —— 表现就是"点的时候编号不出来，
/// 停手后一下全冒出来"。落点是小块重绘，同步刷一次的代价可以忽略。
pub(crate) unsafe fn anno_flush(hwnd: HWND) {
    if !hwnd.is_null() { UpdateWindow(hwnd); }
}

// 抢不得的组合：前一组是所有软件里手指记忆最强的编辑键
// 后一组被系统独占 —— 设上了也注册不成功（Ctrl+Alt+Delete 连消息都到不了应用）

// 把 (mod, vk) 转成可读快捷键名（支持任意自定义）

// 开始录制：显示框变“请按键…”、按钮变“取消”，并先注销三条全局热键 ——
// 否则按到当前绑定的组合会直接触发动作

// 取消录制（Esc / 再点一次 / 切走设置页）：显示框与按钮复原，并把全局热键注册回去

// 三行快捷键各自的控件/存储/注册表键/默认值/标签
// （target: 0=识别(跟随当前页), 2=截图, 3=关压缩软件）

// 设置页：截图 / 识别 / 关压缩软件 三行快捷键，各自预设+自定义录制

// 全局 ESC：按一次就把当前流程收掉（编辑器/框选直接关，主界面最小化到托盘）。
// 返回 true 表示已消费这次按键，消息循环里不再分发。
pub(crate) unsafe fn esc_exit_all(msg_hwnd: HWND) -> bool {
    // ① 截图编辑器：先把正在输入的文字落定，再整窗关闭（编辑态/非编辑态一视同仁）
    let anno = ANNO_HWND.load(Ordering::Relaxed) as HWND;
    if !anno.is_null() && IsWindow(anno) != 0 {
        anno_tx_finish();
        anno_close();
        return true;
    }
    // ② 框选遮罩：取消这次截图，回主界面
    let sel = SEL_HWND.load(Ordering::Relaxed) as HWND;
    if !sel.is_null() && IsWindow(sel) != 0 {
        DestroyWindow(sel);
        SEL_HWND.store(0, Ordering::Relaxed);
        let main = MAIN_HWND.load(Ordering::Relaxed) as HWND;
        if !main.is_null() {
            ShowWindow(main, SW_SHOW);
            SetForegroundWindow(main);
            SetFocus(main);
        }
        return true;
    }
    // ③ 主界面（含二维码输入框等子控件）：ESC = 最小化到托盘，和标题栏最小化 / 点 X 同一个
    // 归宿（SW_HIDE + 托盘常驻），不能 DestroyWindow 退出进程。想退出请走托盘菜单或设置。
    let main = MAIN_HWND.load(Ordering::Relaxed) as HWND;
    if !main.is_null() && IsWindowVisible(main) != 0
        && (msg_hwnd == main || winapi::um::winuser::IsChild(main, msg_hwnd) != 0)
    {
        // 只有"主窗在前台、且没有任何选区流程在跑"才收起来。
        // 否则截图途中被吞掉的一次 ESC 会把主界面顺手藏掉，用户以为程序没了。
        let in_flow = SEL_REENTRANT.load(Ordering::Relaxed);
        if !in_flow && GetForegroundWindow() == main {
            ShowWindow(main, SW_HIDE);
        }
        return true;
    }
    false
}
pub(crate) fn anno_crop_hit(x: i32, y: i32, ox: i32, oy: i32, w: i32, h: i32) -> i32 {
    // 抓边带宽 6 逻辑像素
    let m = ui::dpi::sc(6);
    let l = x >= ox - m && x <= ox + m;
    let r = x >= ox + w - m && x <= ox + w + m;
    let t = y >= oy - m && y <= oy + m;
    let b = y >= oy + h - m && y <= oy + h + m;
    if t && l { 6 } else if t && r { 7 } else if b && l { 8 } else if b && r { 9 }
    else if l { 2 } else if r { 3 } else if t { 4 } else if b { 5 }
    else if x >= ox && x < ox + w && y >= oy && y < oy + h { 1 }
    else { 0 }
}

// 按当前选区(OX/OY/W/H)重新截取底图 ANNO_ORIG，并把这期间选区挪动的位移
// 反向补偿到所有图元上 —— 选区怎么动，已经画好的标注都留在原来的画面内容上
pub(crate) unsafe fn anno_crop_apply() {
    let ox = ANNO_OX.load(Ordering::Relaxed);
    let oy = ANNO_OY.load(Ordering::Relaxed);
    let w = ANNO_W.load(Ordering::Relaxed);
    let h = ANNO_H.load(Ordering::Relaxed);
    // 正在输入的文字先收笔（坐标还是旧的），之后它会跟着图元表一起平移
    anno_tx_finish();
    if w > 0 && h > 0 && ox >= 0 && oy >= 0 {
        // 不能 SEL_BGRA.clone() 再裁剪：拖选区时每个 WM_MOUSEMOVE 都要整屏拷一份
        // （1080p 8MB / 4K 33MB），手一快就是几百 MB/s 的纯拷贝，表现就是"工具条在飘"。
        // 直接借用原图，只拷需要的那一块
        let bgra = SEL_BGRA.lock_ok();
        if let Some((sw, sh, buf)) = bgra.as_ref() {
            let (oxu, oyu) = (ox as u32, oy as u32);
            if oxu + (w as u32) <= *sw && oyu + (h as u32) <= *sh {
                let row_bytes = w as usize * 4;
                let stride = *sw as usize * 4;
                // 新选区的原始像素（未标注）同时成为橡皮擦与整体重放的底图
                let mut orig = ANNO_ORIG.lock_ok();
                orig.resize(row_bytes * h as usize, 0);
                for row in 0..h as usize {
                    let s = (oyu as usize + row) * stride + oxu as usize * 4;
                    let d = row * row_bytes;
                    orig[d..d + row_bytes].copy_from_slice(&buf[s..s + row_bytes]);
                }
            }
        }
    }
    // 底图原点从 (BASE_OX,BASE_OY) 挪到 (ox,oy)，图元坐标以新原点为基准反向平移
    let bx = ANNO_BASE_OX.load(Ordering::Relaxed);
    let by = ANNO_BASE_OY.load(Ordering::Relaxed);
    let dx = ox - bx;
    let dy = oy - by;
    if dx != 0 || dy != 0 {
        {
            let mut sh = ANNO_SHAPES.lock_ok();
            for s in sh.iter_mut() {
                s.x0 -= dx; s.y0 -= dy; s.x1 -= dx; s.y1 -= dy;
                for p in s.pts.iter_mut() { p.0 -= dx; p.1 -= dy; }
            }
        }
        if let Some(l) = ANNO_LAST.lock_ok().as_mut() {
            l.x0 -= dx; l.y0 -= dy; l.x1 -= dx; l.y1 -= dy;
            for p in l.pts.iter_mut() { p.0 -= dx; p.1 -= dy; }
        }
        // 撤销/重做栈里存的也是图元表，同样跟着平移，否则 Ctrl+Z 会跳回旧坐标
        for stack in [&ANNO_UNDO, &ANNO_REDO] {
            for snap in stack.lock_ok().iter_mut() {
                for s in snap.shapes.iter_mut() {
                    s.x0 -= dx; s.y0 -= dy; s.x1 -= dx; s.y1 -= dy;
                    for p in s.pts.iter_mut() { p.0 -= dx; p.1 -= dy; }
                }
            }
        }
    }
    ANNO_BASE_OX.store(ox, Ordering::Relaxed);
    ANNO_BASE_OY.store(oy, Ordering::Relaxed);
    anno_rebuild();
    let ihw = ANNO_HWND.load(Ordering::Relaxed) as HWND;
    if !ihw.is_null() { InvalidateRect(ihw, std::ptr::null_mut(), 0); }
    anno_layout();
}
// ==================== 一笔的重绘范围 ====================
//
// 拖动中每帧都要"先退回落笔前、再画这一帧"。原来的做法是把整张快照拷回工作位图、
// 再整窗失效 —— 1080p 一次整窗合成 ≈ 20ms，手绘（画笔/马赛克/模糊/橡皮）自然跟不上
// 鼠标，表现就是"画一笔要缓半拍"。其实这一笔只可能落在
// 「上一帧的笔迹 ∪ 这一帧的笔迹」这一块里，于是只恢复这一块、只失效这一块。
pub(crate) static ANNO_PREV_BOX: Mutex<(i32, i32, i32, i32)> = Mutex::new((0, 0, 0, 0));

#[inline]
pub(crate) fn anno_box_empty(b: (i32, i32, i32, i32)) -> bool {
    b.2 <= b.0 || b.3 <= b.1
}

pub(crate) fn anno_box_union(a: (i32, i32, i32, i32), b: (i32, i32, i32, i32)) -> (i32, i32, i32, i32) {
    if anno_box_empty(a) { return b; }
    if anno_box_empty(b) { return a; }
    (a.0.min(b.0), a.1.min(b.1), a.2.max(b.2), a.3.max(b.3))
}

/// 这一帧这一笔覆盖的矩形（图像坐标，左闭右开）：手绘取轨迹 bbox、选框取矩形，
/// 外扩笔宽后裁到图内。只用于"恢复/失效哪一块"，宁可多留几像素也不能漏（漏了留残影）
pub(crate) fn anno_stroke_box(tool: i32, sx: i32, sy: i32, ex: i32, ey: i32, w: i32, h: i32) -> (i32, i32, i32, i32) {
    if w <= 0 || h <= 0 { return (0, 0, 0, 0); }
    let width = ANNO_OPT_SLIDER.load(Ordering::Relaxed).max(1);
    // 箭头头部比线宽长得多，单留 width/2 会在旧箭头位置留残影
    let m = if tool == 2 && !ANNO_OPT_B.load(Ordering::Relaxed) { width * 6 + 10 } else { width / 2 + 4 };
    let (mut x0, mut y0, mut x1, mut y1) = (sx, sy, ex, ey);
    let freehand = tool == 4 || ((tool == 6 || tool == 7) && !anno_effect_rect_mode(tool));
    if freehand {
        for &(px, py) in ANNO_PTS.lock_ok().iter() {
            x0 = x0.min(px); y0 = y0.min(py);
            x1 = x1.max(px); y1 = y1.max(py);
        }
    }
    ((x0 - m).max(0), (y0 - m).max(0), (x1 + m + 1).min(w), (y1 + m + 1).min(h))
}

/// 只把 pre 快照里 b 这一块按行拷回 img（整图拷贝在 4K 下是 33MB/帧）
pub(crate) fn anno_img_restore(img: &mut [u8], pre: &[u8], w: i32, h: i32, b: (i32, i32, i32, i32)) {
    if anno_box_empty(b) || w <= 0 || h <= 0 { return; }
    let need = (w as usize) * (h as usize) * 4;
    if img.len() != need || pre.len() != need { return; }
    if b.0 < 0 || b.1 < 0 || b.2 > w || b.3 > h { return; }
    let (x0, y0) = (b.0 as usize, b.1 as usize);
    let (x1, y1) = (b.2 as usize, b.3 as usize);
    let row = w as usize * 4;
    for y in y0..y1 {
        let o = y * row;
        img[o + x0 * 4..o + x1 * 4].copy_from_slice(&pre[o + x0 * 4..o + x1 * 4]);
    }
}

// ---- 原图窗过程：只显示截图（1:1）+ 1px 红色细边框，处理画布上的绘制/拖动 ----
pub(crate) extern "system" fn anno_img_proc(hwnd: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    unsafe {
        // 录制中宿主整窗让鼠标穿过去
        // 选区不拖不缩、图元不可编辑，别让这些消息再进编辑分支
        if REC_PHASE.load(Ordering::Relaxed) == 1 {
            match msg {
                WM_LBUTTONDOWN | WM_LBUTTONUP | WM_MOUSEMOVE | WM_MOUSEWHEEL
                | WM_RBUTTONUP | winapi::um::winuser::WM_RBUTTONDOWN | WM_LBUTTONDBLCLK => return 0,
                _ => {}
            }
        }
        match msg {
            WM_PAINT => {
                let mut ps: PAINTSTRUCT = std::mem::zeroed();
                let hdc = BeginPaint(hwnd, &mut ps);
                let mut rc: RECT = std::mem::zeroed();
                GetClientRect(hwnd, &mut rc);
                let cw = rc.right - rc.left; let ch = rc.bottom - rc.top;
                // 录制中宿主只剩"选区外压暗"这一层（Video 分支）
                // Clear(0) + cutMask->paint(ctx, true, false)）。选区那块已经从窗口区域里
                // 挖掉了（见 rec_host_enter）。
                // 这里用 32% 黑的真 alpha 混合，而不是"铺纯黑 + 整窗 alpha"：
                // 窗口不透明度必须恒定，进入录制时只要改窗口 alpha，DWM 就会把旧画面
                // 按新 alpha 呈现一帧 —— 那一下就是切录制时的闪屏
                // 画的、窗口本身不透明，所以不闪；GDI 这边用 AlphaBlend 等价实现。
                if REC_PHASE.load(Ordering::Relaxed) == 1 {
                    // 录制态：选区那块已经从窗口区域里挖掉（见 rec_host_enter），外面只剩
                    // "选区外 32% 压暗"这一层（onPaint 的 Video 分支：Clear(0) + 只画压暗层）
                    //
                    // 这里必须先铺**未压暗的整屏原图**再压一层：窗口 DC 里本来留着的是截图态
                    // 的画面，而截图态在选区外已经压过 32% 了，直接在那上面再 AlphaBlend 一次
                    // 就是 0.68 × 0.68 ≈ 0.46 —— 点录制的瞬间整屏明显跳暗，就是这一下的问题。
                    anno_paint_backdrop(hdc, cw, ch);
                    crate::blend_black(hdc, 0, 0, cw, ch, ui::alpha::SCRIM);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                let w = ANNO_W.load(Ordering::Relaxed);
                let h = ANNO_H.load(Ordering::Relaxed);
                let ox = ANNO_OX.load(Ordering::Relaxed);
                let oy = ANNO_OY.load(Ordering::Relaxed);
                // 只重绘失效区：常驻合成缓冲里其余像素还是上一帧的正确画面。
                // 悬停亮柄、落序号都只失效最后一条那一小块，不必整窗重合成
                // （4K 下一整窗 = 整屏原图 + 选区图 + 贴屏三次大拷贝）。
                let (dx0, dy0, dx1, dy1) = (ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
                if dx1 <= dx0 || dy1 <= dy0 { EndPaint(hwnd, &ps); return 0; }
                // 这次要重绘的真实矩形（可能是几块不相连的小块）
                let mut prs: Vec<RECT> = Vec::new();
                paint_regions(hdc, dx0, dy0, dx1, dy1, &mut prs);
                // 合成到常驻缓冲再一次性贴屏（尺寸不变就不重建，见 anno_buf_dc）
                let memdc = anno_buf_dc(cw, ch).unwrap_or(hdc);
                let mut saved = 0;
                if memdc != hdc {
                    saved = SaveDC(memdc);
                    // 精确到"更新区域"，而不是 rcPaint 的外接矩形：
                    // 落一个标号会同时脏两块（上一条的手柄要擦掉、新标号要画上），
                    // 两块常常隔了大半屏 —— 按外接矩形重合成，等于每次落点都把中间
                    // 整片重画一遍，4K 下就是肉眼可见的"一卡"。
                    // BeginPaint 返回的窗口 DC 上，裁剪区正是这次要画的更新区域（客户区坐标），
                    // 直接复制到合成缓冲上即可；取不到再退回外接矩形。
                    let rgn = CreateRectRgn(0, 0, 0, 0);
                    let mut clipped = false;
                    if !rgn.is_null() {
                        if GetClipRgn(hdc, rgn) > 0 && saved != 0 {
                            SelectClipRgn(memdc, rgn);
                            clipped = true;
                        }
                        DeleteObject(rgn as *mut winapi::ctypes::c_void);
                    }
                    if saved != 0 && !clipped {
                        IntersectClipRect(memdc, dx0, dy0, dx1, dy1);
                    }
                }
                // 失效块整个落在选框内时，底图/压暗层会被后面的选区图整块盖住 ——
                // 这两步可以直接跳过（4K 下省一次整屏 DIB 转换 + 四次压暗混合）
                //
                // 更彻底的一条：合成缓冲里**本来就有上一帧正确的底图与压暗**，
                // 只要这次不是"整窗失效"、缓冲也没被重建，就完全不用重铺。
                // 旧写法按 rcPaint 的外接矩形判断是否在选区内：落一个靠边的序号，
                // 相邻两次失效一块在选区内、一块外扩到手柄上，外接矩形就跨出去，
                // 于是每次都整屏重铺一遍 —— 这就是"每点一下卡一下"的固定开销。
                let full = memdc == hdc
                    || memdc as isize != ANNO_LAST_DC.load(Ordering::Relaxed)
                    || (dx0 <= 0 && dy0 <= 0 && dx1 >= cw && dy1 >= ch);
                ANNO_LAST_DC.store(memdc as isize, Ordering::Relaxed);
                if full {
                    // 1) 整屏原图 + 选区外 32% 黑压暗（与截图遮罩共用同一套绘制）。
                    // 走 anno_paint_backdrop：缓冲尺寸差一点 / 缺失时也不会留下纯黑底
                    anno_paint_backdrop(memdc, cw, ch);
                    sel_dim_outside(memdc, cw, ch, ox, oy, ox + w, oy + h);
                }
                // 2) 选区内高亮（画当前编辑图，含标注）
                {
                    let img = ANNO_IMG.lock_ok();
                    if !img.is_empty() && w > 0 && h > 0
                        && img.len() >= (w as usize) * (h as usize) * 4
                    {
                        // 只把失效的那一块从 CPU 图按行拷进合成缓冲：两者都是 top-down
                        // BGRA、4 字节/像素，纯 memcpy。旧写法 SetDIBitsToDevice 每次都要
                        // 把整张选区图交给 GDI 转换一遍（4K 下 33MB），落点越密越顿。
                        let dst_bits = if memdc != hdc { anno_buf_bits(cw, ch) } else { std::ptr::null_mut() };
                        if !dst_bits.is_null() {
                            let src_stride = (w as usize) * 4;
                            let dst_stride = (cw as usize) * 4;
                            for r in &prs {
                                let mut sx = (r.left - ox).clamp(0, w);
                                let mut sy = (r.top - oy).clamp(0, h);
                                let mut ex = (r.right - ox).clamp(0, w);
                                let mut ey = (r.bottom - oy).clamp(0, h);
                                // 目标也要落在客户区内（选区可能有一部分在窗口外）
                                if ox < 0 { sx = sx.max(-ox); }
                                if oy < 0 { sy = sy.max(-oy); }
                                if ox + w > cw { ex = ex.min(cw - ox); }
                                if oy + h > ch { ey = ey.min(ch - oy); }
                                if ex <= sx || ey <= sy { continue; }
                                for row in 0..(ey - sy) as usize {
                                    let s = (sy as usize + row) * src_stride + sx as usize * 4;
                                    let d = (oy + sy) as usize * dst_stride + ox as usize * 4
                                        + row * dst_stride + sx as usize * 4;
                                    std::ptr::copy_nonoverlapping(
                                        img.as_ptr().add(s), dst_bits.add(d), (ex - sx) as usize * 4);
                                }
                            }
                        } else {
                            // 缓冲不可用（直接画到窗口 DC）：退回整图贴，靠裁剪区裁
                            let mut bmi: BITMAPINFO = std::mem::zeroed();
                            bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
                            bmi.bmiHeader.biWidth = w;
                            bmi.bmiHeader.biHeight = -h;
                            bmi.bmiHeader.biPlanes = 1;
                            bmi.bmiHeader.biBitCount = 32;
                            bmi.bmiHeader.biCompression = BI_RGB;
                            SetDIBitsToDevice(memdc, ox, oy, w as u32, h as u32,
                                0, 0, 0, h as u32, img.as_ptr() as *const winapi::ctypes::c_void,
                                &bmi, DIB_RGB_COLORS);
                        }
                    }
                    drop(img);
                }
                // 2.2) 文字图元（含正在编辑的那条：正文 + 光标 + 虚线框）。
                // 它们不在底图里 —— 所以才能点回来改；导出时才现烘一份
                anno_tx_paint(memdc, ox, oy);
                // 2.5) 标注手柄：光标压在最后一条标注上时才亮
                // 编辑中一律不挂手柄
                // 那阵子框里的点击全归文本框，角上冒出四角会让人以为点进去是缩放
                let editing = ANNO_TXE.lock_ok().is_some();
                if !editing && ANNO_LAST_HOT.load(Ordering::Relaxed) && !ANNO_DRAG.load(Ordering::Relaxed) {
                    if let Some(l) = ANNO_LAST.lock_ok().clone() {
                        if l.tool != 8 { anno_draw_draggers(memdc, ox, oy, l); }
                        else {
                            // 已提交的文字走非编辑态那一套：
                            // 虚线框 + 四角柄 + 顶上旋转柄，位置/命中共用 anno_tx_hnd_pts
                            let t = if l.idx < 0 { None } else {
                                let sh = ANNO_SHAPES.lock_ok();
                                sh.get(l.idx as usize)
                                    .filter(|s| s.tool == 8).map(anno_shape_text)
                            };
                            if let Some(t) = t {
                                let mut v = anno_txt_edit_view(&t);
                                v.rot_hot = ANNO_TXT_ROT_HOT.load(Ordering::Relaxed);
                                let (fl, ft, fr, fb) = anno_tx_frame_rect(&v);
                                anno_tx_dash(memdc, ox + fl, oy + ft, ox + fr, oy + fb, v.ang, v.rgb);
                                anno_tx_handles(memdc, ox, oy, &v);
                            }
                        }
                    }
                }
                // 3) 蓝色选框（2px）+ 左上角坐标标签
                if w > 0 && h > 0 {
                    // 四条实心边带，外沿严格贴住压暗边界：左 [ox, ox+2)、右 [ox+w-2, ox+w)、
                    // 上/下同理。不用 GDI Rectangle —— 它的右边/下边是"往里缩"画的
                    // （2px 笔只覆盖到 x2-3..x2-2），边框外会留一条没压暗的亮线，
                    // 就是用户看到的"选框周边漏光"
                    let blue = ui::color::cref(ui::color::SELECTION);
                    let br = CreateSolidBrush(blue);
                    let th = ui::metric::SELECT_STROKE.min(w / 2).min(h / 2).max(1);
                    for (x1, y1, x2, y2) in [
                        (ox, oy, ox + w, oy + th),              // 上
                        (ox, oy + h - th, ox + w, oy + h),      // 下
                        (ox, oy, ox + th, oy + h),              // 左
                        (ox + w - th, oy, ox + w, oy + h),      // 右
                    ] {
                        let mut r = RECT { left: x1, top: y1, right: x2, bottom: y2 };
                        FillRect(memdc, &mut r, br);
                    }
                    DeleteObject(br as *mut winapi::ctypes::c_void);
                    draw_size_label(memdc, ox, oy, w, h);
                }
                if memdc != hdc {
                    if saved != 0 { RestoreDC(memdc, saved); }
                    // 逐块贴屏：两块相隔很远时，贴外接矩形等于把中间整片搬到屏幕上
                    for r in &prs {
                        BitBlt(hdc, r.left, r.top, r.right - r.left, r.bottom - r.top,
                            memdc, r.left, r.top, SRCCOPY);
                    }
                }
                EndPaint(hwnd, &ps);
                0
            }
            WM_ERASEBKGND => 1,
            // 点击画布 = 标注窗被激活，系统会把它提到 TOPMOST 组的最前面，
            // 于是整条工具条被那层全屏遮罩压住（看起来"工具条消失了"）。
            // 用 raiseTools 纠正，这里同样处理
            WM_SETFOCUS => {
                anno_raise_bars();
                DefWindowProcW(hwnd, msg, wp, lp)
            }
            // 右键：退回"刚按热键那一帧"的框选界面重新选。
            // 顺序很重要：先把编辑器整套收掉（本窗口也在里面），否则新遮罩抓屏会把
            // 编辑器/工具条一起截进去；收完再起一块新遮罩。
            // 录制中不认右键 —— 那时用户要的是停录，不是重选。
            WM_RBUTTONUP => {
                if REC_PHASE.load(Ordering::Relaxed) == 1 { return 0; }
                anno_close();
                start_region_select(3);
                0
            }
            // 选框内双击 = 完成并复制（等同工具条「复制」：出图到剪贴板并收起编辑器）。
            // 只在「没选绘制工具 + 落在纯选区内部」时生效 —— 这时光标正是四角箭头。
            // 边角是改大小、图元双击是编辑文字，都不该被这个手势抢走。
            WM_LBUTTONDBLCLK => {
                // 选了绘制工具时，快速连点两下就是两次落点。系统会把第二下派发成
                // DBLCLK，原来直接丢给 DefWindowProc —— 序号被吞掉一个，
                // 手感就是"点了没反应、跟不上"。这里按普通按下再走一遍。
                if ANNO_TOOL.load(Ordering::Relaxed) >= 0 {
                    return anno_img_proc(hwnd, WM_LBUTTONDOWN, wp, lp);
                }
                let x = (lp & 0xFFFF) as i16 as i32;
                let y = ((lp >> 16) & 0xFFFF) as i16 as i32;
                let ox = ANNO_OX.load(Ordering::Relaxed);
                let oy = ANNO_OY.load(Ordering::Relaxed);
                let w = ANNO_W.load(Ordering::Relaxed);
                let h = ANNO_H.load(Ordering::Relaxed);
                if ANNO_TOOL.load(Ordering::Relaxed) < 0
                    && anno_crop_hit(x, y, ox, oy, w, h) == 1
                {
                    // 双击的前两下按下已经进了「拖动选区」模式，先复位再出图
                    ANNO_CROP_MODE.store(0, Ordering::Relaxed);
                    if GetCapture() == hwnd { ReleaseCapture(); }
                    anno_copy();
                    return 0;
                }
                DefWindowProcW(hwnd, msg, wp, lp)
            }

            WM_LBUTTONDOWN => {
                SetCapture(hwnd);
                SetFocus(hwnd);
                let x = (lp & 0xFFFF) as i32; let y = ((lp >> 16) & 0xFFFF) as i32;
                let tool = ANNO_TOOL.load(Ordering::Relaxed);
                if tool < 0 {
                    // 无工具：进入选区调整（边/角改大小、内部移位置）
                    let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                    let w = ANNO_W.load(Ordering::Relaxed); let h = ANNO_H.load(Ordering::Relaxed);
                    let mode = anno_crop_hit(x, y, ox, oy, w, h);
                    if mode != 0 {
                        ANNO_CROP_MODE.store(mode, Ordering::Relaxed);
                        ANNO_CROP_OX0.store(ox, Ordering::Relaxed);
                        ANNO_CROP_OY0.store(oy, Ordering::Relaxed);
                        ANNO_CROP_W0.store(w, Ordering::Relaxed);
                        ANNO_CROP_H0.store(h, Ordering::Relaxed);
                        ANNO_CROP_MX0.store(x, Ordering::Relaxed);
                        ANNO_CROP_MY0.store(y, Ordering::Relaxed);
                    }
                    return 0;
                }
                let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                let w = ANNO_W.load(Ordering::Relaxed); let h = ANNO_H.load(Ordering::Relaxed);
                let ix = x - ox; let iy = y - oy; // 映射到选区坐标
                if ix < 0 || iy < 0 || ix >= w || iy >= h {
                    return 0; // 点在亮区外（压暗处）忽略
                }
                // 0) 正在输入时，压在柄上就先办柄：3 个角缩放、右上的 × 关闭、顶上旋转。
                //    这几档必须排在 anno_tx_finish 前面 —— 用户要的正是"输入中就能拉能转"
                {
                    let hit = {
                        let g = ANNO_TXE.lock_ok();
                        match g.as_ref() {
                            Some(e) if e.focused && e.mode == 0 => anno_tx_hnd_hit(e, ix, iy, ui::dpi::sc(ui::metric::TX_HANDLE_HIT)),
                            _ => TxHnd::None,
                        }
                    };
                    match hit {
                        TxHnd::Close => {
                            anno_tx_close_box();
                            InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                            return 0;
                        }
                        TxHnd::Rotate | TxHnd::Scale(_) => {
                            let mut g = ANNO_TXE.lock_ok();
                            if let Some(e) = g.as_mut() {
                                let (cx, cy) = anno_tx_center_of(e);
                                if hit == TxHnd::Rotate {
                                    e.ang0 = e.ang;
                                    e.rot0 = ((iy as f32) - cy).atan2((ix as f32) - cx);
                                    e.mode = 3;
                                } else {
                                    let d = (((ix as f32) - cx).powi(2) + ((iy as f32) - cy).powi(2)).sqrt();
                                    e.mode = 2;
                                    e.d0 = if d < 1.0 { 1.0 } else { d };
                                    e.fs0 = e.fs;
                                    e.pad0 = e.pad;
                                    e.box0 = (e.x, e.y);
                                }
                                // 输入态不打断（拉角/旋转都不收笔，拖完还能接着打字），
                                // 所以 focused 保持真、光标闪烁定时器也不停
                                ANNO_CARET_ON.store(true, Ordering::Relaxed);
                            }
                            InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                            return 0;
                        }
                        TxHnd::None => {
                            // 没压在手柄上、但压在虚线框那一圈：按住整框拖走。
                            // 之前这里直接落空，于是"光标是四向箭头、按下去却拖不动"
                            let mut g = ANNO_TXE.lock_ok();
                            if let Some(e) = g.as_mut() {
                                if anno_tx_frame_hover(e, ix, iy) {
                                    e.mode = 1;
                                    e.m0 = (ix, iy);
                                    e.box0 = (e.x, e.y);
                                    // 编辑态不打断：拖完手一松还能接着打字
                                    ANNO_CARET_ON.store(true, Ordering::Relaxed);
                                    return 0;
                                }
                                // 点回正文：落光标；按住 Shift 是扩选，拖起来就是拖选
                                // （TextBox::onDown → moveCaret(hitTextIndex, isShift)）
                                if e.focused && e.mode == 0 {
                                    let (px, py) = anno_tx_inv(e, ix, iy);
                                    let (bw, bh) = anno_tx_size(e);
                                    if px >= e.x as f32 && px <= (e.x + bw) as f32
                                        && py >= e.y as f32 && py <= (e.y + bh) as f32 {
                                        let ci = anno_tx_caret_at(e, 0, ix, iy);
                                        let shift = ((GetKeyState(VK_SHIFT as i32) as i32) & 0x8000) != 0;
                                        if !shift { e.sel = ci; }
                                        e.caret = ci;
                                        e.drag = true;
                                        ANNO_CARET_ON.store(true, Ordering::Relaxed);
                                        InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                                        return 0;
                                    }
                                }
                            }
                        }
                    }
                }
                //    落在正文里 = 接着编辑、落在角上 = 等比缩放、落在框边 = 整条拖走。
                //    这一档必须排在"新建"前面，否则永远点不回来改
                anno_tx_finish();
                // 0.1) 已提交文字上的手柄：右上 × 删、四角等比缩放
                //      顶上旋转。手柄就压在框角/框顶，必须先于"点回正文"认，否则永远抓不到
                if let Some((i, h)) = anno_txt_hnd_hit(ix, iy) {
                    match h {
                        TxHnd::Close => {
                            anno_txt_drop(i);
                            InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                            return 0;
                        }
                        TxHnd::Rotate => {
                            anno_tx_begin(i as i32, ix, iy, -2);
                            // 先把它从位图里摘掉：手势期间由编辑态现画，旧像素留着会成双影
                            anno_rebuild();
                            SetFocus(hwnd);
                            InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                            return 0;
                        }
                        TxHnd::Scale(k) => {
                            anno_tx_begin(i as i32, ix, iy, k);
                            anno_rebuild();
                            SetFocus(hwnd);
                            InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                            return 0;
                        }
                        TxHnd::None => {}
                    }
                }
                if let Some((i, k)) = anno_tx_corner_hit(ix, iy) {
                    anno_tx_begin(i as i32, ix, iy, k);
                    anno_rebuild();
                    SetFocus(hwnd);
                    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                    return 0;
                }
                if let Some(i) = anno_tx_hit(ix, iy) {
                    anno_tx_begin(i as i32, ix, iy, -1);
                    anno_rebuild();
                    SetFocus(hwnd);
                    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                    return 0;
                }
                // 0.5) 形状手柄：拖它可以改这一条的几何。
                //    必须排在"新建一笔"前面 —— 手柄就压在框角上，先建新的就永远拖不到它
                if ANNO_LAST_HOT.load(Ordering::Relaxed) {
                    let lastl = ANNO_LAST.lock_ok().clone();
                    if let Some(l) = lastl {
                        if l.tool != 8 {
                            let pts = anno_dragger_pts(&l);
                            let mut hitk = usize::MAX;
                            // 其余图元 ±6 的宽松口径（见下一条注释）
                            let tol = if l.tool == 3 { ui::dpi::sc(ui::layout::anno::DRAGGER_HALF) } else { 6 };
                            for (k, p) in pts.iter().enumerate() {
                                // ±6：小方块边长 6，光标落在方块边缘上就该抓住它。
                                // 不能按 ±5 算：鼠标偏一格就抓空、于是当成"新画一笔"
                                if (ix - p.0).abs() <= tol && (iy - p.1).abs() <= tol { hitk = k; break; }
                            }
                            // 没抓手柄但点在框里/框边：整条拖动。
                            // 空心序号例外：盘内是空的，按下该起一个新序号
                            let inside = hitk == usize::MAX && anno_last_hit(&l, ix, iy)
                                && !(l.tool == 3 && !l.fill);
                            if hitk != usize::MAX || inside {
                                // 先备一份「不含这一条」的底图：拖动期间每帧只烘它自己
                                anno_base_prepare();
                                *ANNO_SDRAG.lock_ok() = Some(ShapeDrag {
                                    l0: l.clone(), l, handle: hitk, m0: (ix, iy),
                                    before: anno_snap(),
                                });
                                SetCapture(hwnd);
                                return 0;
                            }
                        }
                    }
                }
                if tool == 3 {
                    anno_push_undo();
                    // getNextVal：现存序号的最大值 + 1（不是简单自增）
                    // 删掉/撤销过最后一条后再画要能补回同一个号）
                    let n = {
                        let sh = ANNO_SHAPES.lock_ok();
                        sh.iter().filter(|s| s.tool == 3).map(|s| s.val).max().unwrap_or(0) + 1
                    };
                    ANNO_NUM.store(n, Ordering::Relaxed);
                    // 滑块是逻辑像素
                    let radius = ui::dpi::sc(ANNO_OPT_SLIDER.load(Ordering::Relaxed).clamp(6, 86));
                    let (cr, cg, cb) = ANNO_COLORS[ANNO_OPT_COLOR.load(Ordering::Relaxed).clamp(0, 8) as usize];
                    // 圆/方两块各记各的实心（circleSolid / squareSolid），当前形状看 B
                    let square = ANNO_OPT_B.load(Ordering::Relaxed);
                    let fill = if square {
                        ANNO_OPT_C.load(Ordering::Relaxed)
                    } else {
                        ANNO_OPT_A.load(Ordering::Relaxed)
                    };
                    // 落点前先把上一条的重绘范围记下来：若它当时亮着手柄，
                    // 新手柄出现后旧的那圈也要擦掉（只刷这一块，不整窗重绘）
                    let old_rect = anno_last_rect();
                    // 序号也是"最后一条"
                    *ANNO_LAST.lock_ok() = Some(AnnoLast {
                        tool: 3,
                        x0: ix - radius, y0: iy - radius, x1: ix + radius, y1: iy + radius,
                        r: cr, g: cg, b: cb,
                        width: radius,
                        fill,
                        ell: square,
                        c2: ANNO_OPT_COLOR2.load(Ordering::Relaxed),
                        smart: false,
                        val: n,
                        ang: 270.0,
                        pts: Vec::new(),
                        idx: -1,
                    });
                    // 入表后只补画这一条：标号落点即定型，画面已经是"前面全部图元"，
                    // 整表重放会让第 N 个标号的耗时随 N 线性增长（连点到十几个就卡）
                    anno_shape_push_last();
                    if !anno_bake_last() { anno_rebuild(); }
                    ANNO_LAST_HOT.store(true, Ordering::Relaxed);
                    if let Some(r) = old_rect { anno_invalidate_rect(hwnd, r); }
                    anno_invalidate_last(hwnd);
                    // 连点时 WM_PAINT 会被点击消息一直压在队尾：同步刷掉这一块，
                    // 否则就是"点的时候不出号，停手后一下全冒出来"
                    anno_flush(hwnd);
                } else if tool == 5 {
                    // 空白处起笔：新建一个空文本框并进入编辑态
                    anno_tx_begin(-1, ix, iy, -1);
                    SetFocus(hwnd);
                    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                } else if tool >= 0 {
                    ANNO_DRAGGED.store(false, Ordering::Relaxed);
                    ANNO_DS.0.store(ix, Ordering::Relaxed); ANNO_DS.1.store(iy, Ordering::Relaxed);
                    ANNO_DE.0.store(ix, Ordering::Relaxed); ANNO_DE.1.store(iy, Ordering::Relaxed);
                    {
                        let mut ps = ANNO_PTS.lock_ok();
                        ps.clear();
                        ps.push((ix, iy));
                    }
                    {
                        // 落笔前快照：复用已有缓冲，不再每次重新分配一张整图。
                        // 空闲时那张整图正躺在前缀底图里（底图对新的一笔已经作废），
                        // 直接借过来用，两者始终只占一份内存。
                        let need = ANNO_IMG.lock_ok().len();
                        ANNO_BASE_N.store(-1, Ordering::Relaxed);
                        let mut pre = ANNO_PRE.lock_ok();
                        if pre.len() != need {
                            let mut base = ANNO_BASE.lock_ok();
                            if base.len() == need { *pre = std::mem::take(&mut *base); }
                            pre.resize(need, 0);
                        }
                        let img = ANNO_IMG.lock_ok();
                        pre.copy_from_slice(&img);
                    }
                    *ANNO_PREV_BOX.lock_ok() = (0, 0, 0, 0);
                    ANNO_DRAG.store(true, Ordering::Relaxed);
                }
                0
            }
            WM_MOUSEWHEEL => {
                // 序号：光标压在徽章上滚轮 ±1 改半径。
                let mut pt = POINT { x: (lp & 0xFFFF) as i16 as i32, y: ((lp >> 16) & 0xFFFF) as i16 as i32 };
                ScreenToClient(hwnd, &mut pt);
                let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                let (mx, my) = (pt.x - ox, pt.y - oy);
                let cur = ANNO_LAST.lock_ok().clone();
                if let Some(l) = cur {
                    if l.tool == 3 && anno_last_hit(&l, mx, my) {
                        let delta = ((wp >> 16) & 0xFFFF) as u16 as i16 as i32;
                        // 滑块值域是逻辑像素，步进 1 逻辑像素 → 半径 ±1*dpi
                        let step = (ui::dpi::sc(1)).max(1);
                        let nr = (l.width + if delta > 0 { step } else { -step })
                            .clamp(ui::dpi::sc(6), ui::dpi::sc(86));
                        if nr != l.width {
                            let old_rect = anno_last_rect();
                            ANNO_OPT_SLIDER.store(ui::dpi::unsc(nr), Ordering::Relaxed);
                            let cx = (l.x0 + l.x1) / 2; let cy = (l.y0 + l.y1) / 2;
                            {
                                let mut g = ANNO_LAST.lock_ok();
                                if let Some(s) = g.as_mut() {
                                    s.x0 = cx - nr; s.y0 = cy - nr;
                                    s.x1 = cx + nr; s.y1 = cy + nr;
                                    s.width = nr;
                                }
                            }
                            {
                                let cur = ANNO_LAST.lock_ok().clone();
                                if let Some(s) = cur { anno_shape_redraw(&s); }
                            }
                            ANNO_LAST_HOT.store(true, Ordering::Relaxed);
                            let sub = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
                            if !sub.is_null() { InvalidateRect(sub, std::ptr::null_mut(), 0); }
                            if let Some(r) = old_rect { anno_invalidate_rect(hwnd, r); }
                            anno_invalidate_last(hwnd);
                        }
                    }
                }
                0
            }
            WM_MOUSEMOVE => {
                let x = (lp & 0xFFFF) as i32; let y = ((lp >> 16) & 0xFFFF) as i32;
                // 选区调整进行中
                let cm = ANNO_CROP_MODE.load(Ordering::Relaxed);
                if cm != 0 {
                    let ox0 = ANNO_CROP_OX0.load(Ordering::Relaxed);
                    let oy0 = ANNO_CROP_OY0.load(Ordering::Relaxed);
                    let w0 = ANNO_CROP_W0.load(Ordering::Relaxed);
                    let h0 = ANNO_CROP_H0.load(Ordering::Relaxed);
                    let mx0 = ANNO_CROP_MX0.load(Ordering::Relaxed);
                    let my0 = ANNO_CROP_MY0.load(Ordering::Relaxed);
                    // 边界要用标注窗口自己的客户区：多显示器下主屏尺寸与实际
                    // 所在屏幕不一致，用 SM_CXSCREEN 会在副屏上提前夹住或允许拖出窗外
                    let mut cr: RECT = std::mem::zeroed();
                    GetClientRect(hwnd, &mut cr);
                    let sw = cr.right.max(1);
                    let sh = cr.bottom.max(1);
                    let dx = x - mx0; let dy = y - my0;
                    let (mut nox, mut noy, mut nw, mut nh) = (ox0, oy0, w0, h0);
                    // 精修时的最小框
                    // 不是 16 —— 16 会让"裁掉一条边几像素"这种精修根本做不到
                    const CROP_MIN: i32 = 4;
                    match cm {
                        2 => { let nl = (ox0 + dx).clamp(0, (ox0 + w0 - CROP_MIN).max(0)); nox = nl; nw = ox0 + w0 - nl; }
                        3 => { nw = (x - ox0).clamp(CROP_MIN, (sw - ox0).max(CROP_MIN)); }
                        4 => { let nt = (oy0 + dy).clamp(0, (oy0 + h0 - CROP_MIN).max(0)); noy = nt; nh = oy0 + h0 - nt; }
                        5 => { nh = (y - oy0).clamp(CROP_MIN, (sh - oy0).max(CROP_MIN)); }
                        6 => { let nl = (ox0 + dx).clamp(0, (ox0 + w0 - CROP_MIN).max(0)); nox = nl; nw = ox0 + w0 - nl;
                               let nt = (oy0 + dy).clamp(0, (oy0 + h0 - CROP_MIN).max(0)); noy = nt; nh = oy0 + h0 - nt; }
                        7 => { let nt = (oy0 + dy).clamp(0, (oy0 + h0 - CROP_MIN).max(0)); noy = nt; nh = oy0 + h0 - nt;
                               nw = (x - ox0).clamp(CROP_MIN, (sw - ox0).max(CROP_MIN)); }
                        8 => { let nl = (ox0 + dx).clamp(0, (ox0 + w0 - CROP_MIN).max(0)); nox = nl; nw = ox0 + w0 - nl;
                               nh = (y - oy0).clamp(CROP_MIN, (sh - oy0).max(CROP_MIN)); }
                        9 => { nw = (x - ox0).clamp(CROP_MIN, (sw - ox0).max(CROP_MIN)); nh = (y - oy0).clamp(CROP_MIN, (sh - oy0).max(CROP_MIN)); }
                        1 => { nox = (ox0 + dx).clamp(0, (sw - w0).max(0)); noy = (oy0 + dy).clamp(0, (sh - h0).max(0)); }
                        _ => {}
                    }
                    if nw >= CROP_MIN && nh >= CROP_MIN {
                        ANNO_OX.store(nox, Ordering::Relaxed);
                        ANNO_OY.store(noy, Ordering::Relaxed);
                        ANNO_W.store(nw, Ordering::Relaxed);
                        ANNO_H.store(nh, Ordering::Relaxed);
                        anno_crop_apply();
                    }
                    return 0;
                }
                // 形状手柄拖动：还原到落笔前那张底图，再按新框重画 ——
                // 直接叠画会在旧框上留残影，还原-重画才是"改这一条"
                {
                    let cur = ANNO_SDRAG.lock_ok().clone();
                    if let Some(sd) = cur {
                        // 上一帧画在哪、这一帧画在哪：只刷这两块的并集
                        let old_rect = anno_rect_of(&sd.l);
                        let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                        let mx = x - ox; let my = y - oy;
                        let (mut x0, mut y0, mut x1, mut y1) =
                            (sd.l0.x0, sd.l0.y0, sd.l0.x1, sd.l0.y1);
                        let mut npts: Vec<(i32, i32)> = sd.l0.pts.clone();
                        if sd.handle == usize::MAX {
                            // 整条拖动：按按下点到位移整体平移（用 l0 当基准，不会累积漂移）
                            let dx = mx - sd.m0.0;
                            let dy = my - sd.m0.1;
                            x0 = sd.l0.x0 + dx; y0 = sd.l0.y0 + dy;
                            x1 = sd.l0.x1 + dx; y1 = sd.l0.y1 + dy;
                            for p in npts.iter_mut() { p.0 += dx; p.1 += dy; }
                        } else if sd.l0.tool == 3 {
                            // 序号：0=整体移动 1=转（尖角跟着转） 2=改半径
                            let c0 = ((sd.l0.x0 + sd.l0.x1) / 2, (sd.l0.y0 + sd.l0.y1) / 2);
                            let mut ncx = c0.0; let mut ncy = c0.1;
                            let mut nr = sd.l0.width.max(1);
                            let mut nang = sd.l0.ang;
                            if sd.handle == 0 {
                                ncx = c0.0 + (mx - sd.m0.0); ncy = c0.1 + (my - sd.m0.1);
                            } else if sd.handle == 1 {
                                // 不做 1° 取整，拖到哪算哪
                                nang = -(((my - c0.1) as f32).atan2((mx - c0.0) as f32))
                                    * 180.0 / std::f32::consts::PI;
                            } else {
                                let d = (((mx - c0.0) as f32).powi(2) + ((my - c0.1) as f32).powi(2)).sqrt();
                                // 序号拖半径：下限 8*dpi（再走滑块值域 6~86）
                                nr = (d.round() as i32).clamp(
                                    ui::dpi::sc(ui::layout::anno::NUM_MIN_R), ui::dpi::sc(86));
                                // 半径回写滑块
                                ANNO_OPT_SLIDER.store(ui::dpi::unsc(nr), Ordering::Relaxed);
                                let hw2 = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
                                if !hw2.is_null() { InvalidateRect(hw2, std::ptr::null_mut(), 0); }
                            }
                            {
                                let mut g = ANNO_SDRAG.lock_ok();
                                if let Some(t) = g.as_mut() {
                                    t.l.x0 = ncx - nr; t.l.y0 = ncy - nr;
                                    t.l.x1 = ncx + nr; t.l.y1 = ncy + nr;
                                    t.l.width = nr; t.l.ang = nang;
                                    let cur = t.l.clone();
                                    drop(g);
                                    let new_rect = anno_rect_of(&cur);
                                    anno_shape_redraw(&cur);
                                    anno_invalidate_rect(hwnd, old_rect);
                                    anno_invalidate_rect(hwnd, new_rect);
                                }
                            }
                            return 0;
                        } else if sd.l0.tool == 2 || (sd.l0.tool == 4 && !sd.l0.ell)
                            || ((sd.l0.tool == 6 || sd.l0.tool == 7) && !sd.l0.fill) {
                            // 箭头/线条/马赛克·橡皮的手绘档只有两端柄：直接挪那一端
                            if sd.handle == 0 { x0 = mx; y0 = my; } else { x1 = mx; y1 = my; }
                            if npts.len() >= 2 {
                                let n = npts.len();
                                if sd.handle == 0 { npts[0] = (mx, my); } else { npts[n - 1] = (mx, my); }
                            }
                        } else {
                            let (l0, t0) = (sd.l0.x0.min(sd.l0.x1), sd.l0.y0.min(sd.l0.y1));
                            let (r0, b0) = (sd.l0.x0.max(sd.l0.x1), sd.l0.y0.max(sd.l0.y1));
                            let (mut nl, mut nt, mut nr, mut nb) = (l0, t0, r0, b0);
                            match sd.handle {
                                0 => { nl = mx; nt = my; }
                                1 => { nt = my; }
                                2 => { nr = mx; nt = my; }
                                3 => { nr = mx; }
                                4 => { nr = mx; nb = my; }
                                5 => { nb = my; }
                                6 => { nl = mx; nb = my; }
                                _ => { nl = mx; }
                            }
                            if nl > nr { std::mem::swap(&mut nl, &mut nr); }
                            if nt > nb { std::mem::swap(&mut nt, &mut nb); }
                            // 四角一起拉时最小 2px
                            // 单边拖动不钳另一条边
                            if matches!(sd.handle, 0 | 2 | 4 | 6) {
                                if nr - nl < 2 { nr = nl + 2; }
                                if nb - nt < 2 { nb = nt + 2; }
                            }
                            // Shift = 正方/正圆：拉长的那条边补齐另一条。
                            // 补齐方向必须按手柄所在边决定，否则会把本应固定的对边也拖走。
                            let shift = (GetKeyState(VK_SHIFT as i32) as i32 & 0x8000) != 0;
                            if shift {
                                let ww = nr - nl;
                                let hh = nb - nt;
                                match sd.handle {
                                    // 上下边中点：宽度对齐高度，左右对称扩
                                    1 | 5 => {
                                        if ww > hh {
                                            if sd.handle == 1 { nt = nb - ww; } else { nb = nt + ww; }
                                        } else {
                                            let d = hh - ww;
                                            nl -= d / 2;
                                            nr += d - d / 2;
                                        }
                                    }
                                    // 左右边中点：高度对齐宽度，上下对称扩
                                    3 | 7 => {
                                        if hh > ww {
                                            if sd.handle == 3 { nr = nl + hh; } else { nl = nr - hh; }
                                        } else {
                                            let d = ww - hh;
                                            nt -= d / 2;
                                            nb += d - d / 2;
                                        }
                                    }
                                    _ => {
                                        if ww > hh {
                                            let s = ww;
                                            if matches!(sd.handle, 0 | 2) { nt = nb - s; } else { nb = nt + s; }
                                        } else {
                                            let s = hh;
                                            if matches!(sd.handle, 0 | 6) { nl = nr - s; } else { nr = nl + s; }
                                        }
                                    }
                                }
                            }
                            x0 = nl; y0 = nt; x1 = nr; y1 = nb;
                        }
                        {
                            let mut g = ANNO_SDRAG.lock_ok();
                            if let Some(t) = g.as_mut() {
                                t.l.x0 = x0; t.l.y0 = y0; t.l.x1 = x1; t.l.y1 = y1;
                                t.l.pts = npts;
                                let cur = t.l.clone();
                                drop(g);
                                let new_rect = anno_rect_of(&cur);
                                anno_shape_redraw(&cur);
                                anno_invalidate_rect(hwnd, old_rect);
                                anno_invalidate_rect(hwnd, new_rect);
                            }
                        }
                        return 0;
                    }
                }
                // 文字：拖动 / 拉角缩放
                {
                    let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                    let mut need = false;
                    let mut fs_sync: Option<i32> = None;
                    {
                        let mut g = ANNO_TXE.lock_ok();
                        if let Some(e) = g.as_mut() {
                            if e.mode == 1 {
                                e.x = e.box0.0 + (x - ox) - e.m0.0;
                                e.y = e.box0.1 + (y - oy) - e.m0.1;
                                need = true;
                            } else if e.mode == 2 {
                                let s0: String = e.ch.iter().collect();
                                let (bw0, bh0, _) = anno_tx_box(&s0, tx_fs_px(e.fs0), e.bold, e.italic);
                                let cx = (e.box0.0 + bw0 / 2) as f32;
                                let cy = (e.box0.1 + bh0 / 2) as f32;
                                let d = (((x - ox) as f32 - cx).powi(2) + ((y - oy) as f32 - cy).powi(2)).sqrt();
                                let k = (d / e.d0).clamp(0.15, 8.0);
                                // 字号钳在 10~180 逻辑像素（min/maxFont*dpi）：
                                // 太小画不动、太大直接撑爆整张图
                                let sc = (dpi() as f32 / 96.0).max(1.0);
                                let nf = (e.fs0 * k).clamp(10.0 * sc, 180.0 * sc);
                                let (nw, nh, _) = anno_tx_box(&s0, tx_fs_px(nf), e.bold, e.italic);
                                e.fs = nf;
                                // 内边距跟字号一起缩放
                                let kk = if e.fs0 > 0.0 { nf / e.fs0 } else { 1.0 };
                                e.pad = ((e.pad0 as f32) * kk).round().max(1.0) as i32;
                                fs_sync = Some(tx_fs_px(nf));
                                e.x = cx as i32 - nw / 2;
                                e.y = cy as i32 - nh / 2;
                                need = true;
                            } else if e.mode == 3 {
                                // 旋转：按下点相对中心的方位角差多少，框就跟着转多少。
                                // 贴近正方向（0/±90/180）时吸回去 —— 手抖差零点几度，
                                // 看着是正的、存出来却是斜的
                                let (cx, cy) = anno_tx_center_of(e);
                                let a = ((y - oy) as f32 - cy).atan2((x - ox) as f32 - cx);
                                // 角度不归一化：angle 直接累加，绘制用三角函数，正负都等价
                                let mut deg = e.ang0 + (a - e.rot0) * 180.0 / std::f32::consts::PI;
                                let snapped = (deg / 90.0).round() * 90.0;
                                if (deg - snapped).abs() < 3.0 { deg = snapped; }
                                e.ang = deg;
                                need = true;
                            }
                        }
                    }
                    if let Some(fs) = fs_sync {
                        // 拖角缩放把当前字号回写给副条滑块
                        // 只在滑块值域内（10~72）回填：拖到 72 以上时滑块定格在最大值，
                        // 之后动滑块才由滑块的值接管字号（字号本身不夹）
                        let v = ui::dpi::unsc(fs).clamp(ANNO_SLIDER_CFG[5][0], ANNO_SLIDER_CFG[5][1]);
                        if v != ANNO_OPT_SLIDER.load(Ordering::Relaxed) {
                            ANNO_OPT_SLIDER.store(v, Ordering::Relaxed);
                            cfg_set_tool_num("text", "fontSize", v as f32);
                            let hw = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
                            if !hw.is_null() { InvalidateRect(hw, std::ptr::null_mut(), 0); }
                        }
                    }
                    if need { InvalidateRect(hwnd, std::ptr::null_mut(), 0); return 0; }
                }
                // 输入中：鼠标压到虚线框/手柄上立刻挂出四角柄 + 旋转柄，移开就收。
                // 只在"编辑中且没在拖"这一档算（handlesVisible 同一条规则）
                {
                    let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                    let mut cur: *const u16 = std::ptr::null();
                    let mut changed = false;
                    {
                        let mut g = ANNO_TXE.lock_ok();
                        if let Some(e) = g.as_mut() {
                            if e.focused && e.mode == 0 {
                                // 鼠标离开落笔点超过 4px 就不再算"刚起笔"，四角重新可判
                                // （freshBox 同一条：translation 之后随手一挪就恢复）
                                if e.fresh {
                                    let (dx, dy) = (x - ox - e.m0.0, y - oy - e.m0.1);
                                    if dx * dx + dy * dy > 16 { e.fresh = false; changed = true; }
                                }
                                let hov = anno_tx_frame_hover(e, x - ox, y - oy);
                                if hov != e.hov { e.hov = hov; changed = true; }
                                if hov {
                                    let h = anno_tx_hnd_hit(e, x - ox, y - oy, ui::dpi::sc(ui::metric::TX_HANDLE_HIT));
                                    let rot_hot = h == TxHnd::Rotate;
                                    if rot_hot != e.rot_hot { e.rot_hot = rot_hot; changed = true; }
                                    match h {
                                        TxHnd::Rotate | TxHnd::Close => cur = IDC_HAND,
                                        TxHnd::Scale(k) => cur = anno_tx_scale_cursor(e.ang, k),
                                        TxHnd::None => {
                                            // 框上（拖动调用区）给四向箭头，一眼看出"这个框能拖走"。
                                            // 正文区不抢：那里交给下面的 I 形光标
                                            let (px, py) = anno_tx_inv(e, x - ox, y - oy);
                                            let (bw, bh) = anno_tx_size(e);
                                            let over_body = px >= e.x as f32 && px < (e.x + bw) as f32
                                                && py >= e.y as f32 && py < (e.y + bh) as f32;
                                            if !over_body { cur = IDC_SIZEALL; }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if changed { InvalidateRect(hwnd, std::ptr::null_mut(), 0); }
                    if !cur.is_null() {
                        SetCursor(LoadCursorW(std::ptr::null_mut(), cur));
                        return 0;
                    }
                }
                // 最后一条标注的手柄：光标压上去才亮，靠每个 Shape 的 hoverDraggerIndex
                // 这里的画布是"直接烧进位图"的，没有 shape 链表，所以只在移动时对最后一条做一次命中
                {
                    let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                    let lastl = ANNO_LAST.lock_ok().clone();
                    let hot = match &lastl {
                        // 文字：压在正文/框边/四角柄/旋转柄/通往柄的走廊上都算
                        // （handlesVisible 非编辑态 = hoverDraggerIndex >= 0）
                        //   正文算 1，也照样把整套手柄挂出来）
                        Some(l) if !ANNO_DRAG.load(Ordering::Relaxed) && l.tool == 8 => {
                            anno_txt_hover(l, x - ox, y - oy)
                        }
                        Some(l) if !ANNO_DRAG.load(Ordering::Relaxed) => anno_last_hit(l, x - ox, y - oy),
                        _ => false,
                    };
                    // 旋转柄的悬停态：柄放大一档、蓝环加粗
                    let rot_hot = lastl.map(|l| l.tool == 8).unwrap_or(false)
                        && matches!(anno_txt_hnd_hit(x - ox, y - oy), Some((_, TxHnd::Rotate)));
                    if rot_hot != ANNO_TXT_ROT_HOT.load(Ordering::Relaxed) {
                        ANNO_TXT_ROT_HOT.store(rot_hot, Ordering::Relaxed);
                        anno_invalidate_last(hwnd);
                    }
                    if hot != ANNO_LAST_HOT.load(Ordering::Relaxed) {
                        ANNO_LAST_HOT.store(hot, Ordering::Relaxed);
                        anno_invalidate_last(hwnd);
                    }
                }
                // 编辑中拖选：按住左键移动，光标跟手，锚点不动
                {
                    let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                    let mut sel_moved = false;
                    if let Some(e) = ANNO_TXE.lock_ok().as_mut() {
                        if e.focused && e.drag {
                            let ci = anno_tx_caret_at(e, 0, x - ox, y - oy);
                            if ci != e.caret { e.caret = ci; sel_moved = true; }
                        }
                    }
                    if sel_moved {
                        ANNO_CARET_ON.store(true, Ordering::Relaxed);
                        InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                    }
                }
                // 已提交文字的手柄光标
                // 旋转/关闭给手型、四角给双箭头；正文留给下面的 I 形
                {
                    let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                    if ANNO_TXE.lock_ok().is_none() && !ANNO_DRAG.load(Ordering::Relaxed) {
                        if let Some((_, h)) = anno_txt_hnd_hit(x - ox, y - oy) {
                            let ang = ANNO_LAST.lock_ok().as_ref().map(|l| l.ang).unwrap_or(0.0);
                            let cur = match h {
                                TxHnd::Rotate | TxHnd::Close => IDC_HAND,
                                TxHnd::Scale(k) => anno_tx_scale_cursor(ang, k),
                                TxHnd::None => std::ptr::null(),
                            };
                            if !cur.is_null() {
                                SetCursor(LoadCursorW(std::ptr::null_mut(), cur));
                                return 0;
                            }
                        }
                    }
                }
                // 压在任何一条文字上（或在编辑中）= I 形光标，跟 TextBox::onCursorEvent 一致
                // 判在"调整光标"之前：文字压在选区内部，先认出它才轮得到缩放箭头
                {
                    let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                    let over_text = {
                        let g = ANNO_TXE.lock_ok();
                        match g.as_ref() {
                            Some(e) if e.focused => {
                                // 编辑中只把 I 形光标留在正文上：落在框边/手柄上要走上面的
                                // 缩放、旋转、关闭那几档，不能被这里抢掉
                                let (px, py) = anno_tx_inv(e, x - ox, y - oy);
                                let (bw, bh) = anno_tx_size(e);
                                px >= e.x as f32 && px < (e.x + bw) as f32
                                    && py >= e.y as f32 && py < (e.y + bh) as f32
                            }
                            _ => anno_tx_hit(x - ox, y - oy).is_some(),
                        }
                    };
                    if over_text {
                        SetCursor(LoadCursorW(std::ptr::null_mut(), IDC_IBEAM));
                        return 0;
                    }
                }
                // 无工具悬停：切换调整光标
                if ANNO_TOOL.load(Ordering::Relaxed) < 0 {
                    let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                    let w = ANNO_W.load(Ordering::Relaxed); let h = ANNO_H.load(Ordering::Relaxed);
                    let m = anno_crop_hit(x, y, ox, oy, w, h);
                    let cur = match m {
                        6 | 9 => 32642, // SIZENWSE
                        7 | 8 => 32643, // SIZENESW
                        2 | 3 => 32644, // SIZEWE
                        4 | 5 => 32645, // SIZENS
                        1 => 32646,     // SIZEALL
                        _ => 0,
                    };
                    if cur != 0 { SetCursor(LoadCursorW(std::ptr::null_mut(), cur as *const u16)); }
                }
                if ANNO_DRAG.load(Ordering::Relaxed) {
                    ANNO_DRAGGED.store(true, Ordering::Relaxed);
                    let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                    let w = ANNO_W.load(Ordering::Relaxed); let h = ANNO_H.load(Ordering::Relaxed);
                    let ix = (x - ox).clamp(0, w - 1); let iy = (y - oy).clamp(0, h - 1);
                    ANNO_DE.0.store(ix, Ordering::Relaxed); ANNO_DE.1.store(iy, Ordering::Relaxed);
                    // 线条、以及马赛克/模糊/橡皮的"手绘"模式都沿轨迹累积顶点（的 mouseDrag）
                    // 是往 path 里追加点，不是拉一条直线）；"选框"模式只要起止两点，不用攒点
                    let tool = ANNO_TOOL.load(Ordering::Relaxed);
                    if tool == 4 || ((tool == 6 || tool == 7) && !anno_effect_rect_mode(tool)) {
                        let mut ps = ANNO_PTS.lock_ok();
                        // 马赛克/橡皮沿轨迹采样
                        // 画笔(4)保持曼哈顿阈值
                        let far = if tool == 4 {
                            ps.last().map_or(true, |&(px, py)| (px - ix).abs() + (py - iy).abs() >= 2)
                        } else {
                            anno_need_sample(ps.last().copied(), ix, iy)
                        };
                        if far { ps.push((ix, iy)); }
                    }
                    // 只恢复「上一帧的笔迹 ∪ 这一帧的笔迹」这一块；
                    // 原来每帧整图拷回 + 整窗失效，1080p 下就是 20ms/帧
                    let nb = anno_stroke_box(tool, ANNO_DS.0.load(Ordering::Relaxed),
                        ANNO_DS.1.load(Ordering::Relaxed), ix, iy, w, h);
                    let u = anno_box_union(*ANNO_PREV_BOX.lock_ok(), nb);
                    {
                        let pre = ANNO_PRE.lock_ok();
                        let mut img = ANNO_IMG.lock_ok();
                        if img.len() == pre.len() {
                            anno_img_restore(&mut img, &pre, w, h, u);
                        } else if !pre.is_empty() {
                            *img = pre.clone();
                        }
                    }
                    // 马赛克/模糊/智能消除/橡皮拖动中只落品红占位，真实像素留到松手
                    if tool == 6 || tool == 7 {
                        anno_effect_preview(tool, ANNO_DS.0.load(Ordering::Relaxed), ANNO_DS.1.load(Ordering::Relaxed), ix, iy);
                    } else {
                        anno_stroke(ANNO_DS.0.load(Ordering::Relaxed), ANNO_DS.1.load(Ordering::Relaxed),
                            ANNO_DE.0.load(Ordering::Relaxed), ANNO_DE.1.load(Ordering::Relaxed));
                    }
                    *ANNO_PREV_BOX.lock_ok() = nb;
                    if !anno_box_empty(u) {
                        let rc = RECT {
                            left: u.0 + ox, top: u.1 + oy,
                            right: u.2 + ox, bottom: u.3 + oy,
                        };
                        InvalidateRect(hwnd, &rc, 0);
                    }
                }
                0
            }
            WM_LBUTTONUP => {
                ReleaseCapture();
                ANNO_SLIDER_DRAG.store(false, Ordering::Relaxed);
                if ANNO_CROP_MODE.load(Ordering::Relaxed) != 0 {
                    ANNO_CROP_MODE.store(0, Ordering::Relaxed);
                }
                // 形状手柄松手。
                // 撤销点要落在"拖动之前那一版"的图元表（sd.before），不能拿整张底图 ——
                // 拿底图入栈的话 Ctrl+Z 一下整条框就没了
                // 框还在、只是变回初始的大小/位置
                {
                    let sd = ANNO_SDRAG.lock_ok().take();
                    if let Some(sd) = sd {
                        let moved = sd.l.x0 != sd.l0.x0 || sd.l.y0 != sd.l0.y0
                            || sd.l.x1 != sd.l0.x1 || sd.l.y1 != sd.l0.y1
                            || sd.l.width != sd.l0.width || sd.l.ang != sd.l0.ang
                            || sd.l.pts != sd.l0.pts;
                        if moved { anno_push_snap(sd.before); }
                        *ANNO_LAST.lock_ok() = Some(sd.l.clone());
                        anno_invalidate_last(hwnd);
                    }
                    // 拖动结束，前缀底图释放
                    anno_base_clear();
                }
                // 拖框/拉角松手：位置与字号就此定下，直接收笔 ——
                // 留着编辑态的话那圈虚线框会一直挂到下一次点击
                let was_moving = {
                    let g = ANNO_TXE.lock_ok();
                    g.as_ref().map(|e| e.mode != 0 && !e.focused).unwrap_or(false)
                };
                if was_moving {
                    anno_tx_finish();
                    // 松手时光标还压在框上：手柄别等下一次鼠标移动才回来
                    // （hoverDraggerIndex 在 refresh 之后依然有效）
                    let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                    let hot = ANNO_LAST.lock_ok().as_ref()
                        .filter(|l| l.tool == 8)
                        .map(|l| anno_txt_hover(l, (lp & 0xFFFF) as i32 - ox, ((lp >> 16) & 0xFFFF) as i32 - oy))
                        .unwrap_or(false);
                    ANNO_LAST_HOT.store(hot, Ordering::Relaxed);
                }
                // 编辑中拉角/旋转松手：只是结束这一次手势，框留在编辑态接着输入
                // （focused 为真时 was_moving 不会命中，得在这里把 mode 归零，
                //   否则之后鼠标一动还会继续按"正在缩放/旋转"处理）
                {
                    let mut g = ANNO_TXE.lock_ok();
                    if let Some(e) = g.as_mut() {
                        e.drag = false;   // 拖选结束
                        if e.focused && e.mode != 0 { e.mode = 0; ANNO_CARET_ON.store(true, Ordering::Relaxed); }
                    }
                }
                if ANNO_DRAG.load(Ordering::Relaxed) {
                    ANNO_DRAG.store(false, Ordering::Relaxed);
                    let tool = ANNO_TOOL.load(Ordering::Relaxed);
                    let ox = ANNO_OX.load(Ordering::Relaxed); let oy = ANNO_OY.load(Ordering::Relaxed);
                    let w = ANNO_W.load(Ordering::Relaxed); let h = ANNO_H.load(Ordering::Relaxed);
                    // 松手后要刷新的范围：这一笔最后占的地方（抖动没成笔时就是占位那块）
                    let mut up_box = (0, 0, 0, 0);
                    if ANNO_DRAGGED.load(Ordering::Relaxed) {
                        // 马赛克/模糊/智能消除/橡皮
                        // （mosaic 位图在 mouseUp 里跑；拖动中只有占位），
                        // 智能消除更只有松手这一下。这里不补这一笔的话，拖动中那个
                        // `if !optc` 一直跳过，智能消除就永远不落笔；顺带也保证最后
                        // 生效的是松手位置，而不是停在上一帧 mouseMove 的几何上。
                        // 先把图退回落笔前那一版，免得同一笔在图上叠两遍。
                        let mut restore = (0, 0, 0, 0);
                        if tool == 6 || tool == 7 {
                            // 几何按松手点定稿
                            let rx = ((lp & 0xFFFF) as i32 - ox).clamp(0, w - 1);
                            let ry = (((lp >> 16) & 0xFFFF) as i32 - oy).clamp(0, h - 1);
                            ANNO_DE.0.store(rx, Ordering::Relaxed); ANNO_DE.1.store(ry, Ordering::Relaxed);
                            if !anno_effect_rect_mode(tool) {
                                let mut ps = ANNO_PTS.lock_ok();
                                if anno_need_sample(ps.last().copied(), rx, ry) { ps.push((rx, ry)); }
                            }
                            // 只退回这一笔走过的区域（整图 clone 是 8~33MB 一次）
                            let nb = anno_stroke_box(tool, ANNO_DS.0.load(Ordering::Relaxed),
                                ANNO_DS.1.load(Ordering::Relaxed), rx, ry, w, h);
                            restore = anno_box_union(*ANNO_PREV_BOX.lock_ok(), nb);
                            up_box = restore;
                        } else {
                            up_box = *ANNO_PREV_BOX.lock_ok();
                        }
                        if !anno_box_empty(restore) {
                            let pre = ANNO_PRE.lock_ok();
                            let mut img = ANNO_IMG.lock_ok();
                            if img.len() == pre.len() {
                                anno_img_restore(&mut img, &pre, w, h, restore);
                            } else if !pre.is_empty() {
                                *img = pre.clone();
                            }
                        }
                        if tool == 6 || tool == 7 {
                            anno_stroke(ANNO_DS.0.load(Ordering::Relaxed), ANNO_DS.1.load(Ordering::Relaxed),
                                ((lp & 0xFFFF) as i32 - ox).clamp(0, w - 1),
                                (((lp >> 16) & 0xFFFF) as i32 - oy).clamp(0, h - 1));
                        }
                        // 撤销点就是"落笔之前"的图元表：先拍快照，再把这一笔入表
                        let n_before = ANNO_SHAPES.lock_ok().len() as i32;
                        anno_push_undo();
                        // 记下这一条的几何：之后光标停上去要亮 8 个手柄（同时写入图元表）
                        anno_last_set(tool);
                        // 落笔前的画面正好是「底图 + 除最后一条外的全部图元」，
                        // 直接把它当前缀底图留着：之后拖滑块改这一条，每移动一格
                        // 只需重烘它自己，不必把前面所有标注（含智能消除的填充）再算一遍
                        {
                            let idx = ANNO_LAST.lock_ok().as_ref().map(|l| l.idx).unwrap_or(-1);
                            let n = ANNO_SHAPES.lock_ok().len() as i32;
                            let need = (w as usize) * (h as usize) * 4;
                            let mut pre = ANNO_PRE.lock_ok();
                            if n == n_before + 1 && idx >= 0 && idx + 1 == n && pre.len() == need {
                                *ANNO_BASE.lock_ok() = std::mem::take(&mut *pre);
                                ANNO_BASE_N.store(idx, Ordering::Relaxed);
                            } else {
                                ANNO_BASE_N.store(-1, Ordering::Relaxed);
                            }
                        }
                    } else {
                        // 没成笔（只是抖了一下）：把占位色退回落笔前，别在图上留一块品红
                        if tool == 6 || tool == 7 {
                            up_box = *ANNO_PREV_BOX.lock_ok();
                            let pre = ANNO_PRE.lock_ok();
                            let mut img = ANNO_IMG.lock_ok();
                            if img.len() == pre.len() {
                                anno_img_restore(&mut img, &pre, w, h, up_box);
                            } else if !pre.is_empty() {
                                *img = pre.clone();
                            }
                        }
                    }
                    ANNO_DRAGGED.store(false, Ordering::Relaxed);
                    ANNO_PRE.lock_ok().clear();
                    *ANNO_PREV_BOX.lock_ok() = (0, 0, 0, 0);
                    if !anno_box_empty(up_box) {
                        let rc = RECT {
                            left: up_box.0 + ox, top: up_box.1 + oy,
                            right: up_box.2 + ox, bottom: up_box.3 + oy,
                        };
                        InvalidateRect(hwnd, &rc, 0);
                    }
                }
                0
            }
            WM_CHAR => {
                // 只有编辑态的文本框收字符；外面一律忽略（工具键都是 WM_KEYDOWN 的事）
                let c = wp as u32;
                let mut changed = false;
                {
                    let mut g = ANNO_TXE.lock_ok();
                    if let Some(e) = g.as_mut() {
                        if e.focused {
                            let (a, b) = anno_tx_sel_range(e);
                            let had = b > a;
                            if had { e.ch.drain(a..b); e.caret = a; e.sel = a; }
                            match c {
                                8 => {
                                    // 退格：删光标前一个字（ch 是 Vec<char>，中文也只删一个）
                                    if !had && e.caret > 0 && e.caret <= e.ch.len() {
                                        e.caret -= 1;
                                        e.ch.remove(e.caret);
                                    }
                                    changed = true;
                                }
                                9 => {
                                    // Tab → 4 个空格
                                    for _ in 0..4 { e.ch.insert(e.caret, ' '); e.caret += 1; }
                                    changed = true;
                                }
                                13 => {
                                    // 回车插换行，的 TextBox 是多行控件，Enter 不提交
                                    e.ch.insert(e.caret, '\n');
                                    e.caret += 1;
                                    changed = true;
                                }
                                27 => {}
                                _ => {
                                    if c >= 0x20 && c != 0x7F {
                                        if let Some(ch) = char::from_u32(c) {
                                            e.ch.insert(e.caret, ch);
                                            e.caret += 1;
                                            changed = true;
                                        }
                                    }
                                }
                            }
                            if changed { e.sel = e.caret; }
                        }
                    }
                }
                if changed {
                    ANNO_CARET_ON.store(true, Ordering::Relaxed);  // 打字时光标常亮，别闪
                    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                }
                0
            }
            WM_KEYDOWN => {
                let k = wp as i32;
                let ctrl = ((GetKeyState(VK_CONTROL as i32) as i32) & 0x8000) != 0;
                // 录制中宿主窗仍然收键
                // Ctrl+S = 停录并落文件、Ctrl+C = 停录并复制、ESC = 停录并丢弃，
                // 其余键在录制中一律吞掉（不响应工具切换 / 撤销等）
                if REC_PHASE.load(Ordering::Relaxed) == 1 {
                    if k == VK_ESCAPE {
                        rec_finish(true, false);
                    } else if ctrl && (k == 0x53 || k == 0x43) {
                        rec_finish(false, k == 0x43);
                    }
                    return 0;
                }
                // 编辑中：键全归文本框（editingText 直接 return）
                // 只把 ESC 交给 quitByEsc），所以 Ctrl+Z/Y/C/S 在编辑态一律不响应
                {
                    let editing = {
                        let g = ANNO_TXE.lock_ok();
                        g.as_ref().map(|e| e.focused).unwrap_or(false)
                    };
                    if editing {
                        if k == VK_ESCAPE { anno_tx_finish(); anno_close(); return 0; }
                        let shift = ((GetKeyState(VK_SHIFT as i32) as i32) & 0x8000) != 0;
                        // 其余组合键在编辑态不响应，不能落到外面的撤销/复制/保存
                        if ctrl {
                            let mut changed = false;
                            {
                                let mut g = ANNO_TXE.lock_ok();
                                if let Some(e) = g.as_mut() {
                                    let (a, b) = anno_tx_sel_range(e);
                                    match k {
                                        0x41 => { e.sel = 0; e.caret = e.ch.len(); changed = true; }
                                        0x43 | 0x58 => {
                                            if b > a {
                                                let s: String = e.ch[a..b].iter().collect();
                                                copy_text_clipboard(&s);
                                                if k == 0x58 {
                                                    e.ch.drain(a..b);
                                                    e.caret = a;
                                                    e.sel = a;
                                                    changed = true;
                                                }
                                            }
                                        }
                                        0x56 => {
                                            if let Some(t) = get_text_clipboard() {
                                                let mut norm = String::with_capacity(t.len());
                                                let mut it = t.chars().peekable();
                                                while let Some(c) = it.next() {
                                                    if c == '\r' {
                                                        if it.peek() == Some(&'\n') { continue; }
                                                        norm.push('\n');
                                                    } else { norm.push(c); }
                                                }
                                                if b > a { e.ch.drain(a..b); e.caret = a; }
                                                for ch in norm.chars() {
                                                    e.ch.insert(e.caret, ch);
                                                    e.caret += 1;
                                                }
                                                e.sel = e.caret;
                                                changed = true;
                                            }
                                        }
                                        _ => {}
                                    }
                                }
                            }
                            if changed {
                                ANNO_CARET_ON.store(true, Ordering::Relaxed);
                                InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                            }
                            return 0;
                        }
                        let mut moved = false;
                        let mut done = false;
                        {
                            let mut g = ANNO_TXE.lock_ok();
                            if let Some(e) = g.as_mut() {
                                let n = e.ch.len();
                                let (sa, sb) = anno_tx_sel_range(e);
                                let has_sel = sb > sa;
                                match k {
                                    VK_ESCAPE => { done = true; }
                                    VK_LEFT => {
                                        if !shift && has_sel { e.caret = sa; }
                                        else if e.caret > 0 { e.caret -= 1; }
                                        if !shift { e.sel = e.caret; }
                                        moved = true;
                                    }
                                    VK_RIGHT => {
                                        if !shift && has_sel { e.caret = sb; }
                                        else if e.caret < n { e.caret += 1; }
                                        if !shift { e.sel = e.caret; }
                                        moved = true;
                                    }
                                    VK_UP | VK_DOWN => {
                                        // 拿当前光标的 x 去命中上/下一行，连续上下保持横向位置
                                        let (cxp, cyp) = anno_tx_caret_xy(e, 0);
                                        let sp = anno_tx_spacing(tx_fs_px(e.fs));
                                        let ty = if k == VK_UP { cyp as f32 - sp * 0.5 }
                                                 else { cyp as f32 + sp * 1.5 };
                                        e.caret = anno_tx_caret_at(e, 0, cxp, ty as i32);
                                        if !shift { e.sel = e.caret; }
                                        moved = true;
                                    }
                                    VK_HOME => {
                                        let (_, cyp) = anno_tx_caret_xy(e, 0);
                                        e.caret = anno_tx_caret_at(e, 0, e.x, cyp);
                                        if !shift { e.sel = e.caret; }
                                        moved = true;
                                    }
                                    VK_END => {
                                        let (_, cyp) = anno_tx_caret_xy(e, 0);
                                        e.caret = anno_tx_caret_at(e, 0, 1_000_000, cyp);
                                        if !shift { e.sel = e.caret; }
                                        moved = true;
                                    }
                                    VK_DELETE => {   // 有选区先删选区，否则删光标后一个字
                                        if has_sel {
                                            e.ch.drain(sa..sb);
                                            e.caret = sa;
                                            e.sel = sa;
                                        } else if e.caret < n {
                                            e.ch.remove(e.caret);
                                        }
                                        moved = true;
                                    }
                                    _ => {}
                                }
                            }
                        }
                        // ESC：收笔并一次关掉整个编辑器（不再需要按第二次）
                        if done { anno_tx_finish(); anno_close(); return 0; }
                        if moved {
                            ANNO_CARET_ON.store(true, Ordering::Relaxed);
                            InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                            return 0;
                        }
                    }
                }
                if k == VK_ESCAPE { anno_tx_finish(); anno_close(); return 0; }
                if ctrl {
                    match k {
                        0x5A => { anno_undo(); }
                        0x59 => { anno_redo(); }
                        0x43 => { anno_copy(); }
                        0x53 => { anno_save(); }
                        0x41 => {}                       // Ctrl+A：文本框里暂无选区的可视反馈
                        _ => {}
                    }
                    return 0;
                }
                // Del：删掉光标压着的最后一条标注
                if k == VK_DELETE {
                    if anno_delete_hot() { InvalidateRect(hwnd, std::ptr::null_mut(), 0); }
                    return 0;
                }
                // 编辑中按回车是换行，不能触发"复制并退出"
                if k == VK_RETURN {
                    let editing = {
                        let g = ANNO_TXE.lock_ok();
                        g.as_ref().map(|e| e.focused).unwrap_or(false)
                    };
                    if !editing { anno_copy(); }
                    return 0;
                }
                0
            }
            WM_TIMER => {
                // 还会自己重排一次，组字开始那一枪会被它盖掉 —— 30ms 后再补一枪才稳
                if wp == ANNO_IME_NUDGE {
                    KillTimer(hwnd, ANNO_IME_NUDGE);
                    anno_ime_place(hwnd, true, true);
                    return 0;
                }
                if wp == ANNO_TX_TIMER {
                    let focused = {
                        let g = ANNO_TXE.lock_ok();
                        g.as_ref().map(|e| e.focused && e.mode == 0).unwrap_or(false)
                    };
                    if focused {
                        ANNO_CARET_ON.store(!ANNO_CARET_ON.load(Ordering::Relaxed), Ordering::Relaxed);
                        InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                    }
                    0
                } else { DefWindowProcW(hwnd, msg, wp, lp) }
            }
            // IME 四条消息按同一套顺序处理，保证每条恰好过一次 DefWindowProc
            //   · 上下文关联 / 组字开始：**先摆位再放行** —— 输入法开窗那一刻就用我们的坐标
            //   · 组字更新：只放行，组字窗由输入法自己维护（这里再插手就是两边互相覆盖）
            //   · 通知（候选框开/翻页）：**先放行再摆位** —— 候选框是它先摆、我们跟着挪
            WM_IME_SETCONTEXT => {
                if wp != 0 {
                    // 输入上下文刚关联上：输入法随即会建自己的组字窗，先把坐标塞给它
                    anno_ime_place(hwnd, true, true);
                    SetTimer(hwnd, ANNO_IME_NUDGE, 30, None);
                }
                DefWindowProcW(hwnd, msg, wp, lp)
            }
            WM_IME_STARTCOMPOSITION => {
                // 组字开始。不设的话候选框会贴到窗口左上角
                // 组字窗也会停在上一组字的位置，看着就是"打字乱飘"
                anno_ime_place(hwnd, true, true);
                SetTimer(hwnd, ANNO_IME_NUDGE, 30, None);
                DefWindowProcW(hwnd, msg, wp, lp)
            }
            WM_IME_COMPOSITION => DefWindowProcW(hwnd, msg, wp, lp),
            WM_IME_NOTIFY => {
                let r = DefWindowProcW(hwnd, msg, wp, lp);
                if wp == IMN_OPENCANDIDATE || wp == IMN_CHANGECANDIDATE {
                    // 候选框刚开 / 翻页：输入法自己摆了，我们把它挪回光标下方
                    anno_ime_place(hwnd, false, true);
                    SetTimer(hwnd, ANNO_IME_NUDGE, 30, None);
                }
                r
            }
            WM_CLOSE => { anno_close(); 0 }
            _ => DefWindowProcW(hwnd, msg, wp, lp),
        }
    }
}

// 打开标注编辑器：bgra 为 top-down 位图（选区内容）；sx/sy 为选区在屏幕上的位置。
// 全屏编辑器：整屏压暗(选区外)、选区内高亮 + 蓝框 + 坐标，直到收尾才消失；工具条/副选项条浮于下方。
pub(crate) unsafe fn anno_open(bgra: Vec<u8>, w: i32, h: i32, sx: i32, sy: i32) {
    // 说明：新截图入口（热键）在编辑器未结束时已拦截；能走到这里说明上一张已解除
    *ANNO_IMG.lock_ok() = bgra.clone();
    *ANNO_ORIG.lock_ok() = bgra;
    ANNO_W.store(w, Ordering::Relaxed);
    ANNO_H.store(h, Ordering::Relaxed);
    ANNO_OX.store(sx, Ordering::Relaxed);
    ANNO_OY.store(sy, Ordering::Relaxed);
    ANNO_TOOL.store(-1, Ordering::Relaxed);
    ANNO_PREVIEW.store(-1, Ordering::Relaxed);
    ANNO_PENDING.store(-1, Ordering::Relaxed);
    ANNO_NUM.store(0, Ordering::Relaxed);
    ANNO_OPT_A.store(false, Ordering::Relaxed);
    ANNO_OPT_B.store(false, Ordering::Relaxed);
    // C 也是"这次会话的开关状态"：上一个工具的椭圆填充/模糊模式不能带到新截图里
    ANNO_OPT_C.store(false, Ordering::Relaxed);
    ANNO_SLIDER_DRAG.store(false, Ordering::Relaxed);
    ANNO_PIN.store(false, Ordering::Relaxed);
    ANNO_UNDO.lock_ok().clear();
    ANNO_REDO.lock_ok().clear();
    anno_tx_cancel();
    *ANNO_SHAPES.lock_ok() = Vec::new();
    // 底图原点 = 选区左上角。之后拖选区时 anno_crop_apply 按这个基准算图元位移
    ANNO_BASE_OX.store(sx, Ordering::Relaxed);
    ANNO_BASE_OY.store(sy, Ordering::Relaxed);
    let hinstance = GetModuleHandleW(std::ptr::null_mut());
    let c_img = wide(CLS_ANNO_IMG);
    let c_main = wide(CLS_ANNO_MAIN);
    let c_sub = wide(CLS_ANNO_SUB);
    let mk = |name: &str, wndproc| {
        // 编辑器里用普通箭头；十字只留在框选遮罩（CLS_SELWND）上
        reg_class(name, Some(wndproc), CS_HREDRAW | CS_VREDRAW, IDC_ARROW as usize, false);
    };
    // 原图窗要收双击（选框内双击 = 完成并复制）：类样式不带 CS_DBLCLKS 的话，
    // 系统根本不会派发 WM_LBUTTONDBLCLK，手势就是死的。
    reg_class(CLS_ANNO_IMG, Some(anno_img_proc),
        CS_HREDRAW | CS_VREDRAW | 0x0008 /*CS_DBLCLKS*/, IDC_ARROW as usize, false);
    mk(CLS_ANNO_MAIN, anno_main_proc);
    mk(CLS_ANNO_SUB, anno_sub_proc);
    let (sox, soy) = sel_org();
    let (sw, sh) = sel_vp();
    // 全屏原图窗（可聚焦收键盘→T文字/快捷键），亮区由 WM_PAINT 按 ANNO_OX/OY 绘制。
    // 窗口贴到遮罩那块屏上：这样窗口客户区坐标与遮罩局部坐标完全重合，
    // ANNO_OX/OY 不用再加偏移，整块压暗底图也和窗口尺寸对得上
    let ihw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, c_img.as_ptr(), wide("").as_ptr(),
        WS_POPUP, sox, soy, sw, sh,
        std::ptr::null_mut(), std::ptr::null_mut(), hinstance, std::ptr::null_mut());
    // 主工具条：独立浮条，位于亮区下方（位置由 anno_layout 定）
    // 先不显示：遮罩还在的时候它一浮出来就是一块亮色，等编辑器首帧画完再一起亮
    let mhw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, c_main.as_ptr(), wide("").as_ptr(),
        WS_POPUP | WS_CLIPCHILDREN, 0, 0, ui::metric::TB_W, ui::metric::TB_H,
        std::ptr::null_mut(), std::ptr::null_mut(), hinstance, std::ptr::null_mut());
    // 副选项条：初始隐藏
    let shw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, c_sub.as_ptr(), wide("").as_ptr(),
        WS_POPUP | WS_CLIPCHILDREN, 0, 0, 40, ui::metric::SUB_H,
        std::ptr::null_mut(), std::ptr::null_mut(), hinstance, std::ptr::null_mut());
    if !ihw.is_null() {
        ANNO_HWND.store(ihw as isize, Ordering::Relaxed);
        // 首帧藏到遮罩下面画：编辑器整屏「原图 + 选区外压暗」是 GDI 直接画到屏幕上，
        // 直接亮出来的话会看到一张画到一半的过渡帧，就是松手时闪一下。
        // 插到遮罩下面之后，用户看到的仍是框选画面，画完再由调用方一步换面
        let mask = SEL_HWND.load(Ordering::Relaxed) as HWND;
        if !mask.is_null() {
            SetWindowPos(ihw, mask, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        ShowWindow(ihw, SW_SHOWNA);
    }
    if !mhw.is_null() {
        ANNO_MAIN_HWND.store(mhw as isize, Ordering::Relaxed);
        tip_create(&TIP_MAIN, mhw);
    }
    if !shw.is_null() {
        ANNO_SUB_HWND.store(shw as isize, Ordering::Relaxed);
        tip_create(&TIP_SUB, shw);
    }
    anno_layout();
    if !ihw.is_null() {
        force_foreground(ihw);
        // 激活时可能被提到遮罩上面，重新压回遮罩下面，等首帧画完再换面
        let mask = SEL_HWND.load(Ordering::Relaxed) as HWND;
        if !mask.is_null() {
            SetWindowPos(ihw, mask, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    // 必须排在 SetForegroundWindow 之后：那句会把全屏遮罩提到 topmost 组的最上层，
    // 先摆工具条的话主条会被那层暗遮罩压住，整条看起来是灰的（白底变成 128 灰）
    anno_layout();
}

// 选区（截图框）可编辑：无工具时拖动边/角改大小、拖动内部移位置
pub(crate) static ANNO_CROP_MODE: AtomicI32 = AtomicI32::new(0); // 0无 1移动 2左 3右 4上 5下 6左上 7右上 8左下 9右下
pub(crate) static ANNO_CROP_OX0: AtomicI32 = AtomicI32::new(0);
pub(crate) static ANNO_CROP_OY0: AtomicI32 = AtomicI32::new(0);
pub(crate) static ANNO_CROP_W0: AtomicI32 = AtomicI32::new(0);
pub(crate) static ANNO_CROP_H0: AtomicI32 = AtomicI32::new(0);
pub(crate) static ANNO_CROP_MX0: AtomicI32 = AtomicI32::new(0);
pub(crate) static ANNO_CROP_MY0: AtomicI32 = AtomicI32::new(0);
