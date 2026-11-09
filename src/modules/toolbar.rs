use crate::prelude::*;
use crate::*;

// 主条上第 i 颗按钮的中心，相对主条左边的偏移。
// 按"格子"累加而不是按按钮实际宽度：按钮在格子里四边留了 pad，
// 拿宽度累加会把每条缝都漏掉，越往右偏得越多（箭头就指不准了）。
pub(crate) fn anno_btn_center_x(i: i32) -> i32 {
    let mut x = 0;
    for k in 0..i {
        if ANNO_SEP_AT.contains(&(k as usize)) { x += ui::metric::SEP; }
        x += ui::metric::CELL;
    }
    x + ui::metric::CELL / 2
}

// ———————— 副条（工具副条 / 录制条）的统一摆放 ————————
// 全程序只有这一处算副条的位置。两套算法（工具副条一套、录制条一套）会让
// （直接左对齐主条），同屏两条东西遵循两条规则，看着就是"有的居中、有的贴左"。
//
// 规则只有三条
//   1. 先把副条**中心**对准它所属那颗按钮的中心；
//   2. 左边界不许越过主条左边 —— 内容多的条（带 9 格色板那种）居中之后必然越界，
//      到这里就被推回去，结果就是"左边与主条齐"；内容少的条（马赛克/橡皮/录制）
//      本来就落在主条右边，保持居中。于是用户看到的是"多的左边齐、少的中心对齐"，
//      而代码里**没有任何按工具分叉的特例**；
//   3. 再夹进所在显示器的工作区（副屏/贴边时不会跑到屏外）。
//
// 悬停预览与点下去必须同一条路径：前者算"居中"、后者算"左对齐"的话，宽条上两种
// 结果差出好几百像素，鼠标一按整条就横跳。两条路径都走这里，位置完全一致。
//
// 返回最终的屏幕 x：窗口内的箭头要用它反算（px 被夹过之后箭头还得指向那颗按钮）。
pub(crate) unsafe fn sub_bar_place(bar: HWND, bar_w: i32, bar_h: i32, anchor_center: i32) -> i32 {
    let mhw = ANNO_MAIN_HWND.load(Ordering::Relaxed) as HWND;
    if bar.is_null() || mhw.is_null() { return 0; }
    let mut mr: RECT = std::mem::zeroed();
    if GetWindowRect(mhw, &mut mr) == 0 { return 0; }
    let (sox, soy) = sel_org();
    let (vw, vh) = sel_vp();
    // 跟随主条所在那块屏（主条已是屏幕坐标，用它的中心问工作区）
    let wa = mon_work_area(mr.left + ui::metric::TB_W / 2, mr.top + ui::metric::TB_H / 2,
        RECT { left: sox, top: soy, right: sox + vw, bottom: soy + vh });
    // 1) 居中到按钮。奇数宽度那 0.5 必须按浮点算完再截断：
    //    先整除再减会整体差 1px（151/2=75 → 683，而 682.5 截断是 682）
    let mut px = (mr.left as f32 + anchor_center as f32 - bar_w as f32 * 0.5) as i32;
    // 2) 左不许越过主条左边（宽条在这里变成"左边齐"）
    if px < mr.left { px = mr.left; }
    // 3) 夹进工作区
    let upper = (wa.right - bar_w).max(wa.left);
    if px > upper { px = upper; }
    if px < wa.left { px = wa.left; }
    // 顶边 = 主条底边。副条窗口比可见框多出顶上那 2px 箭头区，那 2px 落进主副条的缝里，
    // 所以箭头不占额外空间 —— 可见框与主条一样高
    let py = mr.bottom;
    let mut cur: RECT = std::mem::zeroed();
    GetWindowRect(bar, &mut cur);
    if cur.left != px || cur.top != py || cur.right - cur.left != bar_w || cur.bottom - cur.top != bar_h {
        SetWindowPos(bar, HWND_TOPMOST, px, py, bar_w, bar_h, SWP_NOACTIVATE);
    }
    px
}

// 某点所在显示器的工作区（屏幕坐标，不含任务栏）。拿不到显示器信息就用 fallback
pub(crate) unsafe fn mon_work_area(x: i32, y: i32, fallback: RECT) -> RECT {
    let pt = POINT { x, y };
    let mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    let mut mi: MONITORINFO = std::mem::zeroed();
    mi.cbSize = std::mem::size_of::<MONITORINFO>() as u32;
    if !mon.is_null() && GetMonitorInfoW(mon, &mut mi) != 0 { return mi.rcWork; }
    fallback
}

