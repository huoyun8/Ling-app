use crate::prelude::*;
use crate::*;

// 悬停预览的工具：停在主条按钮上 260ms 就预览它的副条
// 但**不改选中工具** —— 选中只由"点主条"或"在副条里动手"决定。
pub(crate) static ANNO_PREVIEW: AtomicI32 = AtomicI32::new(-1);
// 已排上延迟显示、还没到期的目标
pub(crate) static ANNO_PENDING: AtomicI32 = AtomicI32::new(-1);
// 预览态下副条显示的选项：[min, max, value, colorIndex, A, B]。
// 预览不写全局 ANNO_OPT_*，否则鼠标扫过"文字"就会把矩形的线宽改成正文字号
pub(crate) static ANNO_PREV_OPT: Mutex<[i32; 7]> = Mutex::new([0; 7]);
// 序号第二色。上面那张表按 [min,max,val,color,A,B,C]
// 排布，塞不下第二色，单独放：只有序号工具有意义，其余工具恒为 -1
pub(crate) static ANNO_OPT_COLOR2: AtomicI32 = AtomicI32::new(-1);
pub(crate) static ANNO_PREV_COLOR2: AtomicI32 = AtomicI32::new(-1);
// ---- 副选项状态：当前工具的一组选项 ----
// 每个工具自己持有几个 bool，这里压成三格，含义按工具分派
pub(crate) static ANNO_OPT_A: AtomicBool = AtomicBool::new(false);
pub(crate) static ANNO_OPT_B: AtomicBool = AtomicBool::new(false);
// 三格的键名（写盘只写主键，读盘兼容旧键）
//   矩形 rectSolid/ellipse/ellSolid | 箭头 arrowSolid/arrowStraight |
//   标号 circleSolid/square/squareSolid | 画笔 semiTransparent/rect |
//   文本 bold/italic | 马赛克 rect/blur/erase | 橡皮 rect
pub(crate) static ANNO_OPT_C: AtomicBool = AtomicBool::new(false);
pub(crate) static ANNO_OPT_COLOR: AtomicI32 = AtomicI32::new(0);     // 当前色板下标
pub(crate) static ANNO_OPT_SLIDER: AtomicI32 = AtomicI32::new(2);    // 当前滑块值（整数）
pub(crate) static ANNO_OPT_MIN: AtomicI32 = AtomicI32::new(1);       // 滑块最小值
pub(crate) static ANNO_OPT_MAX: AtomicI32 = AtomicI32::new(26);      // 滑块最大值
pub(crate) static ANNO_SLIDER_DRAG: AtomicBool = AtomicBool::new(false); // 拖动滑块中
// 副条顶部的三角箭头尖端，窗口本地 x。由 anno_layout 按主条按钮中心算出来
pub(crate) static ANNO_SUB_ARROW: AtomicI32 = AtomicI32::new(20);
/// 副条选项一变（填充/不填充、线宽、字号、颜色…）立刻联动到画布。
/// 改样式的作用对象：正在编辑的文字优先，其次「当前工具这一组的最后一个标注」
/// 拖矩形滑条不会跑去改文字，切到椭圆也不会顺手改别人的线宽。
pub(crate) unsafe fn anno_style_changed() {
    let tool = ANNO_TOOL.load(Ordering::Relaxed);
    if tool < 0 { return; }
    let ci = ANNO_OPT_COLOR.load(Ordering::Relaxed).clamp(0, 8) as usize;
    let (r, g, b) = ANNO_COLORS[ci];
    let width = ANNO_OPT_SLIDER.load(Ordering::Relaxed).max(1);
    let oa = ANNO_OPT_A.load(Ordering::Relaxed);
    let ob = ANNO_OPT_B.load(Ordering::Relaxed);
    let oc = ANNO_OPT_C.load(Ordering::Relaxed);
    let anno = ANNO_HWND.load(Ordering::Relaxed) as HWND;

    // 1) 文字工具：优先改"手上正在输入的那一段"；
    //    没有在输入时，只有画布上最后一条正好是文字（ANNO_LAST.tool == 8）才动它 ——
    if tool == 5 {
        {
            let mut gd = ANNO_TXE.lock_ok();
            if let Some(e) = gd.as_mut() {
                // 滑块是逻辑像素
                e.fs = ui::dpi::sc(width.clamp(10, 180)) as f32;
                e.fs0 = e.fs;
                e.pad = ui::dpi::sc(ui::metric::TXT_PAD);
                e.pad0 = e.pad;
                e.rgb = rgb(r as u32, g as u32, b as u32);
                e.bold = oa;
                e.italic = ob;
                drop(gd);
                if !anno.is_null() { InvalidateRect(anno, std::ptr::null_mut(), 0); }
                return;
            }
        }
        let sel = ANNO_LAST.lock_ok().clone();
        let Some(l) = sel else { return };
        if l.tool != 8 || l.idx < 0 { return; }
        let before = anno_last_rect();
        let new_box = {
            let mut sh = ANNO_SHAPES.lock_ok();
            if (l.idx as usize) >= sh.len() { return; }
            let t = &mut sh[l.idx as usize];
            // 非编辑态改字号/颜色：框按"文字 + borderPadding*2"重排
            t.fs = ui::dpi::sc(width.clamp(10, 180)) as f32;
            t.pad = ui::dpi::sc(ui::metric::TXT_PAD);
            t.r = r; t.g = g; t.b = b;
            t.bold = oa;
            t.italic = ob;
            anno_shape_text_box(t)
        };
        // 字号变了框也变了，手柄要跟着走
        if let Some(l) = ANNO_LAST.lock_ok().as_mut() {
            l.x0 = new_box.0; l.y0 = new_box.1;
            l.x1 = new_box.2; l.y1 = new_box.3;
        }
        anno_rebuild();
        anno_style_repaint(anno, before);
        return;
    }

    // 2) 画布上最后一条若不是本组工具画的，就不动它
    let cur = ANNO_LAST.lock_ok().clone();
    let Some(l) = cur else { return; };
    // 马赛克/橡皮也要联动：换算法、拖块大小/笔宽都是在改"最后这一条"，
    // 只改副条高亮不改画面的话，看着就是按钮和滑块都坏了
    let same_group = match tool {
        0 => l.tool == 0, 2 => l.tool == 2, 3 => l.tool == 3,
        4 => l.tool == 4, 6 | 7 => l.tool == tool, _ => false,
    };
    if !same_group { return; }
    // 矩形/椭圆：填充各看各的（矩形 A / 椭圆 C），当前画哪种形状看 B；
    // 序号同理（圆 A / 方 C）；箭头 A=实心、线条 A=半透明，都是各自开关直接读 A。
    // 马赛克 A=选框偏好（智能消除恒选框），橡皮只有 A
    let fill = match tool {
        0 | 3 => if ob { oc } else { oa },
        2 | 4 => oa,
        6 => oa || oc,
        7 => oa,
        _ => l.fill,
    };
    // tool 0/3 看 B 选第二副形状；tool 2 的 B = 直线
    // tool 6 的 B = 高斯模糊
    let ell = if matches!(tool, 0 | 2 | 3 | 4 | 6) { ob } else { false };
    let c2 = if tool == 3 { ANNO_OPT_COLOR2.load(Ordering::Relaxed) } else { -1 };

    let nl = if tool == 3 {
        // 序号：按新半径（滑块值）/颜色/实心/形状重画
        let cx = (l.x0 + l.x1) / 2;
        let cy = (l.y0 + l.y1) / 2;
        AnnoLast {
            tool: 3,
            x0: cx - width, y0: cy - width, x1: cx + width, y1: cy + width,
            r, g, b, width, fill, ell, c2, smart: false, val: l.val, ang: l.ang, pts: Vec::new(), idx: l.idx,
        }
    } else {
        // 马赛克/橡皮切到"手绘"而手上没有轨迹（选框画的那一笔只留了一个起点）时，
        // 用对角两点补一条，别让这一笔整个消失
        let mut pts = l.pts.clone();
        if matches!(tool, 6 | 7) && !fill && pts.len() < 2 {
            pts = vec![(l.x0, l.y0), (l.x1, l.y1)];
        }
        AnnoLast {
            tool: l.tool, x0: l.x0, y0: l.y0, x1: l.x1, y1: l.y1,
            r, g, b, width, fill, ell, c2: l.c2,
            smart: if tool == 6 { oc } else { l.smart },
            val: l.val, ang: l.ang, pts, idx: l.idx,
        }
    };
    let before = anno_last_rect();
    anno_shape_redraw(&nl);
    anno_style_repaint(anno, before);
}

