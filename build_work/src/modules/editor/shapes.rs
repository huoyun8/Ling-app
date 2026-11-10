use crate::prelude::*;
use crate::*;

// 鼠标停在最后一条标注上时，给它画 8 个小方块（线段/箭头只有两端两个）：
//   · 边长 6（draggerSize）、1px 描边、空心
//   · 颜色 ColorF(0.28,0.62,1.0,0.95) —— 半透明的浅蓝，压在任何底色上都看得见
//   · 位置：四角 + 四边中点，方块以这些点为中心（±3）
// 每个 Shape 自己持有 draggers[] 并靠 hoverDraggerIndex 决定画不画
// 我们这边画布是"直接烧进位图"的，没有 shape 链表，所以只记**最后一条**的几何 ——
// 用户的口径就是"只认最后一条"
#[derive(Clone)]
pub(crate) struct AnnoLast {
    pub(crate) tool: i32,          // 0 矩形/椭圆 2 箭头 4 线条 6 马赛克 7 橡皮
    pub(crate) x0: i32,
    pub(crate) y0: i32,
    pub(crate) x1: i32,
    pub(crate) y1: i32,
    // 画这一笔时的样式。拖手柄要按同一套样式重画 —— 不能读当前选项，
    // 用户画完往往会改颜色/线宽，重画时读全局就会把描过的框改色
    pub(crate) r: u8,
    pub(crate) g: u8,
    pub(crate) b: u8,
    pub(crate) width: i32,
    pub(crate) fill: bool,         // 实心
    pub(crate) ell: bool,          // tool 0 椭圆 / tool 2 直线 / tool 3 方标号 / tool 6 模糊
    pub(crate) c2: i32,            // 序号第二颜色下标（-1 = 单色）
    pub(crate) smart: bool,        // 马赛克：智能消除
    pub(crate) val: i32,           // 序号：显示的数字（其余工具为 0）
    pub(crate) ang: f32,           // 序号：尖角方向（度），ShapeNumber::angle 初值 270 = 朝下
    pub(crate) pts: Vec<(i32, i32)>,   // 线条工具的手绘顶点（缩放时按比例映射）
    pub(crate) idx: i32,           // 在 ANNO_SHAPES 里的下标（-1 = 还没入表）
}

// 正在拖的手柄
#[derive(Clone)]
pub(crate) struct ShapeDrag {
    pub(crate) l0: AnnoLast,       // 按下时的几何。全程以它为基准算，不吃累计误差
    pub(crate) l: AnnoLast,        // 当前几何（松手时要写回 ANNO_LAST）
    pub(crate) handle: usize,      // 0 左上 1 上中 2 右上 3 右中 4 右下 5 下中 6 左下 7 左中；usize::MAX = 整条拖动
    pub(crate) m0: (i32, i32),     // 按下点（底图坐标）
    pub(crate) before: AnnoSnap,   // 按下时的图元表：松手若真改了，拿它当撤销点
}
pub(crate) static ANNO_SDRAG: Mutex<Option<ShapeDrag>> = Mutex::new(None);

pub(crate) static ANNO_LAST: Mutex<Option<AnnoLast>> = Mutex::new(None);
pub(crate) static ANNO_LAST_HOT: AtomicBool = AtomicBool::new(false); // 光标是否压在最后一条上
pub(crate) static ANNO_TXT_ROT_HOT: AtomicBool = AtomicBool::new(false); // 提交态文字：光标是否压在旋转柄上

// ==================== 图元列表（对应 的 std::vector<ShapeBase*>）====================
//
// 画布像素是**派生物**：ANNO_IMG = ANNO_ORIG 上按顺序重放 ANNO_SHAPES。
// 只有这样层叠顺序才对 —— 后画的盖住先画的，马赛克/模糊也能盖住文字；
// 改一条几何或文字时只动表再整体重放，不会在旧像素上叠出残影。
#[derive(Clone, Default)]
pub(crate) struct AnnoShape {
    pub(crate) tool: i32,           // 0 矩形/椭圆 2 箭头 3 序号 4 线条 6 马赛克 7 橡皮 8 文字
    pub(crate) x0: i32,
    pub(crate) y0: i32,
    pub(crate) x1: i32,
    pub(crate) y1: i32,
    pub(crate) r: u8,
    pub(crate) g: u8,
    pub(crate) b: u8,
    pub(crate) width: i32,
    pub(crate) fill: bool,          // 实心 / 线条半透明 / 马赛克矩形模式
    pub(crate) ell: bool,           // tool 0 椭圆 / tool 2 直线 / tool 3 方标号 / tool 6 模糊
    pub(crate) c2: i32,             // 序号第二颜色下标（-1 = 单色）
    pub(crate) smart: bool,         // 马赛克：智能消除
    pub(crate) val: i32,            // 序号显示的数字
    pub(crate) ang: f32,            // 序号尖角方向 / 文字旋转角（度，不归一化）
    pub(crate) pts: Vec<(i32, i32)>,
    pub(crate) s: String,           // 文字内容（tool 8）
    pub(crate) fs: f32,
    pub(crate) bold: bool,
    pub(crate) italic: bool,
    pub(crate) pad: i32,            // 文字内边距（物理像素）。拉角缩放时随之放大
}

pub(crate) static ANNO_SHAPES: Mutex<Vec<AnnoShape>> = Mutex::new(Vec::new());

impl AnnoShape {
    fn from_last(l: &AnnoLast) -> AnnoShape {
        AnnoShape {
            tool: l.tool,
            x0: l.x0, y0: l.y0, x1: l.x1, y1: l.y1,
            r: l.r, g: l.g, b: l.b,
            width: l.width,
            fill: l.fill, ell: l.ell, c2: l.c2, smart: l.smart,
            val: l.val, ang: l.ang,
            pts: l.pts.clone(),
            s: String::new(), fs: 0.0, bold: false, italic: false,
            pad: ui::dpi::sc(ui::metric::TXT_PAD),
        }
    }
}

/// 文字图元 → 绘制用的视图（anno_tx_blit 只认 AnnoText）
pub(crate) fn anno_shape_text(s: &AnnoShape) -> AnnoText {
    AnnoText {
        s: s.s.clone(), x: s.x0, y: s.y0, fs: s.fs,
        rgb: rgb(s.r as u32, s.g as u32, s.b as u32),
        bold: s.bold, italic: s.italic, ang: s.ang, pad: s.pad,
    }
}

/// 文字图元的外框（含内边距），与 ANNO_LAST 存的口径一致
pub(crate) fn anno_shape_text_box(s: &AnnoShape) -> (i32, i32, i32, i32) {
    let (bw, bh, _) = unsafe { anno_tx_box(&s.s, tx_fs_px(s.fs), s.bold, s.italic) };
    (s.x0 - s.pad, s.y0 - s.pad,
     s.x0 + bw + s.pad, s.y0 + bh + s.pad)
}