pub(crate) unsafe fn anno_layout() {
    let mhw = ANNO_MAIN_HWND.load(Ordering::Relaxed) as HWND;
    let shw = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
    let (sox, soy) = sel_org();
    let (vw, vh) = sel_vp();
    let vwa = RECT { left: sox, top: soy, right: sox + vw, bottom: soy + vh };
    if !mhw.is_null() {
        // OX/OY 是遮罩局部坐标，工具条定位一律在屏幕坐标里算，先加回原点
        let ox = ANNO_OX.load(Ordering::Relaxed) + sox;
        let oy = ANNO_OY.load(Ordering::Relaxed) + soy;
        let w = ANNO_W.load(Ordering::Relaxed);
        let h = ANNO_H.load(Ordering::Relaxed);
        // 定位规则：
        //   · 右侧与选区右边对齐（不是居中 —— 居中在宽选区下会把整条推到屏幕中间去）
        //   · 优先摆在选区下方，下面放不下就翻到上方，上下都不够就压在选区右下角内部
        //   · 最后夹进显示器工作区（工作区不含任务栏）
        let gap = ui::metric::SELECT_STROKE + ui::metric::SPACE_1;  // 与选框边框的间距
        let right = ox + w;
        let bottom = oy + h;
        // 工作区取"选区所在那块屏"的（MonitorFromRect + rcWork），
        // 副屏上不会拿主屏工作区去夹，工具条不会跑到另一块屏去
        let wa = mon_work_area(ox + w / 2, oy + h / 2, vwa);
        let fit_below = bottom + gap + ui::metric::TB_H <= wa.bottom;
        let fit_above = oy - gap - ui::metric::TB_H >= wa.top;
        let mut mx = right - ui::metric::TB_W;
        let my;
        if fit_below {
            my = bottom + gap;
        } else if fit_above {
            my = oy - gap - ui::metric::TB_H;
        } else {
            mx = right - ui::metric::TB_W - ui::layout::anno::TB_FALLBACK_INSET;
            my = bottom - ui::metric::TB_H - ui::layout::anno::TB_FALLBACK_INSET;
        }
        let mut mx = mx.max(wa.left);
        if mx + ui::metric::TB_W > wa.right { mx = wa.right - ui::metric::TB_W; }
        // 拖选区时 anno_layout 每帧都会被调（工具条要跟手），位置没变就别再 SetWindowPos —
        // 否则每帧一次 TOPMOST 重排 + 整条重绘，越拖越飘
        let mut cur: RECT = std::mem::zeroed();
        GetWindowRect(mhw, &mut cur);
        if cur.left != mx || cur.top != my {
            // 提到 HWND_TOPMOST：截图遮罩建好后会 SetForegroundWindow 把自己提到最上层，
            // 主条若只用 SWP_NOZORDER 就会压在那层暗遮罩下面，整条被压暗成灰色。
            // 必须带 SWP_NOACTIVATE：SetWindowPos 默认会顺手激活目标窗口，工具条一被激活
            // 就会从标注窗手里抢走前台/焦点，Ctrl+S 之类的快捷键随即全哑
            SetWindowPos(mhw, HWND_TOPMOST, mx, my, ui::metric::TB_W, ui::metric::TB_H, SWP_NOACTIVATE);
        }
    }
    if !shw.is_null() && !mhw.is_null() {
        let tool = anno_sub_tool();
        let sub_w = anno_sub_width();
        let mut mr: RECT = std::mem::zeroed();
        GetWindowRect(mhw, &mut mr);
        // 定位交给 sub_bar_place 这一处统一算：居中到"副条正指着的那颗按钮"，
        // 宽条被主条左边界推回来就是"左边齐"，窄条保持居中 —— 不再按工具写特例。
        // 预览与选中走同一条路，所以鼠标一按不会横跳
        let bidx = ANNO_BTN_TOOL.iter().position(|&x| x == tool).unwrap_or(0) as i32;
        let anchor = anno_btn_center_x(bidx);
        let px = sub_bar_place(shw, sub_w, ui::metric::SUB_H, anchor);
        // 位置被夹过之后箭头要跟着修正，继续指向那颗按钮（不是副条中心）
        let ax = (mr.left + anchor - px).clamp(ui::metric::ARROW_HALF + 1, sub_w - ui::metric::ARROW_HALF - 1);
        ANNO_SUB_ARROW.store(ax, Ordering::Relaxed);
        // 把窗口裁成"矩形 + 顶上小三角"：两角才不会露出白块。
        // GDI 没有 alpha 窗口，用窗口区域等效
        let r1 = CreateRectRgn(0, ui::metric::ARROW_ZONE, sub_w, ui::metric::SUB_H);
        let tri = [
            POINT { x: ax - ui::metric::ARROW_HALF, y: ui::metric::ARROW_ZONE },
            POINT { x: ax, y: 0 },
            POINT { x: ax + ui::metric::ARROW_HALF, y: ui::metric::ARROW_ZONE },
        ];
        let r2 = CreatePolygonRgn(tri.as_ptr(), 3, WINDING);
        CombineRgn(r1, r1, r2, RGN_OR);
        SetWindowRgn(shw, r1, 1);   // 区域交给系统，不要再 DeleteObject
        DeleteObject(r2 as *mut winapi::ctypes::c_void);
    }
}