/// 改样式后只刷"最后一条"那块（新旧几何的并集）。
/// 以前是整窗 InvalidateRect：点一下色板就要把背景、压暗、整张选区图
/// 重新合成一遍再贴屏，4K 下就是"点个颜色要缓一下"。
unsafe fn anno_style_repaint(anno: HWND, before: Option<(i32, i32, i32, i32)>) {
    if anno.is_null() { return; }
    let after = anno_last_rect();
    if before.is_none() && after.is_none() {
        InvalidateRect(anno, std::ptr::null_mut(), 0);
        return;
    }
    if let Some(r) = before { anno_invalidate_rect(anno, r); }
    if let Some(r) = after { anno_invalidate_rect(anno, r); }
}


// ==================== 副选项栏 ====================
// 选中一个绘制工具(0..7)后，主工具栏下方出现该工具的副选项行：
//   [开关按钮（填充/半透明/加粗/斜体/矩形）] [滑块（线宽/半径/字号/块大小）] [9 色色板]
// mosaic/eraser 无颜色按钮，text 有两个开关。
// 返回 (开关按钮几何, 滑块几何, 色板几何)
// 三个状态：pendingShow（已排延迟显示）→ preview（正在预览）→ 落到 selected（选中）。
// 定时器都挂在主工具条窗口上，不额外开线程。
pub(crate) const ANNO_T_SHOW: usize = 201;
pub(crate) const ANNO_T_HIDE: usize = 202;
pub(crate) const ANNO_SHOW_DELAY: u32 = 260;  // 停在图标上这么久才弹：横向扫过主条不跳界面
pub(crate) const ANNO_HIDE_DELAY: u32 = 300;  // 离开后留这么久，够鼠标从主条滑进副条

// 副条当前该显示哪个工具：预览优先，其次选中。-1 = 都不显示
pub(crate) fn anno_sub_tool() -> i32 {
    let p = ANNO_PREVIEW.load(Ordering::Relaxed);
    if p >= 0 { p } else { ANNO_TOOL.load(Ordering::Relaxed) }
}

// 副条的选项读取：预览态读预览副本，否则读全局（= 选中工具的选项）
pub(crate) fn anno_sub_opt(i: usize) -> i32 {
    if ANNO_PREVIEW.load(Ordering::Relaxed) >= 0 {
        ANNO_PREV_OPT.lock_ok()[i]
    } else {
        match i {
            0 => ANNO_OPT_MIN.load(Ordering::Relaxed),
            1 => ANNO_OPT_MAX.load(Ordering::Relaxed),
            2 => ANNO_OPT_SLIDER.load(Ordering::Relaxed),
            3 => ANNO_OPT_COLOR.load(Ordering::Relaxed),
            4 => if ANNO_OPT_A.load(Ordering::Relaxed) { 1 } else { 0 },
            5 => if ANNO_OPT_B.load(Ordering::Relaxed) { 1 } else { 0 },
            _ => if ANNO_OPT_C.load(Ordering::Relaxed) { 1 } else { 0 },
        }
    }
}

// 序号第二色的读取：与 anno_sub_opt 同一套（预览读预览副本）
pub(crate) fn anno_sub_color2() -> i32 {
    if ANNO_PREVIEW.load(Ordering::Relaxed) >= 0 {
        ANNO_PREV_COLOR2.load(Ordering::Relaxed)
    } else {
        ANNO_OPT_COLOR2.load(Ordering::Relaxed)
    }
}

// 工具号 → config.json toolPin 下的组名
pub(crate) fn anno_tool_cfg_id(tool: i32) -> &'static str {
    match tool {
        0 => "rect",
        2 => "arrow",
        3 => "number",
        4 => "brush",
        5 => "text",
        6 => "mosaic",
        7 => "eraser",
        _ => "",
    }
}

// 工具号 → 滑块在 config 里的键名
pub(crate) fn anno_tool_slider_key(tool: i32) -> &'static str {
    match tool {
        3 => "radius",
        5 => "fontSize",
        _ => "width",
    }
}

// 某工具的选项：[min, max, value, colorIndex, A, B, C]。
// 全部从 config.json 读
// 取不到才用默认态：矩形/椭圆 空心 | 箭头、序号 实心 | 线条 实线 | 文字 常规 |
// 马赛克 像素化+方块 | 橡皮擦 选框
pub(crate) fn anno_tool_defaults(tool: i32) -> [i32; 7] {
    let cfg = ANNO_SLIDER_CFG[tool.clamp(0, 7) as usize];
    let id = anno_tool_cfg_id(tool);
    // beginTool：sliderVal = clamp(getToolNum(id, 键, def), min, max)
    // 之后在同一会话里怎么拖都记住
    let v = if tool == 5 {
        cfg[2]
    } else {
        cfg_tool_num(id, anno_tool_slider_key(tool), cfg[2] as f32)
            .round()
            .clamp(cfg[0] as f32, cfg[1] as f32) as i32
    };
    // beginTool：selectColorIndex = getToolNum(id,"colorIndex", 0)，越界回落 0
    let ci = cfg_tool_num(id, "colorIndex", 0.0).round();
    let ci = if ci >= 0.0 && ci < 9.0 { ci as i32 } else { 0 };
    let (a, b, c) = match tool {
        0 => {
            // showRectTools：矩形填充向后兼容旧的"fill" 键，椭圆用自己的"ellFill"
            // 当前画哪个形状是独立的 "ellipse"
            let rf = cfg_tool_flag("rect", "rectSolid", cfg_tool_flag("rect", "rectFill",
                cfg_tool_flag("rect", "fill", false)));
            let ef = cfg_tool_flag("rect", "ellSolid", cfg_tool_flag("rect", "ellFill", false));
            let ell = cfg_tool_flag("rect", "ellipse", false);
            (rf, ell, ef)
        }
        // 箭头：实心（默认开）/ 直线（默认关）；序号：圆实心 / 方形 / 方实心（默认都开）
        2 => (
            cfg_tool_flag("arrow", "arrowSolid", cfg_tool_flag("arrow", "fill", true)),
            cfg_tool_flag("arrow", "arrowStraight", cfg_tool_flag("arrow", "line", false)),
            false,
        ),
        3 => {
            let solid = cfg_tool_flag("number", "fill", true);
            (
                cfg_tool_flag("number", "circleSolid", cfg_tool_flag("number", "numberSolid", solid)),
                cfg_tool_flag("number", "square", false),
                cfg_tool_flag("number", "squareSolid", solid),
            )
        }
        // 画笔：半透明（默认关）/ 选框（默认关 = 手绘）
        4 => (cfg_tool_flag("brush", "semiTransparent", false), cfg_tool_flag("brush", "rect", false), false),
        5 => (
            cfg_tool_flag("text", "bold", false),
            cfg_tool_flag("text", "italic", false),
            false,
        ),
        // 马赛克：A = 选框偏好（默认开，与算法无关），B = 模糊，C = 智能消除。
        // showMosaicTools：erase 与 blur 互斥（老配置两个都真时以 erase 为准）
        // 且智能消除只能选框 —— 否则开局就落在"拖动没反应"的状态上
        6 => {
            let e = cfg_tool_flag("mosaic", "erase", false);
            let b = if e { false } else { cfg_tool_flag("mosaic", "blur", false) };
            let r = if e { true } else { cfg_tool_flag("mosaic", "rect", true) };
            (r, b, e)
        }
        7 => (cfg_tool_flag("eraser", "rect", true), false, false),
        _ => (false, false, false),
    };
    [cfg[0], cfg[1], v, ci, if a { 1 } else { 0 }, if b { 1 } else { 0 }, if c { 1 } else { 0 }]
}