/// 把文字图元烘进位图：只借一块"文字外接框"大小的小 DIB，
/// 从 img 里抠出对应区域当底、画完再贴回去。
/// 旧实现每条文字都开一张整图大小的 DIB、整图拷进再整图拷回：
/// 4K 选区下 33MB × 每条文字 × 每次重放（拖手柄时每帧一次），
/// 标注一多必然卡死，而且每次分配都峰值一张整图。
pub(crate) unsafe fn anno_text_bake(img: &mut [u8], w: i32, h: i32, s: &AnnoShape) {
    if s.s.is_empty() || w <= 0 || h <= 0 { return; }
    let need = (w as usize) * (h as usize) * 4;
    if img.len() != need { return; }
    let t = anno_shape_text(s);
    let fs = tx_fs_px(t.fs);
    let (bw, bh, _) = anno_tx_box(&t.s, fs, t.bold, t.italic);
    if bw <= 0 || bh <= 0 { return; }
    // 旋转后的外接框：文字块绕中心转，半宽/半高按 |cos|/|sin| 展开
    let (cx, cy) = (s.x0 as f32 + bw as f32 / 2.0, s.y0 as f32 + bh as f32 / 2.0);
    let a = t.ang * std::f32::consts::PI / 180.0;
    let (sn, cs) = (a.sin().abs(), a.cos().abs());
    let pad = (fs as f32 * 0.35) as i32 + 3;   // 斜体出锋 / 抗锯齿留边
    let hw = bw as f32 / 2.0 * cs + bh as f32 / 2.0 * sn;
    let hh = bw as f32 / 2.0 * sn + bh as f32 / 2.0 * cs;
    let l = ((cx - hw).floor() as i32 - pad).clamp(0, w);
    let tp = ((cy - hh).floor() as i32 - pad).clamp(0, h);
    let r = ((cx + hw).ceil() as i32 + pad).clamp(0, w);
    let b = ((cy + hh).ceil() as i32 + pad).clamp(0, h);
    let (pw, ph) = (r - l, b - tp);
    if pw <= 0 || ph <= 0 { return; }
    let pneed = (pw as usize) * (ph as usize) * 4;

    let hdc = CreateCompatibleDC(std::ptr::null_mut());
    if hdc.is_null() { return; }
    let mut bmi: BITMAPINFO = std::mem::zeroed();
    bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
    bmi.bmiHeader.biWidth = pw;
    bmi.bmiHeader.biHeight = -ph;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    let mut bits: *mut winapi::ctypes::c_void = std::ptr::null_mut();
    let hbmp = CreateDIBSection(hdc, &mut bmi, DIB_RGB_COLORS, &mut bits, std::ptr::null_mut(), 0);
    if hbmp.is_null() { DeleteDC(hdc); return; }
    let old = SelectObject(hdc, hbmp as *mut winapi::ctypes::c_void);

    let row = (pw as usize) * 4;
    {
        let dst = std::slice::from_raw_parts_mut(bits as *mut u8, pneed);
        for y in tp..b {
            let so = ((y * w + l) * 4) as usize;
            let dof = ((y - tp) as usize) * row;
            dst[dof..dof + row].copy_from_slice(&img[so..so + row]);
        }
    }
    // blit 按底图绝对坐标画：把视口原点挪到小图左上角对应的位置
    SetViewportOrgEx(hdc, -l, -tp, std::ptr::null_mut());
    anno_tx_blit(hdc, s.x0, s.y0, &t);
    GdiFlush();
    {
        let src = std::slice::from_raw_parts(bits as *const u8, pneed);
        for y in tp..b {
            let so = ((y - tp) as usize) * row;
            let dof = ((y * w + l) * 4) as usize;
            img[dof..dof + row].copy_from_slice(&src[so..so + row]);
        }
    }
    SelectObject(hdc, old);
    DeleteObject(hbmp as *mut winapi::ctypes::c_void);
    DeleteDC(hdc);
}

// 序号的取色：有第二色且为偶数号时用第二色
pub(crate) fn anno_number_color(s: &AnnoShape) -> (u8, u8, u8) {
    if s.c2 >= 0 && (s.val & 1) == 0 {
        let i = (s.c2 as usize).min(ANNO_COLORS.len() - 1);
        let (r, g, b) = ANNO_COLORS[i];
        (r, g, b)
    } else {
        (s.r, s.g, s.b)
    }
}

/// 单个图元重放。样式全部取自图元自己 —— 不能读当前选项，
/// 用户画完往往就去改颜色/线宽，重放时读全局会把描过的框改色
pub(crate) unsafe fn anno_shape_bake(img: &mut [u8], w: i32, h: i32, orig: &[u8], s: &AnnoShape) {
    match s.tool {
        0 => {
            if s.fill {
                if s.ell { anno_fill_ellipse(img, w, h, s.x0, s.y0, s.x1, s.y1, s.b, s.g, s.r); }
                else { anno_fill_rect(img, w, h, s.x0, s.y0, s.x1, s.y1, s.b, s.g, s.r); }
            }
            if s.ell { anno_ellipse(img, w, h, s.x0, s.y0, s.x1, s.y1, s.b, s.g, s.r, s.width); }
            else { anno_rect(img, w, h, s.x0, s.y0, s.x1, s.y1, s.b, s.g, s.r, s.width); }
        }
        // 箭头副条的第二颗是"直线"：两点圆头描边，不画箭头
        2 => {
            if s.ell {
                anno_line_aa(img, w, h, s.x0, s.y0, s.x1, s.y1, s.b, s.g, s.r, s.width, 1.0);
            } else {
                anno_arrow(img, w, h, s.x0, s.y0, s.x1, s.y1, s.b, s.g, s.r, s.width, s.fill);
            }
        }
        3 => {
            let cx = (s.x0 + s.x1) / 2;
            let cy = (s.y0 + s.y1) / 2;
            // 双色：1 红 2 蓝 3 红…
            let (nr, ng, nb) = anno_number_color(s);
            anno_number(img, w, h, cx, cy, s.width, s.ang, s.val, nb, ng, nr, s.fill, s.ell);
        }
        4 => {
            if s.ell {
                // 画笔选框 = 填充框
                let alpha = if s.fill { 0.5 } else { 1.0 };
                anno_rect_blend(img, w, h, s.x0, s.y0, s.x1, s.y1, s.b, s.g, s.r, alpha);
            } else {
                let alpha = if s.fill { 0.5 } else { 1.0 };
                if s.pts.len() >= 2 {
                    anno_polyline_aa(img, w, h, &s.pts, s.b, s.g, s.r, s.width, alpha);
                } else {
                    anno_line_aa(img, w, h, s.x0, s.y0, s.x1, s.y1, s.b, s.g, s.r, s.width, alpha);
                }
            }
        }
        // 马赛克/模糊/橡皮：fill=选框模式（true）或手绘轨迹（false），ell=高斯模糊标记
        6 => anno_mosaic(img, w, h, &s.pts, s.x0, s.y0, s.x1, s.y1, s.width, s.fill, s.ell, s.smart),
        7 => anno_eraser(img, w, h, orig, &s.pts, s.x0, s.y0, s.x1, s.y1, s.width, s.fill),
        8 => anno_text_bake(img, w, h, s),
        _ => {}
    }
}

/// 从底图重放全部图元 → ANNO_IMG。任何"改了表"的地方都在改完后调它一次
pub(crate) unsafe fn anno_rebuild() -> bool {
    // 整表重放后，先前为拖动准备的"前缀底图"不再代表当前画面
    ANNO_BASE_N.store(-1, Ordering::Relaxed);
    let w = ANNO_W.load(Ordering::Relaxed);
    let h = ANNO_H.load(Ordering::Relaxed);
    if w <= 0 || h <= 0 { return false; }
    let need = (w as usize) * (h as usize) * 4;
    // 不能 `ANNO_ORIG.clone()` + `ANNO_SHAPES.clone()`：4K 选区一张底图 33MB，
    // 每改一笔就整张克隆一次、再整张拷进工作位图 —— 峰值内存 3 张整图（约 100MB），
    // 而且图元表也跟着深拷贝一遍。三个缓存是不同的锁，直接各持一把只读/可写借用即可。
    // （anno_shape_bake 只吃切片，内部不再加锁，所以不会自锁。）
    let orig = ANNO_ORIG.lock_ok();
    let shapes = ANNO_SHAPES.lock_ok();
    // 正在编辑的那条文字先不烘（它由编辑态直接画在窗口上，避免出现两遍）
    let skip = ANNO_TXE.lock_ok().as_ref().map(|e| e.idx).unwrap_or(-1);
    let mut img = ANNO_IMG.lock_ok();
    // resize 到目标长度后直接覆盖：容量够时既不清零也不重新分配
    img.resize(need, 0);
    if orig.len() == need { img.copy_from_slice(&orig); }
    for (i, s) in shapes.iter().enumerate() {
        if i as i32 == skip { continue; }
        anno_shape_bake(&mut img, w, h, &orig, s);
    }
    true
}

