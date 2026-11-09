// OCR 排版解析
// 规则：投影切栏 → 栏内聚类成行 → 行内按 x 拼 → 跨行判段 → 竖排单独走。
// 全部基于引擎返回的 quad 外接矩形，无模型、无额外资源。
// 本模块始终编译（不受 ocr feature 门控），供 UI 切档与 ocr_impl 共用。

use std::cmp::Ordering;

/// 一行识别结果：文本 + 置信度 + 四点外接矩形（引擎坐标系，单位像素）
#[derive(Clone, Debug, Default)]
pub struct OcrLine {
    pub text: String,
    #[allow(dead_code)]
    pub score: f32,
    pub x1: f32,
    pub y1: f32,
    pub x2: f32,
    pub y2: f32,
}

/// 排版档位：自动 / 按行 / 整段 / 原始
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LayoutMode {
    Auto,
    Line,
    Flat,
    Raw,
}

impl LayoutMode {
    pub fn from_key(k: &str) -> LayoutMode {
        match k {
            "line" => LayoutMode::Line,
            "flat" => LayoutMode::Flat,
            "raw" => LayoutMode::Raw,
            _ => LayoutMode::Auto,
        }
    }
    pub fn key(self) -> &'static str {
        match self {
            LayoutMode::Line => "line",
            LayoutMode::Flat => "flat",
            LayoutMode::Raw => "raw",
            LayoutMode::Auto => "auto",
        }
    }
    pub fn next(self) -> LayoutMode {
        match self {
            LayoutMode::Auto => LayoutMode::Line,
            LayoutMode::Line => LayoutMode::Flat,
            LayoutMode::Flat => LayoutMode::Raw,
            LayoutMode::Raw => LayoutMode::Auto,
        }
    }
    /// 切档 toast 文案（对应 ocr.layoutAuto/Line/Flat/Raw）
    pub fn label(self) -> &'static str {
        match self {
            LayoutMode::Line => "排版：每框一行（表格 / 代码）",
            LayoutMode::Flat => "排版：合并成一段",
            LayoutMode::Raw => "排版：引擎原始顺序",
            LayoutMode::Auto => "排版：自动合并（多栏 + 自然段）",
        }
    }
    /// 与 AtomicI32 互转（0=Auto,1=Line,2=Flat,3=Raw）
    pub fn from_i32(v: i32) -> LayoutMode {
        match v {
            1 => LayoutMode::Line,
            2 => LayoutMode::Flat,
            3 => LayoutMode::Raw,
            _ => LayoutMode::Auto,
        }
    }
    pub fn as_i32(self) -> i32 {
        match self {
            LayoutMode::Auto => 0,
            LayoutMode::Line => 1,
            LayoutMode::Flat => 2,
            LayoutMode::Raw => 3,
        }
    }
}

/// 内部用的包围盒（对应 C 的 Box），idx 指回原始 OcrLine
#[derive(Clone, Copy)]
struct Bx {
    idx: usize,
    x1: f32,
    y1: f32,
    x2: f32,
    y2: f32,
}

impl Bx {
    fn cx(&self) -> f32 {
        (self.x1 + self.x2) * 0.5
    }
    fn cy(&self) -> f32 {
        (self.y1 + self.y2) * 0.5
    }
    fn w(&self) -> f32 {
        self.x2 - self.x1
    }
    fn h(&self) -> f32 {
        self.y2 - self.y1
    }
}

fn cmp_f(a: f32, b: f32) -> Ordering {
    a.partial_cmp(&b).unwrap_or(Ordering::Equal)
}

fn median(v: &mut [f32]) -> f32 {
    if v.is_empty() {
        return 0.0;
    }
    v.sort_by(|a, b| cmp_f(*a, *b));
    v[v.len() / 2]
}

/// 句末标点：见到它就该换段
fn ends_paragraph(c: char) -> bool {
    matches!(
        c,
        '。' | '！' | '？' | '；' | '：' | '.' | '!' | '?' | ';' | ':' | '…' | '」' | '』' | '》' | '）'
    )
}

/// 拉丁字母/数字：两侧都是它才补空格（中文词语之间补空格是错的）
fn is_word_char(c: char) -> bool {
    let u = c as u32;
    if c.is_ascii_digit() || ('a'..='z').contains(&c) || ('A'..='Z').contains(&c) {
        return true;
    }
    (0xC0..0x2000).contains(&u) // 带重音的西欧字母
}

#[allow(dead_code)]
fn is_wide(c: char) -> bool {
    (c as u32) >= 0x2E80 // 汉字、假名、全角标点
}

