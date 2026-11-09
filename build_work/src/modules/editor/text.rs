use crate::prelude::*;
use crate::*;

// 文字标注：编辑态与已提交态的绘制
// 编辑中由 ui::TextBox 画（光标、选区、IME 都归它），收笔后由本模块自己的排版
// 画进画布 —— 文字**不烘进底图**，存成一条条
// AnnoText：
//   · 收笔后：作为图元叠在底图之上画（导出/复制时再烘进副本）；
//   · 编辑中：同样画，再加 1px 光标和一圈虚线框；
//   · 点已有文字：重新进编辑态原地改 —— 这既是"点回来能改"的前提，
//     也是文字不能像别的工具那样直接烧进位图的唯一原因。
//
// 虚线框：1px、2 实 2 虚、平头、内缩半个笔宽
// GDI 侧用 PS_DOT 的 cosmetic 笔（1px、落在整像素上、方头）。
pub(crate) const ANNO_TX_TIMER: usize = 7;    // 光标闪烁定时器 id
pub(crate) const ANNO_TX_BLINK_MS: u32 = 500; // caretBlinkMs：每次到点翻转一次可见性

#[derive(Clone)]
pub(crate) struct AnnoText {
    pub(crate) s: String,        // '\n' 分多行
    pub(crate) x: i32,
    pub(crate) y: i32,           // 文字左上角（不含内边距；虚线框 = 外扩 pad）
    pub(crate) fs: f32,          // 字号（物理像素）
    pub(crate) rgb: u32,         // COLORREF 0x00BBGGRR
    pub(crate) bold: bool,
    pub(crate) italic: bool,
    pub(crate) ang: f32,         // 旋转角（度，屏幕坐标 y 向下 → 正值为顺时针），绕框中心转
    pub(crate) pad: i32,         // 内边距（物理像素）
}

// 文字已并入 ANNO_SHAPES（tool == 8）：它和别的图元共用一条 z 序，
// 所以后画的马赛克/模糊能盖住它，撤销也是同一份表。

// 正在编辑（或在拖动/缩放）的那一条
#[derive(Clone)]
pub(crate) struct TxEdit {
    pub(crate) idx: i32,          // 改第几条；-1 = 新建
    pub(crate) ch: Vec<char>,     // 按字符存：中文/emoji 退格一次仍然只删一个字
    pub(crate) caret: usize,
    pub(crate) sel: usize,        // 选区锚点：与 caret 不同即有一段选中
    pub(crate) drag: bool,        // 鼠标拖选 / Shift 扩选中（松开即结束）
    pub(crate) x: i32,
    pub(crate) y: i32,
    pub(crate) fs: f32,
    pub(crate) rgb: u32,
    pub(crate) bold: bool,
    pub(crate) italic: bool,
    pub(crate) ang: f32,          // 当前旋转角（度，不归一化）
    pub(crate) ang0: f32,         // 按下旋转柄时的角度
    pub(crate) rot0: f32,         // 按下旋转柄时，中心→鼠标的方向角（弧度）
    pub(crate) hov: bool,         // 鼠标压在虚线框/手柄上：四角柄 + 旋转柄才亮
    pub(crate) rot_hot: bool,     // 鼠标正压在旋转柄上（柄从"白点"换成"旋转箭头"）
    pub(crate) fresh: bool,       // 刚起笔、鼠标还赖在落笔点（这时不认四角，免得一按下去满圈手柄）
    pub(crate) focused: bool,     // true=编辑中（画光标）；false=只是在拖框/拉角
    pub(crate) mode: i32,         // 0 无 1 拖框 2 拉角 3 旋转
    pub(crate) m0: (i32, i32),    // 按下点
    pub(crate) box0: (i32, i32),  // 按下时的文字左上角
    pub(crate) fs0: f32,          // 按下时的字号
    pub(crate) pad: i32,          // 当前内边距（物理像素）
    pub(crate) pad0: i32,         // 按下时的内边距
    pub(crate) d0: f32,           // 按下点到中心的距离（缩放比例的分母）
}

pub(crate) static ANNO_TXE: Mutex<Option<TxEdit>> = Mutex::new(None);
pub(crate) static ANNO_CARET_ON: AtomicBool = AtomicBool::new(true);

/// 标注文本框正文用的雅黑（负字高、灰度 AA）。
///
/// 也就是 400 / 700。不能用 600/800：雅黑没有 semibold 面，系统会拿粗体顶上 ——
/// 字形更胖（"Ag国" 一行宽 2px），一眼能看出"我们的字更黑"
pub(crate) unsafe fn tx_font_new(fs: i32, weight: i32, italic: bool) -> usize {
    CreateFontW(-fs.max(1), 0, 0, 0, weight,
        if italic { 1 } else { 0 },
        0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        VARIABLE_PITCH | FF_SWISS, wide(ui::font::UI).as_ptr()) as usize
}

/// 取标注文本框用的字体。走缓存：`anno_tx_blit` 每帧都要给每一行量宽，
/// 每行现建一次 CreateFontW + CreateCompatibleDC 再删掉，拖字号滑块时
/// 一秒几十次字体解析。返回的句柄归缓存所有，调用方**不要** DeleteObject。
pub(crate) unsafe fn anno_tx_font(fs: i32, bold: bool, italic: bool) -> *mut winapi::ctypes::c_void {
    let f = cached_font(FK_TX, fs.max(1), if bold { ui::font::W_BOLD } else { ui::font::W_REGULAR }, italic);
    f as *mut winapi::ctypes::c_void
}

/// 字号取整到整物理像素。GDI 只有整数号，测量、排版、绘制必须共用这同一个取整值，
/// 否则框和字会差一格；fs 用 f32 存是为了拖动缩放、跨 DPI 时不再有整数的阶梯感。
#[inline]
pub(crate) fn tx_fs_px(fs: f32) -> i32 {
    (fs.round() as i32).max(1)
}

/// 行距是**小数**：ascent+descent+lineGap = 2703/2048 em。
/// 每行的偏移按这个小数累加后取整，整块高度则整体向上取整 —— 两点都不一样，
/// 分开用两个函数表达（见 anno_tx_line_y / anno_tx_box）
#[inline]
pub(crate) fn anno_tx_spacing(fs: i32) -> f32 {
    (2703.0 * fs.max(1) as f32) / 2048.0
}

/// 第 line 行的左上角相对文字原点的偏移（四舍五入到整像素）
#[inline]
pub(crate) fn anno_tx_line_y(fs: i32, line: usize) -> i32 {
    (line as f32 * anno_tx_spacing(fs)).round() as i32
}

/// (行距, 光标高)。
///
/// 行距必须是 **ceil((ascent+descent+lineGap)/unitsPerEm × 字号)**：
/// 雅黑的 2703/2048 = 1.319824
/// 拿 GDI 的 tmHeight 会向下取整 —— fs=16 时算出 21，公式是 22，整个框就矮 1px
/// 在 fs=10/12/14/16/20/24/26/32/48/60 下框高 14/16/19/22/27/32/35/43/64/80
/// 与这个公式逐个对上。
/// 光标高反过来取 tmHeight
/// 即 hm.height：fs=26 时 34 而不是 35，正是 floor。
pub(crate) unsafe fn anno_tx_metrics(fs: i32, bold: bool, italic: bool) -> (i32, i32) {
    let fs = fs.max(1);
    let adv = ((2703 * fs + 2047) / 2048) as i32;
    let hdc = tx_mdc();
    if hdc.is_null() { return (adv, (fs as f32 * 1.32) as i32); }
    let f = anno_tx_font(fs, bold, italic);
    let old = SelectObject(hdc, f);
    let mut tm: TEXTMETRICW = std::mem::zeroed();
    let ok = GetTextMetricsW(hdc, &mut tm);
    SelectObject(hdc, old);
    if ok == 0 || tm.tmHeight <= 0 { (adv, adv) } else { (adv, tm.tmHeight) }
}

// ================= 字体设计推进量 =================
//
// 不能用 GetCharABCWidthsFloatW 拿每个字的 A/B/C 三段宽度再累加小数：
// 它**根本不返回小数** —— 雅黑粗体 26 号下 A=20.0、B=18.0、C=18.0…"ABCDEFGH" 合计
// 148，而按设计推进算是 146，差出 2px（长一行字能差 3~4px，框右边就多露一截）。
//
// 排版算推进量的公式是「设计推进 × 字号 ÷ unitsPerEm」，不做任何整数化。
// 所以这里直接剖开 TTC：cmap 查字形号 → hmtx 取设计推进 → 按 upem 缩放。
// 雅黑粗 26 号"ABCDEFGH" = 146.034 → 146，与框内宽 146 逐像素吻合
pub(crate) struct TtfFace {
pub(crate) d: Vec<u8>,
pub(crate) upem: u16,
pub(crate) nh: u16,        // hhea.numberOfHMetrics
pub(crate) hmtx: usize,
pub(crate) cmap4: usize,   // BMP 段（format 4）
pub(crate) cmap12: usize,  // 增补平面（format 12），没有就是 0
}

#[inline]
pub(crate) fn be16(d: &[u8], o: usize) -> u16 {
    ((d[o] as u16) << 8) | d[o + 1] as u16
}

#[inline]
pub(crate) fn be32(d: &[u8], o: usize) -> u32 {
    ((d[o] as u32) << 24) | ((d[o + 1] as u32) << 16) | ((d[o + 2] as u32) << 8) | d[o + 3] as u32
}

pub(crate) fn ttf_load(path: &str) -> Option<TtfFace> {
    let d = std::fs::read(path).ok()?;
    if d.len() < 32 || &d[0..4] != b"\x00\x01\x00\x00" && &d[0..4] != b"ttcf" { return None; }
    // TTC 只是个外层索引，真正的表目录从它给出的偏移开始
    let base = if &d[0..4] == b"ttcf" {
        if be32(&d, 8) == 0 { return None; }
        be32(&d, 12) as usize
    } else { 0 };
    if base + 12 > d.len() { return None; }
    let num = be16(&d, base + 4) as usize;
    let (mut head, mut hhea, mut hmtx, mut cmap) = (0usize, 0usize, 0usize, 0usize);
    for i in 0..num {
        let p = base + 12 + 16 * i;
        if p + 16 > d.len() { return None; }
        let off = be32(&d, p + 8) as usize;
        match &d[p..p + 4] {
            b"head" => head = off,
            b"hhea" => hhea = off,
            b"hmtx" => hmtx = off,
            b"cmap" => cmap = off,
            _ => {}
        }
    }
    if head == 0 || hhea == 0 || hmtx == 0 || cmap == 0 { return None; }
    if head + 20 > d.len() || hhea + 36 > d.len() { return None; }
    let upem = be16(&d, head + 18);
    let nh = be16(&d, hhea + 34);
    if upem == 0 || nh == 0 { return None; }
    // 挑子表：(3,1) 的 format 4 覆盖 BMP；(3,10) 的 format 12 覆盖增补平面
    let n = be16(&d, cmap + 2) as usize;
    let (mut cmap4, mut cmap12) = (0usize, 0usize);
    for i in 0..n {
        let p = cmap + 4 + 8 * i;
        if p + 8 > d.len() { break; }
        let (pid, eid) = (be16(&d, p), be16(&d, p + 2));
        let so = cmap + be32(&d, p + 4) as usize;
        if so + 2 > d.len() { continue; }
        match (pid, eid, be16(&d, so)) {
            (3, 1, 4) | (0, 3, 4) if cmap4 == 0 => cmap4 = so,
            (3, 10, 12) | (0, 4, 12) if cmap12 == 0 => cmap12 = so,
            _ => {}
        }
    }
    if cmap4 == 0 && cmap12 == 0 { return None; }
    Some(TtfFace { d, upem, nh, hmtx, cmap4, cmap12 })
}