/// 拖动前准备「底图 + 除最后一条外的全部图元」。
/// 一次拖动只算这一下，之后每帧只重烘被拖的那条；抬手即释放。
pub(crate) unsafe fn anno_base_prepare() {
    ANNO_BASE_N.store(-1, Ordering::Relaxed);
    let w = ANNO_W.load(Ordering::Relaxed);
    let h = ANNO_H.load(Ordering::Relaxed);
    if w <= 0 || h <= 0 { return; }
    let need = (w as usize) * (h as usize) * 4;
    let orig = ANNO_ORIG.lock_ok();
    let shapes = ANNO_SHAPES.lock_ok();
    let n = shapes.len();
    if n == 0 || orig.len() != need { return; }
    let mut base = ANNO_BASE.lock_ok();
    base.resize(need, 0);
    base.copy_from_slice(&orig);
    for s in shapes.iter().take(n - 1) {
        anno_shape_bake(&mut base, w, h, &orig, s);
    }
    ANNO_BASE_N.store((n - 1) as i32, Ordering::Relaxed);
}

/// 拖动结束：这张整图底图只有拖动期间有用，释放掉
pub(crate) fn anno_base_clear() {
    ANNO_BASE_N.store(-1, Ordering::Relaxed);
    *ANNO_BASE.lock_ok() = Vec::new();
}

/// 拖动副条滑块改「最后一条」之前调用：底图有效就直接用（之后每移动一格只重烘这一条），
/// 无效才整表重放一次。以前每次移动都走重建，标注一多，改一次马赛克/模糊/智能消除
/// 就把前面所有标注（含每个智能消除的填充计算）全部重算一遍，滑块自然拖不动。
pub(crate) unsafe fn anno_base_ensure() {
    let idx = ANNO_LAST.lock_ok().as_ref().map(|l| l.idx).unwrap_or(-1);
    if idx < 0 { return; }
    let w = ANNO_W.load(Ordering::Relaxed);
    let h = ANNO_H.load(Ordering::Relaxed);
    if w <= 0 || h <= 0 { return; }
    // 只能改"最后一条"：表里它后面还有别的，说明画面与表已不同步，交给重建
    if idx as usize + 1 != ANNO_SHAPES.lock_ok().len() { return; }
    let need = (w as usize) * (h as usize) * 4;
    if ANNO_BASE_N.load(Ordering::Relaxed) == idx && ANNO_BASE.lock_ok().len() == need { return; }
    anno_base_prepare();
}

/// 只把最后一条补画到当前画面。
/// 用于"落点即定型"的图元（标号）：画面此刻已经等于「底图 + 前面所有图元」，
/// 再整表重放等于把前面每条都重画一遍，标号越多越慢。
/// 返回 false 表示画面与图元表不同步，调用方应回退到整表重放。
pub(crate) unsafe fn anno_bake_last() -> bool {
    let w = ANNO_W.load(Ordering::Relaxed);
    let h = ANNO_H.load(Ordering::Relaxed);
    if w <= 0 || h <= 0 { return false; }
    let need = (w as usize) * (h as usize) * 4;
    let orig = ANNO_ORIG.lock_ok();
    if orig.len() != need { return false; }
    let shapes = ANNO_SHAPES.lock_ok();
    let Some(s) = shapes.last() else { return false; };
    let mut img = ANNO_IMG.lock_ok();
    if img.len() != need { return false; }
    anno_shape_bake(&mut img, w, h, &orig, s);
    true
}

pub(crate) unsafe fn anno_last_set(tool: i32) {
    // 只有"拖出来的框"才有手柄；序号是钉死的一次点击，没有可拖的几何
    if !matches!(tool, 0 | 2 | 4 | 6 | 7) { return; }
    let (r, g, b) = ANNO_COLORS[ANNO_OPT_COLOR.load(Ordering::Relaxed).clamp(0, 8) as usize];
    *ANNO_LAST.lock_ok() = Some(AnnoLast {
        tool,
        x0: ANNO_DS.0.load(Ordering::Relaxed), y0: ANNO_DS.1.load(Ordering::Relaxed),
        x1: ANNO_DE.0.load(Ordering::Relaxed), y1: ANNO_DE.1.load(Ordering::Relaxed),
        r, g, b,
        width: ANNO_OPT_SLIDER.load(Ordering::Relaxed).max(1),
        // 矩形/椭圆各记各的填充：当前是椭圆就用 ellFill 那份。
        // 马赛克的 A 恒为"选框/手绘"偏好，B/C 是两个算法
        // 副条从两颗按钮扩到四颗后，形状不能再从 C 取（那是旧口径，取到的是"智能消除"）；
        // 智能消除本身没有手绘
        fill: if tool == 0 && ANNO_OPT_B.load(Ordering::Relaxed) {
            ANNO_OPT_C.load(Ordering::Relaxed)
        } else if tool == 6 {
            ANNO_OPT_A.load(Ordering::Relaxed) || ANNO_OPT_C.load(Ordering::Relaxed)
        } else {
            ANNO_OPT_A.load(Ordering::Relaxed)
        },
        ell: ANNO_OPT_B.load(Ordering::Relaxed),
        c2: ANNO_OPT_COLOR2.load(Ordering::Relaxed),
        smart: tool == 6 && ANNO_OPT_C.load(Ordering::Relaxed),
        val: 0,
        ang: 270.0,
        pts: ANNO_PTS.lock_ok().clone(),
        idx: -1,
    });
    // 这一笔入表（松手时已经画在 ANNO_IMG 上了，入表是为了之后重放/拖手柄/撤销）
    anno_shape_push_last();
    ANNO_LAST_HOT.store(false, Ordering::Relaxed);
}

/// 把 ANNO_LAST 这一条追加进图元表，并把表下标回填到 ANNO_LAST.idx
pub(crate) unsafe fn anno_shape_push_last() {
    let mut g = ANNO_LAST.lock_ok();
    if let Some(l) = g.as_mut() {
        if l.idx >= 0 { return; }
        let mut sh = ANNO_SHAPES.lock_ok();
        sh.push(AnnoShape::from_last(l));
        l.idx = (sh.len() - 1) as i32;
    }
}

pub(crate) fn anno_last_clear() {
    *ANNO_LAST.lock_ok() = None;
    ANNO_LAST_HOT.store(false, Ordering::Relaxed);
}

/// 某一条标注在画布（选区图像坐标）里的重绘范围：几何外扩一圈，
/// 盖住手柄、线宽和抗锯齿。悬停亮柄、拖动改几何都只刷这一块，
/// 不必整窗重绘（4K 下一次整窗合成≈三张整图的拷贝）。
pub(crate) fn anno_rect_of(l: &AnnoLast) -> (i32, i32, i32, i32) {
    let m = anno_rect_margin(l.tool, l.width);
    (
        l.x0.min(l.x1) - m, l.y0.min(l.y1) - m,
        l.x0.max(l.x1) + m, l.y0.max(l.y1) + m,
    )
}

/// 重绘外扩量：箭头的头部远长于线宽，按线宽算会把头留在框外
fn anno_rect_margin(tool: i32, width: i32) -> i32 {
    let w = width.abs().max(8);
    if tool == 8 {
        ui::dpi::sc(50)
    } else if tool == 2 {
        w * 6 + ui::dpi::sc(10)
    } else {
        w + ui::dpi::sc(10)
    }
}

/// 最后一条标注的重绘范围。没有则 None。
pub(crate) fn anno_last_rect() -> Option<(i32, i32, i32, i32)> {
    let g = ANNO_LAST.lock_ok();
    g.as_ref().map(anno_rect_of)
}

/// Del：删掉光标压着的"最后一条"
/// History::removeHoverShape）。只认最后一条 —— 与"非最上层标注不可再编辑"同一口径，
/// 下层元素必须先撤掉上面的才能动。删掉后整表重放一次，后面元素下标自动前移。
pub(crate) unsafe fn anno_delete_hot() -> bool {
    if !ANNO_LAST_HOT.load(Ordering::Relaxed) { return false; }
    let idx = {
        let g = ANNO_LAST.lock_ok();
        match g.as_ref() {
            Some(l) if l.idx >= 0 => l.idx as usize,
            _ => return false,
        }
    };
    if idx >= ANNO_SHAPES.lock_ok().len() { return false; }
    anno_push_undo();                       // 入栈"删除前"的那份图元表
    ANNO_SHAPES.lock_ok().remove(idx);
    anno_last_clear();                      // 最后一条没了，几何/悬停态一起作废
    anno_rebuild();
    true
}