// 序号的第二色：只有序号工具有，配置里没有/越界就当作单色
pub(crate) fn anno_color2_cfg(tool: i32) -> i32 {
    if tool != 3 { return -1; }
    let v = cfg_tool_num("number", "colorIndex2", -1.0).round() as i32;
    if (0..9).contains(&v) { v } else { -1 }
}

pub(crate) unsafe fn anno_opts_store(d: [i32; 7]) {
    ANNO_OPT_MIN.store(d[0], Ordering::Relaxed);
    ANNO_OPT_MAX.store(d[1], Ordering::Relaxed);
    ANNO_OPT_SLIDER.store(d[2], Ordering::Relaxed);
    ANNO_OPT_COLOR.store(d[3], Ordering::Relaxed);
    ANNO_OPT_A.store(d[4] != 0, Ordering::Relaxed);
    ANNO_OPT_B.store(d[5] != 0, Ordering::Relaxed);
    ANNO_OPT_C.store(d[6] != 0, Ordering::Relaxed);
}

pub(crate) unsafe fn anno_kill_timers() {
    let mhw = ANNO_MAIN_HWND.load(Ordering::Relaxed) as HWND;
    if !mhw.is_null() {
        KillTimer(mhw, ANNO_T_SHOW);
        KillTimer(mhw, ANNO_T_HIDE);
    }
}

// 起 300ms 的延迟收起：这段时间里鼠标能滑进副条，到期时再看光标位置决定收不收
pub(crate) unsafe fn anno_schedule_sub_hide() {
    let mhw = ANNO_MAIN_HWND.load(Ordering::Relaxed) as HWND;
    if mhw.is_null() { return; }
    KillTimer(mhw, ANNO_T_HIDE);
    SetTimer(mhw, ANNO_T_HIDE, ANNO_HIDE_DELAY, None);
}

// 光标是否落在某个窗口的屏幕矩形内（副条是 WS_POPUP，rect 即屏幕坐标）
pub(crate) unsafe fn anno_cursor_in(hwnd: HWND) -> bool {
    if hwnd.is_null() { return false; }
    let mut r: RECT = std::mem::zeroed();
    if GetWindowRect(hwnd, &mut r) == 0 { return false; }
    let mut pt: POINT = std::mem::zeroed();
    GetCursorPos(&mut pt);
    pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom
}

// 开始预览：副条改为显示该工具，选项取该工具默认值
pub(crate) unsafe fn anno_preview_begin(tool: i32) {
    *ANNO_PREV_OPT.lock_ok() = anno_tool_defaults(tool);
    ANNO_PREV_COLOR2.store(anno_color2_cfg(tool), Ordering::Relaxed);
    ANNO_PREVIEW.store(tool, Ordering::Relaxed);
    ANNO_PENDING.store(-1, Ordering::Relaxed);
    anno_sub_show();
    anno_layout();
}

// 鼠标进入主条某颗按钮
pub(crate) unsafe fn anno_main_enter(idx: i32) {
    // 悬停提示先排上：13 颗按钮全有
    // 动作按钮也有 —— 提示和副条预览是两条独立的线
    tip_main_hover(idx);
    let cur = ANNO_TOOL.load(Ordering::Relaxed);
    // 动作按钮（撤销/重做/关闭/贴屏/保存/复制）没有副条。停上去不能挂着上一个工具的内容，
    // 那看着像副条卡住不收 —— 有选中工具就回落到它的副条，没有就整个收起
    if !(0..7).contains(&idx) {
        anno_kill_timers();
        ANNO_PENDING.store(-1, Ordering::Relaxed);
        if idx == 7 {
            // 录制（第 8 格）没在 0..7 的绘制工具里，它自己那条录制条就是它的副条。
            // 悬停上来跟别的工具一样把副条弹出来（别的工具的内容同时收掉），
            // 移开后由 300ms 定时器收回 —— 与矩形那套显隐规则完全一致
            ANNO_PREVIEW.store(-1, Ordering::Relaxed);
            let shw = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
            if !shw.is_null() && IsWindowVisible(shw) != 0 { ShowWindow(shw, SW_HIDE); }
            if !rec_settings_visible() { anno_record_open(); }
            return;
        }
        if ANNO_PREVIEW.load(Ordering::Relaxed) >= 0 {
            ANNO_PREVIEW.store(-1, Ordering::Relaxed);
            anno_sub_show();
            anno_layout();
        }
        return;
    }
    let tool = ANNO_BTN_TOOL[idx as usize];
    // 移回当前选中那颗：清掉预览，回落显示它自己的副条
    if tool == cur {
        // 刚在"录制"上把副条收掉的话，移回来要能重新摆出来（preview/pending 这时都是空的）
        let shw = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
        let hidden = shw.is_null() || IsWindowVisible(shw) == 0;
        if ANNO_PREVIEW.load(Ordering::Relaxed) >= 0 || ANNO_PENDING.load(Ordering::Relaxed) >= 0 || hidden {
            ANNO_PREVIEW.store(-1, Ordering::Relaxed);
            ANNO_PENDING.store(-1, Ordering::Relaxed);
            anno_kill_timers();
            anno_sub_show();
            anno_layout();
        }
        return;
    }
    // 正预览的就是它：不重复排
    if tool == ANNO_PREVIEW.load(Ordering::Relaxed) {
        let mhw = ANNO_MAIN_HWND.load(Ordering::Relaxed) as HWND;
        if !mhw.is_null() { KillTimer(mhw, ANNO_T_SHOW); }
        ANNO_PENDING.store(-1, Ordering::Relaxed);
        return;
    }
    // 记下目标、起 260ms 延迟：只是扫过（每颗停留更短）的话永远不会真正弹出
    ANNO_PENDING.store(tool, Ordering::Relaxed);   // 存工具号，定时器到期直接预览
    anno_kill_timers();
    let mhw = ANNO_MAIN_HWND.load(Ordering::Relaxed) as HWND;
    if !mhw.is_null() { SetTimer(mhw, ANNO_T_SHOW, ANNO_SHOW_DELAY, None); }
}