impl TtfFace {
    fn gid4(&self, c: u32) -> u16 {
        let d = &self.d;
        let o = self.cmap4;
        if o == 0 || o + 16 > d.len() { return 0; }
        let seg = be16(d, o + 6) as usize / 2;
        let ends = o + 14;
        let starts = ends + seg * 2 + 2;
        let deltas = starts + seg * 2;
        let ranges = deltas + seg * 2;
        if ranges + seg * 2 > d.len() { return 0; }
        let c16 = if c > 0xFFFF { return 0; } else { c as u16 };
        for i in 0..seg {
            if c16 > be16(d, ends + i * 2) { continue; }
            let st = be16(d, starts + i * 2);
            if c16 < st { return 0; }
            let ro = be16(d, ranges + i * 2) as usize;
            let dl = be16(d, deltas + i * 2);
            if ro == 0 { return (c16.wrapping_add(dl)) as u16; }
            let idx = ranges + i * 2 + ro + 2 * (c16 - st) as usize;
            if idx + 2 > d.len() { return 0; }
            let g = be16(d, idx);
            return if g == 0 { 0 } else { g.wrapping_add(dl) };
        }
        0
    }

    fn gid12(&self, c: u32) -> u16 {
        let d = &self.d;
        let o = self.cmap12;
        if o == 0 || o + 16 > d.len() { return 0; }
        let groups = be32(d, o + 12) as usize;
        for i in 0..groups {
            let p = o + 16 + 12 * i;
            if p + 12 > d.len() { return 0; }
            let (s, e, g) = (be32(d, p), be32(d, p + 4), be32(d, p + 8));
            if c < s { return 0; }
            if c <= e { return (g + (c - s)) as u16; }
        }
        0
    }

    fn gid(&self, c: u32) -> u16 {
        if c > 0xFFFF && self.cmap12 != 0 { self.gid12(c) } else { self.gid4(c) }
    }

    #[inline]
    fn advance_units(&self, g: u16) -> u16 {
        // 超出 numberOfHMetrics 的字形复用最后一档（hmtx 的压缩规则）
        let i = (g as usize).min(self.nh as usize - 1);
        let o = self.hmtx + 4 * i;
        if o + 2 > self.d.len() { 0 } else { be16(&self.d, o) }
    }
}

pub(crate) fn ttf_path(bold: bool) -> String {
    let mut buf = [0u16; 260];
    let n = unsafe { GetWindowsDirectoryW(buf.as_mut_ptr(), 260) } as usize;
    let dir = if n > 0 && n < 260 {
        String::from_utf16_lossy(&buf[..n])
    } else {
        "C:\\Windows".to_string()
    };
    format!("{}\\Fonts\\{}", dir, if bold { "msyhbd.ttc" } else { "msyh.ttc" })
}

pub(crate) static TTF_REG: std::sync::OnceLock<Option<TtfFace>> = std::sync::OnceLock::new();
pub(crate) static TTF_BOLD: std::sync::OnceLock<Option<TtfFace>> = std::sync::OnceLock::new();

pub(crate) fn ttf_face(bold: bool) -> Option<&'static TtfFace> {
    let cell = if bold { &TTF_BOLD } else { &TTF_REG };
    cell.get_or_init(|| {
        // 粗体文件缺失（精简系统）时退回常规面：字形不对但排版不至于崩
        ttf_load(&ttf_path(bold)).or_else(|| if bold { ttf_load(&ttf_path(false)) } else { None })
    }).as_ref()
}

/// 逐字小数累加的设计推进（不做整数化），每步取整后输出起始 x。
/// 返回每个 **UTF-16 单元** 的起始 x（末尾多一项 = 整串宽度）
/// 用 UTF-16 单元而不是 char 索引，是因为 ExtTextOutW 的 dx 数组是逐单元给的。
pub(crate) fn ttf_advances(f: &TtfFace, fs: i32, s: &str) -> Vec<i32> {
    let mut out = Vec::with_capacity(s.encode_utf16().count() + 1);
    out.push(0);
    let scale = fs.max(1) as f32 / f.upem as f32;
    let mut acc = 0f32;
    for ch in s.chars() {
        acc += f.advance_units(f.gid(ch as u32)) as f32 * scale;
        let x = acc.round() as i32;
        // 代理对：整个字形算一次推进，后半个单元补 0（dx 差分后自然是 0）
        for _ in 0..ch.len_utf16() { out.push(x); }
    }
    out
}

/// 逐字的浮点步进。返回值语义同 ttf_advances。
/// 优先走字体表，读不到字体文件时才退回 GDI 的整数字宽。
pub(crate) unsafe fn anno_tx_advances(fs: i32, bold: bool, italic: bool, s: &str) -> Vec<i32> {
    if let Some(f) = ttf_face(bold) {
        return ttf_advances(f, fs, s);
    }
    let units: Vec<u16> = s.encode_utf16().collect();
    let mut out = Vec::with_capacity(units.len() + 1);
    out.push(0);
    if units.is_empty() { return out; }
    let hdc = tx_mdc();
    if hdc.is_null() {
        // 没有 DC 时退回"每字 = 字号"的粗估：只用宽度算个大概，不会崩
        for i in 0..units.len() { out.push(((i + 1) as i32) * fs.max(1)); }
        return out;
    }
    let f = anno_tx_font(fs, bold, italic);
    let old = SelectObject(hdc, f);
    let mut acc = 0f32;
    for &u in units.iter() {
        let mut abc = winapi::um::wingdi::ABCFLOAT { abcfA: 0.0, abcfB: 0.0, abcfC: 0.0 };
        if GetCharABCWidthsFloatW(hdc, u as u32, u as u32, &mut abc) != 0 {
            acc += abc.abcfA + abc.abcfB + abc.abcfC;
        }
        out.push(acc.round() as i32);
    }
    SelectObject(hdc, old);
    out
}

/// 一段文字的宽度：浮点步进累加后取整（含尾部空白）
pub(crate) unsafe fn anno_tx_measure(fs: i32, bold: bool, italic: bool, s: &str) -> i32 {
    if s.is_empty() { return 0; }
    *anno_tx_advances(fs, bold, italic, s).last().unwrap_or(&0)
}

/// 文本框尺寸：(宽, 高, 行距)。空串也算一行。
/// 高 = ceil(行数 × 小数行距)：两行 fs=16 时 是 43 + 12 = 55，不是 44 + 12 = 56
pub(crate) unsafe fn anno_tx_box(s: &str, fs: i32, bold: bool, italic: bool) -> (i32, i32, i32) {
    let mut w = 0;
    let mut n = 0;
    for line in s.split('\n') {
        w = w.max(anno_tx_measure(fs, bold, italic, line));
        n += 1;
    }
    let n = n.max(1);
    let h = (n as f32 * anno_tx_spacing(fs)).ceil() as i32;
    (w.max(1), h, h)
}

// ================= 旋转 / 手柄几何 =================
//
// 文字的旋转是"整块绕框中心转"：画、量光标、命中、手柄位置全部走同一条
// 正/反变换，任何一处单独改都会出现"看得见却点不到"。
// 角度用度、屏幕坐标 y 向下 → 正角看起来是顺时针。

pub(crate) fn anno_rot(px: f32, py: f32, cx: f32, cy: f32, ang: f32) -> (f32, f32) {
    if ang == 0.0 { return (px, py); }
    let a = ang * std::f32::consts::PI / 180.0;
    let (s, c) = (a.sin(), a.cos());
    let (dx, dy) = (px - cx, py - cy);
    (cx + dx * c - dy * s, cy + dx * s + dy * c)
}

// 手柄口径（100% 缩放下）：四角圆底直径 10、判定半径 7
// （编辑中 hr*0.8+2）、旋转柄挂在框顶上方 2.6*hr、判定同 7

/// 编辑态的文字尺寸（宽, 高）
pub(crate) fn anno_tx_size(e: &TxEdit) -> (i32, i32) {
    let s: String = e.ch.iter().collect();
    let (bw, bh, _) = unsafe { anno_tx_box(&s, tx_fs_px(e.fs), e.bold, e.italic) };
    (bw, bh)
}

/// 编辑态的虚线框 (l, t, r, b)（图像坐标，未旋转）
pub(crate) fn anno_tx_frame_rect(e: &TxEdit) -> (i32, i32, i32, i32) {
    let (bw, bh) = anno_tx_size(e);
    (e.x - e.pad, e.y - e.pad, e.x + bw + e.pad, e.y + bh + e.pad)
}

pub(crate) fn anno_tx_center_of(e: &TxEdit) -> (f32, f32) {
    let (bw, bh) = anno_tx_size(e);
    ((e.x + bw / 2) as f32, (e.y + bh / 2) as f32)
}

/// 屏幕/图像坐标 → 文字未旋转坐标（命中、光标定位都先落到这里再判）
pub(crate) fn anno_tx_inv(e: &TxEdit, px: i32, py: i32) -> (f32, f32) {
    let (cx, cy) = anno_tx_center_of(e);
    anno_rot(px as f32, py as f32, cx, cy, -e.ang)
}

/// 手柄种类：三角缩放 + 右上关闭 + 顶边中点外侧旋转
#[derive(Clone, Copy, PartialEq)]
pub(crate) enum TxHnd { None, Scale(i32), Close, Rotate }

/// 手柄点位（图像坐标，已旋转）：四角 [TL, TR, BR, BL]、旋转柄圆心、旋转柄连线起点
pub(crate) fn anno_tx_hnd_pts(e: &TxEdit) -> ([(i32, i32); 4], (i32, i32), (i32, i32)) {
    let (l, t, r, b) = anno_tx_frame_rect(e);
    let (cx, cy) = anno_tx_center_of(e);
    let midx = (l + r) as f32 / 2.0;
    let rp = |px: f32, py: f32| -> (i32, i32) {
        let (x, y) = anno_rot(px, py, cx, cy, e.ang);
        (x.round() as i32, y.round() as i32)
    };
    let corners = [
        rp(l as f32, t as f32), rp(r as f32, t as f32),
        rp(r as f32, b as f32), rp(l as f32, b as f32),
    ];
    (corners, rp(midx, t as f32 - ui::dpi::scale_f(ui::metric::TX_ROT_GAP)), rp(midx, t as f32))
}