/// 把 box 投影到 x 轴，返回"有字的横带"。窄于 min_gap 的缝隙并进相邻带，
/// 免得字距稍大的行被切成两栏。
fn split_bands(bs: &[Bx], min_gap: f32) -> Vec<(f32, f32)> {
    let mut xs: Vec<f32> = Vec::with_capacity(bs.len() * 2);
    for b in bs {
        xs.push(b.x1);
        xs.push(b.x2);
    }
    xs.sort_by(|a, b| cmp_f(*a, *b));
    xs.dedup_by(|a, b| *a == *b);

    let mut raw: Vec<(f32, f32)> = Vec::new();
    let mut inside = false;
    let mut start = 0f32;
    for i in 0..xs.len().saturating_sub(1) {
        let a = xs[i];
        let b = xs[i + 1];
        let mut covered = false;
        for bx in bs {
            if bx.x1 <= a && bx.x2 >= b {
                covered = true;
                break;
            }
        }
        if covered && !inside {
            start = a;
            inside = true;
        }
        if !covered && inside {
            raw.push((start, a));
            inside = false;
        }
    }
    if inside {
        if let Some(last) = xs.last() {
            raw.push((start, *last));
        }
    }

    let mut out: Vec<(f32, f32)> = Vec::new();
    for s in raw {
        if let Some(last) = out.last_mut() {
            if s.0 - last.1 <= min_gap {
                last.1 = s.1;
                continue;
            }
        }
        out.push(s);
    }
    out
}

/// 同一行：中线高度差不超过半个字高
fn cluster_rows(bs: &mut Vec<Bx>, unit: f32) -> Vec<Vec<Bx>> {
    bs.sort_by(|a, b| cmp_f(a.cy(), b.cy()));
    let tol = 0.5 * unit;
    let mut rows: Vec<Vec<Bx>> = Vec::new();
    for b in bs.iter() {
        if let Some(last) = rows.last_mut() {
            let sum: f32 = last.iter().map(|o| o.cy()).sum();
            let cy = sum / last.len() as f32;
            if (b.cy() - cy).abs() <= tol {
                last.push(*b);
                continue;
            }
        }
        rows.push(vec![*b]);
    }
    rows
}

/// 一行里多个框按 x 拼接；缝隙够大才补空格，且只在拉丁文之间补
fn join_row(row: &mut [Bx], lines: &[OcrLine], unit: f32) -> String {
    row.sort_by(|a, b| cmp_f(a.x1, b.x1));
    let mut out = String::new();
    for i in 0..row.len() {
        if i > 0 && !out.is_empty() {
            let gap = row[i].x1 - row[i - 1].x2;
            let prev = out.chars().last().unwrap_or(' ');
            let nextc = lines[row[i].idx].text.chars().next().unwrap_or(' ');
            if gap > 1.8 * unit {
                out.push(' ');
            } else if gap > 0.4 * unit && (is_word_char(prev) || is_word_char(nextc)) {
                out.push(' ');
            }
        }
        out.push_str(&lines[row[i].idx].text);
    }
    out
}

/// 横排：先聚行，再按需判段
fn format_horizontal(col: &[Bx], lines: &[OcrLine], unit: f32, merge_paragraph: bool) -> String {
    let mut col_v = col.to_vec();
    let rows = cluster_rows(&mut col_v, unit);

    let mut left = 0f32;
    let mut right = 0f32;
    for b in col {
        if left == 0.0 && right == 0.0 {
            left = b.x1;
            right = b.x2;
        }
        left = left.min(b.x1);
        right = right.max(b.x2);
    }
    let width = (right - left).max(unit);

    let mut out = String::new();
    let mut first = true;
    let mut prev_text = String::new();
    let mut prev_cy = 0f32;
    let mut prev_x2 = 0f32;
    for r in rows.iter() {
        let mut rr = r.to_vec();
        let cur_text = join_row(&mut rr, lines, unit);
        if cur_text.is_empty() {
            continue;
        }
        let first_box = rr[0];
        let mut cur_x1 = first_box.x1;
        let mut cur_x2 = first_box.x2;
        for b in &rr {
            cur_x1 = cur_x1.min(b.x1);
            cur_x2 = cur_x2.max(b.x2);
        }
        let cur_cy = first_box.cy();

        if first {
            out = cur_text.clone();
        } else {
            let mut brk = !merge_paragraph;
            if !brk && ends_paragraph(prev_text.chars().last().unwrap_or(' ')) {
                brk = true;
            }
            if !brk && cur_cy - prev_cy > 1.9 * unit {
                brk = true; // 段间距
            }
            if !brk && cur_x1 - left > 1.2 * unit {
                brk = true; // 首行缩进
            }
            if !brk && (prev_x2 - left) < 0.78 * width {
                brk = true; // 上一行没写满
            }
            if brk {
                out.push_str("\r\n");
            } else if is_word_char(out.chars().last().unwrap_or(' '))
                && is_word_char(cur_text.chars().next().unwrap_or(' '))
            {
                out.push(' ');
            }
            out.push_str(&cur_text);
        }
        prev_text = cur_text;
        prev_cy = cur_cy;
        prev_x2 = cur_x2;
        first = false;
    }
    out
}

/// 竖排：列内从上到下，每框一行
fn format_vertical(col: &mut Vec<Bx>, lines: &[OcrLine]) -> String {
    col.sort_by(|a, b| cmp_f(a.y1, b.y1));
    let mut out = String::new();
    for b in col.iter() {
        if lines[b.idx].text.is_empty() {
            continue;
        }
        if !out.is_empty() {
            out.push_str("\r\n");
        }
        out.push_str(&lines[b.idx].text);
    }
    out
}