// 鼠标离开主条按钮（含移到按钮之间的空隙）：取消待显示，重新起延迟收起
pub(crate) unsafe fn anno_main_leave_buttons() {
    tip_hide(&TIP_MAIN);          // onLeave → tip->hide(btn)，离开立刻收
    let mhw = ANNO_MAIN_HWND.load(Ordering::Relaxed) as HWND;
    if !mhw.is_null() { KillTimer(mhw, ANNO_T_SHOW); }
    ANNO_PENDING.store(-1, Ordering::Relaxed);
    anno_schedule_sub_hide();
}

//   开关按钮（每颗占一格 cell）→ 滑块（80 + 两侧 3）→ 9 色板（每颗一格）
// 内容区从 ui::metric::ARROW_ZONE 起、整整 34 高（与主条同口径），按钮 28 在里居中 → 上沿 2+3 = 5。
// 返回：(开关 x/y/w/h/图标, 滑块 x/y/w/h, 色板 x/y/w/h/下标)
pub(crate) unsafe fn anno_sub_geom(_cw: i32) -> (Vec<(i32, i32, i32, i32, &'static str)>, (i32, i32, i32, i32), Vec<(i32, i32, i32, i32, usize)>) {
    let tool = anno_sub_tool();
    let cy = ui::layout::anno::SUB_BTN_Y;   // 5
    let n = anno_sub_toggle_count(tool);
    let mut toggles = Vec::new();
    let mut x = 0;
    for k in 0..n {
        toggles.push((x, cy, ui::metric::BTN, ui::metric::BTN, anno_sub_icon(k)));
        x += ui::metric::CELL;
    }
    // 滑块撑满内容区高度（setHeightPercent(100) → 34），命中区/居中都以它为准
    let slider = (x + ui::metric::SLIDER_PAD, ui::metric::ARROW_ZONE, ui::metric::SLIDER_W, ui::metric::CELL + ui::metric::BORDER * 2);
    x += ui::metric::SLIDER_W + ui::metric::SLIDER_PAD * 2;
    let mut colors = Vec::new();
    // 马赛克/橡皮擦没有颜色（图形用不到颜色）
    if (0..=5).contains(&tool) {
        for i in 0..9 {
            colors.push((x + i * ui::metric::CELL, cy, ui::metric::BTN, ui::metric::BTN, i as usize));
        }
    }
    (toggles, slider, colors)
}

// 每个工具副条上的开关数
//   矩形 2（矩形/椭圆）| 箭头 2（箭头/直线）| 标号 2（圆/方）| 画笔 2（半透明/选框）
//   文本 2（加粗/斜体）| 马赛克 4（马赛克/模糊/智能消除/形状）| 橡皮 1（形状）
pub(crate) unsafe fn anno_sub_toggle_count(tool: i32) -> i32 {
    match tool { 0 | 2..=5 => 2, 6 => 4, 7 => 1, _ => 0 }
}

// 第 k 颗开关当前该显示的图标。实心/空心两套随状态切换
pub(crate) unsafe fn anno_sub_icon(k: i32) -> &'static str {
    let tool = anno_sub_tool();
    let a = anno_sub_opt(4) != 0;
    let b = anno_sub_opt(5) != 0;
    let c = anno_sub_opt(6) != 0;
    match tool {
        // 矩形 / 椭圆：各自记自己的填充态
        0 => if k == 0 {
            if a { "shape-rect-solid" } else { "shape-rect-outline" }
        } else if c { "shape-ellipse-solid" } else { "shape-ellipse-outline" },
        // 箭头 / 直线：两块是"画哪种"，箭头那颗的实心态独立于直线
        2 => if k == 0 {
            if a { "shape-arrow-solid" } else { "shape-arrow-outline" }
        } else { "shape-line" },
        // 标号：圆 / 方各记各的实心态
        3 => if k == 0 {
            if a { "shape-number-circle-solid" } else { "shape-number-circle-outline" }
        } else if c { "shape-number-square-solid" } else { "shape-number-square-outline" },
        // 画笔：flag=半透明 对应 tool-line（makeToggleBtn(ToolLine, ToolLineSolid) 的实参顺序一致）
        4 => if k == 0 {
            if a { "tool-line" } else { "tool-line-solid" }
        } else if b { "shape-rect-solid" } else { "shape-circle-solid" },
        5 => if k == 0 { "text-bold" } else { "text-italic" },
        // 马赛克：前三个是算法（静态图标）
        // 第四颗是"选框/手绘"（形状与算法无关，只要不是智能消除就跟着 A 走）
        6 => match k {
            0 => "tool-mosaic",
            1 => "blur-brush",
            2 => "tool-smart-erase",
            _ => if a { "shape-rect-solid" } else { "shape-circle-solid" },
        },
        7 => if a { "shape-rect-solid" } else { "shape-circle-solid" },
        _ => "tool-rect",
    }
}

// 第 i 颗开关是否点亮。
// 矩形/椭圆、箭头/直线、标号圆/方 是"当前是哪一种"而不是"开关打开"，
// 所以点亮的永远是当前那种（syncRectEll 里 applyToggleStyle(btn, !isEllipseShape)）
// 马赛克的形状那颗恒亮（applyToggleStyle(effectShapeBtn, true)），智能消除时置灰。
pub(crate) unsafe fn anno_sub_flag(i: usize) -> bool {
    let tool = anno_sub_tool();
    let a = anno_sub_opt(4) != 0;
    let b = anno_sub_opt(5) != 0;
    let c = anno_sub_opt(6) != 0;
    match tool {
        0 | 2 | 3 => if i == 0 { !b } else { b },
        6 => match i {
            0 => !b && !c,
            1 => b,
            2 => c,
            _ => true,
        },
        7 => a,
        _ => if i == 0 { a } else { b },
    }
}

// 智能消除时"形状"那颗置灰（setBg(0)/setHoverBg(0)/color 0xAAAAAA）
pub(crate) unsafe fn anno_sub_toggle_disabled(i: usize) -> bool {
    anno_sub_tool() == 6 && i == 3 && anno_sub_opt(6) != 0
}