/// 手柄命中。绘制与命中必须共用 anno_tx_hnd_pts —— 分开写迟早"看得见拖不动"
pub(crate) fn anno_tx_hnd_hit(e: &TxEdit, px: i32, py: i32, hit: i32) -> TxHnd {
    let (_, rot, _) = anno_tx_hnd_pts(e);
    // 刚起笔、鼠标还赖在落笔点：空框只有一个字大，整块都落在四角判定里 ——
    // 不挡这一下，一按下去就先把角柄点亮（用户要的是"开始只有虚线框"）。
    // 鼠标挪开 4px（fresh 清掉）或框已经长大，就恢复正常判角
    let (bw, bh) = anno_tx_size(e);
    let too_small = e.fresh
        && (bw + e.pad * 2 <= hit * 4 || bh + e.pad * 2 <= hit * 4);
    if !too_small {
        // 四角按"反向旋转后的本地坐标"判：判定区跟着框一起转，与画出来的柄同一个姿态
        let (l, t, r, b) = anno_tx_frame_rect(e);
        let (lx, ly) = anno_tx_inv(e, px, py);
        let cs = [(l, t), (r, t), (r, b), (l, b)];
        for (k, &(hx, hy)) in cs.iter().enumerate() {
            if (lx - hx as f32).abs() <= hit as f32 && (ly - hy as f32).abs() <= hit as f32 {
                return if k == 1 { TxHnd::Close } else { TxHnd::Scale(k as i32) };
            }
        }
        // 旋转柄按圆形判定，半径与四角柄同一档（hr + 6*dpi），
        // 不是围着中心画一个方框 —— 斜着点柄的四个角上不该算命中
        let (dx, dy) = ((px - rot.0) as f32, (py - rot.1) as f32);
        if (dx * dx + dy * dy).sqrt() <= hit as f32 {
            return TxHnd::Rotate;
        }
    }
    TxHnd::None
}

/// 点到线段的距离（旋转柄那条"走廊"用）
pub(crate) fn anno_dist_seg(px: f32, py: f32, a: (i32, i32), b: (i32, i32)) -> f32 {
    let (dx, dy) = ((b.0 - a.0) as f32, (b.1 - a.1) as f32);
    let l2 = dx * dx + dy * dy;
    if l2 < 0.0001 { return ((px - a.0 as f32).powi(2) + (py - a.1 as f32).powi(2)).sqrt(); }
    let t = (((px - a.0 as f32) * dx + (py - a.1 as f32) * dy) / l2).clamp(0.0, 1.0);
    let qx = a.0 as f32 + dx * t;
    let qy = a.1 as f32 + dy * t;
    ((px - qx).powi(2) + (py - qy).powi(2)).sqrt()
}

/// 鼠标是否"压在虚线框上"：框两侧各 12px 的环带，正文rect内不算
/// （新建时鼠标就落在正文原点，不能一出现就挂一圈手柄）
/// 另外把"框顶中点 → 旋转柄"当一条走廊算进来：鼠标从框边往柄上移时半路不能丢命中，
/// 否则柄和框会一起消失（distToSegment 那段）
pub(crate) fn anno_tx_frame_hover(e: &TxEdit, px: i32, py: i32) -> bool {
    if anno_tx_hnd_hit(e, px, py, ui::dpi::sc(ui::metric::TX_HANDLE_HIT)) != TxHnd::None { return true; }
    let (ix, iy) = anno_tx_inv(e, px, py);
    let (l, t, r, b) = anno_tx_frame_rect(e);
    let (bw, bh) = anno_tx_size(e);
    let m = ui::dpi::scale_f(ui::layout::anno::TX_FRAME_HOVER);
    let inside_ring = ix >= l as f32 - m && ix <= r as f32 + m
        && iy >= t as f32 - m && iy <= b as f32 + m;
    let in_text = ix >= e.x as f32 && ix < (e.x + bw) as f32
        && iy >= e.y as f32 && iy < (e.y + bh) as f32;
    if inside_ring && !in_text { return true; }
    let (_, rot, top_mid) = anno_tx_hnd_pts(e);
    !in_text && anno_dist_seg(px as f32, py as f32, top_mid, rot)
        <= ui::dpi::scale_f(ui::metric::TX_ROT_R as f32)
}

/// 把一条文字画到 dc 的 (x,y)（屏幕坐标，非底图坐标）。
/// 用 ExtTextOutW + 自算的 dx：每个字的落点按小数步进累加后取整，
/// 直接用 TextOutW 会走 GDI 的整数步进，长一行能差出好几像素。
/// 每行在框内水平居中（短行不贴左边）；有旋转角时走世界变换，整块绕中心转
pub(crate) unsafe fn anno_tx_blit(dc: winapi::shared::windef::HDC, x: i32, y: i32, t: &AnnoText) {
    let fs = tx_fs_px(t.fs);
    let (bw, bh, _) = anno_tx_box(&t.s, fs, t.bold, t.italic);
    let mut oldmode = 0;
    let mut rotated = false;
    if t.ang != 0.0 {
        let (cx, cy) = ((x + bw / 2) as f32, (y + bh / 2) as f32);
        let a = t.ang * std::f32::consts::PI / 180.0;
        let (s, c) = (a.sin(), a.cos());
        let xf = XFORM {
            eM11: c, eM12: s,
            eM21: -s, eM22: c,
            eDx: cx - cx * c + cy * s,
            eDy: cy - cx * s - cy * c,
        };
        oldmode = SetGraphicsMode(dc, GM_ADVANCED as i32);
        if SetWorldTransform(dc, &xf) != 0 { rotated = true; }
    }
    let f = anno_tx_font(fs, t.bold, t.italic);
    let old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT as i32);
    SetTextColor(dc, t.rgb);
    for (i, line) in t.s.split('\n').enumerate() {
        if line.is_empty() { continue; }
        let ws: Vec<u16> = line.encode_utf16().collect();
        let pos = anno_tx_advances(fs, t.bold, t.italic, line);
        // dx 是"每个字相对前一个字的推进量"，所以把绝对位置差分回去
        let mut dx: Vec<i32> = Vec::with_capacity(ws.len());
        for k in 0..ws.len() {
            dx.push(pos[k + 1] - pos[k]);
        }
        // 每行左对齐（LEADING）
        // （TextBox 里搜不到 SetTextAlignment）；整块宽度由最宽一行决定，所以左边界即 x
        ExtTextOutW(dc, x, y + anno_tx_line_y(fs, i), 0, std::ptr::null(),
            ws.as_ptr(), ws.len() as u32, dx.as_ptr());
    }
    SelectObject(dc, old);
    if rotated {
        let id = XFORM { eM11: 1.0, eM12: 0.0, eM21: 0.0, eM22: 1.0, eDx: 0.0, eDy: 0.0 };
        SetWorldTransform(dc, &id);
        SetGraphicsMode(dc, oldmode);
    }
}

/// 光标在未旋转坐标里的落点：每行左对齐，行内步进就是前缀字宽
pub(crate) fn anno_tx_caret_xy(e: &TxEdit, _line_h: i32) -> (i32, i32) {
    let n = e.ch.len();
    let caret = e.caret.min(n);
    let mut line = 0;
    let mut start = 0;
    for i in 0..caret {
        if e.ch[i] == '\n' { line += 1; start = i + 1; }
    }
    let prefix: String = e.ch[start..caret].iter().collect();
    let w = unsafe { anno_tx_measure(tx_fs_px(e.fs), e.bold, e.italic, &prefix) };
    (e.x + w, e.y + anno_tx_line_y(tx_fs_px(e.fs), line))
}

/// 光标位置（已按旋转角变换到图像坐标）。画光标、摆输入法都用这一份
pub(crate) fn anno_tx_caret_pos(e: &TxEdit, line_h: i32) -> (i32, i32) {
    let (cx0, cy0) = anno_tx_caret_xy(e, line_h);
    let (cx, cy) = anno_tx_center_of(e);
    let (rx, ry) = anno_rot(cx0 as f32, cy0 as f32, cx, cy, e.ang);
    (rx.round() as i32, ry.round() as i32)
}

/// 屏幕坐标 → 光标下标
pub(crate) fn anno_tx_caret_at(e: &TxEdit, _line_h: i32, px: i32, py: i32) -> usize {
    let n = e.ch.len();
    let (px, py) = anno_tx_inv(e, px, py);
    // 每行起点：第 0 行从 0 起，每遇一个换行就多一个起点
    let mut starts = vec![0usize];
    for (i, &c) in e.ch.iter().enumerate() { if c == '\n' { starts.push(i + 1); } }
    // 先按行高定位到第几行，再在这一行里逐个字符边界比宽度
    let sp = anno_tx_spacing(tx_fs_px(e.fs)).max(1.0);
    let line = ((((py - e.y as f32) as f32) / sp).floor().max(0.0) as usize).min(starts.len() - 1);
    let start = starts[line];
    let mut end = start;
    while end < n && e.ch[end] != '\n' { end += 1; }
    let mut best = start;
    let mut bestd = f32::MAX;
    for k in start..=end {
        let prefix: String = e.ch[start..k].iter().collect();
        let w = unsafe { anno_tx_measure(tx_fs_px(e.fs), e.bold, e.italic, &prefix) };
        let d = (((e.x + w) as f32) - px).abs();
        if d < bestd { bestd = d; best = k; }
    }
    best
}

/// 已提交的文字 → 只用来算几何/手柄的"编辑态视图"。
/// 不聚焦（不画光标、不收键盘），只是让虚线框、四角柄、旋转柄复用同一份几何：
/// 编辑与否只是画不画文本框
pub(crate) fn anno_txt_edit_view(t: &AnnoText) -> TxEdit {
    let ch: Vec<char> = t.s.chars().collect();
    let caret = ch.len();
    TxEdit {
        idx: -1, ch, caret, sel: caret, drag: false,
        x: t.x, y: t.y, fs: t.fs, rgb: t.rgb, bold: t.bold, italic: t.italic,
        ang: t.ang, ang0: t.ang, rot0: 0.0,
        hov: true, rot_hot: false, fresh: false, focused: false, mode: 0,
        m0: (0, 0), box0: (t.x, t.y), fs0: t.fs, pad: t.pad, pad0: t.pad, d0: 1.0,
    }
}

/// 最后一条已提交文字上的手柄命中：只认 isLastShape
/// 的那一个（被压住时画了也点不到），这里同样只认 ANNO_LAST 挂的那条。
pub(crate) unsafe fn anno_txt_hnd_hit(x: i32, y: i32) -> Option<(usize, TxHnd)> {
    let idx = {
        let last = ANNO_LAST.lock_ok();
        match last.as_ref() { Some(l) if l.tool == 8 => l.idx, _ => return None }
    };
    if idx < 0 { return None; }
    let t = {
        let sh = ANNO_SHAPES.lock_ok();
        match sh.get(idx as usize) { Some(s) if s.tool == 8 => anno_shape_text(s), _ => return None }
    };
    // 非编辑态的命中范围比编辑态宽：收了笔的文字
    // 没有文本框兜着，四角柄得更"好按"一点
    let h = anno_tx_hnd_hit(&anno_txt_edit_view(&t), x, y,
        ui::dpi::sc(ui::metric::TX_ROT_R + ui::metric::TX_HIT_PAD));
    if h == TxHnd::None { None } else { Some((idx as usize, h)) }
}