/// 光标是否落在最后一条标注上（框内 or 手柄范围）。line/arrow 按到线段的距离判
pub(crate) fn anno_last_hit(l: &AnnoLast, x: i32, y: i32) -> bool {
    // 马赛克/橡皮按"选框/手绘"判命中：选框是一整块都能抓（mouseMove
    // 里 isRect 时整个矩形都是拖动区），手绘才退化成"贴着轨迹"。不能查 ell
    // （那是"高斯模糊"标记），选框模式下永远走 ends_only，整块反而拖不动
    let ends_only = (l.tool == 2) || (l.tool == 4 && !l.ell)
        || ((l.tool == 6 || l.tool == 7) && !l.fill);
    if ends_only {
        let (dx, dy) = ((l.x1 - l.x0) as f32, (l.y1 - l.y0) as f32);
        let len2 = dx * dx + dy * dy;
        let t = if len2 <= 0.0 { 0.0 } else {
            (((x - l.x0) as f32 * dx + (y - l.y0) as f32 * dy) / len2).clamp(0.0, 1.0)
        };
        let (px, py) = (l.x0 as f32 + dx * t, l.y0 as f32 + dy * t);
        let d = ((x as f32 - px).powi(2) + (y as f32 - py).powi(2)).sqrt();
        return d <= 6.0;
    }
    let (l0, t0) = (l.x0.min(l.x1), l.y0.min(l.y1));
    let (r0, b0) = (l.x0.max(l.x1), l.y0.max(l.y1));
    if l.tool == 3 {
        // 序号：只有实心徽章的盘内算命中（hitR = r + 2*dpi），空心盘内是空的 ——
        // 按下该起一个新序号。
        // 空心时中心夹点仍可抓，范围就是那只 6×6 的小方块
        let (cx, cy) = ((l0 + r0) as f32 / 2.0, (t0 + b0) as f32 / 2.0);
        let rad = l.width.max(1) as f32;
        let (dx, dy) = (x as f32 - cx, y as f32 - cy);
        if !l.fill {
            let h = ui::dpi::sc(ui::layout::anno::DRAGGER_HALF) as f32;
            return dx.abs() <= h && dy.abs() <= h;
        }
        let hitr = rad + ui::dpi::sc(2) as f32;
        if l.ell {
            // 方徽章按旋转后的本地坐标判
            let a = (-l.ang) * std::f32::consts::PI / 180.0;
            let lx = dx * a.cos() + dy * a.sin();
            let ly = -dx * a.sin() + dy * a.cos();
            return lx.abs() <= hitr && ly.abs() <= hitr;
        }
        return (dx * dx + dy * dy).sqrt() <= hitr;
    }
    x >= l0 - 4 && x <= r0 + 4 && y >= t0 - 4 && y <= b0 + 4
}

/// 手柄点位。绘制与命中必须共用这一份 —— 分开写迟早会"看得见的地方拖不动"
pub(crate) fn anno_dragger_pts(l: &AnnoLast) -> Vec<(i32, i32)> {
    // 马赛克/橡皮按"选框/手绘"判命中：选框是一整块都能抓（mouseMove
    // 里 isRect 时整个矩形都是拖动区），手绘才退化成"贴着轨迹"。不能查 ell
    // （那是"高斯模糊"标记），选框模式下永远走 ends_only，整块反而拖不动
    let ends_only = (l.tool == 2) || (l.tool == 4 && !l.ell)
        || ((l.tool == 6 || l.tool == 7) && !l.fill);
    if ends_only {
        return vec![(l.x0, l.y0), (l.x1, l.y1)];
    }
    let (l0, t0) = (l.x0.min(l.x1), l.y0.min(l.y1));
    let (r0, b0) = (l.x0.max(l.x1), l.y0.max(l.y1));
    let (cx, cy) = ((l0 + r0) / 2, (t0 + b0) / 2);
    // 序号只有三只：圆心、尖角、弧中点。
    // 尖角/弧中点跟着 angle 转，和 anno_number 里画的那条路径同一套公式
    if l.tool == 3 {
        let rad = l.width.max(1) as f32;
        let a = (-l.ang) * std::f32::consts::PI / 180.0;
        let (ca, sa) = (a.cos(), a.sin());
        let tp = |px: f32, py: f32| -> (i32, i32) {
            ((cx as f32 + px * ca - py * sa).round() as i32,
             (cy as f32 + px * sa + py * ca).round() as i32)
        };
        return vec![(cx, cy), tp(rad + rad / 3.0, 0.0), tp(-rad, 0.0)];
    }
    vec![
        (l0, t0), (cx, t0), (r0, t0), (r0, cy),
        (r0, b0), (cx, b0), (l0, b0), (l0, cy),
    ]
}

pub(crate) unsafe fn anno_draw_draggers(dc: winapi::shared::windef::HDC, ox: i32, oy: i32, l: AnnoLast) {
    // 序号的夹点按 dpi 放大，其余图元 6px
    let half = if l.tool == 3 { ui::dpi::sc(ui::layout::anno::DRAGGER_HALF) }
               else { ui::layout::anno::DRAGGER_HALF };
    let pts = anno_dragger_pts(&l);
    // COLORREF 是 0x00BBGGRR：RGB(71,158,255) → 0x00FF9E47
    let pen = CreatePen(PS_SOLID as i32, ui::metric::STROKE, ui::color::cref(ui::color::DRAGGER));
    let oldp = SelectObject(dc, pen as *mut winapi::ctypes::c_void);
    let oldb = SelectObject(dc, GetStockObject(NULL_BRUSH as i32));
    for (px, py) in pts {
        Rectangle(dc, ox + px - half, oy + py - half, ox + px + half, oy + py + half);
    }
    SelectObject(dc, oldb);
    SelectObject(dc, oldp);
    DeleteObject(pen as *mut winapi::ctypes::c_void);
}


// ==================== 马赛克 / 高斯模糊 / 橡皮擦 ====================
//
// 三者共用同一套"作用范围"几何
//   rect=true  选框模式：起止点围成的矩形整块作用
//              （橡皮=盖回底图、马赛克=块平均、高斯模糊=三次盒式模糊）
// 源像素一律取**底图**
// 这样叠着画、拖动重画都不会把已经糊过的像素越糊越烂

/// 一笔的轨迹线段：有轨迹点就用轨迹，只有一个点就退化成起点→终点那一段
pub(crate) fn anno_segments(pts: &[(i32, i32)], sx: i32, sy: i32, ex: i32, ey: i32) -> Vec<(i32, i32, i32, i32)> {
    let mut out = Vec::with_capacity(pts.len());
    if pts.len() >= 2 {
        for i in 0..pts.len() - 1 {
            out.push((pts[i].0, pts[i].1, pts[i + 1].0, pts[i + 1].1));
        }
    } else {
        out.push((sx, sy, ex, ey));
    }
    out
}

/// 点到线段距离的平方（判定圆头笔迹覆盖用，比开方便宜）
pub(crate) fn anno_seg_dist2(ax: i32, ay: i32, bx: i32, by: i32, px: i32, py: i32) -> i32 {
    let (abx, aby) = ((bx - ax) as f32, (by - ay) as f32);
    let (apx, apy) = ((px - ax) as f32, (py - ay) as f32);
    let ab2 = abx * abx + aby * aby;
    let t = if ab2 <= 0.0 { 0.0 } else { ((apx * abx + apy * aby) / ab2).clamp(0.0, 1.0) };
    let (dx, dy) = (apx - abx * t, apy - aby * t);
    (dx * dx + dy * dy) as i32
}