// 绘制副选项行（在 WM_PAINT 的 memdc 上，主工具栏已画完）
pub(crate) unsafe fn anno_draw_sub(dst: winapi::shared::windef::HDC, cw: i32) {
    let (toggles, slider, colors) = anno_sub_geom(cw);
    let hov = ANNO_SUB_HOVER.load(Ordering::Relaxed);
    // 开关按钮：白底/淡灰底
    for (i, (tx, ty, _tw, _th, icon)) in toggles.iter().enumerate() {
        let on = anno_sub_flag(i);
        let dis = anno_sub_toggle_disabled(i);
        let hov_i = hov == TIP_TGL + i as i32;
        let bg = if dis { ui::color::TOOL_SURFACE } else if on { if hov_i { ui::color::TOOL_HOVER_ON } else { ui::color::TOOL_PRESSED } }
                 else if hov_i { ui::color::TOOL_HOVER } else { ui::color::TOOL_SURFACE };
        let fg = if dis { ui::color::ICON_DISABLED } else if on { ui::color::ICON_ACTIVE } else { ui::color::ICON };
        // 按钮本体左上 = (格子 + pad, 内容区顶 + 居中余量) = (cell+2, 5)
        anno_blit_icon(dst, *tx + ui::metric::PAD, *ty, ui::metric::BTN, *icon, bg, fg);
    }
    //   几何（Slider::syncVisual）：轨道厚 1（圆角 0.5，就是一条压在两个像素中间的线），
    //   起点 x = r = 3，长度 len = w - 2r = 74；圆点半径 3，中心 cx = 3 + ratio*74；
    //   垂直中心 = 内容区高度的一半 = 34/2，落进副条窗口坐标就是 2 + 17 = 19 的**上半个像素**。
    //   半像素位置 GDI 画不出来（会整行糊成 136），所以轨道和圆点都走覆盖率混色
    let (sx, sy, sw, _sh) = slider;
    let v = anno_sub_opt(2);
    let mn = anno_sub_opt(0).max(0);
    let mx = anno_sub_opt(1).max(mn + 1);
    let f = ((v - mn) as f32 / (mx - mn) as f32).clamp(0.0, 1.0);
    let cy = sy as f32 + ui::layout::anno::SLIDER_CY; // 副条窗口坐标：内容区 2..36（34 行）的中心 19
    let (tr, tg, tb) = ui::color::rgb8(ui::color::SLIDER_TRACK);
    let (fr, fg_, fb) = ui::color::rgb8(ui::color::SLIDER_FILL);
    let tx0 = sx as f32 + ui::layout::anno::SLIDER_DOT_R;
    let tx1 = tx0 + (sw as f32 - 2.0 * ui::layout::anno::SLIDER_DOT_R);
    let tcx = tx0 + (sw as f32 - 2.0 * ui::layout::anno::SLIDER_DOT_R) * f;
    let tkh = ui::layout::anno::SLIDER_TRACK_HALF;
    // 底轨：1px 高的胶囊（两端各一个半径 0.5 的半圆）
    {
        let (x0, y0) = ((tx0 - 1.0).floor() as i32, (cy - tkh - 1.0).floor() as i32);
        let (w, h) = ((tx1 + 2.0).ceil() as i32 - x0, (cy + tkh + 1.0).ceil() as i32 - y0);
        let m = cov_supersample(x0, y0, w, h, &|x, y| {
            (x >= tx0 && x <= tx1 && (y - cy).abs() <= tkh)
                || (x - tx0).hypot(y - cy) <= tkh
                || (x - tx1).hypot(y - cy) <= tkh
        });
        anno_blend_cov(dst, x0, y0, &m, w, h, tr, tg, tb);
    }
    // 已选段与底轨同起点同色同高，只是长度到圆点中心 —— 同一个颜色，看不看得见都一样
    {
        let (x0, y0) = ((tx0 - 1.0).floor() as i32, (cy - tkh - 1.0).floor() as i32);
        let (w, h) = ((tcx + 2.0).ceil() as i32 - x0, (cy + tkh + 1.0).ceil() as i32 - y0);
        if w > 0 {
            let m = cov_supersample(x0, y0, w, h, &|x, y| {
                (x >= tx0 && x <= tcx && (y - cy).abs() <= tkh)
                    || (x - tx0).hypot(y - cy) <= tkh
                    || (x - tcx).hypot(y - cy) <= tkh
            });
            anno_blend_cov(dst, x0, y0, &m, w, h, fr, fg_, fb);
        }
    }
    // 圆点：半径 3 的实心圆
    {
        let dr = ui::layout::anno::SLIDER_DOT_R;
        let (x0, y0) = ((tcx - dr - 1.0).floor() as i32, (cy - dr - 1.0).floor() as i32);
        let (w, h) = ((tcx + dr + 2.0).ceil() as i32 - x0, (cy + dr + 2.0).ceil() as i32 - y0);
        let m = cov_supersample(x0, y0, w, h, &|x, y| (x - tcx).hypot(y - cy) <= dr);
        anno_blend_cov(dst, x0, y0, &m, w, h, fr, fg_, fb);
    }
    // 只有最后那颗白块带 1px #A8A8A8 描边，其余一律无描边：
    // 给每颗都描白边会把色块吃掉一圈，红色只剩 11×11。
    // 位置：按钮本体 pad(2) 起、28 见方，色块再在按钮里居中 →
    //       左上 = cell + 2 + (28-12.8)/2 = cell + 9.6；
    // 背景/描边走的是浮点几何（12.8 的盒子），墨迹等效于
    //       **13 边长、落在 cell+10.05** 的浮点圆角矩形 —— 差这 0.05，
    //       四角那颗像素的红色覆盖率就从 0.19 掉到 0.32，肉眼是一圈"方角"。
    //       所以这里按浮点盒子 + 4×4 超采样画，圆角处才出得来渐变色
    let sw2 = ui::layout::anno::SWATCH;
    let rad = ui::layout::anno::SWATCH_RAD;
    let (br_, bg_, bb) = ui::color::rgb8(ui::color::TOOL_BORDER);
    let cur = anno_sub_opt(3);
    // 序号双色：两格都打勾
    let cur2 = anno_sub_color2();
    for (cx, cy2, _w, _h, idx) in colors.iter() {
        // hover 底色
        if hov == TIP_CLR + *idx as i32 {
            let hb = CreateSolidBrush(ui::color::TOOL_HOVER);
            let mut hrc: RECT = RECT {
                left: cx + ui::metric::PAD, top: cy2 + ui::metric::PAD,
                right: cx + ui::metric::PAD + ui::metric::BTN, bottom: cy2 + ui::metric::PAD + ui::metric::BTN,
            };
            FillRect(dst, &mut hrc, hb);
            DeleteObject(hb as *mut winapi::ctypes::c_void);
        }
        let (r, g, b) = ANNO_COLORS[*idx];
        // 布局值：按钮 28 在格里居中，色块 12.8 在按钮里居中 → 左上 = cell + 2 + 7.6 = cell + 9.6、
        // 内容区顶 2 + (34-12.8)/2 = 12.6，的背景/描边走浮点几何，实测墨迹等效于
        // 13 边长、落在上面再 +0.45 处 —— 差这 0.05，四角那颗像素的覆盖率就从 0.19 掉到 0.32
        let lxf = *cx as f32 + ui::layout::anno::SWATCH_X;
        let lyf = *cy2 as f32 + ui::layout::anno::SWATCH_Y;   // cy2 = 5（按钮顶）→ 13.05，与内容区顶 2 + 10.6 + 0.45 同值
        let mx = lxf.floor() as i32;
        let my = lyf.floor() as i32;
        let mw = (lxf + sw2).ceil() as i32 - mx;
        let mh = (lyf + sw2).ceil() as i32 - my;
        let m = cov_supersample(mx, my, mw, mh, &|x, y| {
            let ix = x.clamp(lxf + rad, lxf + sw2 - rad);
            let iy = y.clamp(lyf + rad, lyf + sw2 - rad);
            (x - ix).hypot(y - iy) <= rad
        });
        anno_blend_cov(dst, mx, my, &m, mw, mh, r, g, b);
        // 于是外沿贴 13×13、中线半径 = 2 - 0.5 = 1.5。取"外圈减内圈"得到这一圈 1px
        if *idx == 8 {
            let m2 = cov_supersample(mx, my, mw, mh, &|x, y| {
                let inr = |rr: f32, x: f32, y: f32| {
                    let ix = x.clamp(lxf + rr, lxf + sw2 - rr);
                    let iy = y.clamp(lyf + rr, lyf + sw2 - rr);
                    (x - ix).hypot(y - iy) <= rr
                };
                inr(rad, x, y) && !inr(rad - ui::layout::anno::SWATCH_BORDER, x, y)
            });
            anno_blend_cov(dst, mx, my, &m2, mw, mh, br_, bg_, bb);
        }
        if *idx as i32 == cur || (cur2 >= 0 && *idx as i32 == cur2) {
            // 图标 surface 的边长按 (int)12.8 = 12 算，于是盒内偏移 = round((12-8)/2) = 2。
            // 不能用 2px 折线按色块百分比画：笔画粗得把整块糊住
            let side = ui::layout::anno::SWATCH_CHECK;
            if let Some(mask) = svg_mask_cached("ui-check", side) {
                let ox = mx + ui::layout::anno::SWATCH_CHECK_OFF;
                let oy = my + ui::layout::anno::SWATCH_CHECK_OFF;
                let ck = if *idx == 8 { ui::color::rgb8(ui::color::ICON) } else { ui::color::rgb8(ui::color::TEXT_INVERSE) };
                anno_blend_cov(dst, ox, oy, &mask, side, side, ck.0, ck.1, ck.2);
            }
        }
    }
}