/// 最后一条已提交文字：光标是否压在正文/框边/手柄/通往旋转柄的走廊上。
/// 非编辑态 handlesVisible = hoverDraggerIndex >= 0（正文算 1，同样把整套手柄挂出来）
pub(crate) unsafe fn anno_txt_hover(l: &AnnoLast, x: i32, y: i32) -> bool {
    if l.idx < 0 { return false; }
    let t = {
        let sh = ANNO_SHAPES.lock_ok();
        match sh.get(l.idx as usize) { Some(s) if s.tool == 8 => anno_shape_text(s), _ => return false }
    };
    let v = anno_txt_edit_view(&t);
    let (px, py) = anno_tx_inv(&v, x, y);
    let (bw, bh) = anno_tx_size(&v);
    let pad = v.pad;
    (px >= (v.x - pad) as f32 && px <= (v.x + bw + pad) as f32
        && py >= (v.y - pad) as f32 && py <= (v.y + bh + pad) as f32)
        || anno_tx_frame_hover(&v, x, y)
}

/// 未编辑的文字：把点反旋到它的未旋转坐标里（命中判的是旋转后的框）
pub(crate) fn anno_txt_inv(t: &AnnoText, px: i32, py: i32) -> (f32, f32) {
    let (bw, bh, _) = unsafe { anno_tx_box(&t.s, tx_fs_px(t.fs), t.bold, t.italic) };
    let (cx, cy) = ((t.x + bw / 2) as f32, (t.y + bh / 2) as f32);
    anno_rot(px as f32, py as f32, cx, cy, -t.ang)
}

/// 命中最上面（最后画）的那条文字。范围含虚线框外扩的那一圈。返回图元表下标
pub(crate) unsafe fn anno_tx_hit(x: i32, y: i32) -> Option<usize> {
    let shapes = ANNO_SHAPES.lock_ok();
    for i in (0..shapes.len()).rev() {
        let s = &shapes[i];
        if s.tool != 8 { continue; }
        let t = anno_shape_text(s);
        let (bw, bh, _) = anno_tx_box(&t.s, tx_fs_px(t.fs), t.bold, t.italic);
        let (px, py) = anno_txt_inv(&t, x, y);
        if px >= (t.x - s.pad) as f32 && px <= (t.x + bw + s.pad) as f32
            && py >= (t.y - s.pad) as f32 && py <= (t.y + bh + s.pad) as f32 {
            return Some(i);
        }
    }
    None
}

/// 点是否落在某个角柄上（边长 6，以角点为中心）。返回 (图元表下标, 角号 0..3)
pub(crate) unsafe fn anno_tx_corner_hit(x: i32, y: i32) -> Option<(usize, i32)> {
    let shapes = ANNO_SHAPES.lock_ok();
    for i in (0..shapes.len()).rev() {
        let s = &shapes[i];
        if s.tool != 8 { continue; }
        let t = anno_shape_text(s);
        let (bw, bh, _) = anno_tx_box(&t.s, tx_fs_px(t.fs), t.bold, t.italic);
        let l = t.x - s.pad;
        let tp = t.y - s.pad;
        let r = t.x + bw + s.pad;
        let b = t.y + bh + s.pad;
        let (cx, cy) = ((t.x + bw / 2) as f32, (t.y + bh / 2) as f32);
        let pts = [(l, tp), (r, tp), (r, b), (l, b)];
        for (k, (px, py)) in pts.iter().enumerate() {
            let (hx, hy) = anno_rot(*px as f32, *py as f32, cx, cy, t.ang);
            if (x as f32 - hx).abs() <= 5.0 && (y as f32 - hy).abs() <= 5.0 {
                return Some((i, k as i32));
            }
        }
    }
    None
}

/// 进入编辑/拖动/缩放态。idx<0 新建；corner>=0 表示是拉角。
/// idx 是 ANNO_SHAPES 的下标（只能指向文字图元）
pub(crate) unsafe fn anno_tx_begin(idx: i32, x: i32, y: i32, corner: i32) {
    let view = if idx >= 0 {
        let shapes = ANNO_SHAPES.lock_ok();
        match shapes.get(idx as usize) {
            Some(t) if t.tool == 8 => Some(anno_shape_text(t)),
            _ => None,
        }
    } else {
        None
    };
    if idx >= 0 && view.is_none() { return; }
    let (s, fs, rgbv, bold, italic, ang, pad, ch, tx, ty) = match view {
        Some(t) => {
            let ch = t.s.chars().collect::<Vec<char>>();
            (t.s, t.fs, t.rgb, t.bold, t.italic, t.ang, t.pad, ch, t.x, t.y)
        }
        None => {
            let (cr, cg, cb) = ANNO_COLORS[ANNO_OPT_COLOR.load(Ordering::Relaxed).clamp(0, 8) as usize];
            (String::new(),
             // 滑块是逻辑像素、字号是底图物理像素（fontSize = getSliderVal）
             ui::dpi::sc(ANNO_OPT_SLIDER.load(Ordering::Relaxed).clamp(10, 72)) as f32,
             rgb(cr as u32, cg as u32, cb as u32),
             ANNO_OPT_A.load(Ordering::Relaxed),
             ANNO_OPT_B.load(Ordering::Relaxed),
             0.0, ui::dpi::sc(ui::metric::TXT_PAD), Vec::new(), x, y)
        }
    };
    let caret = ch.len();
    let (bw, bh, _) = anno_tx_box(&s, tx_fs_px(fs), bold, italic);
    let cx = (tx + bw / 2) as f32;
    let cy = (ty + bh / 2) as f32;
    let d = (((x as f32) - cx).powi(2) + ((y as f32) - cy).powi(2)).sqrt();
    // corner == -2：按在已提交文字顶上的旋转柄。
    // 和编辑态走同一个 mode 3 —— 拖动时按"中心方位角差多少、框就转多少"
    let mode = if corner == -2 { 3 } else if corner >= 0 { 2 } else if idx >= 0 {
        // 落点在正文范围里 = 继续编辑（并落光标）；落在框边那一圈 = 拖动整条
        // （对应 hoverDraggerIndex 的 1 与 8 两档），判定要在未旋转坐标里做
        let (px, py) = anno_rot(x as f32, y as f32, cx, cy, -ang);
        let inside = px >= (tx - pad) as f32 && px <= (tx + bw + pad) as f32
            && py >= (ty - pad) as f32 && py <= (ty + bh + pad) as f32;
        if inside { 0 } else { 1 }
    } else { 0 };
    // 进编辑（startEdit）时内边距回到 6*dpi；从四角拉起来
    // 的那一档保留它自己的（缩放后的）值，拖完写回 AnnoShape
    let pad = if corner < 0 { ui::dpi::sc(ui::metric::TXT_PAD) } else { pad };
    *ANNO_TXE.lock_ok() = Some(TxEdit {
        idx, ch, caret, sel: caret, drag: false,
        x: tx, y: ty, fs, rgb: rgbv, bold, italic,
        ang, ang0: ang,
        rot0: if mode == 3 { ((y as f32) - cy).atan2((x as f32) - cx) } else { 0.0 },
        hov: false, rot_hot: mode == 3, fresh: idx < 0,
        focused: mode == 0, mode,
        m0: (x, y), box0: (tx, ty), fs0: fs, pad, pad0: pad,
        d0: if d < 1.0 { 1.0 } else { d },
    });
    // 点在正文里：光标落到点击位置（TextBox::onDown → moveCaret(hitTextIndex)）
    if mode == 0 && idx >= 0 {
        let (adv, _) = anno_tx_metrics(tx_fs_px(fs), bold, italic);
        let ci = {
            let e = ANNO_TXE.lock_ok();
            match e.as_ref() { Some(e) => anno_tx_caret_at(e, adv, x, y), None => caret }
        };
        if let Some(e) = ANNO_TXE.lock_ok().as_mut() { e.caret = ci; e.sel = ci; }
    }
    ANNO_CARET_ON.store(true, Ordering::Relaxed);
    let hw = ANNO_HWND.load(Ordering::Relaxed) as HWND;
    if !hw.is_null() {
        // 系统光标（1×2，从不 Show，所以屏幕上看不到我们画的这根）：只给输入法报位置用。
        // 没有它的时候，只设组字窗的话有些输入法会按自己的算法摆候选框 —— "乱跑"的来源。
        // 每次进编辑都重建一次（CreateCaret 会顶掉旧的）：它是跟着窗口走的，
        // 截图窗口关掉再开时旧的已经被系统销毁，用一次性开关判断会让新窗口一直没有光标
        CreateCaret(hw, std::ptr::null_mut(), 1, 2);
        SetTimer(hw, ANNO_TX_TIMER, ANNO_TX_BLINK_MS, None);
        InvalidateRect(hw, std::ptr::null_mut(), 0);
    }
}