/// 作用范围的包围盒（手绘模式外扩一个笔头半径）
pub(crate) fn anno_effect_box(pts: &[(i32, i32)], sx: i32, sy: i32, ex: i32, ey: i32,
                   r: i32, rect: bool, w: i32, h: i32) -> (i32, i32, i32, i32) {
    if rect {
        return (sx.min(ex).clamp(0, w), sy.min(ey).clamp(0, h),
                sx.max(ex).clamp(0, w), sy.max(ey).clamp(0, h));
    }
    let (mut l, mut t, mut rr, mut bb) = (i32::MAX, i32::MAX, i32::MIN, i32::MIN);
    if pts.len() >= 2 {
        for &(x, y) in pts { l = l.min(x); t = t.min(y); rr = rr.max(x); bb = bb.max(y); }
    } else {
        l = sx.min(ex); t = sy.min(ey); rr = sx.max(ex); bb = sy.max(ey);
    }
    ((l - r).clamp(0, w), (t - r).clamp(0, h),
     (rr + r + 1).clamp(0, w), (bb + r + 1).clamp(0, h))
}

/// 块平均马赛克：每块取平均色再整块填回去
///
/// 一次马赛克要把整块选区的每个像素都过一遍，所以内层循环不能再乘法取址：
/// 每个像素都算一次 `yy*w+xx` 再 `*4`，4K 全屏选区就是上千万次乘法。
/// 因此行首算一次偏移、之后 `+= 4` 递推；块内像素数也直接由边长乘出来，
/// 顺带省掉恒真的 `n > 0` 判断（ye>y 且 xe>x 必然成立）。
pub(crate) fn anno_mosaic_blocks(buf: &mut [u8], w: i32, h: i32, blk: i32) {
    let blk = blk.max(2);
    let mut y = 0;
    while y < h {
        let ye = (y + blk).min(h);
        let mut x = 0;
        while x < w {
            let xe = (x + blk).min(w);
            let (mut sb, mut sg, mut sr, mut sa) = (0u64, 0u64, 0u64, 0u64);
            for yy in y..ye {
                let mut i = ((yy * w + x) * 4) as usize;
                for _ in x..xe {
                    sb += buf[i] as u64; sg += buf[i + 1] as u64; sr += buf[i + 2] as u64;
                    sa += buf[i + 3] as u64;
                    i += 4;
                }
            }
            let n = ((ye - y) as u64) * ((xe - x) as u64);
            let (ab, ag, ar, aa) = ((sb / n) as u8, (sg / n) as u8, (sr / n) as u8, (sa / n) as u8);
            for yy in y..ye {
                let mut i = ((yy * w + x) * 4) as usize;
                for _ in x..xe {
                    buf[i] = ab; buf[i + 1] = ag; buf[i + 2] = ar; buf[i + 3] = aa;
                    i += 4;
                }
            }
            x += blk;
        }
        y += blk;
    }
}

/// 三次盒式模糊近似高斯：每遍先横后竖，边缘按真实像素数均分
///
/// 结果与"每像素扫一遍 2r+1 邻域"逐位相同，但用前缀和把单遍复杂度从
/// O(w·h·r) 降到 O(w·h)：4K 全屏选一块模糊、半径 20 时，
/// 直接算是 3840×2160×41×6 ≈ 20 亿次加法（拖一下滑块就卡死），现在只需几百万次减法。
/// 区间和 [x0, x1] 天然就是这个范围，不需要额外补边。
pub(crate) fn anno_blur_box(buf: &mut [u8], w: i32, h: i32, rad: i32) {
    if rad < 1 || w <= 0 || h <= 0 { return; }
    let (uw, uh) = (w as usize, h as usize);
    let mut tmp = vec![0u8; uw * uh * 4];
    // 一行（或一列）的前缀和，四个通道各一份；u32 足够：255*3840 < 2^20
    let mut pre = vec![0u32; (uw.max(uh) + 1) * 4];
    let rad = rad as usize;
    for _ in 0..3 {
        // 水平：buf → tmp
        for y in 0..uh {
            let ro = y * uw * 4;
            pre[..4].fill(0);
            for x in 0..uw {
                let i = ro + x * 4;
                let p = (x + 1) * 4;
                pre[p] = pre[p - 4] + buf[i] as u32;
                pre[p + 1] = pre[p - 3] + buf[i + 1] as u32;
                pre[p + 2] = pre[p - 2] + buf[i + 2] as u32;
                pre[p + 3] = pre[p - 1] + buf[i + 3] as u32;
            }
            for x in 0..uw {
                let x0 = x.saturating_sub(rad);
                let x1 = (x + rad).min(uw - 1);
                let n = (x1 - x0 + 1) as u32;
                let (a, b2) = (x0 * 4, (x1 + 1) * 4);
                let d = ro + x * 4;
                tmp[d] = ((pre[b2] - pre[a]) / n) as u8;
                tmp[d + 1] = ((pre[b2 + 1] - pre[a + 1]) / n) as u8;
                tmp[d + 2] = ((pre[b2 + 2] - pre[a + 2]) / n) as u8;
                tmp[d + 3] = ((pre[b2 + 3] - pre[a + 3]) / n) as u8;
            }
        }
        // 垂直：tmp → buf，同样走列前缀和（一整列建一次，逐行取区间）
        for x in 0..uw {
            pre[..4].fill(0);
            for y in 0..uh {
                let i = (y * uw + x) * 4;
                let p = (y + 1) * 4;
                pre[p] = pre[p - 4] + tmp[i] as u32;
                pre[p + 1] = pre[p - 3] + tmp[i + 1] as u32;
                pre[p + 2] = pre[p - 2] + tmp[i + 2] as u32;
                pre[p + 3] = pre[p - 1] + tmp[i + 3] as u32;
            }
            for y in 0..uh {
                let y0 = y.saturating_sub(rad);
                let y1 = (y + rad).min(uh - 1);
                let n = (y1 - y0 + 1) as u32;
                let (a, b2) = (y0 * 4, (y1 + 1) * 4);
                let d = (y * uw + x) * 4;
                buf[d] = ((pre[b2] - pre[a]) / n) as u8;
                buf[d + 1] = ((pre[b2 + 1] - pre[a + 1]) / n) as u8;
                buf[d + 2] = ((pre[b2 + 2] - pre[a + 2]) / n) as u8;
                buf[d + 3] = ((pre[b2 + 3] - pre[a + 3]) / n) as u8;
            }
        }
    }
}

/// 画笔轨迹采样
/// 欧氏距离）；不足一个步长就丢掉这个点，避免同一位置堆点把路径做成锯齿。
pub(crate) fn anno_need_sample(last: Option<(i32, i32)>, x: i32, y: i32) -> bool {
    let Some((px, py)) = last else { return true };
    let step = ui::dpi::scale_f(2.0);
    let (dx, dy) = ((x - px) as f32, (y - py) as f32);
    dx * dx + dy * dy >= step * step
}