// 处理副选项行上的点击（开关/色板/滑块按下）
// 返回 true 表示已消费（不再走主工具栏/画布逻辑）
pub(crate) unsafe fn anno_sub_hit_down(x: i32, y: i32, cw: i32) -> bool {
    let (toggles, slider, colors) = anno_sub_geom(cw);
    // 命中什么先判定清楚，再"提升"——提升会把预览那套选项落到全局，
    // 之后读写 ANNO_OPT_* 才是这个工具的选项
    let mut hit_toggle: i32 = -1;
    for (i, (tx, ty, tw, th, _)) in toggles.iter().enumerate() {
        if x >= *tx && x < tx + tw && y >= *ty && y < ty + th { hit_toggle = i as i32; break; }
    }
    let (hx, hy, hw2, hh) = slider;
    let hit_slider = x >= hx && x <= hx + hw2 && y >= hy && y < hy + hh;
    let mut hit_color: i32 = -1;
    for (cx, cy, cw2, ch2, idx) in colors.iter() {
        if x >= *cx && x < cx + cw2 && y >= *cy && y < cy + ch2 { hit_color = *idx as i32; break; }
    }
    if hit_toggle < 0 && !hit_slider && hit_color < 0 { return false; }
    anno_sub_promote();
    {
        let idx = hit_toggle;
        if idx >= 0 {
            let i = idx as usize;
            let tool = ANNO_TOOL.load(Ordering::Relaxed);
            let a = ANNO_OPT_A.load(Ordering::Relaxed);
            let b = ANNO_OPT_B.load(Ordering::Relaxed);
            let c = ANNO_OPT_C.load(Ordering::Relaxed);
            match tool {
                // 矩形/椭圆两段式：点另一种形状只换形状不动填充
                // 点当前形状才切实心/空心 —— 否则从椭圆切回矩形会顺手把矩形填充翻一下。
                // 两个填充态各记各的键（rectSolid / ellSolid），形状记 ellipse
                0 => {
                    if i == 0 {
                        if b {
                            ANNO_OPT_B.store(false, Ordering::Relaxed);
                            cfg_set_tool_flag("rect", "ellipse", false);
                        } else {
                            let na = !a;
                            ANNO_OPT_A.store(na, Ordering::Relaxed);
                            cfg_set_tool_flag("rect", "rectSolid", na);
                        }
                    } else if !b {
                        ANNO_OPT_B.store(true, Ordering::Relaxed);
                        cfg_set_tool_flag("rect", "ellipse", true);
                    } else {
                        let nc = !c;
                        ANNO_OPT_C.store(nc, Ordering::Relaxed);
                        cfg_set_tool_flag("rect", "ellSolid", nc);
                    }
                }
                // 箭头/直线：同样两段式
                2 => {
                    if i == 0 {
                        if b {
                            ANNO_OPT_B.store(false, Ordering::Relaxed);
                            cfg_set_tool_flag("arrow", "arrowStraight", false);
                        } else {
                            let na = !a;
                            ANNO_OPT_A.store(na, Ordering::Relaxed);
                            cfg_set_tool_flag("arrow", "arrowSolid", na);
                        }
                    } else if !b {
                        ANNO_OPT_B.store(true, Ordering::Relaxed);
                        cfg_set_tool_flag("arrow", "arrowStraight", true);
                    }
                }
                // 标号：圆/方两块，各自记实心态（circleSolid / squareSolid），当前形状记 square
                3 => {
                    if i == 0 {
                        if b {
                            ANNO_OPT_B.store(false, Ordering::Relaxed);
                            cfg_set_tool_flag("number", "square", false);
                        } else {
                            let na = !a;
                            ANNO_OPT_A.store(na, Ordering::Relaxed);
                            cfg_set_tool_flag("number", "circleSolid", na);
                        }
                    } else if !b {
                        ANNO_OPT_B.store(true, Ordering::Relaxed);
                        cfg_set_tool_flag("number", "square", true);
                    } else {
                        let nc = !c;
                        ANNO_OPT_C.store(nc, Ordering::Relaxed);
                        cfg_set_tool_flag("number", "squareSolid", nc);
                    }
                }
                // 画笔：半透明 / 选框（两组都是普通开关）
                4 => {
                    if i == 0 {
                        let na = !a;
                        ANNO_OPT_A.store(na, Ordering::Relaxed);
                        cfg_set_tool_flag("brush", "semiTransparent", na);
                    } else {
                        let nb = !b;
                        ANNO_OPT_B.store(nb, Ordering::Relaxed);
                        cfg_set_tool_flag("brush", "rect", nb);
                    }
                }
                5 => {
                    if i == 0 {
                        let na = !a;
                        ANNO_OPT_A.store(na, Ordering::Relaxed);
                        cfg_set_tool_flag("text", "bold", na);
                    } else {
                        let nb = !b;
                        ANNO_OPT_B.store(nb, Ordering::Relaxed);
                        cfg_set_tool_flag("text", "italic", nb);
                    }
                }
                // 马赛克：前三个是互斥的算法（马赛克/模糊/智能消除），第四颗是形状。
                // A 恒为"选框"偏好（与算法无关），B = 模糊，C = 智能消除
                6 => {
                    match i {
                        0 => {
                            ANNO_OPT_B.store(false, Ordering::Relaxed);
                            ANNO_OPT_C.store(false, Ordering::Relaxed);
                            cfg_set_tool_flag("mosaic", "blur", false);
                            cfg_set_tool_flag("mosaic", "erase", false);
                        }
                        1 => {
                            ANNO_OPT_B.store(true, Ordering::Relaxed);
                            ANNO_OPT_C.store(false, Ordering::Relaxed);
                            cfg_set_tool_flag("mosaic", "blur", true);
                            cfg_set_tool_flag("mosaic", "erase", false);
                        }
                        2 => {
                            ANNO_OPT_B.store(false, Ordering::Relaxed);
                            ANNO_OPT_C.store(true, Ordering::Relaxed);
                            cfg_set_tool_flag("mosaic", "blur", false);
                            cfg_set_tool_flag("mosaic", "erase", true);
                            // 智能消除没有"手绘"，形状强制回选框并落盘
                            if !a {
                                ANNO_OPT_A.store(true, Ordering::Relaxed);
                                cfg_set_tool_flag("mosaic", "rect", true);
                            }
                        }
                        _ => {
                            // 智能消除时那颗置灰
                            if !c {
                                let na = !a;
                                ANNO_OPT_A.store(na, Ordering::Relaxed);
                                cfg_set_tool_flag("mosaic", "rect", na);
                            }
                        }
                    }
                }
                7 => {
                    let na = !a;
                    ANNO_OPT_A.store(na, Ordering::Relaxed);
                    cfg_set_tool_flag("eraser", "rect", na);
                }
                _ => { ANNO_OPT_A.store(!a, Ordering::Relaxed); }
            }
            let hw = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
            if !hw.is_null() { InvalidateRect(hw, std::ptr::null_mut(), 0); }
        } else if hit_slider {
            let (sx, _sy, sw, _sh) = slider;
            // 马赛克/橡皮改的是像素效果：先把"改之前"的底图准备好，
            // 整段拖动都复用它，每移动一格只重烘最后一条
            let t = ANNO_TOOL.load(Ordering::Relaxed);
            if (t == 6 || t == 7)
                && ANNO_LAST.lock_ok().as_ref().map_or(false, |l| l.tool == t) {
                anno_base_ensure();
            }
            let mn = ANNO_OPT_MIN.load(Ordering::Relaxed).max(0);
            let mx = ANNO_OPT_MAX.load(Ordering::Relaxed).max(mn + 1);
            // Slider::onDown → setValue(posToValue)：轨道两端各让开一个圆点半径 r=3
            // 行程 = w - 2r。点轨道任意处滑块直接跳过去（标准滑块行为）
            let r = ui::layout::anno::SLIDER_DOT_R;
            let f = ((x - sx) as f32 - r) / (sw as f32 - 2.0 * r);
            let f = f.clamp(0.0, 1.0);
            let v = (mn as f32 + f * (mx - mn) as f32).round() as i32;
            let v = v.clamp(mn, mx);
            // Slider::onValueChanged：值一变就落盘
            if v != ANNO_OPT_SLIDER.load(Ordering::Relaxed) {
                ANNO_OPT_SLIDER.store(v, Ordering::Relaxed);
                cfg_set_tool_num(
                    anno_tool_cfg_id(ANNO_TOOL.load(Ordering::Relaxed)),
                    anno_tool_slider_key(ANNO_TOOL.load(Ordering::Relaxed)),
                    v as f32,
                );
            }
            ANNO_SLIDER_DRAG.store(true, Ordering::Relaxed);
            let hw = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
            if !hw.is_null() { InvalidateRect(hw, std::ptr::null_mut(), 0); }
        } else {
            // 序号是双色的：点一格 = 把它设为第二色，原来的第一色挪到"另一色"；
            // 再点已亮的那格 = 取消双色回到单色
            if ANNO_TOOL.load(Ordering::Relaxed) == 3 {
                let c1 = ANNO_OPT_COLOR.load(Ordering::Relaxed);
                let c2 = ANNO_OPT_COLOR2.load(Ordering::Relaxed);
                let (mut n1, mut n2) = (c1, c2);
                if hit_color == c1 || hit_color == c2 {
                    if c2 >= 0 {
                        if hit_color == c1 { n1 = c2; }
                        n2 = -1;
                    }
                } else {
                    if c2 >= 0 { n1 = c2; }
                    n2 = hit_color;
                }
                ANNO_OPT_COLOR.store(n1, Ordering::Relaxed);
                ANNO_OPT_COLOR2.store(n2, Ordering::Relaxed);
                cfg_set_tool_num("number", "colorIndex", n1 as f32);
                cfg_set_tool_num("number", "colorIndex2", n2 as f32);
            } else {
                ANNO_OPT_COLOR.store(hit_color, Ordering::Relaxed);
                // onColorSelect：选中的颜色下标记到当前工具名下
                cfg_set_tool_num(
                    anno_tool_cfg_id(ANNO_TOOL.load(Ordering::Relaxed)),
                    "colorIndex",
                    hit_color as f32,
                );
            }
            let hw = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
            if !hw.is_null() { InvalidateRect(hw, std::ptr::null_mut(), 0); }
        }
    }
    // 选项落定后立刻联动画布：开关换填充/换形状、滑块改线宽字号、点色板换颜色，
    // 都当场体现在「当前工具这一组的最后一个标注」上。
    // 只重画副条、不通知画布会让画面停在旧值
    anno_style_changed();
    true
}