/// 收笔：把编辑中的那条落地。空文本自己消失
/// 内容没变就什么都不做 —— 免得"点一下又点走"也占一步撤销
pub(crate) unsafe fn anno_tx_finish() {
    let ed = match ANNO_TXE.lock_ok().take() { Some(e) => e, None => return };
    let hw = ANNO_HWND.load(Ordering::Relaxed) as HWND;
    if !hw.is_null() { KillTimer(hw, ANNO_TX_TIMER); }
    // 收笔直接取编辑框里的文本，尾部换行照样留着
    // 多敲的 Enter 重新编辑时还得在，框高也按它算（尾部 \n 算一整行）
    let s: String = ed.ch.iter().collect();

    let nr = (ed.rgb & 0xFF) as u8;
    let ng = ((ed.rgb >> 8) & 0xFF) as u8;
    let nb = ((ed.rgb >> 16) & 0xFF) as u8;

    // 先判断"要不要改"，改之前拍快照 —— 快照必须落在改动之前，撤销才能退回去
    enum Act { Nothing, Push, Replace(usize), Remove(usize) }
    let act = {
        let shapes = ANNO_SHAPES.lock_ok();
        if ed.idx >= 0 {
            let i = ed.idx as usize;
            match shapes.get(i) {
                Some(o) if o.tool == 8 => {
                    let same = o.s == s && o.x0 == ed.x && o.y0 == ed.y && o.fs == ed.fs
                        && o.r == nr && o.g == ng && o.b == nb
                        && o.pad == ed.pad
                        && o.bold == ed.bold && o.italic == ed.italic && o.ang == ed.ang;
                    if s.is_empty() { Act::Remove(i) }
                    else if same { Act::Nothing }
                    else { Act::Replace(i) }
                }
                _ => Act::Nothing,
            }
        } else if s.is_empty() { Act::Nothing } else { Act::Push }
    };

    let mut keep = ed.idx;
    match act {
        // 编辑期间这条被重建跳过了（避免画两遍），收笔总要重放一次把它放回去
        Act::Nothing => { anno_rebuild(); }
        // 清空已有文字 = 直接把这条摘掉
        // History::removeShape 只是 erase，不拍快照）
        Act::Remove(i) => {
            ANNO_SHAPES.lock_ok().remove(i);
            keep = -1;
            anno_rebuild();
        }
        Act::Push | Act::Replace(_) => {
            anno_push_undo();
            let ns = AnnoShape {
                tool: 8, x0: ed.x, y0: ed.y, x1: ed.x, y1: ed.y,
                r: nr, g: ng, b: nb, width: 1, fill: false, ell: false,
                c2: -1, smart: false,
                val: 0, ang: ed.ang, pts: Vec::new(),
                s: s.clone(), fs: ed.fs, bold: ed.bold, italic: ed.italic, pad: ed.pad,
            };
            {
                let mut sh = ANNO_SHAPES.lock_ok();
                match act {
                    Act::Push => { sh.push(ns); keep = (sh.len() - 1) as i32; }
                    Act::Replace(i) => { sh[i] = ns; }
                    _ => {}
                }
            }
            anno_rebuild();
        }
    }

    // 最后一条 = 这条文字：手柄要能挂出来
    if keep >= 0 {
        let bx = {
            let sh = ANNO_SHAPES.lock_ok();
            sh.get(keep as usize).filter(|t| t.tool == 8).map(anno_shape_text_box)
        };
        if let Some((l, tp, r, b)) = bx {
            *ANNO_LAST.lock_ok() = Some(AnnoLast {
                tool: 8, x0: l, y0: tp, x1: r, y1: b,
                r: nr, g: ng, b: nb, width: 1, fill: false, ell: false,
                c2: -1, smart: false,
                val: 0, ang: ed.ang, pts: Vec::new(), idx: keep,
            });
            ANNO_LAST_HOT.store(false, Ordering::Relaxed);
        }
    } else if ed.idx >= 0 {
        anno_last_clear();
    }
    if !hw.is_null() { InvalidateRect(hw, std::ptr::null_mut(), 0); }
}

/// 丢掉编辑态，不落地（撤销/重做/裁剪时用：那一刻文字表已经整体换过了，
/// 再把编辑中的内容写回去会错位）
pub(crate) unsafe fn anno_tx_cancel() {
    if ANNO_TXE.lock_ok().take().is_some() {
        let hw = ANNO_HWND.load(Ordering::Relaxed) as HWND;
        if !hw.is_null() { KillTimer(hw, ANNO_TX_TIMER); }
        // 编辑期间那条文字被重建跳过了，取消后要重放一次让它回到画布上
        anno_rebuild();
    }
}

/// 右上角那个 ×：直接关掉文本框。
/// 输入中的内容一律不落地；本来就是已有的文字框就整条删掉 —— 先压一笔撤销，
/// 误点还能 Ctrl+Z 找回来（有字就走撤销栈）
pub(crate) unsafe fn anno_tx_close_box() {
    let idx = match ANNO_TXE.lock_ok().as_ref() {
        Some(e) => e.idx,
        None => return,
    };
    anno_tx_cancel();   // 编辑态摘掉，输到一半的字丢掉
    if idx >= 0 {
        anno_push_undo();
        {
            let mut sh = ANNO_SHAPES.lock_ok();
            if (idx as usize) < sh.len() && sh[idx as usize].tool == 8 { sh.remove(idx as usize); }
        }
        anno_rebuild();
        *ANNO_LAST.lock_ok() = None;
        ANNO_LAST_HOT.store(false, Ordering::Relaxed);
    }
    let hw = ANNO_HWND.load(Ordering::Relaxed) as HWND;
    if !hw.is_null() { InvalidateRect(hw, std::ptr::null_mut(), 0); }
}

/// 已提交文字右上角的 ×：整条删掉。
/// 先压一笔撤销 —— 走 history->undo，误点之后 Ctrl+Z 还能找回来
pub(crate) unsafe fn anno_txt_drop(i: usize) {
    anno_push_undo();
    {
        let mut sh = ANNO_SHAPES.lock_ok();
        if i < sh.len() && sh[i].tool == 8 { sh.remove(i); }
    }
    *ANNO_LAST.lock_ok() = None;
    ANNO_LAST_HOT.store(false, Ordering::Relaxed);
    ANNO_TXT_ROT_HOT.store(false, Ordering::Relaxed);
    anno_rebuild();
}

/// 单像素按覆盖率混色（src-over，sRGB 直混）。
/// 50% 覆盖格：底色 247 + 前景 (207,19,34) → (227,133,140)
/// 正好是 round(0.5*前景 + 0.5*底色)，说明是简单线性混合、不做 gamma 校正。
pub(crate) unsafe fn anno_blend_px(dc: winapi::shared::windef::HDC, x: i32, y: i32, rgb: u32, cov: f32) {
    if cov <= 0.002 { return; }
    let c = GetPixel(dc, x, y);
    if c == CLR_INVALID { return; }
    let a = if cov > 1.0 { 1.0 } else { cov };
    let k = 1.0 - a;
    let dr = (c & 0xFF) as f32;
    let dg = ((c >> 8) & 0xFF) as f32;
    let db = ((c >> 16) & 0xFF) as f32;
    let sr = (rgb & 0xFF) as f32;
    let sg = ((rgb >> 8) & 0xFF) as f32;
    let sb = ((rgb >> 16) & 0xFF) as f32;
    let r = (sr * a + dr * k).round() as u32;
    let g = (sg * a + dg * k).round() as u32;
    let b = (sb * a + db * k).round() as u32;
    SetPixelV(dc, x, y, r | (g << 8) | (b << 16));
}

/// 任意方向的 1px 抗锯齿实线段：逐像素按到线段的距离算覆盖率。
/// 框转了角度之后四条边不再是横竖线，逐像素铺的那种写法用不上，只能走这一版
pub(crate) unsafe fn anno_aa_seg(dc: winapi::shared::windef::HDC, p0: (f32, f32), p1: (f32, f32), rgb: u32) {
    let (dx, dy) = (p1.0 - p0.0, p1.1 - p0.1);
    let len2 = dx * dx + dy * dy;
    if len2 < 0.0001 { return; }
    let minx = p0.0.min(p1.0).floor() as i32 - 1;
    let maxx = p0.0.max(p1.0).ceil() as i32 + 1;
    let miny = p0.1.min(p1.1).floor() as i32 - 1;
    let maxy = p0.1.max(p1.1).ceil() as i32 + 1;
    for y in miny..=maxy {
        for x in minx..=maxx {
            let cx = x as f32 + 0.5;
            let cy = y as f32 + 0.5;
            let t = (((cx - p0.0) * dx + (cy - p0.1) * dy) / len2).clamp(0.0, 1.0);
            let qx = p0.0 + dx * t;
            let qy = p0.1 + dy * t;
            let d = ((cx - qx) * (cx - qx) + (cy - qy) * (cy - qy)).sqrt();
            let cov = 1.0 - d;   // 半宽 0.5：距离 0.5 时盖一半
            if cov > 0.002 { anno_blend_px(dc, x, y, rgb, cov); }
        }
    }
}

/// 旋转后的虚线框：几何口径与 anno_dash_frame 完全一致（中心线内缩半格、2 实 2 虚、
/// 里程沿路径连续累加），只是每条边先绕框中心转过去，再用任意方向线段铺
pub(crate) unsafe fn anno_tx_dash(dc: winapi::shared::windef::HDC, l: i32, t: i32, r: i32, b: i32,
                       ang: f32, rgb: u32) {
    if ang == 0.0 {
        anno_dash_frame(dc, l, t, r, b, rgb);
        return;
    }
    let x0 = l as f32 + 0.5;
    let y0 = t as f32 + 0.5;
    let x1 = r as f32 - 0.5;
    let y1 = b as f32 - 0.5;
    let w = x1 - x0;
    let h = y1 - y0;
    if w < 1.0 || h < 1.0 { return; }
    const DASH: f32 = ui::layout::anno::TXT_DASH;
    const PERIOD: f32 = ui::layout::anno::TXT_DASH_PERIOD;
    let (cx, cy) = ((l + r) as f32 / 2.0, (t + b) as f32 / 2.0);
    let rp = |x: f32, y: f32| anno_rot(x, y, cx, cy, ang);
    let cs = [rp(x0, y0), rp(x1, y0), rp(x1, y1), rp(x0, y1)];
    let mut base = 0.0f32;
    for i in 0..4 {
        let (ax, ay) = cs[i];
        let (bx, by) = cs[(i + 1) % 4];
        let len = if i % 2 == 0 { w } else { h };
        let (ux, uy) = ((bx - ax) / len, (by - ay) / len);
        let mut k = (base / PERIOD).floor() as i32;
        let end = base + len;
        loop {
            let s = k as f32 * PERIOD;
            let e = s + DASH;
            let a = if s > base { s } else { base };
            let bb = if e < end { e } else { end };
            if a < bb {
                let ta = a - base;
                let tb = bb - base;
                anno_aa_seg(dc, (ax + ux * ta, ay + uy * ta), (ax + ux * tb, ay + uy * tb), rgb);
            }
            if e > end { break; }
            k += 1;
        }
        base = end;
    }
}

/// 单像素覆盖率混色 + 图元 alpha。
pub(crate) unsafe fn anno_blend_px_a(dc: winapi::shared::windef::HDC, x: i32, y: i32, rgb: u32, cov: f32, alpha: f32) {
    if cov <= 0.002 { return; }
    anno_blend_px(dc, x, y, rgb, cov * alpha);
}

/// 抗锯齿实心圆：覆盖率按到圆周的距离线性过渡。
pub(crate) unsafe fn anno_disc(dc: winapi::shared::windef::HDC, cx: f32, cy: f32, r: f32, rgb: u32, alpha: f32) {
    let x0 = (cx - r - 1.0).floor() as i32;
    let x1 = (cx + r + 1.0).ceil() as i32;
    let y0 = (cy - r - 1.0).floor() as i32;
    let y1 = (cy + r + 1.0).ceil() as i32;
    for y in y0..=y1 {
        for x in x0..=x1 {
            let dx = x as f32 + 0.5 - cx;
            let dy = y as f32 + 0.5 - cy;
            let cov = r + 0.5 - (dx * dx + dy * dy).sqrt();
            if cov > 0.002 { anno_blend_px_a(dc, x, y, rgb, cov, alpha); }
        }
    }
}