/// 马赛克 / 高斯模糊 / 智能消除：rect=true 选框整块作用，false 沿轨迹涂抹
///
/// 取样源是**当前画布**（底图 + 已经画在前面的图元），不是底图 ——
/// 马赛克压在前面的标注上时，糊的也是那笔标注；只有橡皮擦才回到 screenImg。
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn anno_mosaic(buf: &mut [u8], w: i32, h: i32,
                      pts: &[(i32, i32)], sx: i32, sy: i32, ex: i32, ey: i32,
                      sz: i32, rect: bool, blur: bool, smart: bool) {
    let need = (w as usize) * (h as usize) * 4;
    if w <= 0 || h <= 0 || buf.len() != need { return; }
    if smart {
        let (bx0, by0) = (sx.min(ex).clamp(0, w), sy.min(ey).clamp(0, h));
        let (bx1, by1) = (sx.max(ex).clamp(0, w), sy.max(ey).clamp(0, h));
        if bx1 - bx0 < 2 || by1 - by0 < 2 { return; }
        // 已知像素做供体，框贴边时由窗口裁剪兜底
        let pad = (((bx1 - bx0).max(by1 - by0) as f32) * 0.6).round() as i32;
        let pad = pad.clamp(24, 128);
        let (l, t) = ((bx0 - pad).max(0), (by0 - pad).max(0));
        let (rr, bb) = ((bx1 + pad).min(w), (by1 + pad).min(h));
        let (bw, bh) = (rr - l, bb - t);
        if bw <= 0 || bh <= 0 { return; }
        let mut eff = vec![0u8; (bw * bh * 4) as usize];
        for yy in 0..bh {
            let si = (((t + yy) * w + l) * 4) as usize;
            let di = (yy * bw * 4) as usize;
            eff[di..di + (bw * 4) as usize].copy_from_slice(&buf[si..si + (bw * 4) as usize]);
        }
        let mut mask = vec![0u8; (bw * bh) as usize];
        for y in by0..by1 {
            let ro = ((y - t) * bw + (bx0 - l)) as usize;
            mask[ro..ro + (bx1 - bx0) as usize].fill(1);
        }
        if smart_erase::fill(&mut eff, (bw * 4) as usize, bw as usize, bh as usize, &mask) {
            for y in by0..by1 {
                let si = (((y - t) * bw + (bx0 - l)) * 4) as usize;
                let di = ((y * w + bx0) * 4) as usize;
                let n = ((bx1 - bx0) * 4) as usize;
                buf[di..di + n].copy_from_slice(&eff[si..si + n]);
            }
        } else {
            // 建位图失败时 mosaicBrush 为空，paint 画的是占位色（品红 38%）
            // 这里不能拿"猜测色"去填 —— 那会把失败伪装成成功，导出后才发现是糊的
            let c = ui::color::PREVIEW_STROKE;
            let (b_, g_, r_) = ((c & 0xFF) as u8, ((c >> 8) & 0xFF) as u8, ((c >> 16) & 0xFF) as u8);
            let a = ui::alpha::PREVIEW_STROKE as f32 / 255.0;
            anno_rect_blend(buf, w, h, bx0, by0, bx1, by1, b_, g_, r_, a);
        }
        return;
    }
    // 滑块是逻辑像素
    let sz = ui::dpi::sc(sz).max(1);
    let r = (sz / 2).max(2);
    // 强度条同时派生色块与模糊半径
    // 色块 = round(宽度/3) 下限 6；模糊半径 = clamp((宽度-18)*0.4, 0, 20)（截断，非四舍五入）
    let block = ((sz as f32 / 3.0).round() as i32).max(6);
    let blur_rad = (((sz - 18) as f32) * 0.4).clamp(0.0, 20.0) as i32;
    let pad = if blur { blur_rad } else { block };
    // 取样框按 的口径外扩：floor - pad .. ceil + pad + 1，马赛克的色块网格从外扩框
    // 起算，模糊要吃到框外像素
    let (rx0, ry0) = (sx.min(ex), sy.min(ey));
    let (rx1, ry1) = (sx.max(ex), sy.max(ey));
    let (l, t, rr, bb) = if rect {
        (rx0 - pad, ry0 - pad, rx1 + pad + 1, ry1 + pad + 1)
    } else {
        anno_effect_box(pts, sx, sy, ex, ey, r + pad, rect, w, h)
    };
    let (l, t) = (l.max(0), t.max(0));
    let (rr, bb) = (rr.min(w), bb.min(h));
    let (bw, bh) = (rr - l, bb - t);
    if bw <= 0 || bh <= 0 { return; }
    // 画布这一块抄成"效果缓冲"，再按模式算像素
    let mut eff = vec![0u8; (bw * bh * 4) as usize];
    for yy in 0..bh {
        let si = (((t + yy) * w + l) * 4) as usize;
        let di = (yy * bw * 4) as usize;
        eff[di..di + (bw * 4) as usize].copy_from_slice(&buf[si..si + (bw * 4) as usize]);
    }
    if blur {
        // 半径 0 = 不处理
        if blur_rad > 0 { anno_blur_box(&mut eff, bw, bh, blur_rad); }
    } else {
        anno_mosaic_blocks(&mut eff, bw, bh, block);
    }
    if rect {
        let (cl, ct) = (rx0.max(l), ry0.max(t));
        let (cr, cb) = (rx1.min(rr), ry1.min(bb));
        if cl >= cr || ct >= cb { return; }
        for y in ct..cb {
            let si = (((y - t) * bw + (cl - l)) * 4) as usize;
            let di = ((y * w + cl) * 4) as usize;
            let n = ((cr - cl) * 4) as usize;
            buf[di..di + n].copy_from_slice(&eff[si..si + n]);
        }
        return;
    }
    let r2 = r * r;
    for (ax, ay, bx, by) in anno_segments(pts, sx, sy, ex, ey) {
        let lx = (ax.min(bx) - r).max(l); let hx = (ax.max(bx) + r).min(rr - 1);
        let ly = (ay.min(by) - r).max(t); let hy = (ay.max(by) + r).min(bb - 1);
        for y in ly..=hy {
            for x in lx..=hx {
                if anno_seg_dist2(ax, ay, bx, by, x, y) > r2 { continue; }
                let si = (((y - t) * bw + (x - l)) * 4) as usize;
                let di = ((y * w + x) * 4) as usize;
                buf[di..di + 4].copy_from_slice(&eff[si..si + 4]);
            }
        }
    }
}

/// 橡皮擦：rect=true 选框整块盖回底图，false 沿轨迹以 sz 为直径的圆头笔迹擦除
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn anno_eraser(buf: &mut [u8], w: i32, h: i32, orig: &[u8],
                      pts: &[(i32, i32)], sx: i32, sy: i32, ex: i32, ey: i32,
                      sz: i32, rect: bool) {
    let need = (w as usize) * (h as usize) * 4;
    if w <= 0 || h <= 0 || orig.len() != need || buf.len() != need { return; }
    if rect {
        let (l, t, rr, bb) = anno_effect_box(pts, sx, sy, ex, ey, 0, true, w, h);
        for y in t..bb {
            let i = ((y * w + l) * 4) as usize;
            let n = ((rr - l) * 4) as usize;
            buf[i..i + n].copy_from_slice(&orig[i..i + n]);
        }
        return;
    }
    let sz = ui::dpi::sc(sz).max(1);
    let r = (sz / 2).max(2);
    let r2 = r * r;
    for (ax, ay, bx, by) in anno_segments(pts, sx, sy, ex, ey) {
        let lx = (ax.min(bx) - r).max(0); let hx = (ax.max(bx) + r).min(w - 1);
        let ly = (ay.min(by) - r).max(0); let hy = (ay.max(by) + r).min(h - 1);
        for y in ly..=hy {
            for x in lx..=hx {
                if anno_seg_dist2(ax, ay, bx, by, x, y) > r2 { continue; }
                let i = ((y * w + x) * 4) as usize;
                buf[i..i + 4].copy_from_slice(&orig[i..i + 4]);
            }
        }
    }
}

/// 当前状态的快照（撤销用）。只存图元表 + 标号：位图是重放出来的，不用存
pub(crate) unsafe fn anno_snap() -> AnnoSnap {
    AnnoSnap {
        shapes: ANNO_SHAPES.lock_ok().clone(),
        num: ANNO_NUM.load(Ordering::Relaxed),
    }
}

pub(crate) unsafe fn anno_apply_snap(s: AnnoSnap) {
    *ANNO_SHAPES.lock_ok() = s.shapes;
    ANNO_NUM.store(s.num, Ordering::Relaxed);
    anno_rebuild();
}

pub(crate) unsafe fn anno_push_undo() {
    let snap = anno_snap();
    anno_push_snap(snap);
}

/// 一份快照的近似字节数：图元记录 + 文字 + 手绘顶点。
/// 撤销栈按"总字节"限，不按"步数"限 —— 每份快照都是整张图元表的克隆，
/// 图元一多，同样 120 步会从几 MB 涨到上百 MB（O(N²)）。
fn anno_snap_cost(s: &AnnoSnap) -> usize {
    let per = std::mem::size_of::<AnnoShape>() + 16;
    s.shapes.iter().map(|x| per + x.s.len() + x.pts.len() * 8).sum::<usize>() + 32
}