// 副选项行上的鼠标移动：悬停提示 + 拖滑块时更新值
pub(crate) unsafe fn anno_sub_mousemove(x: i32, y: i32, cw: i32) {
    // 滑块上时提示跟着鼠标横向走，显示鼠标底下那个位置的值；离开只收副条这一个气泡
    let hov = anno_sub_hit_hover(x, y, cw);
    if hov != ANNO_SUB_HOVER.load(Ordering::Relaxed) {
        ANNO_SUB_HOVER.store(hov, Ordering::Relaxed);
        // 副条也有 hover 底色
        let hw = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
        if !hw.is_null() { InvalidateRect(hw, std::ptr::null_mut(), 0); }
    }
    if hov >= 0 { tip_sub_hover(hov, x, y); } else { tip_hide(&TIP_SUB); }
    if !ANNO_SLIDER_DRAG.load(Ordering::Relaxed) { return; }
    let (_, slider, _) = anno_sub_geom(cw);
    let (sx, _, sw, _) = slider;
    let mn = ANNO_OPT_MIN.load(Ordering::Relaxed).max(0);
    let mx = ANNO_OPT_MAX.load(Ordering::Relaxed).max(mn + 1);
    // Slider::posToValue：轨道两端各让开一个圆点半径 r=3，行程 = w - 2r
    // 不能按整条滑块宽换算：圆点半径要扣掉
    let r = ui::layout::anno::SLIDER_DOT_R;
    let f = ((x - sx) as f32 - r) / (sw as f32 - 2.0 * r);
    let f = f.clamp(0.0, 1.0);
    let v = (mn as f32 + f * (mx - mn) as f32).round() as i32;
    let v = v.clamp(mn, mx);
    // 值真变了才落盘
    let changed = v != ANNO_OPT_SLIDER.load(Ordering::Relaxed);
    if changed {
        ANNO_OPT_SLIDER.store(v, Ordering::Relaxed);
        cfg_set_tool_num(
            anno_tool_cfg_id(ANNO_TOOL.load(Ordering::Relaxed)),
            anno_tool_slider_key(ANNO_TOOL.load(Ordering::Relaxed)),
            v as f32,
        );
    }
    let hw = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
    if !hw.is_null() { InvalidateRect(hw, std::ptr::null_mut(), 0); }
    // 拖滑条时画布上最后一条跟着变粗/变细
    if changed { anno_style_changed(); }
}