/// 抗锯齿圆环描边：中心线半径 r、线宽 w，覆盖率按 |d-r| 过渡。
pub(crate) unsafe fn anno_ring(dc: winapi::shared::windef::HDC, cx: f32, cy: f32, r: f32, w: f32, rgb: u32, alpha: f32) {
    let hw = w * 0.5;
    let rr = r + hw + 1.0;
    let x0 = (cx - rr).floor() as i32;
    let x1 = (cx + rr).ceil() as i32;
    let y0 = (cy - rr).floor() as i32;
    let y1 = (cy + rr).ceil() as i32;
    for y in y0..=y1 {
        for x in x0..=x1 {
            let dx = x as f32 + 0.5 - cx;
            let dy = y as f32 + 0.5 - cy;
            let d = (dx * dx + dy * dy).sqrt();
            let cov = hw + 0.5 - (d - r).abs();
            if cov > 0.002 { anno_blend_px_a(dc, x, y, rgb, cov, alpha); }
        }
    }
}

/// 两段 150° 圆弧：右侧 -75°→75°、左侧 105°→255°，正上/正下各留 30° 缺口。
/// 端头是径向切口：|角度| 落在 (75°,105°) 的像素跳过
/// 不会画成整圈。
pub(crate) unsafe fn anno_rot_arcs(dc: winapi::shared::windef::HDC, cx: f32, cy: f32, r: f32, w: f32,
                        rgb: u32, alpha: f32) {
    let hw = w * 0.5;
    let rr = r + hw + 1.0;
    let half_gap = ui::layout::anno::TX_ROT_ARC_GAP_DEG * 0.5;
    let x0 = (cx - rr).floor() as i32;
    let x1 = (cx + rr).ceil() as i32;
    let y0 = (cy - rr).floor() as i32;
    let y1 = (cy + rr).ceil() as i32;
    for y in y0..=y1 {
        for x in x0..=x1 {
            let dx = x as f32 + 0.5 - cx;
            let dy = y as f32 + 0.5 - cy;
            let d = (dx * dx + dy * dy).sqrt();
            let cov = hw + 0.5 - (d - r).abs();
            if cov <= 0.002 { continue; }
            let deg = dy.atan2(dx).to_degrees().abs();
            if deg > 90.0 - half_gap && deg < 90.0 + half_gap { continue; }
            anno_blend_px_a(dc, x, y, rgb, cov, alpha);
        }
    }
}

/// 任意方向的抗锯齿实线段，线宽 w，端头平齐。
pub(crate) unsafe fn anno_line_w(dc: winapi::shared::windef::HDC, a: (f32, f32), b: (f32, f32), w: f32,
                      rgb: u32, alpha: f32) {
    let (dx, dy) = (b.0 - a.0, b.1 - a.1);
    let len2 = dx * dx + dy * dy;
    if len2 < 0.0001 { return; }
    let len = len2.sqrt();
    let hw = w * 0.5;
    let x0 = (a.0.min(b.0) - w - 1.0).floor() as i32;
    let x1 = (a.0.max(b.0) + w + 1.0).ceil() as i32;
    let y0 = (a.1.min(b.1) - w - 1.0).floor() as i32;
    let y1 = (a.1.max(b.1) + w + 1.0).ceil() as i32;
    for y in y0..=y1 {
        for x in x0..=x1 {
            let px = x as f32 + 0.5 - a.0;
            let py = y as f32 + 0.5 - a.1;
            let t = (px * dx + py * dy) / len2;
            // 侧向距离（到无限长直线）+ 端头截断，合起来就是"平头线段"的覆盖率
            let side = ((px * dy - py * dx) / len).abs();
            let cov = (hw + 0.5 - side).min((0.5 + (t * len).min((1.0 - t) * len)).min(1.0));
            if cov > 0.002 { anno_blend_px_a(dc, x, y, rgb, cov, alpha); }
        }
    }
}

/// 三角形（tip / b1 / b2，对应 addArrowHead 的几何）的有符号距离：内部为正
pub(crate) fn anno_tri_sdf(tip: (f32, f32), b1: (f32, f32), b2: (f32, f32), px: f32, py: f32) -> f32 {
    let mut v = [tip, b1, b2];
    let cr = (v[1].0 - v[0].0) * (v[2].1 - v[0].1) - (v[1].1 - v[0].1) * (v[2].0 - v[0].0);
    if cr < 0.0 { v.swap(1, 2); }   // 统一成逆时针，内部 = 每条边左侧
    let mut d = f32::MAX;
    for i in 0..3 {
        let (ax, ay) = v[i];
        let (bx, by) = v[(i + 1) % 3];
        let (ex, ey) = (bx - ax, by - ay);
        let l = (ex * ex + ey * ey).sqrt();
        if l <= 0.0 { continue; }
        let s = (ex * (py - ay) - ey * (px - ax)) / l;
        if s < d { d = s; }
    }
    d
}

/// 实心三角抗锯齿填充：覆盖率 = 0.5 + 有符号距离。
pub(crate) unsafe fn anno_tri_fill(dc: winapi::shared::windef::HDC, tip: (f32, f32), dir: (f32, f32),
                        hs: f32, rgb: u32, alpha: f32) {
    let l = (dir.0 * dir.0 + dir.1 * dir.1).sqrt();
    if l <= 0.0 { return; }
    let (ux, uy) = (dir.0 / l, dir.1 / l);
    let (px, py) = (-uy, ux);
    let bw = hs * 0.75;
    let base = (tip.0 - ux * hs, tip.1 - uy * hs);
    let b1 = (base.0 + px * bw, base.1 + py * bw);
    let b2 = (base.0 - px * bw, base.1 - py * bw);
    let x0 = tip.0.min(b1.0).min(b2.0).floor() as i32 - 1;
    let x1 = tip.0.max(b1.0).max(b2.0).ceil() as i32 + 1;
    let y0 = tip.1.min(b1.1).min(b2.1).floor() as i32 - 1;
    let y1 = tip.1.max(b1.1).max(b2.1).ceil() as i32 + 1;
    for y in y0..=y1 {
        for x in x0..=x1 {
            let cov = 0.5 + anno_tri_sdf(tip, b1, b2, x as f32 + 0.5, y as f32 + 0.5);
            if cov > 0.002 { anno_blend_px_a(dc, x, y, rgb, cov, alpha); }
        }
    }
}

/// 三角外一圈白晕： 是先按 2*halo 描边再填蓝，留下的就是这一圈
pub(crate) unsafe fn anno_tri_halo(dc: winapi::shared::windef::HDC, tip: (f32, f32), dir: (f32, f32),
                        hs: f32, halo: f32, rgb: u32) {
    let l = (dir.0 * dir.0 + dir.1 * dir.1).sqrt();
    if l <= 0.0 { return; }
    let (ux, uy) = (dir.0 / l, dir.1 / l);
    let (px, py) = (-uy, ux);
    let bw = hs * 0.75;
    let base = (tip.0 - ux * hs, tip.1 - uy * hs);
    let b1 = (base.0 + px * bw, base.1 + py * bw);
    let b2 = (base.0 - px * bw, base.1 - py * bw);
    let x0 = tip.0.min(b1.0).min(b2.0).floor() as i32 - halo as i32 - 1;
    let x1 = tip.0.max(b1.0).max(b2.0).ceil() as i32 + halo as i32 + 1;
    let y0 = tip.1.min(b1.1).min(b2.1).floor() as i32 - halo as i32 - 1;
    let y1 = tip.1.max(b1.1).max(b2.1).ceil() as i32 + halo as i32 + 1;
    for y in y0..=y1 {
        for x in x0..=x1 {
            let sdf = anno_tri_sdf(tip, b1, b2, x as f32 + 0.5, y as f32 + 0.5);
            let cov = halo + 0.5 - sdf.abs();
            if cov > 0.002 { anno_blend_px(dc, x, y, rgb, cov); }
        }
    }
}

/// 四角柄的光标：跟框的实际姿态走。转了 45° 之后，看起来是 "\" 的那个角
/// 已经成了一条竖线，再给斜双箭头就怪了 —— 按这个角在屏幕上的实际朝向挑最接近的那款
pub(crate) fn anno_tx_scale_cursor(ang: f32, k: i32) -> *const u16 {
    let (dx, dy) = match k {
        0 => (-1.0f32, -1.0f32),
        2 => (1.0, 1.0),
        _ => (-1.0, 1.0),
    };
    let a = ang * std::f32::consts::PI / 180.0;
    let (cs, sn) = (a.cos(), a.sin());
    let wx = dx * cs - dy * sn;
    let wy = dx * sn + dy * cs;
    let sector = (((wy.atan2(wx) * 180.0
        / std::f32::consts::PI / 45.0).round() as i32) % 4 + 4) % 4;
    match sector {
        1 => IDC_SIZENWSE,
        2 => 32645 as *const u16,   // IDC_SIZENS
        3 => IDC_SIZENESW,
        _ => IDC_SIZEWE,
    }
}

/// 编辑中鼠标压在柄上时那圈"旋转箭头"
/// 两段 150° 蓝弧（正上/正下各留 30° 缺口）+ 缺口里两个实心箭头，上朝右、下朝左。
/// 蓝弧外面先垫一圈白晕（同一路径描粗 lw+2*halo），箭头也是先描白边再填蓝 ——
/// 线宽和半径都按 dpi 走，不是整像素圆环。
pub(crate) unsafe fn anno_rot_arrows(dc: winapi::shared::windef::HDC, cx: f32, cy: f32) {
    let hr = ui::dpi::scale_f(ui::metric::TX_ROT_R as f32);
    let r = hr * ui::layout::anno::TX_ROT_HOT_K;
    let hs = hr * ui::layout::anno::TX_ROT_HOT_HS;
    let dpi = ui::dpi::scale_f(1.0).max(1.0);
    let lw = dpi * ui::layout::anno::TX_ROT_LW_K;
    let halo = dpi;
    let white = ui::color::cref(ui::color::TEXT_INVERSE);
    let blue = ui::color::cref(ui::color::DRAGGER);
    let alpha = ui::alpha::DRAGGER as f32 / 255.0;
    // 白的那遍描宽 2*halo，露在蓝弧外的就成了一圈白晕
    anno_rot_arcs(dc, cx, cy, r, lw + halo * 2.0, white, 1.0);
    anno_rot_arcs(dc, cx, cy, r, lw, blue, alpha);
    // 箭头基点落在 pt(-90)/pt(90) 上，尖端沿切线再伸 hs：addArrowHead 的 tip = p + dir*hs
    let top = (cx + hs, cy - r);
    let bot = (cx - hs, cy + r);
    anno_tri_halo(dc, top, (1.0, 0.0), hs, halo, white);
    anno_tri_halo(dc, bot, (-1.0, 0.0), hs, halo, white);
    anno_tri_fill(dc, top, (1.0, 0.0), hs, blue, alpha);
    anno_tri_fill(dc, bot, (-1.0, 0.0), hs, blue, alpha);
}