/// 用一份现成的图元表入撤销栈（拖手柄前拍的那份）
pub(crate) unsafe fn anno_push_snap(snap: AnnoSnap) {
    anno_undo_push(snap);
    ANNO_REDO.lock_ok().clear();
}

/// 撤销栈入栈 + 双重上限（步数 60 / 总内存 8MB）。
/// 注意每一份快照都是**整张图元表的克隆**，所以 N 步撤销的内存是
/// O(N²) 条图元，不是"几十字节一条"。
/// 按字节限而不是死按步数：几十条图元时能存满 60 步，
/// 上千条图元时自动只留最近几步，内存有天花板。
/// 重做也要走这里 —— 否则反复 undo/redo 会无限往撤销栈里塞。
fn anno_undo_push(snap: AnnoSnap) {
    const UNDO_MAX: usize = 60;
    const UNDO_BYTES: usize = 8 * 1024 * 1024;
    let mut u = ANNO_UNDO.lock_ok();
    let mut total: usize = u.iter().map(anno_snap_cost).sum::<usize>() + anno_snap_cost(&snap);
    u.push(snap);
    while u.len() > 1 && (u.len() > UNDO_MAX || total > UNDO_BYTES) {
        let d = anno_snap_cost(&u[0]);
        u.remove(0);
        total -= d;
    }
}

pub(crate) unsafe fn anno_undo() {
    let snap = {
        let mut u = ANNO_UNDO.lock_ok();
        let got = u.pop();
        if got.is_some() {
            let cur = anno_snap();
            ANNO_REDO.lock_ok().push(cur);
        }
        got
    };
    if let Some(sn) = snap {
        anno_tx_cancel();          // 撤销时正在编辑的那条先收掉，避免下标串位
        anno_apply_snap(sn);
    }
    anno_last_clear();
    let hw = ANNO_HWND.load(Ordering::Relaxed) as HWND;
    if !hw.is_null() { InvalidateRect(hw, std::ptr::null_mut(), 0); }
}

pub(crate) unsafe fn anno_redo() {
    let got = ANNO_REDO.lock_ok().pop();
    if let Some(sn) = got {
        let cur = anno_snap();
        anno_undo_push(cur);   // 同样走上限，反复 undo/redo 不会撑爆撤销栈
        anno_tx_cancel();
        anno_apply_snap(sn);
    }
    anno_last_clear();
    let hw = ANNO_HWND.load(Ordering::Relaxed) as HWND;
    if !hw.is_null() { InvalidateRect(hw, std::ptr::null_mut(), 0); }
}

/// 马赛克/模糊/橡皮当前是不是"选框"模式。
/// 马赛克与模糊共用同一个形状偏好（A，Cut 时会跨算法保留），橡皮也只有一个（A）；
/// 智能消除（C）在 里 isRect 恒真、没有手绘形态，预览必须跟着落笔口径走
/// 否则拖动时画成一条手绘线、松手却整块生效
pub(crate) fn anno_effect_rect_mode(tool: i32) -> bool {
    match tool {
        6 => ANNO_OPT_A.load(Ordering::Relaxed) || ANNO_OPT_C.load(Ordering::Relaxed),
        7 => ANNO_OPT_A.load(Ordering::Relaxed),
        _ => false,
    }
}

/// 拖动中的占位预览：铺一层 38% 的品红。色块、模糊、擦除作用在纯色区域上
/// 本来就看不出像素差别，没有这层占位就跟"拖了没反应"一样
pub(crate) unsafe fn anno_effect_preview(tool: i32, sx: i32, sy: i32, ex: i32, ey: i32) {
    let (w, h) = (ANNO_W.load(Ordering::Relaxed), ANNO_H.load(Ordering::Relaxed));
    if w <= 0 || h <= 0 { return; }
    let c = ui::color::PREVIEW_STROKE;
    let (b, g, r) = ((c & 0xFF) as u8, ((c >> 8) & 0xFF) as u8, ((c >> 16) & 0xFF) as u8);
    let a = ui::alpha::PREVIEW_STROKE as f32 / 255.0;
    let pts = ANNO_PTS.lock_ok().clone();
    let width = ANNO_OPT_SLIDER.load(Ordering::Relaxed).max(1);
    let mut img = ANNO_IMG.lock_ok();
    if img.len() != (w as usize) * (h as usize) * 4 { return; }
    // 选框：整块铺占位色，手绘：沿轨迹描一条粗线（对应 paint 的两个分支）
    if anno_effect_rect_mode(tool) {
        anno_rect_blend(&mut img, w, h, sx, sy, ex, ey, b, g, r, a);
    } else if pts.len() >= 2 {
        anno_polyline_aa(&mut img, w, h, &pts, b, g, r, width, a);
    } else {
        anno_line_aa(&mut img, w, h, sx, sy, ex, ey, b, g, r, width, a);
    }
}

// 按当前工具在起点→终点间绘制一笔
pub(crate) unsafe fn anno_stroke(sx: i32, sy: i32, ex: i32, ey: i32) {
    let tool = ANNO_TOOL.load(Ordering::Relaxed);
    let (r, g, b) = ANNO_COLORS[ANNO_OPT_COLOR.load(Ordering::Relaxed).clamp(0, 8) as usize];
    let width = ANNO_OPT_SLIDER.load(Ordering::Relaxed).max(1);
    let opta = ANNO_OPT_A.load(Ordering::Relaxed);
    let optb = ANNO_OPT_B.load(Ordering::Relaxed);
    // 矩形/椭圆各记各的填充，序号也是圆/方各一份
    // （circleSolid / squareSolid）：B 表示"当前是第二种形状"，那时用 C 那份。
    // 马赛克：A = 选框偏好，智能消除没有手绘
    let fill = if (tool == 0 || tool == 3) && optb {
        ANNO_OPT_C.load(Ordering::Relaxed)
    } else if tool == 6 {
        opta || ANNO_OPT_C.load(Ordering::Relaxed)
    } else {
        opta
    };
    let pts = ANNO_PTS.lock_ok().clone();
    let optc = ANNO_OPT_C.load(Ordering::Relaxed);
    anno_stroke_styled(tool, r, g, b, width, fill, optb, optc, &pts, sx, sy, ex, ey);
}