// 标注窗获焦时会被顶到最前、盖住工具条；这里把它们再拎回 TOPMOST 最上面。
// 逐条提层：只对**可见**的那条
// 并且用 SWP_NOACTIVATE —— 只调层叠不抢焦点，键盘焦点仍留在标注窗上。
pub(crate) unsafe fn anno_raise_bars() {
    let mhw = ANNO_MAIN_HWND.load(Ordering::Relaxed) as HWND;
    if !mhw.is_null() && IsWindowVisible(mhw) != 0 {
        SetWindowPos(mhw, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    let shw = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
    if !shw.is_null() && IsWindowVisible(shw) != 0 {
        SetWindowPos(shw, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    // 录制中那条录制条就是 的 toolSub：同样要提回最上层
    // 否则宿主一获焦就会把压暗层盖到条上面（看起来"录制条被蒙了一层灰"）
    let rb = REC_BAR.load(Ordering::Relaxed) as HWND;
    if !rb.is_null() && IsWindowVisible(rb) != 0 {
        SetWindowPos(rb, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// 显示/隐藏副选项条（选中绘制工具时显示）
pub(crate) unsafe fn anno_sub_show() {
    // 录制条的设置态和普通副条占同一块位置：要换回某个工具的副条，先把录制条收掉
    // （录制中那条是控制条，rec_settings_hide 自己会跳过）
    rec_settings_hide();
    // 按钮整批销毁重建之前先把提示收掉
    // （按钮没了 onLeave 不会触发，不手动收就会挂着上一个工具的气泡）
    tip_hide(&TIP_SUB);
    ANNO_SUB_HOVER.store(-1, Ordering::Relaxed);
    let shw = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
    if shw.is_null() { return; }
    if (0..8).contains(&anno_sub_tool()) {
        // SW_SHOWNA：只显示不激活 —— 副条是工具条，焦点必须留在标注窗上
        ShowWindow(shw, SW_SHOWNA);
        anno_layout();
        InvalidateRect(shw, std::ptr::null_mut(), 0);
    } else {
        ShowWindow(shw, SW_HIDE);
    }
}

// 副条宽度：N 格开关 + 滑块（含两侧留白）+ 9 格色板。
// 一律按格子算，按钮本体的留白由格内 margin 吃掉，所以这里不必关心按钮实际多宽。
pub(crate) unsafe fn anno_sub_width() -> i32 {
    let tool = anno_sub_tool();
    let n = anno_sub_toggle_count(tool);
    // initSize 的 logicW = cell*(btnCount + 色板数) + sliderW + sliderPad*2（没有分隔条）
    let mut w = n * ui::metric::CELL + ui::metric::SLIDER_W + ui::metric::SLIDER_PAD * 2;
    if (0..=5).contains(&tool) { w += 9 * ui::metric::CELL; }
    w
}

pub(crate) unsafe fn anno_sub_client() -> (i32, i32) {
    let hwnd = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
    let mut rc: RECT = std::mem::zeroed();
    GetClientRect(hwnd, &mut rc);
    (rc.right - rc.left, rc.bottom - rc.top)
}

// 命中主工具条按钮：返回 0..13，未命中 -1（窗口本地坐标）
pub(crate) unsafe fn anno_main_hit(mx: i32, my: i32) -> i32 {
    let mut bx = 0;
    let by = (ui::metric::TB_H - ui::metric::BTN) / 2;
    for i in 0..14usize {
        if ANNO_SEP_AT.contains(&i) { bx += ui::metric::SEP; }
        if mx >= bx + ui::metric::PAD && mx < bx + ui::metric::PAD + ui::metric::BTN && my >= by && my < by + ui::metric::BTN { return i as i32; }
        bx += ui::metric::CELL;
    }
    -1
}


// ---- 主工具条过程：13+1 个按钮（8 工具 | 撤销 重做 | 关闭 贴屏 保存 复制） ----
pub(crate) extern "system" fn anno_main_proc(hwnd: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    unsafe {
        match msg {
            WM_PAINT => {
                let mut ps: PAINTSTRUCT = std::mem::zeroed();
                let hdc = BeginPaint(hwnd, &mut ps);
                let mut rc: RECT = std::mem::zeroed();
                GetClientRect(hwnd, &mut rc);
                let cw = rc.right - rc.left; let ch = rc.bottom - rc.top;
                let buf = PaintBuf::new(hdc, cw, ch);
                let memdc = buf.as_ref().map_or(hdc, |b| b.dc());
                // 白底 + 1px #A8A8A8 外框
                fill_solid(memdc, &rc, ui::color::cref(ui::color::TOOL_SURFACE));
                let bpen = CreatePen(PS_SOLID as i32, ui::metric::STROKE, ui::color::cref(ui::color::TOOL_BORDER));
                let oldbpen = SelectObject(memdc, bpen as *mut winapi::ctypes::c_void);
                let oldbbr = SelectObject(memdc, GetStockObject(NULL_BRUSH as i32));
                // 边框路径给到 (0,0,cw,ch) 而不是 (cw-1,ch-1)：GDI 的 Rectangle 把 1px 笔画在
                // **路径内侧**，(cw-1,ch-1) 会让右边框落在第 cw-2 列、下边框落在第 ch-2 行，
                // 1px stroke 覆盖
                // [0,1] 与 [cw-1,cw]（行/列同理），即第 0 行/第 ch-1 行、第 0 列/第 cw-1 列 ——
                // 与 Rectangle(0,0,cw,ch) 的落点逐像素一致
                Rectangle(memdc, 0, 0, cw, ch);
                SelectObject(memdc, oldbbr);
                SelectObject(memdc, oldbpen);
                DeleteObject(bpen as *mut winapi::ctypes::c_void);
                // 逐格排布：14 个按钮格 + 2 个分隔条格 = 450，与 ui::metric::TB_W 严格对上
                let mut bx = 0;
                let by = (ui::metric::TB_H - ui::metric::BTN) / 2;
                let sel = ANNO_TOOL.load(Ordering::Relaxed);
                let hov = ANNO_HOVER.load(Ordering::Relaxed);
                for i in 0..14usize {
                    if ANNO_SEP_AT.contains(&i) {
                        // **整高**，不是格子的 56%。
                        // 上下那两行被 body 自己的 1px 描边盖住（borderVisual 插在子节点之上），
                        // 所以可见部分正好是内容区这 32 行
                        let s_rc: RECT = RECT { left: bx, top: ui::metric::BORDER, right: bx + ui::metric::SEP, bottom: ui::metric::TB_H - ui::metric::BORDER };
                        fill_solid(memdc, &s_rc, ui::color::TOOL_SEP);
                        bx += ui::metric::SEP;
                    }
                    // 工具位按"按钮→工具"折算后再比选中；动作位永远不会亮成选中态
                    let on = (i as i32) < 7 && ANNO_BTN_TOOL[i] == sel;
                    let hv = (i as i32) == hov;
                    // 选中 > 悬停 > 常态；选中态自己也有悬停色（更深一档）
                    let bg = if on { if hv { ui::color::TOOL_HOVER_ON } else { ui::color::TOOL_PRESSED } } else if hv { ui::color::TOOL_HOVER } else { ui::color::TOOL_SURFACE };
                    let fg = if i == 10 { ui::color::ICON_DANGER } else if on { ui::color::ICON_ACTIVE } else { ui::color::ICON };
                    // 按钮本体的位置
                    // body 的 alignItems 又是 Center —— 按钮左上 = (cell+2, 3)，与网格计算一致
                    anno_blit_icon(memdc, bx + ui::metric::PAD, by, ui::metric::BTN, ANNO_BTN_ICON[i], bg, fg);
                    bx += ui::metric::CELL;
                }
                if let Some(b) = buf.as_ref() { b.present(hdc); }
                EndPaint(hwnd, &ps);
                0
            }
            WM_ERASEBKGND => 1,
            WM_LBUTTONDOWN => {
                let x = (lp & 0xFFFF) as i16 as i32; let y = ((lp >> 16) & 0xFFFF) as i16 as i32;
                let idx = anno_main_hit(x, y);
                if idx >= 0 {
                    if (0..7).contains(&idx) {
                        let tool = ANNO_BTN_TOOL[idx as usize];
                        // 再点一次当前工具 = 取消选中，副条跟着收起
                        if ANNO_TOOL.load(Ordering::Relaxed) == tool {
                            ANNO_PREVIEW.store(-1, Ordering::Relaxed);
                            ANNO_PENDING.store(-1, Ordering::Relaxed);
                            anno_kill_timers();
                            ANNO_TOOL.store(-1, Ordering::Relaxed);
                            anno_sub_show();
                            InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                            return 0;
                        }
                        anno_set_tool(tool, None);
                    } else {
                        match idx {
                            // 视频按钮 = 点击即录
                            // 设置条仍由悬停预览，不再用点击展开
                            7 => { anno_record_start(); }
                            8 => { anno_undo(); }
                            9 => { anno_redo(); }
                            10 => { anno_close(); }
                            11 => { anno_pin(); } // 贴在屏幕上：钉住图 + 解除编辑器界面
                            12 => { anno_save(); }
                            13 => { anno_copy(); }
                            _ => {}
                        }
                    }
                }
                0
            }
            WM_MOUSEMOVE => {
                let x = (lp & 0xFFFF) as i16 as i32; let y = ((lp >> 16) & 0xFFFF) as i16 as i32;
                track_leave(hwnd);
                let hov = anno_main_hit(x, y);
                if hov != ANNO_HOVER.load(Ordering::Relaxed) {
                    ANNO_HOVER.store(hov, Ordering::Relaxed);
                    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                    // 进按钮 → 排 260ms 延迟显示；移到空隙 → 取消待显示、起 300ms 延迟收起
                    if hov >= 0 { anno_main_enter(hov); } else { anno_main_leave_buttons(); }
                }
                0
            }
            WM_MOUSELEAVE => {
                if ANNO_HOVER.load(Ordering::Relaxed) != -1 {
                    ANNO_HOVER.store(-1, Ordering::Relaxed);
                    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                }
                anno_main_leave_buttons();
                0
            }
            WM_TIMER => {
                if wp == ANNO_T_SHOW {
                    // 延迟显示到期：目标期间可能又变了，也可能已经离开（pending 被清成 -1）
                    KillTimer(hwnd, ANNO_T_SHOW);
                    let target = ANNO_PENDING.swap(-1, Ordering::Relaxed);
                    if target >= 0 && target != ANNO_TOOL.load(Ordering::Relaxed) {
                        anno_preview_begin(target);
                    }
                } else if wp == ANNO_T_HIDE {
                    KillTimer(hwnd, ANNO_T_HIDE);
                    let shw = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
                    let rb = REC_BAR.load(Ordering::Relaxed) as HWND;
                    // 光标还在主条 / 副条 / 录制条里就不收：那 300ms 是给"滑过去"用的，人到位置了就别收
                    if !anno_cursor_in(hwnd) && !anno_cursor_in(shw) && !anno_cursor_in(rb) {
                        let cur = ANNO_TOOL.load(Ordering::Relaxed);
                        if cur >= 0 {
                            // 有选中工具：把 hover 预览清掉，回落显示选中工具自己的副条
                            if ANNO_PREVIEW.load(Ordering::Relaxed) >= 0 {
                                ANNO_PREVIEW.store(-1, Ordering::Relaxed);
                                anno_sub_show();
                                anno_layout();
                            }
                        } else {
                            // 没选中任何工具：纯预览态，离开就整个收起
                            ANNO_PREVIEW.store(-1, Ordering::Relaxed);
                            anno_sub_show();
                            anno_layout();
                        }
                        // 录制条的设置态同样按"离开就收"处理（录制中那条不动）
                        rec_settings_hide();
                    }
                } else if wp == TIP_TIMER {
                    // 悬停提示的延迟到点（TIP_DELAY_MS）
                    tip_timer(&TIP_MAIN);
                }
                0
            }
            WM_CLOSE => { anno_close(); 0 }
            _ => DefWindowProcW(hwnd, msg, wp, lp),
        }
    }
}