/// 旋转柄静息态：白底实心圆点 + 一圈蓝描边（paintDragger 的 !hot 分支）
/// FillEllipse 白 + DrawEllipse 蓝，线宽 lw*1.4，全都按 dpi 为浮点）
pub(crate) unsafe fn anno_rot_dot(dc: winapi::shared::windef::HDC, cx: f32, cy: f32) {
    let r = ui::dpi::scale_f(ui::metric::TX_ROT_R as f32) * ui::layout::anno::TX_ROT_DOT_K;
    let dpi = ui::dpi::scale_f(1.0).max(1.0);
    anno_disc(dc, cx, cy, r, ui::color::cref(ui::color::TEXT_INVERSE), 1.0);
    anno_ring(dc, cx, cy, r, dpi * ui::layout::anno::TX_ROT_DOT_LW_K,
              ui::color::cref(ui::color::DRAGGER), ui::alpha::DRAGGER as f32 / 255.0);
}

/// 编辑态的四角柄 + 旋转柄。
/// 位置全部来自 anno_tx_hnd_pts —— 画在这里、命中也用同一份，保证"看得见的就能点得到"。
/// 四角是白环 + 蓝实心圆：3 个画随框旋转的 45° 双箭头（缩放），右上角画 ×（关闭）。
/// 尺寸/线宽/颜色都按 dpi 的 handleRadius=6*dpi、side=10*dpi、ring=max(1,dpi) 走
/// 用抗锯齿光栅化，不再用 GDI 的整像素 Ellipse/Polygon。
pub(crate) unsafe fn anno_tx_handles(dc: winapi::shared::windef::HDC, ox: i32, oy: i32, e: &TxEdit) {
    let (corners, rot, _) = anno_tx_hnd_pts(e);
    let side = ui::dpi::scale_f(ui::layout::anno::TX_HND_DIA as f32); // 10*dpi
    let half = side * 0.5;
    let dpi = ui::dpi::scale_f(1.0).max(1.0);
    let ring = dpi;                     // 白环厚度 = max(1,dpi)
    let lw = dpi * ui::layout::anno::TX_HND_LW_K; // 图形线宽 = max(1,dpi)*1.1
    let white = ui::color::cref(ui::color::TEXT_INVERSE);
    let blue = ui::color::cref(ui::color::DRAGGER);
    let alpha = ui::alpha::DRAGGER as f32 / 255.0;
    // 柄里的图形跟着框转：同一个 angle、以柄心为中心
    let a = e.ang * std::f32::consts::PI / 180.0;
    let (sn, cs) = (a.sin(), a.cos());
    let rotv = |vx: f32, vy: f32| (vx * cs - vy * sn, vx * sn + vy * cs);
    for (k, &(hx_, hy_)) in corners.iter().enumerate() {
        let hx = (ox + hx_) as f32;
        let hy = (oy + hy_) as f32;
        anno_disc(dc, hx, hy, half + ring, white, 1.0); // 白环：蓝圆压在蓝色调截图上才分得清
        anno_disc(dc, hx, hy, half, blue, alpha);       // 蓝底
        if k == 1 {
            // ×：留一点边距，撑满圆底但不顶边（offset = side*0.18）
            let o = side * ui::layout::anno::TX_HND_CLOSE;
            let p = |dx: f32, dy: f32| {
                let (rx, ry) = rotv(dx, dy);
                (hx + rx, hy + ry)
            };
            anno_line_w(dc, p(-o, -o), p(o, o), lw, white, 1.0);
            anno_line_w(dc, p(-o, o), p(o, -o), lw, white, 1.0);
        } else {
            // 45° 双头箭头：一根斜杆 + 两头各一个实心三角。左上是 "\"，左下是 "/"
            let slope: f32 = if k == 3 { -1.0 } else { 1.0 };
            let inv = std::f32::consts::FRAC_1_SQRT_2;
            let d = rotv(inv, inv * slope);
            let sl = side * ui::layout::anno::TX_HND_SLOPE; // 斜杆半长
            let hs = side * ui::layout::anno::TX_HND_ARROW; // 箭头长度
            let (ax, ay) = (hx - d.0 * sl, hy - d.1 * sl);
            let (bx, by) = (hx + d.0 * sl, hy + d.1 * sl);
            anno_line_w(dc, (ax, ay), (bx, by), lw, white, 1.0);
            anno_tri_fill(dc, (bx + d.0 * hs, by + d.1 * hs), d, hs, white, 1.0);
            anno_tri_fill(dc, (ax - d.0 * hs, ay - d.1 * hs), (-d.0, -d.1), hs, white, 1.0);
        }
    }
    let rx = (ox + rot.0) as f32;
    let ry = (oy + rot.1) as f32;
    // 静息：白点 + 蓝环；悬停/按下：两段弧 + 箭头（状态只换形态，不换颜色）
    if e.rot_hot { anno_rot_arrows(dc, rx, ry); } else { anno_rot_dot(dc, rx, ry); }
}


pub(crate) unsafe fn anno_dash_seg(dc: winapi::shared::windef::HDC, bx: f32, by: f32, ux: f32, uy: f32,
                       pa: f32, pb: f32, rgb: u32) {
    let (ax, ay) = (bx + ux * pa, by + uy * pa);
    let (ex, ey) = (bx + ux * pb, by + uy * pb);
    if ux != 0.0 {
        let py = by.floor() as i32;
        let (lo, hi) = if ax < ex { (ax, ex) } else { (ex, ax) };
        let mut xi = lo.floor() as i32;
        while (xi as f32) < hi {
            let cov = hi.min(xi as f32 + 1.0) - lo.max(xi as f32);
            anno_blend_px(dc, xi, py, rgb, cov);
            xi += 1;
        }
    } else {
        let px = bx.floor() as i32;
        let (lo, hi) = if ay < ey { (ay, ey) } else { (ey, ay) };
        let mut yi = lo.floor() as i32;
        while (yi as f32) < hi {
            let cov = hi.min(yi as f32 + 1.0) - lo.max(yi as f32);
            anno_blend_px(dc, px, yi, rgb, cov);
            yi += 1;
        }
    }
}

/// 编辑态的虚线框 —— 逐像素几何
///
/// 几何是从实测像素反推出来的（纯色底上取 RGB 反算覆盖率，10 个字号都对得上）：
///   · 中心线 = floor(rect.边) ± 半个笔宽。1px 笔正好压满一整列/行，不留半格；
///   · 路径从上左角出发、顺时针绕一圈，dash 是 **2 实 2 虚**，相位沿路径**连续累加**：
///     所以下边比上边错开 1 格、左右竖边各自与上边对齐（周长 2*(12+33)=90，
///     90 mod 4 = 2，绕回来正好接上）；
///   · 端头按面积覆盖率抗锯齿，覆盖率序列实测为 1.0 / 0.5 / 0 / 0.5。
/// GDI 的虚线笔画不出最后这半格（它只有整格开/关），所以这里自己逐像素混，
/// 放大 6 倍和 并排也看不出差别
pub(crate) unsafe fn anno_dash_frame(dc: winapi::shared::windef::HDC, l: i32, t: i32, r: i32, b: i32, rgb: u32) {
    let x0 = l as f32 + 0.5;
    let y0 = t as f32 + 0.5;
    let x1 = r as f32 - 0.5;
    let y1 = b as f32 - 0.5;
    let w = x1 - x0;
    let h = y1 - y0;
    if w < 1.0 || h < 1.0 { return; }   // 框比笔还小：D2D 那边也是直接跳过
    const DASH: f32 = ui::layout::anno::TXT_DASH;
    const PERIOD: f32 = ui::layout::anno::TXT_DASH_PERIOD;
    // 四条边：(起点, 方向, 边长, 该边起点在整条路径上的里程)
    let edges: [(f32, f32, f32, f32, f32); 4] = [
        (x0, y0, 1.0, 0.0, w),          // 上：→
        (x1, y0, 0.0, 1.0, h),          // 右：↓
        (x1, y1, -1.0, 0.0, w),         // 下：←
        (x0, y1, 0.0, -1.0, h),         // 左：↑
    ];
    let mut base = 0.0f32;
    let mut corner = [(0i32, 0i32); 4];   // TL / TR / BR / BL 四角所在的那个像素
    let mut straddle = [false; 4];        // 转折处是否"一段实线正好跨过角"
    for (i, &(bx, by, ux, uy, len)) in edges.iter().enumerate() {
        let mut k = (base / PERIOD).floor() as i32;
        let end = base + len;
        loop {
            let s = k as f32 * PERIOD;
            let e = s + DASH;
            let a = if s > base { s } else { base };
            let bb = if e < end { e } else { end };
            if a < bb {
                anno_dash_seg(dc, bx, by, ux, uy, a - base, bb - base, rgb);
            }
            if e > end { break; }
            k += 1;
        }
        base = end;
        // 角点像素（中心线各偏半格，所以就是 floor(中心线)）
        let (cx, cy) = match i {
            0 => (x1.floor() as i32, y0.floor() as i32),   // 右上
            1 => (x1.floor() as i32, y1.floor() as i32),   // 右下
            2 => (x0.floor() as i32, y1.floor() as i32),   // 左下
            _ => (x0.floor() as i32, y0.floor() as i32),   // 左上（闭合点）
        };
        corner[(i + 1) % 4] = (cx, cy);
        let m = base % PERIOD;
        straddle[(i + 1) % 4] = m > 0.01 && m < DASH - 0.01;
    }
    // 转角处一段实线若**跨过**角（不是正好起/止于角），斜接（MITER）要把外侧那 1/4 格补满：
    // 那格的实心面积 = 横边半格 ∪ 竖边半格 ∪ 斜接的 1/4（互不重叠）= 整整一格。
    // 这两格就是纯色（207,19,34），不补的话两个下角只有 0.75 覆盖率、淡一档
    // 实线正好在角上起/止的角（右上、左上）不补。
    for i in 0..4 {
        if straddle[i] {
            anno_blend_px(dc, corner[i].0, corner[i].1, rgb, 1.0);
        }
    }
}

pub(crate) fn anno_tx_sel_range(e: &TxEdit) -> (usize, usize) {
    let n = e.ch.len();
    if e.sel < e.caret { (e.sel.min(n), e.caret.min(n)) }
    else { (e.caret.min(n), e.sel.min(n)) }
}

/// 选中文字的高亮块（未旋转坐标）
/// alpha 0x99（≈0.6）盖在底图上，文字再压在上面
pub(crate) unsafe fn anno_tx_sel_rects(e: &TxEdit) -> Vec<(i32, i32, i32, i32)> {
    let n = e.ch.len();
    let (a, b) = anno_tx_sel_range(e);
    if a == b { return Vec::new(); }
    let mut starts = vec![0usize];
    for (i, &c) in e.ch.iter().enumerate() { if c == '\n' { starts.push(i + 1); } }
    let mut out = Vec::new();
    for (line, &s0) in starts.iter().enumerate() {
        let mut e0 = s0;
        while e0 < n && e.ch[e0] != '\n' { e0 += 1; }
        if b <= s0 { break; }
        if a > e0 { continue; }
        let ls = a.max(s0);
        let le = b.min(e0);
        if le <= ls { continue; }
        let p1: String = e.ch[s0..ls].iter().collect();
        let p2: String = e.ch[s0..le].iter().collect();
        let x0 = e.x + anno_tx_measure(tx_fs_px(e.fs), e.bold, e.italic, &p1);
        let x1 = e.x + anno_tx_measure(tx_fs_px(e.fs), e.bold, e.italic, &p2);
        let y0 = e.y + anno_tx_line_y(tx_fs_px(e.fs), line);
        let y1 = e.y + (anno_tx_line_y(tx_fs_px(e.fs), line + 1));
        if x1 > x0 && y1 > y0 { out.push((x0, y0, x1, y1)); }
    }
    out
}