// 同一笔，但样式由调用方给。拖手柄重画时用这一版：必须按**画这一笔时**的颜色/线宽/
// 实心标记重画，读当前选项会把已经画好的框改色（用户画完往往会去改副条）
#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn anno_stroke_styled(tool: i32, r: u8, g: u8, b: u8, width: i32, opta: bool, optb: bool,
                            optc: bool, pts: &[(i32, i32)], sx: i32, sy: i32, ex: i32, ey: i32) {
    let mut img = ANNO_IMG.lock_ok();
    let w = ANNO_W.load(Ordering::Relaxed);
    let h = ANNO_H.load(Ordering::Relaxed);
    match tool {
        // 矩形/椭圆是同一个工具的两副形状，B 标记椭圆
        0 => {
            let ell = optb;
            if opta {
                if ell { anno_fill_ellipse(&mut img, w, h, sx, sy, ex, ey, b, g, r); }
                else { anno_fill_rect(&mut img, w, h, sx, sy, ex, ey, b, g, r); }
            }
            if ell { anno_ellipse(&mut img, w, h, sx, sy, ex, ey, b, g, r, width); }
            else { anno_rect(&mut img, w, h, sx, sy, ex, ey, b, g, r, width); }
        }
        // 箭头副条第二颗 = 直线：换成两点圆头描边，不画箭头
        2 => {
            if optb {
                anno_line_aa(&mut img, w, h, sx, sy, ex, ey, b, g, r, width, 1.0);
            } else {
                anno_arrow(&mut img, w, h, sx, sy, ex, ey, b, g, r, width, opta);
            }
        }
        // 按住 Shift 才退化成"起点到当前点"的一条直线。我们这边没有 path 对象，
        // 就存一份顶点表，每帧从快照重放整条折线（重放而不是增量画，是为了拖完不重复叠加）
        4 => {
            if optb {
                // 画笔副条第二颗 = 选框：实心框，四角四边都能拉
                anno_rect_blend(&mut img, w, h, sx, sy, ex, ey, b, g, r, if opta { 0.5 } else { 1.0 });
            } else {
                let alpha = if opta { 0.5 } else { 1.0 };
                if pts.len() < 2 {
                    let shift = (GetKeyState(VK_SHIFT as i32) as i32 & 0x8000) != 0;
                    if shift {
                        anno_line_aa(&mut img, w, h, sx, sy, ex, ey, b, g, r, width, alpha);
                    }
                } else {
                    anno_polyline_aa(&mut img, w, h, pts, b, g, r, width, alpha);
                }
            }
        }
        6 => {
            // 三种算法都在这里落笔：拖动中只画占位，这里由松手（或拖手柄后的重放）调用，
            // 智能消除也才真正算得上—— 若"拖动时 if !optc 跳过 + 松手不补"两处叠加，
            // 智能消除从头到尾没有一次真正执行
            anno_mosaic(&mut img, w, h, &pts, sx, sy, ex, ey, width, opta, optb, optc);
        }
        7 => {
            let o = ANNO_ORIG.lock_ok();
            anno_eraser(&mut img, w, h, &o, &pts, sx, sy, ex, ey, width, opta);
        }
        _ => {}
    }
}

/// 把 ANNO_LAST 的当前几何/样式写回图元表，再从底图整体重放。
/// 不是"在旧像素上再画一遍"—— 那样改样式会在旧框上留残影
pub(crate) unsafe fn anno_shape_redraw(l: &AnnoLast) {
    let mut nl = l.clone();
    let old;
    {
        let mut sh = ANNO_SHAPES.lock_ok();
        let has = nl.idx >= 0 && (nl.idx as usize) < sh.len();
        // 改之前的重绘范围：上一帧烘的就是它，局部还原必须连旧位置一起擦
        old = if has { Some(anno_shape_rect(&sh[nl.idx as usize])) } else { None };
        if has {
            sh[nl.idx as usize] = AnnoShape::from_last(&nl);
        } else {
            sh.push(AnnoShape::from_last(&nl));
            nl.idx = (sh.len() - 1) as i32;
        }
    }
    *ANNO_LAST.lock_ok() = Some(nl.clone());
    if anno_redraw_last(&nl, old) { return; }
    anno_rebuild();
}

/// 图元表里某一条的重绘范围（与 anno_rect_of 同一口径）
fn anno_shape_rect(s: &AnnoShape) -> (i32, i32, i32, i32) {
    let m = anno_rect_margin(s.tool, s.width);
    (
        s.x0.min(s.x1) - m, s.y0.min(s.y1) - m,
        s.x0.max(s.x1) + m, s.y0.max(s.y1) + m,
    )
}

/// 只改最后一条时走这里：从前缀底图还原它占的那块，再烘这一条。
/// 前提是「表里恰好只有它还没进底图」，且当前没有文字在编辑（编辑中的文字不入底图）。
unsafe fn anno_redraw_last(l: &AnnoLast, old: Option<(i32, i32, i32, i32)>) -> bool {
    let w = ANNO_W.load(Ordering::Relaxed);
    let h = ANNO_H.load(Ordering::Relaxed);
    if w <= 0 || h <= 0 { return false; }
    let need = (w as usize) * (h as usize) * 4;
    let orig = ANNO_ORIG.lock_ok();
    if orig.len() != need { return false; }
    let sh = ANNO_SHAPES.lock_ok();
    if l.idx < 0 || l.idx as usize + 1 != sh.len() { return false; }
    if ANNO_TXE.lock_ok().is_some() { return false; }
    let base = ANNO_BASE.lock_ok();
    if base.len() != need || ANNO_BASE_N.load(Ordering::Relaxed) != l.idx { return false; }
    let mut img = ANNO_IMG.lock_ok();
    if img.len() != need { img.resize(need, 0); }
    // 只还原「旧位置 ∪ 新位置」这一块：其余像素上一帧烘完就等于底图，
    // 整张重拷在 4K 下是 33MB/次，拖滑块每移动一格都来一遍就是卡顿的来源
    let dirty = match old {
        Some(o) => {
            let nb = anno_rect_of(l);
            (
                o.0.min(nb.0).max(0), o.1.min(nb.1).max(0),
                o.2.max(nb.2).min(w), o.3.max(nb.3).min(h),
            )
        }
        None => (0, 0, w, h),
    };
    if dirty.2 > dirty.0 && dirty.3 > dirty.1 {
        let row = w as usize * 4;
        for y in dirty.1 as usize..dirty.3 as usize {
            let o = y * row;
            let a = o + dirty.0 as usize * 4;
            let b = o + dirty.2 as usize * 4;
            img[a..b].copy_from_slice(&base[a..b]);
        }
    }
    anno_shape_bake(&mut img, w, h, &orig, &AnnoShape::from_last(l));
    true
}

// 一步撤销 = 一份图元表 + 标号计数器。
// 画布像素不再入栈：位图完全由「底图 + 图元重放」决定，存一份表就等于存了整张图，
// 也不会出现"位图与图元对不上"（位图与文字表分开存时，撤销后经常错位）


// 每按钮类型：0..7 绘制工具；8 撤销 9 重做 10 关闭 11 贴在屏幕上(图钉) 12 保存 13 复制
// 度量：所有尺寸都从 cell 派生
// 所有东西都排在一张 cell × cell 的网格上：按钮本体 = cell - 2*pad，四边留白相同；
// 分隔条占 sep 宽；副条滑块占 sliderW + 两侧 sliderPad。改 cell 就整体等比缩放。
// 主条按钮：0..6 绘制工具 | 7 录制 | 8 撤销 9 重做 | 10 关闭 11 贴屏 12 保存 13 复制
#[derive(Clone, Default)]
pub(crate) struct AnnoSnap {
    pub(crate) shapes: Vec<AnnoShape>,
    pub(crate) num: i32,
}
pub(crate) static ANNO_UNDO: Mutex<Vec<AnnoSnap>> = Mutex::new(Vec::new());
pub(crate) static ANNO_REDO: Mutex<Vec<AnnoSnap>> = Mutex::new(Vec::new());
pub(crate) static ANNO_DRAG: AtomicBool = AtomicBool::new(false);
pub(crate) static ANNO_DRAGGED: AtomicBool = AtomicBool::new(false); // 本次笔画是否真拖动过（区分纯点击）
pub(crate) static ANNO_DS: (AtomicI32, AtomicI32) = (AtomicI32::new(0), AtomicI32::new(0));
pub(crate) static ANNO_DE: (AtomicI32, AtomicI32) = (AtomicI32::new(0), AtomicI32::new(0));
pub(crate) static ANNO_PRE: Mutex<Vec<u8>> = Mutex::new(Vec::new());   // 拖动前快照（用于擦旧帧）
// 拖动最后一条时的"前缀底图"：底图 + 除最后一条外的全部图元。
// 只在一次拖动期间存在，抬手释放；有了它，拖动时每帧只烘这一条，
// 不必整表重放（标注一多，重放就是每帧 O(N) 次 GDI 文本+图形绘制）。
pub(crate) static ANNO_BASE: Mutex<Vec<u8>> = Mutex::new(Vec::new());
pub(crate) static ANNO_BASE_N: AtomicI32 = AtomicI32::new(-1);   // 已烘入底图的图元数；-1 = 无效
pub(crate) static ANNO_PTS: Mutex<Vec<(i32, i32)>> = Mutex::new(Vec::new()); // 线条工具的手绘顶点
pub(crate) static ANNO_NUM: AtomicI32 = AtomicI32::new(0);   // 标号自增