/// 旧行为：引擎给什么顺序就是什么顺序（Raw 档）
fn raw_join(lines: &[OcrLine]) -> String {
    let mut out = String::new();
    for l in lines {
        if !out.is_empty() {
            out.push_str("\r\n");
        }
        out.push_str(&l.text);
    }
    out
}

/// 对外入口：把识别行按指定档位排成文本
pub fn format(lines: &[OcrLine], mode: LayoutMode) -> String {
    let mut bs: Vec<Bx> = Vec::with_capacity(lines.len());
    for (i, l) in lines.iter().enumerate() {
        if l.text.is_empty() {
            continue;
        }
        bs.push(Bx { idx: i, x1: l.x1, y1: l.y1, x2: l.x2, y2: l.y2 });
    }
    // 没坐标或明确要旧行为，都退回原始顺序
    if bs.is_empty() || mode == LayoutMode::Raw {
        return raw_join(lines);
    }

    // 字高用 min(w,h) 的中位数：横排的 min 是行高，竖排的 min 是列宽
    let mut units: Vec<f32> = bs.iter().map(|b| b.w().min(b.h())).collect();
    let mut unit = median(&mut units);
    if unit < 2.0 {
        unit = 2.0;
    }

    let tall = bs.iter().filter(|b| b.h() > 1.5 * b.w()).count();
    let global_vertical = tall * 2 >= bs.len();

    if mode == LayoutMode::Line {
        if global_vertical {
            // 竖排从右往左
            bs.sort_by(|a, b| {
                if (a.cx() - b.cx()).abs() > 2.0 {
                    cmp_f(b.cx(), a.cx())
                } else {
                    cmp_f(a.y1, b.y1)
                }
            });
            let mut out = String::new();
            for b in &bs {
                if !out.is_empty() {
                    out.push_str("\r\n");
                }
                out.push_str(&lines[b.idx].text);
            }
            return out;
        }
        let mut rows = cluster_rows(&mut bs, unit);
        let mut out = String::new();
        for r in rows.iter_mut() {
            r.sort_by(|a, b| cmp_f(a.x1, b.x1));
            for b in r.iter() {
                if !out.is_empty() {
                    out.push_str("\r\n");
                }
                out.push_str(&lines[b.idx].text);
            }
        }
        return out;
    }

    if mode == LayoutMode::Flat {
        if global_vertical {
            let mut v = bs.clone();
            v.sort_by(|a, b| {
                if (a.cx() - b.cx()).abs() > 2.0 {
                    cmp_f(b.cx(), a.cx())
                } else {
                    cmp_f(a.y1, b.y1)
                }
            });
            let mut out = String::new();
            for b in &v {
                out.push_str(&lines[b.idx].text);
            }
            return out;
        }
        let mut rows = cluster_rows(&mut bs, unit);
        let mut out = String::new();
        for r in rows.iter_mut() {
            let t = join_row(r, lines, unit);
            if t.is_empty() {
                continue;
            }
            if !out.is_empty() && (is_word_char(out.chars().last().unwrap_or(' ')) || is_word_char(t.chars().next().unwrap_or(' '))) {
                out.push(' ');
            }
            out.push_str(&t);
        }
        return out;
    }

    // Auto：先切栏，栏间按方向排序，栏内再聚行/判段
    let bands = split_bands(&bs, 0.9 * unit);
    if bands.len() <= 1 {
        return if global_vertical {
            let mut v = bs.clone();
            format_vertical(&mut v, lines)
        } else {
            format_horizontal(&bs, lines, unit, true)
        };
    }

    let mut cols: Vec<((f32, f32), Vec<Bx>)> = Vec::new();
    for band in &bands {
        let mut col: Vec<Bx> = Vec::new();
        for b in &bs {
            let cx = b.cx();
            if cx >= band.0 && cx <= band.1 {
                col.push(*b);
            }
        }
        if !col.is_empty() {
            cols.push((*band, col));
        }
    }
    if cols.is_empty() {
        return format_horizontal(&bs, lines, unit, true);
    }

    if global_vertical {
        cols.sort_by(|a, b| cmp_f(b.0 .0, a.0 .0)); // 中文竖排：从右往左读
    } else {
        cols.sort_by(|a, b| cmp_f(a.0 .0, b.0 .0));
    }

    let mut out = String::new();
    for (_, mut col) in cols {
        let t = col.iter().filter(|b| b.h() > 1.5 * b.w()).count();
        let vertical = t * 2 >= col.len();
        let piece = if vertical {
            format_vertical(&mut col, lines)
        } else {
            format_horizontal(&col, lines, unit, true)
        };
        if piece.is_empty() {
            continue;
        }
        if !out.is_empty() {
            out.push_str("\r\n");
        }
        out.push_str(&piece);
    }
    if out.is_empty() {
        raw_join(lines)
    } else {
        out
    }
}