// 选中工具时载入该工具的选项，并显示副选项条
// 真正切换选中工具。opts=None 用该工具默认值（直接点主条）；
// opts=Some 调用方给的值（用户在副条上把"预览"提升成"选中"时，值就是刚调好的那些）
pub(crate) unsafe fn anno_set_tool(tool: i32, opts: Option<[i32; 7]>) {
    // 切走工具前先收笔：编辑中的那条落地成图元，光标定时器一并停掉
    anno_tx_finish();
    ANNO_PREVIEW.store(-1, Ordering::Relaxed);
    ANNO_PENDING.store(-1, Ordering::Relaxed);
    anno_kill_timers();
    ANNO_TOOL.store(tool, Ordering::Relaxed);
    let mut d = opts.unwrap_or_else(|| anno_tool_defaults(tool));
    // show*Tools(selecting=true)：点选马赛克/橡皮擦时一律先给"选框"并落盘
    // 要手绘再点一下形状按钮；悬停预览（opts=Some 那条路）不碰它
    if opts.is_none() {
        match tool {
            6 => {
                d[4] = 1;
                cfg_set_tool_flag("mosaic", "rect", true);
            }
            7 => {
                d[4] = 1;
                cfg_set_tool_flag("eraser", "rect", true);
            }
            _ => {}
        }
    }
    anno_opts_store(d);
    ANNO_OPT_COLOR2.store(anno_color2_cfg(tool), Ordering::Relaxed);
    anno_sub_show();
    anno_layout();
    let mhw = ANNO_MAIN_HWND.load(Ordering::Relaxed) as HWND;
    if !mhw.is_null() { InvalidateRect(mhw, std::ptr::null_mut(), 0); }
}

// 用户在副条上做了实质操作：把 hover 预览"提升"为选中。
// 之后鼠标离开主条会回落到这个工具的副条，而不是把它整个收掉
pub(crate) unsafe fn anno_sub_promote() {
    let p = ANNO_PREVIEW.load(Ordering::Relaxed);
    if p < 0 { return; }
    let d = *ANNO_PREV_OPT.lock_ok();
    anno_set_tool(p, Some(d));
}

// 布局：主工具条定位在亮区（选区）下方（水平居中），副选项条在主工具条下方
// 选区（截图框）命中检测：返回调整模式

// ---- 副选项条过程：开关按钮 / 滑块 / 9 色色板 ----
pub(crate) extern "system" fn anno_sub_proc(hwnd: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    unsafe {
        match msg {
            // 背景刷为空，明确挡掉系统擦背景（否则首次显示可能先露一帧黑）
            WM_ERASEBKGND => 1,
            WM_PAINT => {
                let mut ps: PAINTSTRUCT = std::mem::zeroed();
                let hdc = BeginPaint(hwnd, &mut ps);
                let mut rc: RECT = std::mem::zeroed();
                GetClientRect(hwnd, &mut rc);
                let cw = rc.right - rc.left; let ch = rc.bottom - rc.top;
                let buf = PaintBuf::new(hdc, cw, ch);
                let memdc = buf.as_ref().map_or(hdc, |b| b.dc());
                // 白底 + 带三角的边框，三角指向主条上那颗按钮
                let ax = ANNO_SUB_ARROW.load(Ordering::Relaxed);
                let top = ui::metric::ARROW_ZONE;
                let pts = [
                    POINT { x: 0, y: top },
                    POINT { x: (ax - ui::metric::ARROW_HALF).max(1), y: top },
                    POINT { x: ax, y: 0 },
                    POINT { x: (ax + ui::metric::ARROW_HALF).min(cw - 1), y: top },
                    POINT { x: cw, y: top },
                    POINT { x: cw, y: ch },
                    POINT { x: 0, y: ch },
                ];
                let wb = CreateSolidBrush(ui::color::cref(ui::color::TOOL_SURFACE));
                let bp = CreatePen(PS_SOLID as i32, ui::metric::STROKE, ui::color::cref(ui::color::TOOL_BORDER));
                let ob = SelectObject(memdc, wb as *mut winapi::ctypes::c_void);
                let op = SelectObject(memdc, bp as *mut winapi::ctypes::c_void);
                Polygon(memdc, pts.as_ptr(), 7);
                SelectObject(memdc, op);
                SelectObject(memdc, ob);
                DeleteObject(wb as *mut winapi::ctypes::c_void);
                DeleteObject(bp as *mut winapi::ctypes::c_void);
                // GDI 的 Polygon 把 1px 笔画在路径**内侧**，而副条路径的右/下边正好压在位图边界上
                // （(cw,ch) 在 0..cw-1 / 0..ch-1 之外），那两条边会被整体裁掉 —— 实测底边框整行是白的。
                // 的描边路径内缩 bw/2=0.5，1px stroke 覆盖 [cw-1,cw] 与 [ch-1,ch]
                // 即**最后一列/最后一行**。用 FillRect 把那两条边按同色补上
                let bb = CreateSolidBrush(ui::color::cref(ui::color::TOOL_BORDER));
                let obb = SelectObject(memdc, bb as *mut winapi::ctypes::c_void);
                let mut b1: RECT = RECT { left: 0, top: ch - 1, right: cw, bottom: ch };
                FillRect(memdc, &mut b1, bb);
                let mut b2: RECT = RECT { left: cw - 1, top: ui::metric::ARROW_ZONE, right: cw, bottom: ch };
                FillRect(memdc, &mut b2, bb);
                SelectObject(memdc, obb);
                DeleteObject(bb as *mut winapi::ctypes::c_void);
                anno_draw_sub(memdc, cw);
                if let Some(b) = buf.as_ref() { b.present(hdc); }
                EndPaint(hwnd, &ps);
                0
            }
            WM_LBUTTONDOWN => {
                SetCapture(hwnd);
                let x = (lp & 0xFFFF) as i16 as i32; let y = ((lp >> 16) & 0xFFFF) as i16 as i32;
                let (cw, _) = anno_sub_client();
                anno_sub_hit_down(x, y, cw);
                0
            }
            WM_MOUSEMOVE => {
                let x = (lp & 0xFFFF) as i16 as i32; let y = ((lp >> 16) & 0xFFFF) as i16 as i32;
                track_leave(hwnd);
                let (cw, _) = anno_sub_client();
                anno_sub_mousemove(x, y, cw);
                0
            }
            WM_MOUSELEAVE => {
                // 从副条滑走：重新起 300ms 延迟收起（到期时再判光标在不在主条/副条里）
                if !ANNO_SLIDER_DRAG.load(Ordering::Relaxed) { anno_schedule_sub_hide(); }
                // onLeave → tip->hide：气泡立刻收，hover 底色一起清
                tip_hide(&TIP_SUB);
                ANNO_SUB_HOVER.store(-1, Ordering::Relaxed);
                InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                0
            }
            WM_LBUTTONUP => {
                ANNO_SLIDER_DRAG.store(false, Ordering::Relaxed);
                ReleaseCapture();
                0
            }
            WM_TIMER => {
                if wp == TIP_TIMER { tip_timer(&TIP_SUB); }
                0
            }
            WM_CLOSE => { anno_close(); 0 }
            _ => DefWindowProcW(hwnd, msg, wp, lp),
        }
    }
}