/// 把选区高亮混进底图（旋转时逐点反算是否落在块内）
pub(crate) unsafe fn anno_tx_paint_sel(dc: winapi::shared::windef::HDC, ox: i32, oy: i32, e: &TxEdit) {
    let rects = anno_tx_sel_rects(e);
    if rects.is_empty() { return; }
    const SEL_RGB: u32 = ui::color::cref(ui::color::TEXT_SELECTION);   // RGB(0x99,0xC9,0xEF)
    let (ccx, ccy) = anno_tx_center_of(e);
    for (x0, y0, x1, y1) in rects {
        if e.ang == 0.0 {
            for y in y0..y1 {
                for x in x0..x1 { anno_blend_px(dc, ox + x, oy + y, SEL_RGB, 0.6); }
            }
        } else {
            let corners = [
                anno_rot(x0 as f32, y0 as f32, ccx, ccy, e.ang),
                anno_rot(x1 as f32, y0 as f32, ccx, ccy, e.ang),
                anno_rot(x1 as f32, y1 as f32, ccx, ccy, e.ang),
                anno_rot(x0 as f32, y1 as f32, ccx, ccy, e.ang),
            ];
            let minx = corners.iter().map(|p| p.0).fold(f32::MAX, f32::min).floor() as i32;
            let maxx = corners.iter().map(|p| p.0).fold(f32::MIN, f32::max).ceil() as i32;
            let miny = corners.iter().map(|p| p.1).fold(f32::MAX, f32::min).floor() as i32;
            let maxy = corners.iter().map(|p| p.1).fold(f32::MIN, f32::max).ceil() as i32;
            for y in miny..=maxy {
                for x in minx..=maxx {
                    let (lx, ly) = anno_rot(x as f32 + 0.5, y as f32 + 0.5, ccx, ccy, -e.ang);
                    if lx >= x0 as f32 && lx < x1 as f32 && ly >= y0 as f32 && ly < y1 as f32 {
                        anno_blend_px(dc, ox + x, oy + y, SEL_RGB, 0.6);
                    }
                }
            }
        }
    }
}

/// 只画"正在编辑的那条文字"（正文 + 光标 + 虚线框 + 手柄）。
/// 收笔后的文字已经烘进位图、按 z 序参与了层叠，这里不再画第二遍 ——
/// 不能所有文字都在这一层统一后画，否则文字永远压在马赛克/模糊上面
pub(crate) unsafe fn anno_tx_paint(dc: winapi::shared::windef::HDC, ox: i32, oy: i32) {
    let e = match ANNO_TXE.lock_ok().clone() { Some(e) => e, None => return };
    let s: String = e.ch.iter().collect();
    let cur = AnnoText { s: s.clone(), x: e.x, y: e.y, fs: e.fs, rgb: e.rgb,
                         bold: e.bold, italic: e.italic, ang: e.ang, pad: e.pad };
    // 选区高亮先铺、文字后压：跟 的绘制顺序一致（selectionBg 在文字下面）
    anno_tx_paint_sel(dc, ox, oy, &e);
    anno_tx_blit(dc, ox + e.x, oy + e.y, &cur);
    let (_, _, adv) = anno_tx_box(&s, tx_fs_px(e.fs), e.bold, e.italic);
    if e.focused && ANNO_CARET_ON.load(Ordering::Relaxed) {
        let (cxp, cyp) = anno_tx_caret_xy(&e, adv);
        let (_, caret_h) = anno_tx_metrics(tx_fs_px(e.fs), e.bold, e.italic);
        let (cx, cy) = anno_tx_center_of(&e);
        let (ax, ay) = anno_rot(cxp as f32, cyp as f32, cx, cy, e.ang);
        let (bx, by) = anno_rot(cxp as f32, (cyp + caret_h.max(2)) as f32, cx, cy, e.ang);
        let br = CreateSolidBrush(e.rgb);
        if e.ang == 0.0 {
            let rc = RECT { left: ox + cxp, top: oy + cyp, right: ox + cxp + 1, bottom: oy + cyp + caret_h.max(2) };
            FillRect(dc, &rc, br);
        } else {
            let pen = CreatePen(PS_SOLID as i32, ui::metric::STROKE, e.rgb);
            let oldp = SelectObject(dc, pen as *mut winapi::ctypes::c_void);
            MoveToEx(dc, ox + ax.round() as i32, oy + ay.round() as i32, std::ptr::null_mut());
            LineTo(dc, ox + bx.round() as i32, oy + by.round() as i32);
            SelectObject(dc, oldp);
            DeleteObject(pen as *mut winapi::ctypes::c_void);
        }
        DeleteObject(br as *mut winapi::ctypes::c_void);
    }
    // 虚线框：2 实 2 虚 + 逐图元抗锯齿
    // PS_DOT 是 1 实 1 虚、几何笔也只有整格开关，两头都对不上，所以自己画
    let (l, t, r, b) = anno_tx_frame_rect(&e);
    anno_tx_dash(dc, ox + l, oy + t, ox + r, oy + b, e.ang, e.rgb);
    // 输入过程中鼠标压到虚线上：四角柄 + 旋转柄亮出来；移开就收。
    // handlesVisible 就为真，整段手势里柄不消失
    if e.mode != 0 || (e.hov && e.focused) {
        anno_tx_handles(dc, ox, oy, &e);
    }
}

/// 导出/复制/贴屏用的成品位图。文字已经在图元重放时按 z 序烘进去了，
/// 所以这里只管把手上的编辑收笔、把工作位图交出去
pub(crate) unsafe fn anno_tx_flat() -> Vec<u8> {
    if ANNO_TXE.lock_ok().is_some() { anno_tx_finish(); }
    ANNO_IMG.lock_ok().clone()
}

// 输入法接口：组字窗 / 候选框要贴到光标上
// 不设的话候选框会跑到窗口左上角
#[repr(C)]
#[derive(Clone, Copy)]
#[allow(non_snake_case)]
pub(crate) struct COMPOSITIONFORM {
    dwStyle: u32,
    ptCurrentPos: POINT,
    rcArea: RECT,
}
#[repr(C)]
#[derive(Clone, Copy)]
#[allow(non_snake_case)]
pub(crate) struct CANDIDATEFORM {
    dwIndex: u32,
    dwStyle: u32,
    ptCurrentPos: POINT,
    rcArea: RECT,
}
#[link(name = "imm32")]
extern "system" {
    fn ImmGetContext(hwnd: HWND) -> isize;
    fn ImmReleaseContext(hwnd: HWND, himc: isize) -> i32;
    fn ImmSetCompositionWindow(himc: isize, form: *const COMPOSITIONFORM) -> i32;
    fn ImmSetCandidateWindow(himc: isize, form: *const CANDIDATEFORM) -> i32;
    // 截图遮罩要摘掉输入法：中文状态下按 C 是组字，WM_KEYDOWN 收不到
    pub(crate) fn ImmAssociateContext(hwnd: HWND, himc: isize) -> isize;
}
pub(crate) const WM_IME_STARTCOMPOSITION: UINT = 0x010D;
pub(crate) const WM_IME_COMPOSITION: UINT = 0x010F;
pub(crate) const WM_IME_SETCONTEXT: UINT = 0x0281;
pub(crate) const WM_IME_NOTIFY: UINT = 0x0282;
pub(crate) const IMN_OPENCANDIDATE: usize = 0x0005;
pub(crate) const IMN_CHANGECANDIDATE: usize = 0x0006;
pub(crate) const CFS_FORCE_POSITION: u32 = 0x0020;
pub(crate) const CFS_CANDIDATEPOS: u32 = 0x0002;
pub(crate) const ANNO_IME_NUDGE: usize = 8;   // 组字窗补摆定时器（30ms）

/// 把组字窗 / 候选框摆到光标处。comp/cand 分开控：组字开始摆两个，
/// 候选框开/翻页只摆候选框。
///
/// 坐标一律用宿主窗口的**客户区**坐标 —— 宿主是全屏窗，按屏幕坐标递进去，
/// 多屏时窗口原点不等于 (0,0)，整块组字窗就会偏出去。
///
/// 候选框这一档不用 CFS_EXCLUDE：那是"告诉输入法别压住这块区域，自己找地方放"，
/// 各输入法对"放上面还是放下面"的取舍不同，组字串一变长就翻边、左右横跳 ——
/// 也就是用户看到的"候选框乱跑"。因此用 CFS_CANDIDATEPOS：位置由我们给定，
/// 每次组字都落在同一处，跟输入法自己的算法无关。
/// 另外把系统光标（隐藏的）也摆到同一格：部分输入法不看组字窗、直接跟 GetCaretPos。
pub(crate) unsafe fn anno_ime_place(hwnd: HWND, comp: bool, cand: bool) {
    let info = {
        let g = ANNO_TXE.lock_ok();
        match g.as_ref() {
            Some(e) if e.focused => {
                let s: String = e.ch.iter().collect();
                let (_, _, adv) = anno_tx_box(&s, tx_fs_px(e.fs), e.bold, e.italic);
                let (cx, cy) = anno_tx_caret_pos(e, adv);
                let (_, caret_h) = anno_tx_metrics(tx_fs_px(e.fs), e.bold, e.italic);
                Some((cx, cy, caret_h))
            }
            _ => None,
        }
    };
    let (lx, ly, lh) = match info { Some(v) => v, None => return };
    let cx = ANNO_OX.load(Ordering::Relaxed) + lx;
    let cy_top = ANNO_OY.load(Ordering::Relaxed) + ly;
    SetCaretPos(cx, cy_top);
    let himc = ImmGetContext(hwnd);
    if himc == 0 { return; }
    if comp {
        // 组字窗贴着光标顶端：组字中的文字应该出现在正在编辑的那一行上
        let f = COMPOSITIONFORM {
            dwStyle: CFS_FORCE_POSITION,
            ptCurrentPos: POINT { x: cx, y: cy_top },
            rcArea: std::mem::zeroed(),
        };
        ImmSetCompositionWindow(himc, &f);
    }
    if cand {
        // 候选框左上角钉在光标下方
        let f = CANDIDATEFORM {
            dwIndex: 0,
            dwStyle: CFS_CANDIDATEPOS,
            ptCurrentPos: POINT { x: cx, y: cy_top + lh.max(2) },
            rcArea: std::mem::zeroed(),
        };
        ImmSetCandidateWindow(himc, &f);
    }
    ImmReleaseContext(hwnd, himc);
}
