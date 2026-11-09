//
// 控件、坐标、绘制顺序、命中判定、规则组合顺序、状态判定、两阶段搬迁与撤销，
// 全部照 （96dpi 基准，用 `ui::sc` 等比放大），视觉一律取 `ui::*` token
// 本文件没有裸色值，几何位置集中在本文件的 `geo` 段，主线落 `ui.rs` layout 时即可
//
// 结构差异只有一处、且是被迫的： 把控件挂在主窗口上、页面自绘
// 本页同构（`create_page` 建齐控件、`draw()` 负责页面美术），这样页面能随
// `PAGE_RENAME` 一起显隐，不用另开顶层窗口。

#[path = "rn_pinyin.rs"]
mod rn_pinyin;

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::mem::zeroed;
use std::path::PathBuf;

use winapi::shared::minwindef::{FALSE, LPARAM, LRESULT, TRUE, UINT, WPARAM};
use winapi::shared::windef::{HBRUSH, HDC, HFONT, HGDIOBJ, HMENU, HWND, POINT, RECT, SIZE};
use winapi::um::commdlg::{GetOpenFileNameW, OFN_ALLOWMULTISELECT, OFN_EXPLORER, OFN_FILEMUSTEXIST, OFN_HIDEREADONLY, OPENFILENAMEW};

use winapi::um::shellapi::{DragAcceptFiles, DragFinish, DragQueryFileW, HDROP};
use winapi::um::sysinfoapi::GetLocalTime;
use winapi::um::winbase::MoveFileExW;
use winapi::um::winuser::*;
use winapi::um::wingdi::*;

use crate::ui;
use crate::{reg_class, track_leave};

// —————————————————————— 页面标识（主线接） ——————————————————————
/// 页号： 的 `PageRename` 落在页签序列第 7 位（0 基）
pub const PAGE_RENAME: usize = 6;
/// 页签控件 ID
pub const TAB_ID: usize = 4125;
/// 页签文字
pub const TAB_LABEL: &str = "文件改名";

// —————————————————————— 控件 ID（5500–5599） ——————————————————————
// 是 5400 起的连号，这里整体 +100 平移，避免与既有 41xx/52xx/53xx 撞号
pub const ID_ADD_FILES: usize = 5500;
pub const ID_ADD_FOLDER: usize = 5501;
pub const ID_COPY: usize = 5502;
pub const ID_CLEAR: usize = 5503;
pub const ID_RESET: usize = 5504;
pub const ID_UNDO: usize = 5505;
pub const ID_RUN: usize = 5506;
pub const ID_FILTER: usize = 5507;
pub const ID_CHECK_ALL: usize = 5508;
pub const ID_INVERT: usize = 5509;
pub const ID_LIST: usize = 5510;
pub const ID_EDIT: usize = 5511;
pub const ID_CASE_UPPER: usize = 5512;
pub const ID_CASE_LOWER: usize = 5513;
pub const ID_CASE_TITLE: usize = 5514;
pub const ID_NUM_ENABLE: usize = 5515;
pub const ID_CONVERT: usize = 5516;
pub const ID_CONV_TO: usize = 5517;
pub const ID_PREFIX: usize = 5518;
pub const ID_SUFFIX: usize = 5519;
pub const ID_SEQ_ON: usize = 5520;
pub const ID_SEQ_START: usize = 5521;
pub const ID_SEQ_STEP: usize = 5522;
pub const ID_SEQ_DIGITS: usize = 5523;
pub const ID_DEL_SPACE: usize = 5524;
pub const ID_DEL_LOWER: usize = 5525;
pub const ID_DEL_UPPER: usize = 5526;
pub const ID_DEL_DIGIT: usize = 5527;
pub const ID_DEL_CJK: usize = 5528;
pub const ID_DEL_SPECIAL: usize = 5529;
pub const ID_DEL_TEXT: usize = 5530;
pub const ID_REP_CASE: usize = 5531;
pub const ID_REP_ALL: usize = 5532;
pub const ID_FIND: usize = 5533;
pub const ID_REPLACE: usize = 5534;
pub const ID_EXT_UPPER: usize = 5535;
pub const ID_EXT_LOWER: usize = 5536;
pub const ID_EXT_KEEP: usize = 5537;
pub const ID_EXT_DROP: usize = 5538;
pub const ID_EXT_SET: usize = 5539;
pub const ID_AFFIX_ON: usize = 5540;

// —————————————————————— 几何（逻辑像素，全部经 ui::sc） ——————————————————————
// 名字与 常量同名，值取自上表，主线落 ui.rs 时按同一组键名登记
// 几何常量统一登记在 `ui::layout::rename`（本文件不另起常量，只做别名）
use crate::ui::layout::rename as geo;

fn sc(v: i32) -> i32 {
    ui::sc(v)
}

// 文字（与 Lang/zh-CN.json 的 rn 段一一对应）
const T_TOOL: [(&str, &str); 7] = [
    ("文件", "addFiles"),
    ("文件夹", "addFolder"),
    ("复制", "copy"),
    ("清空", "clear"),
    ("重置", "reset"),
    ("撤销", "undo"),
    ("执行改名", "run"),
];
const T_CASE_UPPER: &str = "全部大写";
const T_CASE_LOWER: &str = "全部小写";
const T_CASE_TITLE: &str = "首字大写";
const T_NUM: &str = "数字递增";
const T_CONV_LBL: &str = "转换：";
const T_CONV_TO_LBL: &str = "改为：";
const T_PREFIX_LBL: &str = "前缀：";
const T_SUFFIX_LBL: &str = "后缀：";
const T_FIND_LBL: &str = "查找：";
const T_REPL_LBL: &str = "替换为：";
const T_CASE_SENS: &str = "区分大小写";
const T_REP_ALL: &str = "替换所有";
const T_SEQ_ON: &str = "序号";
const T_AFFIX_ON: &str = "添加";
const T_SEQ_START: &str = "起始值：";
const T_SEQ_STEP: &str = "增量：";
const T_SEQ_DIGITS: &str = "位数：";
const T_EXT_UPPER: &str = "全部大写";
const T_EXT_LOWER: &str = "全部小写";
const T_EXT_KEEP: &str = "不变";
const T_EXT_DROP: &str = "删除";
const T_EXT_LBL: &str = "改为：";
const T_DEL_UPPER: &str = "大写字母";
const T_DEL_LOWER: &str = "小写字母";
const T_DEL_SPACE: &str = "空格";
const T_DEL_DIGIT: &str = "数字";
const T_DEL_CJK: &str = "中文";
const T_DEL_SPEC: &str = "特殊字符";
const T_DEL_TEXT: &str = "文本：";
const T_FILTER: &str = "过滤";
const T_ALL_FMT: &str = "全部格式";
const T_CHECK_ALL: &str = "全选";
const T_INVERT: &str = "反选";
const T_PANEL: [&str; 5] = ["文件名", "扩展名", "替换", "添加", "删除"];
const T_CONV_ITEMS: [&str; 7] = [
    "不选择", "简体转繁体", "繁体转简体", "一二三转123", "123转一二三", "汉语转拼音", "汉语转拼音首字母",
];
/// 前缀 / 后缀的候选：显示文案取自 `Lang/zh-CN.json` 的 rn.token*
/// 选中时只把 token（第一个空格之前那段）填进框，后面的说明留在下拉里看。
const T_AFFIX_ITEMS: [&str; 8] = [
    "<name> 原文件名",
    "<self> 新文件名",
    "<#> 表示数字占1位: 如1",
    "<##> 表示数字占2位: 如01",
    "<dir> 文件夹名",
    "<now> 当前日期时间",
    "<date> 当前日期",
    "<time> 当前时间",
];
const T_EXT_ITEMS: [&str; 10] = ["png", "jpg", "jpeg", "webp", "bmp", "gif", "mp4", "pdf", "txt", "zip"];

// —————————————————————— 数据 ——————————————————————
#[derive(Clone, Copy, PartialEq, Eq)]
enum Stat {
    Ready,
    Same,
    Bad,
    Conflict,
    Missing,
    Done,
    Fail,
}

struct Row {
    dir: String,
    name: String,
    new_name: String,
    manual: String,
    size: u64,
    status: Stat,
    checked: bool,
    done: bool,
}

/// `RnRuleSet`：整表重算只读一遍控件
struct Rules {
    find: String,
    replace: String,
    del_text: String,
    ext_set: String,
    conv_to: String,
    prefix: String,
    suffix: String,
    conv: i32,
    now: [u16; 6],
}

struct State {
    parent: HWND,
    list: HWND,
    edit: HWND,
    ctls: Vec<(usize, HWND)>,
    rows: Vec<Row>,
    page_on: bool,
    panel: i32,
    case: i32, // -1 无 / 0 全大写 / 1 全小写 / 2 首字大写
    seq_on: bool,
    seq_start: i64,
    seq_step: i64,
    seq_digits: i64,
    affix_on: bool,
    del: [bool; 6], // 空格 小写 大写 数字 中文 特殊
    del_text: String,
    find: String,
    replace: String,
    rep_case: bool,
    rep_all: bool,
    ext_mode: i32, // -1 不变 / 1 上 / 2 下 / 3 删 / 4 改为
    ext_set: String,
    fmt_all: Vec<String>,
    fmt_on: Vec<String>,
    fmt_text: String,
    undos: Vec<(String, String)>,
    hot: usize,
    /// 规则勾选钮的勾选态。BS_OWNERDRAW 按钮是「自绘的普通按钮」，
    /// BM_SETCHECK 对普通按钮无效（勾上再读还是空），必须自己存。
    checks: HashMap<usize, bool>,
    edit_row: i32,
    syncing: bool,
    // 量字宽后落定的 x（relayout 写、draw 读）
    conv_x: i32,
    conv_to_x: i32,
    find_x: i32,
    repl_lbl_x: i32,
    replace_x: i32,
    find_w: i32,
    pfx_lbl_x: i32,
    pfx_x: i32,
    suf_lbl_x: i32,
    suf_x: i32,
    pfx_w: i32,
    seq_lbl: [i32; 6], // start lbl/x, step lbl/x, digits lbl/x
    seq_w: i32,
    ext_set_x: i32,
    del_txt_x: i32,
    font: HFONT,
    font_mid: HFONT,
    font_bold: HFONT,
    font_large: HFONT,
    font_large_bold: HFONT,
    // 列表
    sel: i32,
    top: i32,
    hot_row: i32,
    user_w: [i32; 6],
    drag_div: i32,
    drag_x: i32,
    drag_l: i32,
    drag_r: i32,
    track: bool,
}

impl State {
    fn new() -> Self {
        State {
            parent: std::ptr::null_mut(),
            list: std::ptr::null_mut(),
            edit: std::ptr::null_mut(),
            ctls: Vec::new(),
            rows: Vec::new(),
            page_on: false,
            panel: 0,
            case: -1,
            seq_on: false,
            seq_start: 1,
            seq_step: 1,
            seq_digits: 1,
            affix_on: false,
            del: [false; 6],
            del_text: String::new(),
            find: String::new(),
            replace: String::new(),
            rep_case: false,
            rep_all: true,
            ext_mode: -1,
            ext_set: String::new(),
            fmt_all: Vec::new(),
            fmt_on: Vec::new(),
            fmt_text: String::new(),
            undos: Vec::new(),
            hot: 0,
            checks: HashMap::new(),
            edit_row: -1,
            syncing: false,
            conv_x: 0,
            conv_to_x: 0,
            find_x: 0,
            repl_lbl_x: 0,
            replace_x: 0,
            find_w: 0,
            pfx_lbl_x: 0,
            pfx_x: 0,
            suf_lbl_x: 0,
            suf_x: 0,
            pfx_w: 0,
            seq_lbl: [0; 6],
            seq_w: 0,
            ext_set_x: 0,
            del_txt_x: 0,
            font: std::ptr::null_mut(),
            font_mid: std::ptr::null_mut(),
            font_bold: std::ptr::null_mut(),
            font_large: std::ptr::null_mut(),
            font_large_bold: std::ptr::null_mut(),
            sel: -1,
            top: 0,
            hot_row: -1,
            user_w: [0; 6],
            drag_div: -1,
            drag_x: 0,
            drag_l: 0,
            drag_r: 0,
            track: false,
        }
    }
}

thread_local! {
    static ST: RefCell<State> = RefCell::new(State::new());
}

fn st<R>(f: impl FnOnce(&State) -> R) -> R {
    ST.with(|s| f(&s.borrow()))
}

fn st_mut<R>(f: impl FnOnce(&mut State) -> R) -> R {
    ST.with(|s| f(&mut s.borrow_mut()))
}

// —————————————————————— 基础工具 ——————————————————————
fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

fn ctl(id: usize) -> HWND {
    st(|s| {
        s.ctls
            .iter()
            .find(|(i, _)| *i == id)
            .map(|(_, h)| *h)
            .unwrap_or(std::ptr::null_mut())
    })
}

fn ctl_text(h: HWND) -> String {
    if h.is_null() {
        return String::new();
    }
    unsafe {
        let n = GetWindowTextLengthW(h);
        if n <= 0 {
            return String::new();
        }
        let mut buf = vec![0u16; n as usize + 1];
        GetWindowTextW(h, buf.as_mut_ptr(), n + 1);
        buf.pop();
        String::from_utf16_lossy(&buf)
    }
}

fn set_text(h: HWND, s: &str) {
    if !h.is_null() {
        let w = wide(s);
        unsafe {
            SetWindowTextW(h, w.as_ptr());
        }
    }
}

fn ctl_check(id: usize) -> bool {
    st(|s| s.checks.get(&id).copied().unwrap_or(false))
}

fn ctl_set_check(id: usize, on: bool) {
    st_mut(|s| {
        s.checks.insert(id, on);
    });
    // 自绘按钮不会因为状态变化自己重画，这里补一次失效（WM_DRAWITEM 里按状态重画）
    let h = ctl(id);
    if !h.is_null() {
        unsafe {
            InvalidateRect(h, std::ptr::null(), FALSE);
        }
    }
}

/// `rnSeqInt`：只收数字，空 / 非数字退回上一次的合法值，越界夹回上下限
fn seq_int(id: usize, fallback: i64, lo: i64, hi: i64) -> i64 {
    let raw = ctl_text(ctl(id));
    let s = raw.trim();
    if s.is_empty() {
        return fallback.clamp(lo, hi);
    }
    if !s.chars().all(|c| c.is_ascii_digit()) {
        return fallback.clamp(lo, hi);
    }
    match s.parse::<i64>() {
        Ok(v) => v.clamp(lo, hi),
        Err(_) => hi,
    }
}

fn solid(color: u32) -> HBRUSH {
    // `ui::color::*` 是 0xRRGGBB，GDI 要 COLORREF（0x00BBGGRR），必须在这里换一次
    unsafe { CreateSolidBrush(ui::color::cref(color)) }
}

fn fill(dc: HDC, r: &RECT, color: u32) {
    let b = solid(color);
    unsafe {
        FillRect(dc, r, b);
        DeleteObject(b as HGDIOBJ);
    }
}

fn fill_rc(dc: HDC, l: i32, t: i32, r: i32, b: i32, color: u32) {
    fill(dc, &RECT { left: l, top: t, right: r, bottom: b }, color);
}

fn frame(dc: HDC, r: &RECT, color: u32) {
    let b = solid(color);
    unsafe {
        FrameRect(dc, r, b);
        DeleteObject(b as HGDIOBJ);
    }
}

/// 一段斜线（45° 折线用）：按列铺，相邻列错 1px
fn skew(dc: HDC, x1: i32, y1: i32, x2: i32, y2: i32, t: i32, color: u32) {
    let dx = (x2 - x1).abs();
    let dy = (y2 - y1).abs();
    let n = dx.max(dy);
    if n <= 0 {
        fill_rc(dc, x1, y1, x1 + t, y1 + t, color);
        return;
    }
    for i in 0..=n {
        let x = x1 + (x2 - x1) * i / n;
        let y = y1 + (y2 - y1) * i / n;
        fill_rc(dc, x, y, x + t, y + t, color);
    }
}

fn draw_str(dc: HDC, s: &str, r: &mut RECT, color: u32, font: HFONT, flags: u32) {
    let w = wide(s);
    unsafe {
        let old = SelectObject(dc, font as HGDIOBJ);
        SetBkMode(dc, TRANSPARENT as i32);
        SetTextColor(dc, ui::color::cref(color));
        DrawTextW(dc, w.as_ptr(), -1, r, flags | DT_NOPREFIX);
        SelectObject(dc, old);
    }
}

fn text_w(dc: HDC, s: &str, font: HFONT) -> i32 {
    let w = wide(s);
    let mut sz: SIZE = unsafe { zeroed() };
    unsafe {
        let old = SelectObject(dc, font as HGDIOBJ);
        GetTextExtentPoint32W(dc, w.as_ptr(), w.len() as i32 - 1, &mut sz);
        SelectObject(dc, old);
    }
    sz.cx
}

/// `rnLabelW`：按 `FONT_BODY` 500 量，再掐掉 DPI 倍率
fn measure(s: &str) -> i32 {
    let p = st(|s| s.parent);
    if p.is_null() {
        return s.chars().count() as i32 * sc(ui::FONT_BODY) / 2;
    }
    unsafe {
        let dc = GetDC(p);
        let w = text_w(dc, s, st(|s| s.font_mid));
        ReleaseDC(p, dc);
        w
    }
}

fn toast(msg: &str) {
    unsafe {
        crate::show_toast(msg);
    }
}

// 子类化要存旧过程：winapi 的 commctrl 不在依赖特性里，用 GWLP_WNDPROC 手接
thread_local! {
    static OLD: RefCell<Vec<(isize, isize)>> = RefCell::new(Vec::new());
}

fn old_proc(h: HWND) -> isize {
    OLD.with(|m| {
        m.borrow()
            .iter()
            .find(|(k, _)| *k == h as isize)
            .map(|(_, v)| *v)
            .unwrap_or(0)
    })
}

fn subclass(h: HWND, proc_: unsafe extern "system" fn(HWND, UINT, WPARAM, LPARAM) -> LRESULT) {
    unsafe {
        let old = SetWindowLongPtrW(h, GWLP_WNDPROC, proc_ as isize);
        OLD.with(|m| m.borrow_mut().push((h as isize, old)));
    }
}

const LIST_CLASS: &str = "SnapCoreRnList";
/// winapi 0.3.9 没导这个（CB_SETMINVISIBLE = WM_USER + 24）
const CB_SETMINVISIBLE: UINT = 0x0400 + 24;

// —————————————————————— 建页 ——————————————————————
fn mk(parent: HWND, class: &str, text: &str, style: u32, ex: u32, id: usize) -> HWND {
    let c = wide(class);
    let t = wide(text);
    unsafe {
        CreateWindowExW(
            ex,
            c.as_ptr(),
            t.as_ptr(),
            style,
            0,
            0,
            0,
            0,
            parent,
            id as HMENU,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    }
}

fn add_items(id: usize, items: &[&str]) {
    let h = ctl(id);
    if h.is_null() {
        return;
    }
    for it in items {
        let w = wide(it);
        unsafe {
            SendMessageW(h, CB_ADDSTRING, 0, w.as_ptr() as LPARAM);
        }
    }
}

pub fn create_page(parent: HWND) {
    if !st(|s| s.parent).is_null() {
        return;
    }
    unsafe {
        // CS_DBLCLKS：列表要靠双击「新文件名」格进就地编辑，没这一位收不到 WM_LBUTTONDBLCLK
        reg_class(LIST_CLASS, Some(list_proc), winapi::um::winuser::CS_DBLCLKS,
            IDC_ARROW as usize, false);
    }
    st_mut(|s| {
        s.parent = parent;
        s.font = ui::font(ui::FONT_BODY, 400) as HFONT;
        s.font_mid = ui::font(ui::FONT_BODY, 500) as HFONT;
        s.font_bold = ui::font(ui::FONT_BODY, 700) as HFONT;
        s.font_large = ui::font(ui::FONT_LABEL_LARGE, 500) as HFONT;
        s.font_large_bold = ui::font(ui::FONT_LABEL_LARGE, 700) as HFONT;
    });

    let btn = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
    let mut ids = Vec::new();
    for (i, t) in T_TOOL.iter().enumerate() {
        let id = ID_ADD_FILES + i;
        let h = mk(parent, "BUTTON", t.0, btn, 0, id);
        ids.push((id, h));
    }

    // 过滤下拉（可编辑，候选由 rnRebuildFilter 重建）
    let h = mk(
        parent,
        "COMBOBOX",
        "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL,
        0,
        ID_FILTER,
    );
    ids.push((ID_FILTER, h));

    // 列表：自建类，不用系统 ListView
    let h = mk(
        parent,
        LIST_CLASS,
        "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS,
        0,
        ID_LIST,
    );
    ids.push((ID_LIST, h));

    // 就地编辑框：常驻但默认藏着
    let h = mk(
        parent,
        "EDIT",
        "",
        WS_CHILD | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
        0,
        ID_EDIT,
    );
    ids.push((ID_EDIT, h));

    for id in [
        ID_CASE_UPPER,
        ID_CASE_LOWER,
        ID_CASE_TITLE,
        ID_NUM_ENABLE,
        ID_EXT_UPPER,
        ID_EXT_LOWER,
        ID_EXT_KEEP,
        ID_EXT_DROP,
        ID_REP_CASE,
        ID_REP_ALL,
        ID_SEQ_ON,
        ID_AFFIX_ON,
        ID_DEL_SPACE,
        ID_DEL_LOWER,
        ID_DEL_UPPER,
        ID_DEL_DIGIT,
        ID_DEL_CJK,
        ID_DEL_SPECIAL,
        ID_CHECK_ALL,
        ID_INVERT,
    ] {
        let h = mk(parent, "BUTTON", "", btn, 0, id);
        ids.push((id, h));
    }

    let edit = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL;
    for id in [
        ID_FIND, ID_REPLACE, ID_DEL_TEXT, ID_SEQ_START, ID_SEQ_STEP, ID_SEQ_DIGITS,
    ] {
        let h = mk(parent, "EDIT", "", edit, 0, id);
        ids.push((id, h));
    }

    let combo = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL;
    for id in [ID_CONV_TO, ID_PREFIX, ID_SUFFIX, ID_EXT_SET] {
        let h = mk(parent, "COMBOBOX", "", combo, 0, id);
        ids.push((id, h));
    }
    let h = mk(
        parent,
        "COMBOBOX",
        "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST,
        0,
        ID_CONVERT,
    );
    ids.push((ID_CONVERT, h));

    st_mut(|s| s.ctls = ids);

    // 自绘按钮统一挂悬停转发（无主题热态，只能自己盯 WM_MOUSEMOVE）
    for id in [
        ID_ADD_FILES, ID_ADD_FOLDER, ID_COPY, ID_CLEAR, ID_RESET, ID_UNDO, ID_RUN, ID_CHECK_ALL,
        ID_INVERT, ID_CASE_UPPER, ID_CASE_LOWER, ID_CASE_TITLE, ID_NUM_ENABLE, ID_EXT_UPPER,
        ID_EXT_LOWER, ID_EXT_KEEP, ID_EXT_DROP, ID_REP_CASE, ID_REP_ALL, ID_SEQ_ON, ID_AFFIX_ON,
        ID_DEL_SPACE, ID_DEL_LOWER, ID_DEL_UPPER, ID_DEL_DIGIT, ID_DEL_CJK, ID_DEL_SPECIAL,
    ] {
        let h = ctl(id);
        if !h.is_null() {
            subclass(h, hover_proc);
        }
    }

    add_items(ID_CONVERT, &T_CONV_ITEMS);
    {
        let h = ctl(ID_CONVERT);
        unsafe {
            SendMessageW(h, CB_SETCURSEL, 0, 0);
        }
    }
    for id in [ID_PREFIX, ID_SUFFIX] {
        let h = ctl(id);
        for it in T_AFFIX_ITEMS.iter() {
            let w = wide(it);
            unsafe {
                SendMessageW(h, CB_ADDSTRING, 0, w.as_ptr() as LPARAM);
            }
        }
    }
    add_items(ID_EXT_SET, &T_EXT_ITEMS);
    // 「改为」不挂第一项：默认是"不变"
    {
        let h = ctl(ID_EXT_SET);
        if !h.is_null() {
            unsafe {
                SendMessageW(h, 0x014E /*CB_SETCURSEL*/, !0usize, 0);
            }
        }
    }
    // 「过滤」首项是「全部格式」，默认就停在它上面
    {
        let h = ctl(ID_FILTER);
        if !h.is_null() {
            unsafe {
                SendMessageW(h, CB_SETCURSEL, 0, 0);
            }
        }
    }
    set_text(ctl(ID_FILTER), T_ALL_FMT);

    // 五只下拉挂"叠格"子类 + 最小可见条数
    for id in [ID_CONVERT, ID_CONV_TO, ID_PREFIX, ID_SUFFIX, ID_EXT_SET, ID_FILTER] {
        let h = ctl(id);
        if !h.is_null() {
            unsafe {
                SendMessageW(h, CB_SETMINVISIBLE, 10, 0);
            }
            subclass(h, combo_proc);
        }
    }
    let h = ctl(ID_EDIT);
    if !h.is_null() {
        // 就地编辑框的句柄必须落进 state：begin_edit / commit_edit / hide_editor_raw
        // 全靠它。之前只 push 进 ctls 表、没写 s.edit，begin_edit 一进门就
        // 撞上 `e.is_null()` 直接 return —— 双击新文件名格什么都不发生
        st_mut(|s| s.edit = h);
        subclass(h, edit_proc);
    }

    ctl_set_check(ID_SEQ_ON, false);
    ctl_set_check(ID_AFFIX_ON, false);
    ctl_set_check(ID_REP_ALL, true);
    set_text(ctl(ID_SEQ_START), "1");
    set_text(ctl(ID_SEQ_STEP), "1");
    set_text(ctl(ID_SEQ_DIGITS), "1");

    unsafe {
        DragAcceptFiles(parent, TRUE);
    }
    relayout(parent);
    sync_panel();
}

pub fn relayout(parent: HWND) {
    st_mut(|s| s.parent = parent);

    let place = |id: usize, x: i32, y: i32, w: i32, h: i32| {
        let hw = ctl(id);
        if !hw.is_null() {
            unsafe {
                SetWindowPos(
                    hw,
                    std::ptr::null_mut(),
                    sc(x),
                    sc(y),
                    sc(w.max(1)),
                    sc(h),
                    SWP_NOZORDER | SWP_NOACTIVATE,
                );
            }
        }
    };

    // 工具条：x 一路累加，间隔 kSpaceMd
    let mut x = ui::PAGE_MARGIN;
    let tw = [56, 72, 56, 56, 56, 56, 93];
    for (i, w) in tw.iter().enumerate() {
        place(ID_ADD_FILES + i, x, ui::TOOLBAR_TOP, *w, ui::CONTROL_HEIGHT);
        x += *w + ui::SPACE_MD;
    }

    // 「文件名」面板：左两列勾选
    place(ID_CASE_UPPER, geo::RULE1_X, geo::ROW1_Y, geo::RULE1_W, ui::CONTROL_HEIGHT);
    place(ID_CASE_TITLE, geo::RULE2_X, geo::ROW1_Y, geo::RULE2_W, ui::CONTROL_HEIGHT);
    place(ID_CASE_LOWER, geo::RULE1_X, geo::ROW2_Y, geo::RULE1_W, ui::CONTROL_HEIGHT);
    place(ID_NUM_ENABLE, geo::RULE2_X, geo::ROW2_Y, geo::RULE2_W, ui::CONTROL_HEIGHT);

    // 右半边「转换：/改为：」：框贴标签后，铺到面板内右边界
    let conv_x = geo::CONV_LBL_X + measure(T_CONV_LBL) + 2;
    let conv_to_x = geo::CONV_LBL_X + measure(T_CONV_TO_LBL) + 2;
    place(ID_CONVERT, conv_x, geo::ROW1_Y, geo::PANEL_IN_R - conv_x, ui::CONTROL_HEIGHT);
    place(ID_CONV_TO, conv_to_x, geo::ROW2_Y, geo::PANEL_IN_R - conv_to_x, ui::CONTROL_HEIGHT);

    // 「替换」面板：两格等宽，余量摊进组间，末框右缘收面板右缘
    let find_x = geo::FIND_LBL_X + measure(T_FIND_LBL) + 2;
    let l_repl = measure(T_REPL_LBL);
    let budget = geo::PANEL_IN_R - find_x - l_repl - 2 - ui::SPACE_LG;
    let box_w = (budget / 2).max(40);
    let repl_lbl_x = find_x + box_w + ui::SPACE_LG + (budget - box_w * 2);
    let replace_x = repl_lbl_x + l_repl + 2;
    place(ID_REP_CASE, geo::REP_CASE_X, geo::ROW1_Y, geo::REP_CASE_W, ui::CONTROL_HEIGHT);
    place(ID_REP_ALL, geo::REP_ALL_X, geo::ROW1_Y, geo::REP_ALL_W, ui::CONTROL_HEIGHT);
    place(ID_FIND, find_x, geo::ROW2_Y, box_w, ui::CONTROL_HEIGHT);
    place(ID_REPLACE, replace_x, geo::ROW2_Y, box_w, ui::CONTROL_HEIGHT);

    // 「序号」面板：上排「添加」+ 前缀/后缀，下排「序号」+ 起始/增量/位数
    let gap_base = 12;
    let lp1 = measure(T_PREFIX_LBL);
    let lp2 = measure(T_SUFFIX_LBL);
    let row1_l = geo::SEQ_LBL_X + geo::AFFIX_ON_W + gap_base;
    let mut abox = (geo::PANEL_IN_R - row1_l - gap_base - lp1 - lp2 - 4) / 2;
    if abox < 100 {
        abox = 100;
    }
    let pfx_lbl_x = row1_l;
    let pfx_x = pfx_lbl_x + lp1 + 2;
    let suf_lbl_x = pfx_x + abox + gap_base;
    let suf_x = suf_lbl_x + lp2 + 2;
    place(ID_AFFIX_ON, geo::AFFIX_ON_X, geo::ROW1_Y, geo::AFFIX_ON_W, ui::CONTROL_HEIGHT);
    place(ID_PREFIX, pfx_x, geo::ROW1_Y, abox, ui::CONTROL_HEIGHT);
    place(ID_SUFFIX, suf_x, geo::ROW1_Y, (geo::PANEL_IN_R - suf_x).min(abox), ui::CONTROL_HEIGHT);

    let l1 = measure(T_SEQ_START);
    let l2 = measure(T_SEQ_STEP);
    let l3 = measure(T_SEQ_DIGITS);
    let col_l = geo::SEQ_LBL_X + geo::SEQ_ON_W + gap_base;
    let s_budget = geo::PANEL_IN_R - col_l - (l1 + l2 + l3) - 6;
    let sbox = ((s_budget - gap_base * 2) / 3).max(40);
    let gap2 = ((s_budget - sbox * 3) / 2).max(4);
    let start_lbl = col_l;
    let start_x = col_l + l1 + 2;
    let step_lbl = start_x + sbox + gap2;
    let step_x = step_lbl + l2 + 2;
    let dig_lbl = step_x + sbox + gap2;
    let dig_x = dig_lbl + l3 + 2;
    place(ID_SEQ_ON, geo::SEQ_LBL_X, geo::ROW2_Y, geo::SEQ_ON_W, ui::CONTROL_HEIGHT);
    place(ID_SEQ_START, start_x, geo::ROW2_Y, sbox, ui::CONTROL_HEIGHT);
    place(ID_SEQ_STEP, step_x, geo::ROW2_Y, sbox, ui::CONTROL_HEIGHT);
    place(ID_SEQ_DIGITS, dig_x, geo::ROW2_Y, sbox, ui::CONTROL_HEIGHT);

    // 「扩展名」面板：左两列勾选，右「改为：」+ 固定 140 宽下拉
    let ext_set_x = geo::EXT_LBL_X + measure(T_EXT_LBL) + 2;
    place(ID_EXT_UPPER, geo::EXT_UPPER_X, geo::ROW1_Y, geo::EXT_UPPER_W, ui::CONTROL_HEIGHT);
    place(ID_EXT_KEEP, geo::EXT_KEEP_X, geo::ROW1_Y, geo::EXT_KEEP_W, ui::CONTROL_HEIGHT);
    place(ID_EXT_LOWER, geo::EXT_LOWER_X, geo::ROW2_Y, geo::EXT_LOWER_W, ui::CONTROL_HEIGHT);
    place(ID_EXT_DROP, geo::EXT_DROP_X, geo::ROW2_Y, geo::EXT_DROP_W, ui::CONTROL_HEIGHT);
    place(ID_EXT_SET, ext_set_x, geo::ROW2_Y, geo::EXT_SET_W, ui::CONTROL_HEIGHT);

    // 「删除」面板
    place(ID_DEL_UPPER, geo::DEL_UPPER_X, geo::ROW1_Y, geo::DEL_UPPER_W, ui::CONTROL_HEIGHT);
    place(ID_DEL_SPACE, geo::DEL_SPACE_X, geo::ROW1_Y, geo::DEL_SPACE_W, ui::CONTROL_HEIGHT);
    place(ID_DEL_CJK, geo::DEL_CJK_X, geo::ROW1_Y, geo::DEL_CJK_W, ui::CONTROL_HEIGHT);
    place(ID_DEL_SPECIAL, geo::DEL_SPEC_X, geo::ROW1_Y, geo::DEL_SPEC_W, ui::CONTROL_HEIGHT);
    place(ID_DEL_LOWER, geo::DEL_LOWER_X, geo::ROW2_Y, geo::DEL_LOWER_W, ui::CONTROL_HEIGHT);
    place(ID_DEL_DIGIT, geo::DEL_DIGIT_X, geo::ROW2_Y, geo::DEL_DIGIT_W, ui::CONTROL_HEIGHT);
    let del_txt_x = geo::DEL_LBL_X + measure(T_DEL_TEXT) + 2;
    place(ID_DEL_TEXT, del_txt_x, geo::ROW2_Y, geo::PANEL_IN_R - del_txt_x, ui::CONTROL_HEIGHT);

    // 过滤框 + 列表 + 底部两颗文字链
    place(ID_FILTER, geo::FILTER_X, geo::G3_ROW_Y, geo::FILTER_W, ui::CONTROL_HEIGHT);
    place(ID_LIST, geo::LIST_X, geo::LIST_Y, geo::LIST_W, geo::LIST_H);
    place(ID_CHECK_ALL, ui::PAGE_MARGIN, geo::BOT_Y, geo::BOT_BTN_W, geo::BOT_H);
    place(ID_INVERT, geo::BOT_BTN2_X, geo::BOT_Y, geo::BOT_BTN_W, geo::BOT_H);

    st_mut(|s| {
        s.conv_x = conv_x;
        s.conv_to_x = conv_to_x;
        s.find_x = find_x;
        s.find_w = box_w;
        s.repl_lbl_x = repl_lbl_x;
        s.replace_x = replace_x;
        s.pfx_lbl_x = pfx_lbl_x;
        s.pfx_x = pfx_x;
        s.pfx_w = abox;
        s.suf_lbl_x = suf_lbl_x;
        s.suf_x = suf_x;
        s.seq_lbl = [start_lbl, start_x, step_lbl, step_x, dig_lbl, dig_x];
        s.seq_w = sbox;
        s.ext_set_x = ext_set_x;
        s.del_txt_x = del_txt_x;
    });

    // 控件字体：正文 400（表头/大标签另取）。
    // 字体是全局缓存的 HFONT，每次 relayout 拿到的都是同一个句柄；只有句柄真变了
    // 才需要重设，且 lParam=0 不强制同步重画。若每次切页（以及 WM_SIZE 触发的
    // 第二次 relayout）都给几十个控件发 WM_SETFONT(重绘=TRUE)，把整页同步重画两遍，
    // 这就是「切到文件改名页闪得最厉害」的主要来源。
    let f = st(|s| s.font);
    static LAST_FONT: std::sync::atomic::AtomicIsize = std::sync::atomic::AtomicIsize::new(0);
    if LAST_FONT.swap(f as isize, std::sync::atomic::Ordering::Relaxed) != f as isize {
        for (id, h) in st(|s| s.ctls.clone()) {
            if id == ID_LIST || h.is_null() {
                continue;
            }
            unsafe {
                SendMessageW(h, WM_SETFONT, f as WPARAM, 0 as LPARAM);
            }
        }
    }
}

// —————————————————————— 显隐 ——————————————————————
pub fn show_page(parent: HWND, visible: bool) {
    let page_on = visible;
    st_mut(|s| {
        s.parent = parent;
        s.page_on = page_on;
    });
    for id in [
        ID_ADD_FILES, ID_ADD_FOLDER, ID_COPY, ID_CLEAR, ID_RESET, ID_UNDO, ID_RUN,
        ID_FILTER, ID_CHECK_ALL, ID_INVERT,
    ] {
        let h = ctl(id);
        if !h.is_null() {
            unsafe {
                ShowWindow(h, if page_on { SW_SHOW } else { SW_HIDE });
            }
        }
    }
    sync_panel();
    let l = st(|s| s.list);
    if !l.is_null() {
        unsafe {
            ShowWindow(l, if page_on { SW_SHOW } else { SW_HIDE });
        }
    }
    if !page_on {
        hide_editor();
    }
}

/// `rnSyncPanel`：按 `rnPanel` 露一组，其余藏，切换不清值、不重算
fn sync_panel() {
    let panel = st(|s| s.panel);
    let on_page = st(|s| s.page_on);
    let show = |id: usize, on: bool| {
        let h = ctl(id);
        if !h.is_null() {
            unsafe {
                ShowWindow(h, if on { SW_SHOW } else { SW_HIDE });
            }
        }
    };
    for id in [
        ID_CASE_UPPER, ID_CASE_LOWER, ID_CASE_TITLE, ID_NUM_ENABLE, ID_CONVERT, ID_CONV_TO,
    ] {
        show(id, on_page && panel == 0);
    }
    for id in [ID_EXT_UPPER, ID_EXT_LOWER, ID_EXT_KEEP, ID_EXT_DROP, ID_EXT_SET] {
        show(id, on_page && panel == 1);
    }
    for id in [ID_REP_CASE, ID_REP_ALL, ID_FIND, ID_REPLACE] {
        show(id, on_page && panel == 2);
    }
    for id in [ID_SEQ_ON, ID_SEQ_START, ID_SEQ_STEP, ID_SEQ_DIGITS, ID_AFFIX_ON, ID_PREFIX, ID_SUFFIX] {
        show(id, on_page && panel == 3);
    }
    for id in [
        ID_DEL_SPACE, ID_DEL_LOWER, ID_DEL_CJK, ID_DEL_SPECIAL, ID_DEL_DIGIT, ID_DEL_UPPER, ID_DEL_TEXT,
    ] {
        show(id, on_page && panel == 4);
    }
}

/// 标签行命中：先落在 y 带里，再按五段 x 区间判（返回面板号，-1 = 未命中）
fn tab_hit(x: i32, y: i32) -> i32 {
    if y < sc(geo::TITLE_Y) || y >= sc(geo::TITLE_Y + geo::TITLE_H) {
        return -1;
    }
    for (i, (tx, tw)) in geo::TABS.iter().enumerate() {
        if x >= sc(*tx) && x < sc(*tx + *tw) {
            return i as i32;
        }
    }
    -1
}

// —————————————————————— 文字规则工具 ——————————————————————
/// `BaseOfName`："./" 开头不算扩展名、结尾是点也不算（"abc." 的主名就是"abc."）
fn base_of_name(name: &str) -> &str {
    match name.rfind('.') {
        Some(i) if i > 0 && i + 1 < name.len() => &name[..i],
        _ => name,
    }
}

fn ext_of_name(name: &str) -> &str {
    match name.rfind('.') {
        Some(i) if i > 0 => &name[i + 1..],
        _ => "",
    }
}

fn upper_all(s: &str) -> String {
    s.to_uppercase()
}

fn lower_all(s: &str) -> String {
    s.to_lowercase()
}

/// `TitleFirst`：只把第一个字母抬上去，其余保持
fn title_first(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut done = false;
    for c in s.chars() {
        if !done && c.is_alphabetic() {
            out.extend(c.to_uppercase());
            done = true;
        } else {
            out.push(c);
        }
    }
    out
}

fn pad(n: i64, digits: i64) -> String {
    let d = digits.max(1) as usize;
    let v = n.max(0);
    let s = v.to_string();
    if s.len() >= d {
        s
    } else {
        format!("{}{}", "0".repeat(d - s.len()), s)
    }
}

/// `ValidName`：除非法字符外，结尾的点和空格也不收（资源管理器同款口径）
fn valid_name(name: &str) -> bool {
    if name.is_empty() || name == "." || name == ".." || name.chars().count() > 255 {
        return false;
    }
    if name.ends_with('.') || name.ends_with(' ') {
        return false;
    }
    if reserved_device_name(name) {
        return false;
    }
    !name.chars().any(|c| {
        matches!(c, '\\' | '/' | ':' | '*' | '?' | '"' | '<' | '>' | '|') || (c as u32) < 0x20
    })
}

/// Windows 保留设备名：CON/PRN/AUX/NUL/COM1-9/LPT1-9，
/// 带扩展名（CON.txt）或尾随空格同样保留，改成这些名字会在执行阶段失败。
fn reserved_device_name(name: &str) -> bool {
    let stem = name.split('.').next().unwrap_or(name).trim_end_matches(' ');
    let up = stem.to_ascii_uppercase();
    match up.as_str() {
        "CON" | "PRN" | "AUX" | "NUL" => true,
        _ => {
            let b = up.as_bytes();
            b.len() == 4
                && (up.starts_with("COM") || up.starts_with("LPT"))
                && matches!(b[3], b'1'..=b'9')
        }
    }
}

/// `NaturalLess`：资源管理器口径的自然序 —— 数字段按数值比、其余忽略大小写
/// 必须满足严格弱序（`sort_by` 的比较函数），所以两边数字段数值相等时一起跳过去，
/// 不能拿"总长度"当收尾判据（那会让 "1a" / "01" / "1b" 互相判等，等号不传递）。
fn natural_less(a: &str, b: &str) -> bool {
    let av: Vec<char> = a.chars().collect();
    let bv: Vec<char> = b.chars().collect();
    let (mut i, mut j) = (0usize, 0usize);
    while i < av.len() && j < bv.len() {
        if av[i].is_ascii_digit() && bv[j].is_ascii_digit() {
            let ia = (i..av.len()).find(|&k| !av[k].is_ascii_digit()).unwrap_or(av.len());
            let jb = (j..bv.len()).find(|&k| !bv[k].is_ascii_digit()).unwrap_or(bv.len());
            // 去掉前导 0 再比：先比位数（等价于比数值），位数相同再逐位比
            let mut sa = i;
            let mut sb = j;
            while sa + 1 < ia && av[sa] == '0' {
                sa += 1;
            }
            while sb + 1 < jb && bv[sb] == '0' {
                sb += 1;
            }
            let (la, lb) = (ia - sa, jb - sb);
            if la != lb {
                return la < lb;
            }
            for k in 0..la {
                if av[sa + k] != bv[sb + k] {
                    return av[sa + k] < bv[sb + k];
                }
            }
            // 数值相等（"1" 与 "01"）：两边一起跳过这一段，先后交给后面的记号
            i = ia;
            j = jb;
            continue;
        }
        let ca = lower_char(av[i]);
        let cb = lower_char(bv[j]);
        if ca != cb {
            return ca < cb;
        }
        i += 1;
        j += 1;
    }
    // 共同记号都比完了：谁还有剩谁就大；两边都用尽 = 相等
    i == av.len() && j < bv.len()
}

/// 单字符小写（多字符映射取第一个，够用于排序比较）
fn lower_char(c: char) -> char {
    c.to_lowercase().next().unwrap_or(c)
}

fn replace_text(s: &str, find: &str, repl: &str, case_sens: bool, all: bool) -> String {
    if find.is_empty() {
        return s.to_string();
    }
    let hs = s.to_lowercase();
    let hf = find.to_lowercase();
    // 复用原文字节索引的前提是大小写映射逐字节等长；只有 ASCII 能保证，
    // 其它字符（如 İ）可能变长，继续按索引切片会落在字符中间 panic
    let ci = !case_sens && s.is_ascii() && find.is_ascii();
    let (hay, needle) = if ci { (hs.as_str(), hf.as_str()) } else { (s, find) };
    let mut out = String::with_capacity(s.len());
    let mut last = 0usize;
    let mut from = 0usize;
    while let Some(p) = hay[from..].find(needle) {
        let at = from + p;
        let end = at + needle.len();
        out.push_str(&s[last..at]);
        out.push_str(repl);
        last = end;
        if !all {
            break;
        }
        from = end;
        if from >= hay.len() {
            break;
        }
    }
    out.push_str(&s[last..]);
    out
}

const LU: [char; 10] = ['零', '一', '二', '三', '四', '五', '六', '七', '八', '九'];

fn convert_text(s: &str, mode: i32) -> String {
    match mode {
        1 => lc_map(s, LCMAP_TRADITIONAL),
        2 => lc_map(s, LCMAP_SIMPLIFIED),
        3 => s
            .chars()
            .map(|c| match c {
                '〇' | '零' => '0',
                '一' => '1',
                '二' | '两' => '2',
                '三' => '3',
                '四' => '4',
                '五' => '5',
                '六' => '6',
                '七' => '7',
                '八' => '8',
                '九' => '9',
                other => other,
            })
            .collect(),
        4 => s
            .chars()
            .map(|c| {
                if c.is_ascii_digit() {
                    LU[(c as u8 - b'0') as usize]
                } else {
                    c
                }
            })
            .collect(),
        5 => s
            .chars()
            .map(|c| match pinyin_of(c) {
                Some(p) => p.to_string(),
                None => c.to_string(),
            })
            .collect(),
        6 => s
            .chars()
            .map(|c| match pinyin_of(c) {
                Some(p) => p.chars().next().unwrap_or(c).to_string(),
                None => c.to_string(),
            })
            .collect(),
        _ => s.to_string(),
    }
}

// kernel32 里直接取（winapi 的 stringapiset / winnls 两个 feature 不在本包依赖里）
const LCMAP_SIMPLIFIED: u32 = 0x0200_0000;
const LCMAP_TRADITIONAL: u32 = 0x0400_0000;
const CP_GB2312: u32 = 936;

extern "system" {
    fn LCMapStringW(locale: u32, flags: u32, src: *const u16, cch: i32, dst: *mut u16, cch2: i32) -> i32;
    fn WideCharToMultiByte(
        cp: u32,
        flags: u32,
        s: *const u16,
        cch: i32,
        out: *mut u8,
        cb: i32,
        def: *const u8,
        used: *mut i32,
    ) -> i32;
}

fn lc_map(s: &str, flags: u32) -> String {
    let w = wide(s);
    let n = unsafe { LCMapStringW(0x0804, flags, w.as_ptr(), w.len() as i32 - 1, std::ptr::null_mut(), 0) };
    if n <= 0 {
        return s.to_string();
    }
    let mut out = vec![0u16; n as usize + 1];
    let m = unsafe { LCMapStringW(0x0804, flags, w.as_ptr(), w.len() as i32 - 1, out.as_mut_ptr(), n) };
    if m <= 0 {
        return s.to_string();
    }
    out.truncate(m as usize);
    String::from_utf16_lossy(&out)
}

/// GB2312 区位 → `rn_pinyin` 索引（用 `WideCharToMultiByte(936)` 求码位后查表）
/// 一级汉字 0xB0A1..0xD7F9 连号；二级从 0xD8A1 起另起一段，索引用 3755 打底 ——
/// 直接按 (hi-0xB0)*94 算会把一级末尾那 5 个空码位也数进去，二级汉字整体错位。
fn gb_index(ch: char) -> Option<usize> {
    let w: Vec<u16> = ch.to_string().encode_utf16().collect();
    if w.len() != 1 {
        return None;
    }
    let mut buf = [0u8; 4];
    let n = unsafe {
        WideCharToMultiByte(CP_GB2312, 0, w.as_ptr(), 1, buf.as_mut_ptr(), 2, std::ptr::null(), std::ptr::null_mut())
    };
    if n != 2 {
        return None;
    }
    let hi = buf[0] as usize;
    let lo = buf[1] as usize;
    if !(0xA1..=0xFE).contains(&lo) {
        return None;
    }
    let idx = if (0xB0..=0xD7).contains(&hi) {
        // 一级汉字到 0xD7F9 为止，0xD7FA..FE 是空的
        if hi == 0xD7 && lo > 0xF9 {
            return None;
        }
        (hi - 0xB0) * 94 + (lo - 0xA1)
    } else if (0xD8..=0xF7).contains(&hi) {
        3755 + (hi - 0xD8) * 94 + (lo - 0xA1)
    } else {
        return None;
    };
    if idx < rn_pinyin::INDEX.len() {
        Some(idx)
    } else {
        None
    }
}

fn pinyin_of(c: char) -> Option<&'static str> {
    let i = gb_index(c)?;
    let k = rn_pinyin::INDEX[i] as usize;
    rn_pinyin::SYLLABLES.get(k).copied()
}

fn now_parts(now: &[u16; 6]) -> (String, String, String) {
    let date = format!("{:04}{:02}{:02}", now[0], now[1], now[2]);
    let time = format!("{:02}{:02}{:02}", now[3], now[4], now[5]);
    let all = format!("{}{}", date, time);
    (date, time, all)
}

/// `ExpandTokens`：`<name> <self> <#> <##> <dir> <now> <date> <time>`
fn expand_tokens(pat: &str, orig: &str, cur: &str, dir: &str, ordinal: i32, now: &[u16; 6]) -> String {
    let (date, time, all) = now_parts(now);
    let mut out = pat.to_string();
    out = out.replace("<name>", orig);
    out = out.replace("<self>", cur);
    out = out.replace("<##>", &pad(ordinal as i64, 2));
    out = out.replace("<#>", &ordinal.to_string());
    out = out.replace("<dir>", dir);
    out = out.replace("<now>", &all);
    out = out.replace("<date>", &date);
    out = out.replace("<time>", &time);
    out
}

fn dir_name_of(dir: &str) -> String {
    let t = dir.trim_end_matches('\\');
    match t.rfind('\\') {
        Some(i) => t[i + 1..].to_string(),
        None => t.to_string(),
    }
}

// —————————————————————— 规则 ——————————————————————
fn combo_sel(id: usize) -> i32 {
    let h = ctl(id);
    if h.is_null() {
        return -1;
    }
    unsafe { SendMessageW(h, CB_GETCURSEL, 0, 0) as i32 }
}

fn combo_text(id: usize) -> String {
    let h = ctl(id);
    if h.is_null() {
        return String::new();
    }
    ctl_text(h)
}

/// 下拉列表里第 `sel` 项的文本（空 = 取不到）
fn combo_item(id: usize, sel: i32) -> String {
    let h = ctl(id);
    if h.is_null() || sel < 0 {
        return String::new();
    }
    let mut buf = [0u16; 256];
    let n = unsafe {
        SendMessageW(
            h,
            CB_GETLBTEXT,
            sel as WPARAM,
            buf.as_mut_ptr() as LPARAM,
        )
    };
    if n <= 0 {
        return String::new();
    }
    String::from_utf16_lossy(&buf[..n as usize])
}

/// 从下拉里点了一项：系统要等 CBN_SELCHANGE 返回之后才把选中项文本回填进编辑框，
/// 而「改为 / 过滤」的取值口径都是"先读框里看得见的字" —— 不在这里先摆正，
/// 取到的就是上一次的旧值（表现为"手打了 123，再从列表挑 png，列表一动不动"）。
fn sync_combo_sel_text(id: usize) {
    let sel = combo_sel(id);
    if sel < 0 {
        return;
    }
    let item = combo_item(id, sel);
    if item.is_empty() {
        return;
    }
    // SetWindowText 会立刻回来一次 CBN_EDITCHANGE，用 syncing 挡掉那一趟
    st_mut(|s| s.syncing = true);
    set_text(ctl(id), &item);
    st_mut(|s| s.syncing = false);
}

/// `rnPickExtSet`：框里一有字就进「改为」这一档（四颗勾选钮全灭）
/// 框被删空则退回"不动扩展名"
fn pick_ext_set() {
    let t = combo_text(ID_EXT_SET).trim().to_string();
    let mode = if t.is_empty() { -1 } else { 4 };
    if st(|s| s.ext_mode) != mode {
        for id in [ID_EXT_UPPER, ID_EXT_LOWER, ID_EXT_KEEP, ID_EXT_DROP] {
            ctl_set_check(id, false);
        }
        st_mut(|s| s.ext_mode = mode);
    }
    apply_rules();
}

/// `rnPickAffixToken`：候选里挑一项时只把 token（第一个空格之前那段）填进框
/// 后面的说明文字留在下拉里看 —— 整句填进去会变成名字的一部分
fn pick_affix_token(id: usize) {
    let sel = combo_sel(id);
    if sel < 0 {
        return;
    }
    let item = combo_item(id, sel);
    if item.is_empty() {
        return;
    }
    let token = match item.find(' ') {
        Some(p) => &item[..p],
        None => item.as_str(),
    };
    st_mut(|s| s.syncing = true);
    set_text(ctl(id), token);
    st_mut(|s| s.syncing = false);
    apply_rules();
}

/// 序号三格离开焦点时把框里的字换回"夹过界的规范值"，
/// 免得界面上挂着 999 而实际按 999 上限之外的数编号
fn seq_write_back() {
    let (a, b, c) = st(|s| (s.seq_start, s.seq_step, s.seq_digits));
    st_mut(|s| s.syncing = true);
    set_text(ctl(ID_SEQ_START), &a.to_string());
    set_text(ctl(ID_SEQ_STEP), &b.to_string());
    set_text(ctl(ID_SEQ_DIGITS), &c.to_string());
    st_mut(|s| s.syncing = false);
}

/// 把控件状态收进 State（勾选钮是唯一真源）
fn sync_from_controls() {
    let case = if ctl_check(ID_CASE_UPPER) {
        0
    } else if ctl_check(ID_CASE_LOWER) {
        1
    } else if ctl_check(ID_CASE_TITLE) {
        2
    } else {
        -1
    };
    let seq_on = ctl_check(ID_SEQ_ON) || ctl_check(ID_NUM_ENABLE);
    // 扩展名五档：全大写 / 全小写 / 删除 / 不变 四颗勾选钮 + 「改为」下拉。
    // 「改为」没有勾选钮（框里有字即该档），所以一颗都没勾时不能直接把状态抹成 -1
    let checked_ext = if ctl_check(ID_EXT_UPPER) {
        1
    } else if ctl_check(ID_EXT_LOWER) {
        2
    } else if ctl_check(ID_EXT_DROP) {
        3
    } else if ctl_check(ID_EXT_KEEP) {
        0
    } else {
        -1
    };
    let del = [
        ctl_check(ID_DEL_SPACE),
        ctl_check(ID_DEL_LOWER),
        ctl_check(ID_DEL_UPPER),
        ctl_check(ID_DEL_DIGIT),
        ctl_check(ID_DEL_CJK),
        ctl_check(ID_DEL_SPECIAL),
    ];
    // 序号三格：只收数字，越界就地夹回。
    // 夹值放在这里而不是只放在 EN_CHANGE：清规则 / 撤销 / 切页后再读也都走同一条口径
    let (ps, pt, pd) = st(|s| (s.seq_start, s.seq_step, s.seq_digits));
    let seq_start = seq_int(ID_SEQ_START, ps, 0, 999_999);
    let seq_step = seq_int(ID_SEQ_STEP, pt.max(1), 1, 999);
    let seq_digits = seq_int(ID_SEQ_DIGITS, pd, 0, 9);
    // 必须在 st_mut 之外取控件状态：ctl/ctl_check 自己也要借 ST，
    // 一旦在可变借用里调用就是 RefCell 重入 panic（建页时 EN_CHANGE 直接触发）
    let affix_on = ctl_check(ID_AFFIX_ON);
    let rep_case = ctl_check(ID_REP_CASE);
    let rep_all = ctl_check(ID_REP_ALL);
    st_mut(|s| {
        s.case = case;
        s.seq_on = seq_on;
        s.ext_mode = if checked_ext >= 0 {
            checked_ext
        } else if s.ext_mode == 4 {
            4
        } else {
            -1
        };
        s.del = del;
        s.seq_start = seq_start;
        s.seq_step = seq_step;
        s.seq_digits = seq_digits;
        s.affix_on = affix_on;
        s.rep_case = rep_case;
        s.rep_all = rep_all;
    });
}

fn read_rules() -> Rules {
    sync_from_controls();
    let mut ext_set = combo_text(ID_EXT_SET).trim().to_string();
    while ext_set.starts_with('.') || ext_set.starts_with('*') {
        ext_set.remove(0);
    }
    let mut conv_to = ctl_text(ctl(ID_CONV_TO)).trim().to_string();
    conv_to.retain(|c| !matches!(c, '\\' | '/' | ':' | '*' | '?' | '"' | '<' | '>' | '|'));
    let del_text = ctl_text(ctl(ID_DEL_TEXT)).trim().to_string();
    let now: [u16; 6] = unsafe {
        let mut st_now = zeroed::<winapi::um::minwinbase::SYSTEMTIME>();
        GetLocalTime(&mut st_now);
        [
            st_now.wYear,
            st_now.wMonth,
            st_now.wDay,
            st_now.wHour,
            st_now.wMinute,
            st_now.wSecond,
        ]
    };
    Rules {
        find: ctl_text(ctl(ID_FIND)),
        replace: ctl_text(ctl(ID_REPLACE)),
        del_text,
        ext_set,
        conv_to,
        prefix: ctl_text(ctl(ID_PREFIX)),
        suffix: ctl_text(ctl(ID_SUFFIX)),
        conv: combo_sel(ID_CONVERT),
        now,
    }
}

/// `rnCompose`：手工名 >「改为」> 大小写 > 序号 > 转换 > 替换 > 删除 > 前后缀 > 扩展名
fn compose(row: &Row, ordinal: i32, r: &Rules) -> String {
    if !row.manual.is_empty() {
        return row.manual.clone();
    }
    let (case, seq_on, seq_start, seq_step, seq_digits) =
        st(|s| (s.case, s.seq_on, s.seq_start, s.seq_step, s.seq_digits));
    let del = st(|s| s.del);
    let ext_mode = st(|s| s.ext_mode);
    let affix_on = st(|s| s.affix_on);

    let orig = base_of_name(&row.name).to_string();
    let seq_pad = |ord: i32| -> String {
        let step = if seq_step == 0 { 1 } else { seq_step };
        let digits = seq_digits.max(1);
        let v = seq_start + step * (ord as i64 - 1);
        pad(v.clamp(0, 999_999_999), digits.max(1))
    };

    let mut base;
    if !r.conv_to.is_empty() {
        base = r.conv_to.clone();
        if seq_on {
            base.push_str(&seq_pad(ordinal));
        }
    } else {
        base = orig.clone();
        match case {
            0 => base = upper_all(&base),
            1 => base = lower_all(&base),
            2 => base = title_first(&base),
            _ => {}
        }
        if seq_on {
            base = seq_pad(ordinal);
        }
        if r.conv > 0 {
            base = convert_text(&base, r.conv);
        }
        if !r.find.is_empty() {
            base = replace_text(&base, &r.find, &r.replace, st(|s| s.rep_case), st(|s| s.rep_all));
        }
        if del.iter().any(|v| *v) {
            let kept: String = base
                .chars()
                .filter(|c| {
                    let cp = *c as u32;
                    if del[0] && c.is_whitespace() {
                        return false;
                    }
                    if del[1] && c.is_lowercase() && cp < 0x2E80 {
                        return false;
                    }
                    if del[2] && c.is_uppercase() && cp < 0x2E80 {
                        return false;
                    }
                    if del[3] && c.is_numeric() && cp < 0x2E80 {
                        return false;
                    }
                    if del[4] && (0x3000..=0x9FFF).contains(&cp) {
                        return false;
                    }
                    if del[5] && cp < 0x2E80 && !c.is_alphanumeric() && !c.is_whitespace() {
                        return false;
                    }
                    true
                })
                .collect();
            base = kept;
        }
        if !r.del_text.is_empty() {
            base = replace_text(&base, &r.del_text, "", true, true);
        }
    }

    if affix_on {
        // 没开前后缀时不要白跑两轮 token 展开（每轮 8 次 replace，每行每次重算都做）
        let dir = dir_name_of(&row.dir);
        let prefix = expand_tokens(&r.prefix, &orig, &base, &dir, ordinal, &r.now);
        let suffix = expand_tokens(&r.suffix, &orig, &base, &dir, ordinal, &r.now);
        if !prefix.is_empty() || !suffix.is_empty() {
            base = format!("{}{}{}", prefix, base, suffix);
        }
    }

    let mut ext = ext_of_name(&row.name).to_string();
    if row.checked {
        match ext_mode {
            1 => ext = upper_all(&ext),
            2 => ext = lower_all(&ext),
            3 => ext.clear(),
            4 => {
                if !r.ext_set.is_empty() {
                    ext = r.ext_set.clone();
                }
            }
            _ => {}
        }
    }
    if ext.is_empty() {
        return base;
    }
    format!("{}.{}", base, ext)
}

/// `rnRecalcStatus`：一遍算新名，一遍判状态
fn recalc() {
    let rules = read_rules();
    // 就地在原行上算：原来先克隆整表、再逐行重建一个 probe Row、再克隆回写，
    // 每敲一个键对每行要做 4~5 次字符串分配；千级文件时输入明显发涩
    st_mut(|s| {
        for (i, r) in s.rows.iter_mut().enumerate() {
            if r.done {
                r.new_name = r.name.clone();
            } else {
                r.new_name = compose(r, i as i32 + 1, &rules);
            }
        }
    });

    let mut dup: HashMap<String, i32> = HashMap::new();
    let mut sources: HashSet<String> = HashSet::new();
    let snap = st(|s| {
        s.rows
            .iter()
            .map(|r| (r.dir.clone(), r.name.clone(), r.new_name.clone(), r.checked, r.done))
            .collect::<Vec<_>>()
    });
    for (dir, name, new_name, checked, done) in &snap {
        if *done || !*checked || *new_name == *name {
            continue;
        }
        if !file_exists(&join(dir, name)) {
            continue;
        }
        if !valid_name(new_name) {
            continue;
        }
        *dup.entry(format!("{}|{}", lower_all(dir), lower_all(new_name))).or_insert(0) += 1;
        sources.insert(format!("{}|{}", lower_all(dir), lower_all(name)));
    }
    st_mut(|s| {
        for r in s.rows.iter_mut() {
            if r.done {
                r.status = Stat::Done;
                continue;
            }
            let key = format!("{}|{}", lower_all(&r.dir), lower_all(&r.new_name));
            if !file_exists(&join(&r.dir, &r.name)) {
                r.status = Stat::Missing;
            } else if !valid_name(&r.new_name) {
                r.status = Stat::Bad;
            } else if dup.get(&key).copied().unwrap_or(0) > 1 {
                r.status = Stat::Conflict;
            } else if r.new_name == r.name {
                r.status = Stat::Same;
            } else if file_exists(&join(&r.dir, &r.new_name)) && !sources.contains(&key) {
                r.status = Stat::Conflict;
            } else {
                r.status = Stat::Ready;
            }
        }
    });
}

fn apply_rules() {
    if st(|s| s.syncing) {
        return;
    }
    hide_editor();
    recalc();
    refresh();
    let p = st(|s| s.parent);
    if !p.is_null() {
        unsafe {
            InvalidateRect(p, std::ptr::null(), FALSE);
        }
    }
}

// —————————————————————— 盘上动作 ——————————————————————
fn join(dir: &str, name: &str) -> String {
    format!("{}{}", dir, name)
}

fn file_exists(p: &str) -> bool {
    // std::fs 会为长路径自动加 \\?\ 前缀；Win32 直调不做，>260 字符会被误判为不存在
    std::fs::metadata(p).is_ok()
}

fn file_size(p: &str) -> u64 {
    std::fs::metadata(p).map(|m| m.len()).unwrap_or(0)
}

fn norm_dir(dir: &str) -> String {
    if dir.ends_with('\\') {
        dir.to_string()
    } else {
        format!("{}\\", dir)
    }
}

/// MoveFileExW 不认长路径，超过 248 字符的绝对路径先补 \\?\ 前缀（UNC 用 \\?\UNC\）
fn wide_long(p: &str) -> Vec<u16> {
    if p.len() < 248 || p.starts_with("\\\\?\\") {
        return wide(p);
    }
    let q = match p.strip_prefix("\\\\") {
        Some(rest) => format!("\\\\?\\UNC\\{rest}"),
        None => format!("\\\\?\\{p}"),
    };
    wide(&q)
}

fn move_file(src: &str, dst: &str) -> bool {
    let s = wide_long(src);
    let d = wide_long(dst);
    unsafe { MoveFileExW(s.as_ptr(), d.as_ptr(), 0) != 0 }
}

/// `MoveAll`：先全部挪成临时名，再挪到目标名，任何一步失败都整体回滚
fn move_all(moves: &[(String, String)]) -> Option<usize> {
    let pid = std::process::id();
    let mut temp: Vec<(String, String)> = Vec::with_capacity(moves.len());
    for (i, (src, dst)) in moves.iter().enumerate() {
        let dir = match dst.rfind('\\') {
            Some(p) => &dst[..p + 1],
            None => "",
        };
        // 中转名必须是"源文件所在目录 + 文件名"的完整路径（只给文件名会被拿当前目录解析），
        // 而且先确认它真的不存在：同名残留会让第一趟搬迁就失败
        let mut t = String::new();
        for n in 0..10_000u32 {
            let cand = format!("{}~rn{}_{}.tmp", dir, pid, n);
            if !file_exists(&cand) {
                t = cand;
                break;
            }
        }
        if t.is_empty() {
            t = format!("{}~rn{}_{}.tmp", dir, pid, i);
        }
        if !move_file(src, &t) {
            for (s, t2) in temp.iter().rev() {
                let _ = move_file(t2, s);
            }
            return Some(i);
        }
        temp.push((src.clone(), t));
    }
    let mut placed: Vec<(String, String)> = Vec::with_capacity(moves.len());
    for (i, (_, t)) in temp.iter().enumerate() {
        let dst = &moves[i].1;
        if !move_file(t, dst) {
            for (t2, d2) in placed.iter().rev() {
                let _ = move_file(d2, t2);
            }
            for (s, t2) in temp.iter().rev() {
                let _ = move_file(t2, s);
            }
            return Some(i);
        }
        placed.push((t.clone(), dst.clone()));
    }
    None
}

/// `rnAddPaths`：目录递归（visited 防 junction 环）、按"目录|名"去重、排序
fn add_paths(paths: Vec<String>) {
    hide_editor();
    let mut found: Vec<(String, String, u64)> = Vec::new();
    let mut stack: Vec<PathBuf> = paths.into_iter().map(PathBuf::from).collect();
    let mut visited: HashSet<String> = HashSet::new();
    while let Some(p) = stack.pop() {
        match std::fs::metadata(&p) {
            Ok(md) if md.is_dir() => {
                // 用解析后的真实路径做键：junction / symlink 环每层路径都会变长，
                // 只用原路径去重永远不命中，遍历会无限膨胀
                let key = std::fs::canonicalize(&p)
                    .unwrap_or_else(|_| p.clone())
                    .to_string_lossy()
                    .to_lowercase();
                if !visited.insert(key) {
                    continue;
                }
                if let Ok(rd) = std::fs::read_dir(&p) {
                    for e in rd.flatten() {
                        stack.push(e.path());
                    }
                }
            }
            Ok(md) => {
                let dir = p
                    .parent()
                    .map(|d| norm_dir(&d.to_string_lossy()))
                    .unwrap_or_default();
                let name = p
                    .file_name()
                    .map(|n| n.to_string_lossy().to_string())
                    .unwrap_or_default();
                if !name.is_empty() {
                    found.push((dir, name, md.len()));
                }
            }
            Err(_) => {}
        }
    }
    let mut added = 0usize;
    st_mut(|s| {
        let mut keys: HashSet<String> = s
            .rows
            .iter()
            .map(|r| lower_all(&join(&r.dir, &r.name)))
            .collect();
        for (dir, name, size) in found {
            let k = lower_all(&join(&dir, &name));
            if !keys.insert(k) {
                continue;
            }
            s.rows.push(Row {
                dir,
                name,
                new_name: String::new(),
                manual: String::new(),
                size,
                status: Stat::Ready,
                checked: true,
                done: false,
            });
            added += 1;
        }
        s.rows.sort_by(|a, b| {
            let la = lower_all(&a.dir);
            let lb = lower_all(&b.dir);
            la.cmp(&lb).then_with(|| {
                if natural_less(&a.name, &b.name) {
                    std::cmp::Ordering::Less
                } else if natural_less(&b.name, &a.name) {
                    std::cmp::Ordering::Greater
                } else {
                    std::cmp::Ordering::Equal
                }
            })
        });
        s.sel = -1;
    });
    rebuild_filter();
    apply_rules();
    if added == 0 {
        toast("没有新增文件");
    }
}

/// `rnDropFiles`
pub fn on_drop_files(hdrop: HDROP) {
    let n = unsafe { DragQueryFileW(hdrop, 0xFFFF_FFFF, std::ptr::null_mut(), 0) };
    let mut paths = Vec::new();
    for i in 0..n {
        let len = unsafe { DragQueryFileW(hdrop, i, std::ptr::null_mut(), 0) };
        let mut buf = vec![0u16; len as usize + 1];
        unsafe {
            DragQueryFileW(hdrop, i, buf.as_mut_ptr(), len + 1);
        }
        buf.pop();
        paths.push(String::from_utf16_lossy(&buf));
    }
    unsafe {
        DragFinish(hdrop);
    }
    if !paths.is_empty() {
        add_paths(paths);
    }
}

/// `rnPickFiles`
fn pick_files() {
    let mut buf = vec![0u16; 32768];
    let flt = wide("所有文件 (*.*)\0*.*\0\0");
    let mut ofn: OPENFILENAMEW = unsafe { zeroed() };
    ofn.lStructSize = std::mem::size_of::<OPENFILENAMEW>() as u32;
    ofn.hwndOwner = st(|s| s.parent);
    ofn.lpstrFilter = flt.as_ptr();
    ofn.lpstrFile = buf.as_mut_ptr();
    ofn.nMaxFile = buf.len() as u32;
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    let ok = unsafe { GetOpenFileNameW(&mut ofn) };
    if ok == 0 {
        return;
    }
    // 多选返回的是"目录\0文件1\0文件2\0\0"
    let mut items: Vec<String> = Vec::new();
    let mut i = 0usize;
    while i < buf.len() && buf[i] != 0 {
        let mut j = i;
        while j < buf.len() && buf[j] != 0 {
            j += 1;
        }
        items.push(String::from_utf16_lossy(&buf[i..j]));
        i = j + 1;
        if items.len() == 1 && (i >= buf.len() || buf[i] == 0) {
            break;
        }
    }
    let paths = if items.len() > 1 {
        let base = items[0].clone();
        items[1..]
            .iter()
            .map(|f| format!("{}\\{}", base, f))
            .collect::<Vec<_>>()
    } else {
        items
    };
    add_paths(paths);
}

/// `rnPickFolder`
fn pick_folder() {
    let p = st(|s| s.parent);
    if p.is_null() {
        return;
    }
    if let Some(path) = crate::modules::settings::pick_folder(p, "选择文件夹") {
        add_paths(vec![path]);
    }
}

// —————————————————————— 执行 / 撤销 / 重置 ——————————————————————
fn run_rename() {
    hide_editor();
    let picks: Vec<(usize, String, String)> = st(|s| {
        s.rows
            .iter()
            .enumerate()
            .filter(|(_, r)| !r.done && r.checked && r.status == Stat::Ready && r.new_name != r.name)
            .map(|(i, r)| (i, join(&r.dir, &r.name), join(&r.dir, &r.new_name)))
            .collect()
    });
    if picks.is_empty() {
        toast("没有可改名的文件");
        return;
    }
    let moves: Vec<(String, String)> = picks.iter().map(|(_, a, b)| (a.clone(), b.clone())).collect();
    // skipped 是"这一批一个没动的行数"：未勾选、已改过、同名、状态不可改都算进去
    let skip = st(|s| s.rows.len()).saturating_sub(picks.len());
    match move_all(&moves) {
        None => {
            let mut undo_add: Vec<(String, String)> = Vec::new();
            st_mut(|s| {
                for (i, src, dst) in &picks {
                    undo_add.push((dst.clone(), src.clone()));
                    let r = &mut s.rows[*i];
                    r.name = r.new_name.clone();
                    r.manual.clear();
                    r.done = true;
                    r.status = Stat::Done;
                    r.size = file_size(dst);
                }
            });
            st_mut(|s| s.undos.extend(undo_add));
            refresh();
            toast(&format!("已改名 {} 个，跳过 {} 个", picks.len(), skip));
        }
        Some(bad) => {
            // 只把真正卡住的那一行标失败：其余行已被整体退回原样，仍然是"可以改"
            st_mut(|s| {
                for (i, (idx, _, _)) in picks.iter().enumerate() {
                    s.rows[*idx].status = if i == bad { Stat::Fail } else { Stat::Ready };
                }
            });
            refresh();
            toast("改名失败，已回滚");
        }
    }
    rebuild_filter();
    apply_rules();
}

fn undo_rename() {
    // 撤销会改行里的名字，先把还开着的编辑框落定，免得输入写到已经换了名的行上
    hide_editor();
    let pairs = st(|s| s.undos.clone());
    if pairs.is_empty() {
        toast("没有可撤销的改名");
        return;
    }
    // 逆序还原：后改的先退，避免中途互相占名
    let moves: Vec<(String, String)> = pairs
        .iter()
        .rev()
        .map(|(dst, src)| (dst.clone(), src.clone()))
        .collect();
    if move_all(&moves).is_some() {
        toast("改名失败，已回滚");
        return;
    }
    st_mut(|s| {
        for (dst, src) in pairs.iter() {
            // 找的键必须是"撤销前它在盘上的名字"（= rows 里的 name），
            // 拿原名去找永远找不到，撤销完了列表还挂着"成功"
            let key = lower_all(dst);
            if let Some(r) = s
                .rows
                .iter_mut()
                .find(|r| lower_all(&join(&r.dir, &r.name)) == key)
            {
                r.name = match src.rfind('\\') {
                    Some(p) => src[p + 1..].to_string(),
                    None => src.clone(),
                };
                r.new_name = r.name.clone();
                r.done = false;
                r.manual.clear();
                r.status = Stat::Ready;
                r.size = file_size(src);
            }
        }
        s.undos.clear();
    });
    // 扩展名可能跟着回来了，格式表也要重建
    rebuild_filter();
    // 撤销后把规则一并清空：规则还挂在界面上，下面 apply_rules 会立刻按旧规则重算，
    // 「新文件名」列看着跟没撤销一模一样，用户会以为撤销没生效
    clear_rules();
    apply_rules();
    toast("已撤销改名");
}

fn clear_rows() {
    hide_editor();
    st_mut(|s| {
        s.rows.clear();
        s.undos.clear();
        s.sel = -1;
        s.top = 0;
        // 过滤是"看哪一部分"的视图状态，必须跟着一起清：
        // 留着旧关键字的话，清空之后再拖进来的文件会被它筛掉，
        // 列表看着还是空的，像是根本没加进来
        s.fmt_on.clear();
        s.fmt_text.clear();
    });
    st_mut(|s| s.syncing = true);
    set_text(ctl(ID_FILTER), "");
    st_mut(|s| s.syncing = false);
    rebuild_filter();
    apply_rules();
    // 右下角那个「N 个」也跟着重画
    let p = st(|s| s.parent);
    if !p.is_null() {
        unsafe {
            InvalidateRect(p, std::ptr::null(), FALSE);
        }
    }
}

/// `rnReset`：回到刚导入的样子 —— 规则全清、手工名全清、改过的行重新算作起点
/// 勾选全恢复。这样重置完不用重新拖文件，直接选行改就行。
/// （只清规则而勾选和手工改过的名字还留着，看着像重置没生效）
fn reset_rules() {
    hide_editor();
    clear_rules();
    let p = st(|s| s.parent);
    st_mut(|s| {
        for r in s.rows.iter_mut() {
            // 已经改过名的行：盘上现在叫 name，把它当成新的原始名
            if r.done {
                r.done = false;
                r.size = file_size(&join(&r.dir, &r.name));
            }
            r.manual.clear();
            r.checked = true;
            r.status = Stat::Ready;
        }
        // 起点换了，上一轮的撤销记录就对不上了
        s.undos.clear();
    });
    apply_rules();
    if !p.is_null() {
        unsafe {
            InvalidateRect(p, std::ptr::null(), FALSE);
        }
    }
}

/// `rnClearRules`：规则全回默认，行与"已完成"状态不碰
fn clear_rules() {
    st_mut(|s| s.syncing = true);
    for id in [
        ID_CASE_UPPER, ID_CASE_LOWER, ID_CASE_TITLE, ID_NUM_ENABLE, ID_EXT_UPPER, ID_EXT_LOWER,
        ID_EXT_KEEP, ID_EXT_DROP, ID_REP_CASE, ID_SEQ_ON, ID_AFFIX_ON, ID_DEL_SPACE, ID_DEL_LOWER,
        ID_DEL_UPPER, ID_DEL_DIGIT, ID_DEL_CJK, ID_DEL_SPECIAL,
    ] {
        ctl_set_check(id, false);
    }
    ctl_set_check(ID_REP_ALL, true);
    for id in [ID_FIND, ID_REPLACE, ID_DEL_TEXT, ID_PREFIX, ID_SUFFIX, ID_CONV_TO] {
        set_text(ctl(id), "");
    }
    // 「改为」：和初始化一致 —— 清掉选中项、框里留空，
    // 免得重置后又挂着上一轮的扩展名（框空但下拉亮着旧项，看着像两档同时生效）
    {
        let c = ctl(ID_EXT_SET);
        if !c.is_null() {
            unsafe {
                SendMessageW(c, 0x014E /*CB_SETCURSEL*/, !0usize, 0);
                SetWindowTextW(c, wide("").as_ptr());
            }
        }
    }
    set_text(ctl(ID_SEQ_START), "1");
    set_text(ctl(ID_SEQ_STEP), "1");
    set_text(ctl(ID_SEQ_DIGITS), "1");
    {
        let h = ctl(ID_CONVERT);
        unsafe {
            SendMessageW(h, CB_SETCURSEL, 0, 0);
        }
    }
    st_mut(|s| {
        s.case = -1;
        s.seq_on = false;
        s.seq_start = 1;
        s.seq_step = 1;
        s.seq_digits = 1;
        s.affix_on = false;
        s.del = [false; 6];
        s.del_text.clear();
        s.find.clear();
        s.replace.clear();
        s.rep_case = false;
        s.rep_all = true;
        s.ext_mode = -1;
        s.ext_set.clear();
        s.syncing = false;
    });
}

/// 一颗都没勾时复制全部（不是什么都不做）
fn copy_names() {
    let (text, count) = st(|s| {
        let mut picked: Vec<String> = s
            .rows
            .iter()
            .filter(|r| r.checked)
            .map(|r| r.new_name.clone())
            .collect();
        if picked.is_empty() {
            picked = s.rows.iter().map(|r| r.new_name.clone()).collect();
        }
        (picked.join("\r\n"), picked.len())
    });
    if count == 0 {
        return;
    }
    // 复用二维码页的剪贴板实现：同一份逻辑，失败时会释放 GlobalAlloc 的内存
    let copied = crate::copy_text_clipboard(&text);
    // 复制失败（另一个程序正占着剪贴板）就别报"已复制"：见到的和做到的一致
    if copied {
        toast("已复制新文件名");
    }
}

// —————————————————————— 筛选 ——————————————————————
fn refresh() {
    let n = st(|s| s.rows.len()) as i32;
    st_mut(|s| {
        if s.sel >= n {
            s.sel = -1;
        }
    });
    let l = st(|s| s.list);
    if !l.is_null() {
        unsafe {
            // 行数变了（加文件 / 执行改名 / 过滤），滚动条范围要同步，
            // 否则新加的行滚不到、旧范围还挂着
            update_scroll(l);
            InvalidateRect(l, std::ptr::null(), FALSE);
        }
    }
}

/// `rnRebuildFilter`：候选 = 首项「全部格式」+ 各扩展名（去重排序）
fn rebuild_filter() {
    let mut fmts: Vec<String> = st(|s| {
        s.rows
            .iter()
            .map(|r| lower_all(ext_of_name(&r.name)))
            .filter(|e| !e.is_empty())
            .collect()
    });
    fmts.sort();
    fmts.dedup();

    let keep: Vec<String> = st(|s| {
        let on = s.fmt_on.clone();
        on.into_iter().filter(|e| fmts.contains(e)).collect()
    });

    let h = ctl(ID_FILTER);
    let cur = if h.is_null() { String::new() } else { ctl_text(h) };
    if !h.is_null() {
        let all = wide(T_ALL_FMT);
        // 候选就是扩展名本身（不带点）
        let items: Vec<Vec<u16>> = fmts.iter().map(|e| wide(e)).collect();
        unsafe {
            SendMessageW(h, CB_RESETCONTENT, 0, 0);
            SendMessageW(h, CB_ADDSTRING, 0, all.as_ptr() as LPARAM);
            for it in &items {
                SendMessageW(h, CB_ADDSTRING, 0, it.as_ptr() as LPARAM);
            }
        }
        if !cur.is_empty() {
            set_text(h, &cur);
        }
    }
    st_mut(|s| {
        s.fmt_all = fmts;
        s.fmt_on = keep;
        // 筛选条件变了就回到第一行
        s.top = 0;
    });
    apply_fmt();
}

/// `rnApplyFmt`：手打关键字优先，否则 `fmt_on` 空 = 不过滤
fn apply_fmt() {
    let (text, on) = st(|s| (s.fmt_text.clone(), s.fmt_on.clone()));
    if !text.is_empty() {
        if on.is_empty() {
            st_mut(|s| s.fmt_on = s.fmt_all.clone());
        }
        return;
    }
}

/// `rnSyncFilter`
fn sync_filter() {
    // 代码自己往框里写文本时（勾选格式条 / 清空 / 重建候选）不该触发一轮筛选：
    // CBN_EDITCHANGE 是同步回来的，没有这道闸，勾两个格式会被下一趟重置成"全部"
    if st(|s| s.syncing) {
        return;
    }
    let raw = ctl_text(ctl(ID_FILTER));
    let t = lower_all(raw.trim());
    let all = st(|s| s.fmt_all.clone());
    if t.is_empty() || t == lower_all(T_ALL_FMT) {
        st_mut(|s| {
            s.fmt_on.clear();
            s.fmt_text.clear();
        });
    } else if all.iter().any(|e| lower_all(e) == t) {
        st_mut(|s| {
            s.fmt_on = vec![t.clone()];
            s.fmt_text.clear();
        });
    } else {
        st_mut(|s| {
            s.fmt_on = all.clone();
            s.fmt_text = t;
        });
    }
    st_mut(|s| s.top = 0);
    refresh();
}

fn row_matches(r: &Row, text: &str, on: &[String]) -> bool {
    if !text.is_empty() {
        return lower_all(&r.name).contains(text);
    }
    if on.is_empty() {
        return true;
    }
    let e = lower_all(ext_of_name(&r.name));
    on.iter().any(|x| *x == e)
}

fn visible_rows() -> Vec<usize> {
    let (text, on) = st(|s| (s.fmt_text.clone(), s.fmt_on.clone()));
    st(|s| {
        s.rows
            .iter()
            .enumerate()
            .filter(|(_, r)| row_matches(r, &text, &on))
            .map(|(i, _)| i)
            .collect()
    })
}

/// `rnFmtHit`：与绘制同一右边界（`fx + FMT_W > FMT_END` 即 break）
fn fmt_hit(x: i32, y: i32) -> Option<String> {
    if y < sc(geo::BOT_Y) || y >= sc(geo::BOT_Y + geo::BOT_H) {
        return None;
    }
    let mut fx = sc(geo::FMT_X);
    let all = st(|s| s.fmt_all.clone());
    for e in all {
        let right = fx + sc(geo::FMT_W);
        if right > sc(geo::FMT_END) {
            break;
        }
        if x >= fx && x < right {
            return Some(e);
        }
        fx += sc(geo::FMT_PITCH);
    }
    None
}

/// `rnToggleFmt`：全勾时点一个 = 除它以外全勾，勾满即清空
fn toggle_fmt(ext: &str) {
    let all = st(|s| s.fmt_all.clone());
    let on = st(|s| s.fmt_on.clone());
    let next: Vec<String> = if on.is_empty() {
        all.iter().filter(|e| **e != *ext).cloned().collect()
    } else if on.iter().any(|e| e == ext) {
        let v: Vec<String> = on.into_iter().filter(|e| e != ext).collect();
        if v.is_empty() || v.len() == all.len() {
            Vec::new()
        } else {
            v
        }
    } else {
        let mut v = on;
        v.push(ext.to_string());
        if v.len() == all.len() {
            Vec::new()
        } else {
            v.sort();
            v
        }
    };
    let label = if next.len() == 1 {
        next[0].clone()
    } else {
        T_ALL_FMT.to_string()
    };
    st_mut(|s| {
        s.fmt_on = next;
        s.fmt_text.clear();
        s.syncing = true;
    });
    set_text(ctl(ID_FILTER), &label);
    st_mut(|s| s.syncing = false);
    refresh();
    // 页底这排格式勾选是父窗口画的，只刷列表看不出勾没勾上
    let p = st(|s| s.parent);
    if !p.is_null() {
        unsafe {
            InvalidateRect(p, std::ptr::null(), FALSE);
        }
    }
}

fn check_all(on: bool) {
    hide_editor();
    st_mut(|s| {
        for r in s.rows.iter_mut() {
            if !r.done {
                r.checked = on;
            }
        }
    });
    apply_rules();
}

fn invert_checks() {
    hide_editor();
    st_mut(|s| {
        for r in s.rows.iter_mut() {
            if !r.done {
                r.checked = !r.checked;
            }
        }
    });
    apply_rules();
}

// —————————————————————— 面板与互斥规则 ——————————————————————
fn set_panel(p: i32) {
    if st(|s| s.panel) == p {
        return;
    }
    st_mut(|s| s.panel = p);
    sync_panel();
    // 压线标签 / 行标签 / 卡片边都是父窗口画的；切完必须立刻失效父窗口，
    // 否则要等下一次别的重绘才看见切换结果（表现为“点了半天才反应”）
    let pr = st(|s| s.parent);
    if !pr.is_null() {
        unsafe {
            InvalidateRect(pr, std::ptr::null(), FALSE);
        }
    }
}

fn set_case(n: i32) {
    let cur = st(|s| s.case);
    let next = if cur == n { -1 } else { n };
    st_mut(|s| {
        s.syncing = true;
    });
    ctl_set_check(ID_CASE_UPPER, next == 0);
    ctl_set_check(ID_CASE_LOWER, next == 1);
    ctl_set_check(ID_CASE_TITLE, next == 2);
    if next >= 0 {
        ctl_set_check(ID_NUM_ENABLE, false);
        ctl_set_check(ID_SEQ_ON, false);
    }
    st_mut(|s| {
        s.syncing = false;
        s.case = next;
    });
    apply_rules();
}

fn toggle_seq() {
    // 「序号」与「数字递增」是同一颗开关的两个入口
    // 点任意一颗，两颗一起亮/灭，否则被点的那颗看着像没反应
    let cur = ctl_check(ID_SEQ_ON) || ctl_check(ID_NUM_ENABLE);
    let next = !cur;
    ctl_set_check(ID_SEQ_ON, next);
    ctl_set_check(ID_NUM_ENABLE, next);
    if next {
        ctl_set_check(ID_CASE_UPPER, false);
        ctl_set_check(ID_CASE_LOWER, false);
        ctl_set_check(ID_CASE_TITLE, false);
        st_mut(|s| s.case = -1);
    }
    apply_rules();
}

fn set_ext_mode(n: i32) {
    let cur = st(|s| s.ext_mode);
    let next = if cur == n { -1 } else { n };
    ctl_set_check(ID_EXT_UPPER, next == 1);
    ctl_set_check(ID_EXT_LOWER, next == 2);
    ctl_set_check(ID_EXT_KEEP, next == 0);
    ctl_set_check(ID_EXT_DROP, next == 3);
    // 没停在「改为」这一档就把下拉里的字清掉，不然它挂着一个不生效的格式
    if next != 4 && !combo_text(ID_EXT_SET).is_empty() {
        let c = ctl(ID_EXT_SET);
        if !c.is_null() {
            st_mut(|s| s.syncing = true);
            unsafe {
                SendMessageW(c, 0x014E /*CB_SETCURSEL*/, !0usize, 0);
                SetWindowTextW(c, wide("").as_ptr());
            }
            st_mut(|s| s.syncing = false);
        }
    }
    st_mut(|s| s.ext_mode = next);
    apply_rules();
}

fn toggle_btn(id: usize) {
    let v = !ctl_check(id);
    ctl_set_check(id, v);
    apply_rules();
}

// —————————————————————— 命令分发 ——————————————————————
// 通知码（winuser.h / winres.h）：CBN_SELCHANGE=1、CBN_EDITCHANGE=5、
// EN_CHANGE=0x0300、EN_KILLFOCUS=0x0200
pub const CBN_SELCHANGE: u16 = 1;
pub const CBN_EDITCHANGE: u16 = 5;
pub const EN_CHANGE: u16 = 0x0300;
pub const EN_KILLFOCUS: u16 = 0x0200;

/// `WM_COMMAND` 里 page == PageRename 的那一段：先按"改变的值"分流
/// 再落到按钮动作。只按 id 分发、丢掉通知码会让
/// 「改为 / 过滤」从下拉里挑一项、前缀后缀挑 token、序号框夹值这些全都失效。
pub fn on_command(id: usize, code: u16, hwnd: HWND) {
    st_mut(|s| {
        if s.parent.is_null() {
            s.parent = hwnd;
        }
    });

    if code == CBN_SELCHANGE || code == CBN_EDITCHANGE {
        // 从列表里点了一项：这一瞬间编辑框里还挂着上一次的字，先摆正再取值
        if code == CBN_SELCHANGE && (id == ID_EXT_SET || id == ID_FILTER) {
            sync_combo_sel_text(id);
        }
        match id {
            ID_FILTER => {
                sync_filter();
                return;
            }
            ID_CONVERT | ID_CONV_TO => {
                apply_rules();
                return;
            }
            ID_EXT_SET => {
                // 只在"框已经被删空"时清一次选中项：这时清选中没有字可丢。
                // 绝不能每敲一个字都 CB_SETCURSEL(-1) —— 对可编辑下拉来说那句
                // 会连编辑框里的字一起清空，表现就是"字打不进去"
                if code == CBN_EDITCHANGE && ctl_text(ctl(ID_EXT_SET)).is_empty() {
                    let c = ctl(ID_EXT_SET);
                    if !c.is_null() {
                        st_mut(|s| s.syncing = true);
                        unsafe {
                            SendMessageW(c, 0x014E /*CB_SETCURSEL*/, !0usize, 0);
                        }
                        st_mut(|s| s.syncing = false);
                    }
                }
                pick_ext_set();
                return;
            }
            ID_PREFIX | ID_SUFFIX => {
                if code == CBN_SELCHANGE {
                    pick_affix_token(id);
                } else {
                    apply_rules();
                }
                return;
            }
            _ => {}
        }
    }

    if code == EN_CHANGE && matches!(id, ID_FIND | ID_REPLACE | ID_DEL_TEXT) {
        apply_rules();
        return;
    }
    if matches!(id, ID_SEQ_START | ID_SEQ_STEP | ID_SEQ_DIGITS) {
        if code == EN_CHANGE {
            apply_rules();
        } else if code == EN_KILLFOCUS {
            seq_write_back();
        }
        return;
    }
    if code == EN_KILLFOCUS || code == EN_CHANGE {
        // 其它输入框（就地编辑框 ID_EDIT 也在这一档）不在这里动作
        return;
    }

    match id {
        ID_ADD_FILES => pick_files(),
        ID_ADD_FOLDER => pick_folder(),
        ID_COPY => copy_names(),
        ID_CLEAR => clear_rows(),
        ID_RESET => reset_rules(),
        ID_UNDO => undo_rename(),
        ID_RUN => run_rename(),
        ID_FILTER => sync_filter(),
        ID_CHECK_ALL => check_all(true),
        ID_INVERT => invert_checks(),
        ID_CASE_UPPER => set_case(0),
        ID_CASE_LOWER => set_case(1),
        ID_CASE_TITLE => set_case(2),
        ID_NUM_ENABLE => toggle_seq(),
        ID_EXT_UPPER => set_ext_mode(1),
        ID_EXT_LOWER => set_ext_mode(2),
        ID_EXT_DROP => set_ext_mode(3),
        ID_EXT_KEEP => set_ext_mode(0),
        ID_EXT_SET => pick_ext_set(),
        ID_SEQ_ON => toggle_seq(),
        ID_AFFIX_ON => toggle_btn(ID_AFFIX_ON),
        ID_REP_CASE => toggle_btn(ID_REP_CASE),
        ID_REP_ALL => toggle_btn(ID_REP_ALL),
        ID_DEL_SPACE => toggle_btn(ID_DEL_SPACE),
        ID_DEL_LOWER => toggle_btn(ID_DEL_LOWER),
        ID_DEL_UPPER => toggle_btn(ID_DEL_UPPER),
        ID_DEL_DIGIT => toggle_btn(ID_DEL_DIGIT),
        ID_DEL_CJK => toggle_btn(ID_DEL_CJK),
        ID_DEL_SPECIAL => toggle_btn(ID_DEL_SPECIAL),
        ID_CONVERT | ID_CONV_TO | ID_PREFIX | ID_SUFFIX => apply_rules(),
        _ => {}
    }
}

// —————————————————————— 勾选框 / 勾 ——————————————————————
fn check_tick(dc: HDC, x: i32, y: i32, size: i32, color: u32) {
    let t = sc(ui::LINE_THICK).max(1);
    let p = |rx: f32, ry: f32| -> (i32, i32) {
        (
            x + (size as f32 * rx) as i32,
            y + (size as f32 * ry) as i32,
        )
    };
    let (x1, y1) = p(ui::CHECK_TICK_P1X, ui::CHECK_TICK_P1Y);
    let (x2, y2) = p(ui::CHECK_TICK_P2X, ui::CHECK_TICK_P2Y);
    let (x3, y3) = p(ui::CHECK_TICK_P3X, ui::CHECK_TICK_P3Y);
    skew(dc, x1, y1, x2, y2, t, color);
    skew(dc, x2, y2, x3, y3, t, color);
}

/// 勾选框（列表里用灰框，规则行/格式行用主题边框色）
fn draw_box(dc: HDC, x: i32, y: i32, size: i32, on: bool, _hot: bool, gray: bool) {
    let r = RECT {
        left: x,
        top: y,
        right: x + size,
        bottom: y + size,
    };
    if on {
        fill(dc, &r, ui::C_ACCENT_DEEP);
        // 勾画在整框上，不要再内缩
        check_tick(dc, x, y, size, ui::C_SURFACE);
    } else {
        fill(dc, &r, ui::C_SURFACE);
        if gray {
            unsafe {
                FrameRect(dc, &r, GetStockObject(GRAY_BRUSH as i32) as HBRUSH);
            }
        } else {
            // 未选中的方框描边一律走主题边框色： 里只有"降为次要"的那种
            // （定时关机页非生效行）才用更浅的 SUBTLE，改名页的规则勾选不用
            frame(dc, &r, ui::C_BORDER);
        }
    }
}

// —————————————————————— 列表几何 ——————————————————————
// 六列：序号 / 文件名 / 新文件名 / 扩展名 / 大小 / 状态
const COL_CHECK: i32 = 22;   // 勾选槽宽：行号从这之后开始
const COL_DIGIT: i32 = 7;    // 一个数字约 7 宽
const COL_EXT: i32 = 46;
const COL_SIZE: i32 = 52;
const COL_STATUS: i32 = 46;
const COL_NAME_MAX: i32 = 240;  // 名字两列的宽上限
const COL_TABLE_W: i32 = 540;   // 列表可用宽兜底

fn seq_col_width(n: usize) -> i32 {
    let digits = if n >= 100 { 3 } else if n >= 10 { 2 } else { 1 };
    sc(COL_CHECK) + sc(COL_DIGIT) * digits + sc(4)
}

/// `RnList::colWidth`：六列的当前宽度（自左往右）
/// 名字两列吃剩余宽度，合计必须正好等于客户区宽 —— 差额记到「新文件名」头上
fn col_widths(w: i32, user: &[i32; 6]) -> [i32; 6] {
    let n = st(|s| s.rows.len());
    let mut cw = [0i32; 6];
    cw[0] = if user[0] > 0 { user[0].max(seq_col_width(n)) } else { seq_col_width(n) };
    cw[3] = if user[3] > 0 { user[3] } else { sc(COL_EXT) };
    cw[4] = if user[4] > 0 { user[4] } else { sc(COL_SIZE) };
    cw[5] = if user[5] > 0 { user[5] } else { sc(COL_STATUS) };
    let free = w - cw[0] - cw[3] - cw[4] - cw[5];
    let min_name = sc(50);
    match (user[1] > 0, user[2] > 0) {
        (true, true) => {
            cw[1] = user[1];
            cw[2] = user[2];
        }
        (true, false) => {
            cw[1] = user[1];
            cw[2] = (free - cw[1]).max(min_name);
        }
        (false, true) => {
            cw[2] = user[2];
            cw[1] = (free - cw[2]).max(min_name);
        }
        (false, false) => {
            let half = free / 2;
            cw[1] = half - sc(6);
            cw[2] = free - cw[1];
        }
    }
    // 合计必须正好等于客户区宽，否则右侧会空一截；差额记到“新文件名”头上
    let sum: i32 = cw.iter().sum();
    let delta = w - sum;
    cw[2] = (cw[2] + delta).max(min_name);
    cw
}

fn col_edges(w: i32, user: &[i32; 6]) -> [i32; 5] {
    let cw = col_widths(w, user);
    let mut acc = 0;
    let mut e = [0i32; 5];
    for i in 0..5 {
        acc += cw[i];
        e[i] = acc;
    }
    e
}

/// `RnList::colMin`：拖列宽时每列的下限
/// 序号列要永远装得下当前位数；名字两列 50、扩展名 38、大小 44、状态 40
fn col_min(col: usize) -> i32 {
    match col {
        0 => (sc(COL_CHECK) + sc(4)).max(seq_col_width(st(|s| s.rows.len()))),
        1 | 2 => sc(50),
        3 => sc(38),
        4 => sc(44),
        _ => sc(40),
    }
}

/// `RnList::colMax`：名字两列封顶 240 —— 再宽下去别的列全被压成一条缝
fn col_max(col: usize, table_w: i32) -> i32 {
    match col {
        1 | 2 => sc(COL_NAME_MAX),
        _ => if table_w > 0 { table_w } else { sc(COL_TABLE_W) },
    }
}

/// 命中表头分隔线：任意一条都能拖
fn divider_at(w: i32, user: &[i32; 6], x: i32, y: i32) -> i32 {
    if y >= sc(ui::HEADER_H) {
        return -1;
    }
    let e = col_edges(w, user);
    let tol = sc(3);
    for (c, ex) in e.iter().enumerate() {
        if (x - ex).abs() <= tol {
            return c as i32;
        }
    }
    -1
}

fn rows_view(h: HWND) -> i32 {
    let mut rc: RECT = unsafe { zeroed() };
    unsafe {
        GetClientRect(h, &mut rc);
    }
    ((rc.bottom - rc.top - sc(ui::HEADER_H)) / sc(ui::ROW_H)).max(1)
}

fn cell_rect(h: HWND, vis: i32, col: usize) -> RECT {
    let w = unsafe {
        let mut rc: RECT = zeroed();
        GetClientRect(h, &mut rc);
        rc.right - rc.left
    };
    let edges = col_edges(w, &st(|s| s.user_w));
    let top = sc(ui::HEADER_H) + vis * sc(ui::ROW_H);
    let left = if col == 0 { 0 } else { edges[col - 1] };
    let right = if col >= 5 { w } else { edges[col] };
    RECT {
        left,
        top,
        right,
        bottom: top + sc(ui::ROW_H),
    }
}

fn update_scroll(h: HWND) {
    let total = visible_rows().len() as i32;
    let page = rows_view(h);
    let top = st(|s| s.top);
    let top = top.min((total - page).max(0)).max(0);
    st_mut(|s| s.top = top);
    let mut si: SCROLLINFO = unsafe { zeroed() };
    si.cbSize = std::mem::size_of::<SCROLLINFO>() as u32;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (total - 1).max(0);
    si.nPage = page as u32;
    si.nPos = top;
    unsafe {
        SetScrollInfo(h, SB_VERT as i32, &si, 1);
    }
}

fn size_text(n: u64) -> String {
    if n < 1024 {
        format!("{} B", n)
    } else if n < 1024 * 1024 {
        format!("{:.1} KB", n as f64 / 1024.0)
    } else if n < 1024 * 1024 * 1024 {
        format!("{:.1} MB", n as f64 / 1024.0 / 1024.0)
    } else {
        format!("{:.2} GB", n as f64 / 1024.0 / 1024.0 / 1024.0)
    }
}

fn ext_raw(name: &str) -> String {
    match name.rfind('.') {
        Some(d) if d > 0 && d + 1 < name.len() => name[d + 1..].to_string(),
        _ => String::new(),
    }
}

fn draw_list(h: HWND, win_dc: HDC) {
    let mut rc: RECT = unsafe { zeroed() };
    unsafe {
        GetClientRect(h, &mut rc);
    };
    let w = rc.right;
    let hgt = rc.bottom;
    // 双缓冲：整幅在内存位图上画完再一次 BitBlt 贴上去。
    // 直接往窗口 DC 上画，外层任何一次全量失效都会看到"白底 → 逐行铺"的过程，
    // 那就是用户报告的"表格不停闪烁"
    let mem = unsafe { CreateCompatibleDC(win_dc) };
    let bmp = if mem.is_null() {
        std::ptr::null_mut()
    } else {
        unsafe { CreateCompatibleBitmap(win_dc, w, hgt) }
    };
    let old_bmp = if bmp.is_null() {
        std::ptr::null_mut()
    } else {
        unsafe { SelectObject(mem, bmp as HGDIOBJ) }
    };
    let dc = if mem.is_null() { win_dc } else { mem };
    let rows = st(|s| {
        s.rows
            .iter()
            .map(|r| {
                (
                    r.name.clone(),
                    r.new_name.clone(),
                    r.size,
                    r.status,
                    r.checked,
                    r.done,
                    !r.manual.is_empty(),
                )
            })
            .collect::<Vec<_>>()
    });
    let vis = visible_rows();
    let top = st(|s| s.top);
    let sel = st(|s| s.sel);
    let hot_row = st(|s| s.hot_row);
    let edges = col_edges(w, &st(|s| s.user_w));
    let font = st(|s| s.font);
    let hf = st(|s| s.font_bold);

    fill_rc(dc, 0, 0, w, rc.bottom, ui::C_SURFACE);

    // 表头：浅底、弱化字、加粗；列分隔只在表头里画，且和底色同色
    let hh = sc(ui::HEADER_H);
    fill_rc(dc, 0, 0, w, hh, ui::C_SURFACE_SUBTLE);
    let heads = ["序号", "文件名", "新文件名", "扩展名", "大小", "状态"];
    for (i, t) in heads.iter().enumerate() {
        let left = if i == 0 { 0 } else { edges[i - 1] };
        let right = if i >= 5 { w } else { edges[i] };
        let mut tr = RECT {
            left: left + sc(5),
            top: 0,
            right: right - sc(4),
            bottom: hh,
        };
        draw_str(dc, t, &mut tr, ui::C_TEXT_MUTED, hf, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }
    fill_rc(dc, 0, hh - 1, w, hh, ui::C_BORDER);

    // 列表为空：表头下方居中给一句拖入提示
    if vis.is_empty() {
        let mut tr = RECT { left: 0, top: hh, right: w, bottom: rc.bottom };
        draw_str(dc, "把文件或文件夹拖到这里", &mut tr, ui::C_TEXT_MUTED, font,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    let row_h = sc(ui::ROW_H);
    for (vi, ri) in vis.iter().enumerate().skip(top as usize) {
        let y = hh + (vi as i32 - top) * row_h;
        if y >= rc.bottom {
            break;
        }
        let idx = *ri as i32;
        let (name, new_name, size, status, checked, done, manual) = rows[*ri].clone();
        if idx == sel {
            fill_rc(dc, 0, y, w, y + row_h, ui::C_ROW_HOVER);
        } else if idx == hot_row {
            fill_rc(dc, 0, y, w, y + row_h, ui::C_SURFACE_PAGE);
        }
        // 序号列：勾选框 + 右对齐行号
        let bs = sc(ui::CHECK_BOX);
        draw_box(dc, sc(6), y + (row_h - bs) / 2, bs, checked, false, true);
        let mut tr = RECT { left: sc(COL_CHECK), top: y, right: edges[0] - sc(4), bottom: y + row_h };
        draw_str(dc, &format!("{}", vi + 1), &mut tr, ui::C_TEXT_MUTED, font,
            DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        // 文件名
        let mut tr = RECT { left: edges[0] + sc(5), top: y, right: edges[1] - sc(4), bottom: y + row_h };
        draw_str(dc, &name, &mut tr, ui::C_TEXT, font, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        // 新文件名：手工改过或刚执行成功的标红
        let nc = if done || manual { ui::color::WARNING } else { ui::C_TEXT };
        let mut tr = RECT { left: edges[1] + sc(5), top: y, right: edges[2] - sc(4), bottom: y + row_h };
        draw_str(dc, &new_name, &mut tr, nc, font, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        // 扩展名（带前导点）
        let ext = ext_raw(&new_name);
        let ext = if ext.is_empty() { String::new() } else { format!(".{}", ext) };
        let mut tr = RECT { left: edges[2] + sc(5), top: y, right: edges[3] - sc(4), bottom: y + row_h };
        draw_str(dc, &ext, &mut tr, ui::C_TEXT_MUTED, font, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        // 大小
        let mut tr = RECT { left: edges[3] + sc(4), top: y, right: edges[4] - sc(4), bottom: y + row_h };
        draw_str(dc, &size_text(size), &mut tr, ui::C_TEXT_MUTED, font, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        // 状态：只说“成了没有”
        let (st_txt, st_col) = match status {
            Stat::Done => ("成功", ui::C_SUCCESS),
            Stat::Fail => ("失败", ui::color::WARNING),
            _ => ("", ui::C_TEXT_MUTED),
        };
        if !st_txt.is_empty() {
            let mut tr = RECT { left: edges[4] + sc(5), top: y, right: w - sc(4), bottom: y + row_h };
            draw_str(dc, st_txt, &mut tr, st_col, font, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        if y + row_h < rc.bottom {
            fill_rc(dc, 0, y + row_h - 1, w, y + row_h, ui::C_SURFACE_SUBTLE);
        }
    }
    frame(dc, &RECT { left: 0, top: 0, right: w, bottom: rc.bottom }, ui::C_BORDER);

    if !mem.is_null() {
        unsafe {
            BitBlt(win_dc, 0, 0, w, hgt, mem, 0, 0, SRCCOPY);
            if !old_bmp.is_null() {
                SelectObject(mem, old_bmp);
            }
            if !bmp.is_null() {
                DeleteObject(bmp as HGDIOBJ);
            }
            DeleteDC(mem);
        }
    }
}

fn col_at(w: i32, x: i32, user: &[i32; 6]) -> usize {
    let e = col_edges(w, user);
    for (i, r) in e.iter().enumerate() {
        if x < *r {
            return i;
        }
    }
    5
}

fn row_at(_h: HWND, y: i32) -> Option<usize> {
    let hh = sc(ui::HEADER_H);
    if y < hh {
        return None;
    }
    let vi = (y - hh) / sc(ui::ROW_H) + st(|s| s.top);
    visible_rows().get(vi as usize).copied()
}

unsafe extern "system" fn list_proc(h: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_CREATE => {
            st_mut(|s| s.list = h);
            0
        }
        WM_ERASEBKGND => 1,
        WM_PAINT => {
            let mut ps: PAINTSTRUCT = zeroed();
            let dc = BeginPaint(h, &mut ps);
            draw_list(h, dc);
            EndPaint(h, &ps);
            0
        }
        WM_SIZE => {
            update_scroll(h);
            0
        }
        WM_VSCROLL => {
            let total = visible_rows().len() as i32;
            let page = rows_view(h);
            // 取 lParam 低 16 位在长列表上会跳回
            let mut si: SCROLLINFO = zeroed();
            si.cbSize = std::mem::size_of::<SCROLLINFO>() as u32;
            si.fMask = SIF_ALL;
            GetScrollInfo(h, SB_VERT as i32, &mut si);
            let old = st(|s| s.top);
            let mut top = old;
            match (wp & 0xFFFF) as isize {
                SB_LINEUP => top -= 1,
                SB_LINEDOWN => top += 1,
                SB_PAGEUP => top -= page,
                SB_PAGEDOWN => top += page,
                SB_THUMBPOSITION | SB_THUMBTRACK => top = si.nTrackPos,
                SB_TOP => top = 0,
                SB_BOTTOM => top = si.nMax,
                _ => return 0,
            }
            top = top.clamp(0, (total - page).max(0));
            if top == old {
                update_scroll(h);
                return 0;
            }
            st_mut(|s| s.top = top);
            update_scroll(h);
            // `onScroll = rnHideEditor`：行整体错位了，编辑框还钉在老格子上就对不上
            hide_editor();
            InvalidateRect(h, std::ptr::null(), FALSE);
            st_mut(|s| s.hot_row = -1);
            0
        }
        WM_MOUSEWHEEL => {
            // `onScroll = rnHideEditor`：滚一下行就整体错位了
            // 编辑框还钉在老格子上就会跟内容对不上
            hide_editor();
            let total = visible_rows().len() as i32;
            let page = rows_view(h);
            let delta = (wp >> 16) as i16 as i32;
            let mut top = st(|s| s.top) - delta / 120 * 3;
            top = top.clamp(0, (total - page).max(0));
            st_mut(|s| s.top = top);
            update_scroll(h);
            InvalidateRect(h, std::ptr::null(), FALSE);
            0
        }
        WM_MOUSEMOVE => {
            let x = (lp as i32 & 0xFFFF) as i16 as i32;
            let y = ((lp as i32) >> 16) as i16 as i32;
            if st(|s| s.drag_div) >= 0 {
                let w = {
                    let mut rc: RECT = zeroed();
                    GetClientRect(h, &mut rc);
                    rc.right
                };
                let i = st(|s| s.drag_div) as usize;
                if i < 5 {
                    // `dragTo`：只改被拖的这一对相邻列，两列之和不变
                    // 各列合计还是正好铺满，远处的列不会跟着乱跳
                    let dx = x - st(|s| s.drag_x);
                    let pair = st(|s| s.drag_l) + st(|s| s.drag_r);
                    let minl = col_min(i);
                    let minr = col_min(i + 1);
                    let maxl = (pair - minr).min(col_max(i, w));
                    let left = (st(|s| s.drag_l) + dx).clamp(minl, maxl.max(minl));
                    let mut uw = st(|s| s.user_w);
                    uw[i] = left;
                    uw[i + 1] = (pair - left).max(minr);
                    st_mut(|s| s.user_w = uw);
                }
                InvalidateRect(h, std::ptr::null(), FALSE);
                0
            } else {
                let idx = row_at(h, y).map(|i| i as i32).unwrap_or(-1);
                if st(|s| s.hot_row) != idx {
                    st_mut(|s| s.hot_row = idx);
                    InvalidateRect(h, std::ptr::null(), FALSE);
                }
                st_mut(|s| {
                    if !s.track {
                        s.track = true;
                    }
                });
                track_leave(h);
                0
            }
        }
        WM_MOUSELEAVE => {
            st_mut(|s| {
                s.hot_row = -1;
                s.track = false;
            });
            InvalidateRect(h, std::ptr::null(), FALSE);
            0
        }
        WM_LBUTTONDOWN => {
            // 列表是自绘的子窗口，点一下要先拿到键盘焦点，方向键 / 空格 / 回车才落到这里
            SetFocus(h);
            let x = (lp as i32 & 0xFFFF) as i16 as i32;
            let y = ((lp as i32) >> 16) as i16 as i32;
            let user = st(|s| s.user_w);
            let w = {
                let mut rc: RECT = zeroed();
                GetClientRect(h, &mut rc);
                rc.right
            };
            let div = divider_at(w, &user, x, y);
            // 分隔线拖拽：表头那一条横带上的任意一条都能拉。
            // 没有这道 y 判定，点在数据行靠近列线的位置就会变成拖列宽，行也选不中
            if div >= 0 {
                let cw = col_widths(w, &user);
                st_mut(|s| {
                    // 第一次拖：把当前这一套宽度整个"冻"下来。只冻一对是不够的 ——
                    // 没冻的列按"剩余宽度"重算，拖这里会让远处的列也跟着动
                    if s.user_w[0] <= 0 {
                        s.user_w = cw;
                    }
                    s.drag_div = div;
                    s.drag_x = x;
                    s.drag_l = cw[div as usize];
                    s.drag_r = cw[div as usize + 1];
                });
                SetCapture(h);
                // 在 beginDrag 里回调 onResizeColumns = rnHideEditor
                // 一改列宽，编辑框还按老格子摆着，先收起来
                hide_editor();
                return 0;
            }
            hide_editor();
            if let Some(idx) = row_at(h, y) {
                let col = col_at(w, x, &user);
                if col == 0 && x < sc(COL_CHECK) {
                    st_mut(|s| s.rows[idx].checked = !s.rows[idx].checked);
                    st_mut(|s| s.sel = idx as i32);
                    apply_rules();
                } else {
                    st_mut(|s| s.sel = idx as i32);
                    InvalidateRect(h, std::ptr::null(), FALSE);
                }
            } else {
                st_mut(|s| s.sel = -1);
                InvalidateRect(h, std::ptr::null(), FALSE);
            }
            0
        }
        WM_LBUTTONUP => {
            if st(|s| s.drag_div) >= 0 {
                ReleaseCapture();
                st_mut(|s| s.drag_div = -1);
                InvalidateRect(h, std::ptr::null(), FALSE);
            }
            0
        }
        WM_CAPTURECHANGED => {
            // 拖到一半被别处抢了捕获（弹出菜单之类）：收尾，别让列宽跟着乱跑
            if st(|s| s.drag_div) >= 0 {
                st_mut(|s| s.drag_div = -1);
                InvalidateRect(h, std::ptr::null(), FALSE);
            }
            0
        }
        WM_LBUTTONDBLCLK => {
            let y = ((lp as i32) >> 16) as i16 as i32;
            let x = (lp as i32 & 0xFFFF) as i16 as i32;
            let user = st(|s| s.user_w);
            let w = {
                let mut rc: RECT = zeroed();
                GetClientRect(h, &mut rc);
                rc.right
            };
            // 双击任意一条表头分隔线 = 整表列宽回到自动分配，等于"撤销这次调宽"
            if divider_at(w, &user, x, y) >= 0 {
                st_mut(|s| s.user_w = [0; 6]);
                InvalidateRect(h, std::ptr::null(), FALSE);
                return 0;
            }
            let col = col_at(w, x, &user);
            if let Some(idx) = row_at(h, y) {
                // 双击的第二下也要落到"选中这一行"上，否则快点的两次里
                // 第一下选中、第二下进编辑，视觉上却像没选中
                st_mut(|s| s.sel = idx as i32);
                // `onDblClick` 只认 col == 2（新文件名）：别的列双击只是选中
                if col == 2 {
                    begin_edit(idx);
                } else {
                    InvalidateRect(h, std::ptr::null(), FALSE);
                }
            }
            0
        }
        WM_SETCURSOR => {
            // 悬停在表头分隔线上就换成左右箭头，不然用户根本不知道这儿能拖
            if (lp & 0xFFFF) as i32 == HTCLIENT as i32 {
                let mut pt: POINT = zeroed();
                GetCursorPos(&mut pt);
                ScreenToClient(h, &mut pt);
                let w = {
                    let mut rc: RECT = zeroed();
                    GetClientRect(h, &mut rc);
                    rc.right
                };
                if st(|s| s.drag_div) >= 0
                    || divider_at(w, &st(|s| s.user_w), pt.x, pt.y) >= 0
                {
                    SetCursor(LoadCursorW(std::ptr::null_mut(), IDC_SIZEWE));
                    return 1;
                }
            }
            DefWindowProcW(h, msg, wp, lp)
        }
        WM_KEYDOWN => {
            let vis = visible_rows();
            let total = vis.len() as i32;
            if total == 0 {
                return 0;
            }
            // sel 存的是「行下标」，键盘走的是「可见序号」；过滤生效时两者不等，
            // 这里先换算，再在结果上挪动
            let sel_row = st(|s| s.sel);
            let cur = vis
                .iter()
                .position(|i| *i as i32 == sel_row)
                .map(|v| v as i32)
                .unwrap_or(-1);
            let page = rows_view(h);
            match wp as i32 {
                VK_DOWN => select_only(h, (cur + 1).clamp(0, total - 1)),
                VK_UP => select_only(h, if cur <= 0 { 0 } else { cur - 1 }),
                VK_HOME => select_only(h, 0),
                VK_END => select_only(h, total - 1),
                VK_PRIOR => select_only(h, (cur - page).max(0)),
                VK_NEXT => select_only(h, (cur.max(0) + page).min(total - 1)),
                VK_SPACE => {
                    if cur >= 0 {
                        let idx = vis[cur as usize];
                        st_mut(|s| s.rows[idx].checked = !s.rows[idx].checked);
                        apply_rules();
                    }
                }
                VK_RETURN => {
                    if cur >= 0 {
                        begin_edit(vis[cur as usize]);
                    }
                }
                _ => {}
            }
            0
        }
        WM_GETDLGCODE => (DLGC_WANTARROWS | DLGC_WANTCHARS) as LRESULT,
        _ => DefWindowProcW(h, msg, wp, lp),
    }
}

fn select_only(h: HWND, vis_idx: i32) {
    if let Some(idx) = visible_rows().get(vis_idx as usize).copied() {
        st_mut(|s| s.sel = idx as i32);
        let top = st(|s| s.top);
        let page = rows_view(h);
        if vis_idx < top {
            st_mut(|s| s.top = vis_idx);
        } else if vis_idx >= top + page {
            st_mut(|s| s.top = vis_idx - page + 1);
        }
        update_scroll(h);
        unsafe {
            InvalidateRect(h, std::ptr::null(), FALSE);
        }
    }
}

/// 只收起编辑框（不动里面的内容、不提交）
fn hide_editor_raw() {
    let e = st(|s| s.edit);
    if !e.is_null() {
        unsafe {
            ShowWindow(e, SW_HIDE);
        }
    }
    st_mut(|s| s.edit_row = -1);
}

/// `rnHideEditor`：正在编辑时被滚动 / 切页 / 输入打断 → 先提交
/// 用户的输入不该悄悄丢掉（直接藏框，敲了一半的名字说没就没）
fn hide_editor() {
    if st(|s| s.edit_row) >= 0 {
        commit_edit();
    } else {
        hide_editor_raw();
    }
}

fn begin_edit(row: usize) {
    let l = st(|s| s.list);
    let p = st(|s| s.parent);
    let e = st(|s| s.edit);
    if l.is_null() || p.is_null() || e.is_null() {
        return;
    }
    let top = st(|s| s.top);
    let vis = visible_rows()
        .iter()
        .position(|i| *i == row)
        .map(|v| v as i32 - top)
        .unwrap_or(-1);
    if vis < 0 {
        return;
    }
    let rc = cell_rect(l, vis, 2);
    let mut pts = [
        POINT { x: rc.left, y: rc.top },
        POINT { x: rc.right, y: rc.bottom },
    ];
    // cellRect 给的是列表客户区坐标；编辑框是主窗口的子窗口，
    // SetWindowPos 要的是主窗口客户区坐标 —— 必须换到父窗口，不能换屏幕坐标
    unsafe {
        MapWindowPoints(l, p, pts.as_mut_ptr(), 2);
    }
    let new_name = st(|s| s.rows[row].new_name.clone());
    // 进编辑时框里只放主名，扩展名不进去：一进去就是全选，退格一下清空重打，
    // 也不会顺手把 .png 删掉；列表那格照旧显示带扩展名的完整名字
    let text = base_of_name(&new_name).to_string();
    let hh = (pts[1].y - pts[0].y - sc(2)).max(sc(16));
    // 先把"正在编辑哪一行"落下来，再 SetFocus：SetFocus 会让上一个焦点窗口
    // 走一遍 WM_KILLFOCUS，若这时 edit_row 还是旧值（或 -1），
    // 就会把刚摆好的框当成"该提交了"直接收掉 —— 表现就是双击一闪就没
    st_mut(|s| {
        s.edit_row = row as i32;
        s.sel = row as i32;
    });
    unsafe {
        SetWindowPos(
            e,
            HWND_TOP,
            pts[0].x - sc(1),
            pts[0].y,
            pts[1].x - pts[0].x + sc(2),
            hh,
            SWP_SHOWWINDOW | SWP_NOACTIVATE,
        );
        SetWindowTextW(e, wide(&text).as_ptr());
        SetFocus(e);
        // 列表带了 WS_CLIPSIBLINGS：这一行的待重画不会盖到编辑框上，全选也稳得住
        SendMessageW(e, EM_SETSEL as UINT, 0 as WPARAM, -1 as LPARAM);
    }
    unsafe {
        InvalidateRect(l, std::ptr::null(), FALSE);
    }
}

/// `rnCommitEdit`：框里只有主名，落定时接回这一行原有的扩展名
/// 用户自己把扩展名也打进去了（比如真想要 a.tar.gz）就不再接一遍
fn commit_edit() {
    let row = st(|s| s.edit_row);
    if row < 0 {
        return;
    }
    let e = st(|s| s.edit);
    let t = ctl_text(e).trim().to_string();
    // 先收框再判：不合法的输入丢掉这次输入，列表上还留着原来那行
    hide_editor_raw();
    if row as usize >= st(|s| s.rows.len()) {
        return;
    }
    if t.is_empty() || !valid_name(&t) {
        toast("文件名不合法");
        refresh();
        return;
    }
    let mut full = t;
    let (new_name, ext) = st(|s| {
        let r = &s.rows[row as usize];
        (r.new_name.clone(), ext_of_name(&r.new_name).to_string())
    });
    if !ext.is_empty() {
        let tail = format!(".{}", ext);
        // 不能按字节长度截取 full：中文主名的字节边界可能落在字符中间（panic）
        let typed_tail = lower_all(&full).ends_with(&lower_all(&tail));
        if !typed_tail {
            full.push_str(&tail);
        }
    }
    // 名字跟原来一模一样（点进来又原样确认）：不算"改过名"，
    // 否则这一行会一直挂着红色，让人以为动过手
    if full != new_name {
        st_mut(|s| s.rows[row as usize].manual = full);
    }
    apply_rules();
}

fn cancel_edit() {
    hide_editor_raw();
    refresh();
}

unsafe extern "system" fn edit_proc(h: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    let old = old_proc(h);
    match msg {
        WM_GETDLGCODE => return DLGC_WANTALLKEYS as LRESULT,
        WM_KEYDOWN => {
            if wp == VK_RETURN as usize {
                commit_edit();
                return 0;
            }
            if wp == VK_ESCAPE as usize {
                cancel_edit();
                return 0;
            }
        }
        WM_CHAR => {
            if wp == 13 || wp == 27 {
                return 0;
            }
        }
        WM_KILLFOCUS => {
            if st(|s| s.edit_row) >= 0 {
                commit_edit();
                return 0;
            }
        }
        _ => {}
    }
    if old == 0 {
        return DefWindowProcW(h, msg, wp, lp);
    }
    let f: WNDPROC = std::mem::transmute(old);
    CallWindowProcW(f, h, msg, wp, lp)
}

const COMBO_TIMER: usize = 0x5251;

fn combo_stretch(h: HWND) {
    let n = unsafe { SendMessageW(h, CB_GETCOUNT, 0, 0) as i32 };
    let items = n.clamp(1, 8);
    let mut rc: RECT = unsafe { zeroed() };
    unsafe {
        GetWindowRect(h, &mut rc);
    }
    let w = rc.right - rc.left;
    unsafe {
        SetWindowPos(
            h,
            std::ptr::null_mut(),
            0,
            0,
            w,
            sc(ui::CONTROL_HEIGHT) + sc(22) * items,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE,
        );
        SetTimer(h, COMBO_TIMER, 150, None);
    }
}

fn combo_shrink(h: HWND) {
    let mut rc: RECT = unsafe { zeroed() };
    unsafe {
        GetWindowRect(h, &mut rc);
    }
    let w = rc.right - rc.left;
    unsafe {
        SetWindowPos(
            h,
            std::ptr::null_mut(),
            0,
            0,
            w,
            sc(ui::CONTROL_HEIGHT),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE,
        );
    }
}

unsafe extern "system" fn combo_proc(h: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_LBUTTONDOWN | WM_LBUTTONDBLCLK => combo_stretch(h),
        WM_KEYDOWN | WM_SYSKEYDOWN => {
            let vk = wp as i32;
            let alt_down = GetKeyState(VK_MENU) as u16 & 0x8000 != 0;
            if vk == VK_F4 || (vk == VK_DOWN && alt_down) {
                combo_stretch(h);
            }
        }
        WM_TIMER => {
            if wp == COMBO_TIMER {
                if SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) == 0 {
                    combo_shrink(h);
                    KillTimer(h, COMBO_TIMER);
                }
                return 0;
            }
        }
        WM_SHOWWINDOW => {
            if wp == 0 {
                combo_shrink(h);
            }
        }
        _ => {}
    }
    let old = old_proc(h);
    if old == 0 {
        return DefWindowProcW(h, msg, wp, lp);
    }
    let f: WNDPROC = std::mem::transmute(old);
    CallWindowProcW(f, h, msg, wp, lp)
}

// —————————————————————— 自绘按钮：悬停转发 + 绘制 ——————————————————————
unsafe extern "system" fn hover_proc(h: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_MOUSEMOVE => {
            let id = GetDlgCtrlID(h) as usize;
            let prev = st(|s| s.hot);
            if prev != id {
                let prev_h = st(|s| {
                    s.ctls
                        .iter()
                        .find(|(i, _)| *i == prev)
                        .map(|(_, h)| *h)
                        .unwrap_or(std::ptr::null_mut())
                });
                st_mut(|s| s.hot = id);
                // 上一颗也要重画，否则鼠标移走后它还停在悬停色（多个按钮同时亮）
                if !prev_h.is_null() {
                    InvalidateRect(prev_h, std::ptr::null(), FALSE);
                }
                InvalidateRect(h, std::ptr::null(), FALSE);
            }
            track_leave(h);
        }
        WM_MOUSELEAVE => {
            let id = GetDlgCtrlID(h) as usize;
            if st(|s| s.hot) == id {
                st_mut(|s| s.hot = 0);
            }
            InvalidateRect(h, std::ptr::null(), FALSE);
        }
        _ => {}
    }
    let old = old_proc(h);
    if old == 0 {
        return DefWindowProcW(h, msg, wp, lp);
    }
    let f: WNDPROC = std::mem::transmute(old);
    CallWindowProcW(f, h, msg, wp, lp)
}

fn btn_label(id: usize) -> &'static str {
    match id {
        ID_ADD_FILES => T_TOOL[0].0,
        ID_ADD_FOLDER => T_TOOL[1].0,
        ID_COPY => T_TOOL[2].0,
        ID_CLEAR => T_TOOL[3].0,
        ID_RESET => T_TOOL[4].0,
        ID_UNDO => T_TOOL[5].0,
        ID_RUN => T_TOOL[6].0,
        ID_CHECK_ALL => T_CHECK_ALL,
        ID_INVERT => T_INVERT,
        ID_CASE_UPPER => T_CASE_UPPER,
        ID_CASE_LOWER => T_CASE_LOWER,
        ID_CASE_TITLE => T_CASE_TITLE,
        ID_NUM_ENABLE => T_NUM,
        ID_EXT_UPPER => T_EXT_UPPER,
        ID_EXT_LOWER => T_EXT_LOWER,
        ID_EXT_KEEP => T_EXT_KEEP,
        ID_EXT_DROP => T_EXT_DROP,
        ID_REP_CASE => T_CASE_SENS,
        ID_REP_ALL => T_REP_ALL,
        ID_SEQ_ON => T_SEQ_ON,
        ID_AFFIX_ON => T_AFFIX_ON,
        ID_DEL_SPACE => T_DEL_SPACE,
        ID_DEL_LOWER => T_DEL_LOWER,
        ID_DEL_UPPER => T_DEL_UPPER,
        ID_DEL_DIGIT => T_DEL_DIGIT,
        ID_DEL_CJK => T_DEL_CJK,
        ID_DEL_SPECIAL => T_DEL_SPEC,
        _ => "",
    }
}

/// 的三类按钮：规则勾选钮（框+字）/ 工具条实心钮 / 页底文字链
pub fn on_draw_item(d: *const DRAWITEMSTRUCT) {
    if d.is_null() {
        return;
    }
    let (id, dc, rc, pressed) = unsafe {
        (
            (*d).CtlID as usize,
            (*d).hDC,
            (*d).rcItem,
            (*d).itemState & ODS_SELECTED != 0,
        )
    };
    let hot = st(|s| s.hot) == id;
    let label = btn_label(id);
    let mid = st(|s| s.font_mid);
    let h = rc.bottom - rc.top;

    // 工具条实心钮
    if (ID_ADD_FILES..=ID_RUN).contains(&id) {
        // 「清空」会丢东西，红底；其余蓝底白字
        let base = if id == ID_CLEAR { ui::C_DANGER } else { ui::C_ACCENT_DEEP };
        // ui::dim 的第二个参数是「保留百分比」，常量给的是「压暗百分比」，要互转
        let bg = if pressed {
            ui::dim(base, 100 - ui::PRESS_DIM_PCT)
        } else if hot {
            ui::dim(base, 100 - ui::HOVER_DIM_PCT)
        } else {
            base
        };
        fill(dc, &rc, bg);
        let mut tr = RECT { left: rc.left, top: rc.top, right: rc.right, bottom: rc.bottom };
        draw_str(dc, label, &mut tr, ui::C_SURFACE, mid, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return;
    }
    // 页底文字链：只是两个词，不带按钮底，落在页面底色上；悬停把底色压暗一档
    if id == ID_CHECK_ALL || id == ID_INVERT {
        let bg = if hot {
            ui::dim(ui::C_SURFACE_PAGE, 100 - ui::HOVER_DIM_SOFT)
        } else {
            ui::C_SURFACE_PAGE
        };
        // 头两像素在 里被列表窗口盖着（列表 170..416、按钮 414..438）
        // 这里补画成列表的一格白底 + 底边框，不然列表下边框会被抹掉一段
        let top = RECT { left: rc.left, top: rc.top, right: rc.right, bottom: rc.top + 2 };
        fill(dc, &top, ui::C_SURFACE);
        let edge = RECT { left: rc.left, top: rc.top + 1, right: rc.right, bottom: rc.top + 2 };
        fill(dc, &edge, ui::C_BORDER);
        let inner = RECT { left: rc.left, top: rc.top + 2, right: rc.right, bottom: rc.bottom };
        fill(dc, &inner, bg);
        let mut tr = RECT { left: rc.left, top: rc.top, right: rc.right, bottom: rc.bottom };
        draw_str(dc, label, &mut tr, ui::C_TEXT, mid, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return;
    }
    // 规则勾选钮
    let bg = if hot { ui::C_SURFACE_PAGE } else { ui::C_SURFACE };
    fill(dc, &rc, bg);
    let bs = sc(ui::CHECK_BOX);
    let bx = rc.left + sc(ui::CHECK_INSET);
    let by = rc.top + (h - bs) / 2;
    draw_box(dc, bx, by, bs, ctl_check(id), hot, false);
    let mut tr = RECT {
        left: bx + bs + sc(ui::CHECK_GAP),
        top: rc.top,
        right: rc.right,
        bottom: rc.bottom,
    };
    draw_str(dc, label, &mut tr, ui::C_TEXT, mid, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

fn card(dc: HDC, x: i32, y: i32, w: i32, h: i32, title: Option<&str>) {
    let (l, t, r, b) = (sc(x), sc(y), sc(x + w), sc(y + h));
    fill_rc(dc, l, t, r, b, ui::C_SURFACE);
    frame(dc, &RECT { left: l, top: t, right: r, bottom: b }, ui::C_BORDER);
    if let Some(s) = title {
        // drawRenamePage：在卡片上沿把白底擦出一道缺口，再把标题压上去
        // 字号取正文 18、字重 700，正文色（不是次级灰）。
        let tf = st(|s| s.font_bold);
        let wt = text_w(dc, s, tf);
        let tx = l + sc(12);
        let ty = t - sc(9);
        fill_rc(dc, l + sc(8), ty, l + sc(8) + sc(4) + wt + sc(8), ty + sc(18), ui::C_SURFACE);
        let mut tr = RECT { left: tx, top: ty, right: tx + wt + sc(24), bottom: ty + sc(18) };
        draw_str(dc, s, &mut tr, ui::C_TEXT, tf, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    }
}

fn label(dc: HDC, s: &str, x: i32, row_y: i32) {
    let f = st(|s| s.font_mid);
    let wt = text_w(dc, s, f);
    let y = sc(row_y) + (sc(ui::CONTROL_HEIGHT) - sc(ui::FONT_BODY)) / 2;
    let mut tr = RECT { left: sc(x), top: y, right: sc(x) + wt + sc(2), bottom: y + sc(ui::FONT_BODY) };
    draw_str(dc, s, &mut tr, ui::C_TEXT, f, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
}

pub fn draw(dc: HDC) {
    let page_top = sc(ui::TAB_BAR_BOTTOM);
    fill_rc(dc, 0, page_top, sc(ui::WINDOW_W), sc(geo::PAGE_BOT), ui::C_SURFACE_PAGE);

    card(dc, geo::LIST_X, geo::PANEL_Y, geo::PANEL_W, geo::PANEL_H, None);
    card(dc, geo::G3_X, geo::G3_Y, geo::G3_W, geo::G3_H, Some(T_FILTER));

    // 五枚压线标签：先擦白底（不然卡片上边线会从字缝里穿过去），再写字
    let panel = st(|s| s.panel);
    for (i, t) in T_PANEL.iter().enumerate() {
        let f = st(|s| if panel == i as i32 { s.font_large_bold } else { s.font_large });
        let (tx, tw) = geo::TABS[i];
        let ty = sc(geo::TITLE_Y);
        let wt = text_w(dc, t, f);
        fill_rc(dc, sc(tx) - sc(2), ty, sc(tx) - sc(2) + wt + sc(4), ty + sc(18), ui::C_SURFACE);
        // 绘制矩形要比槽宽更宽：槽宽（44/33/46）只用于点击命中，
        // 「文件名」三个 19px 的字要 57px 才放得下，按槽宽画会被裁掉半个字
        let mut tr = RECT {
            left: sc(tx),
            top: ty,
            right: sc(tx + tw + 24),
            bottom: ty + sc(18),
        };
        draw_str(dc, t, &mut tr, ui::C_TEXT, f, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    }

    // 各面板的行标签
    match panel {
        0 => {
            label(dc, T_CONV_LBL, geo::CONV_LBL_X, geo::ROW1_Y);
            label(dc, T_CONV_TO_LBL, geo::CONV_LBL_X, geo::ROW2_Y);
        }
        1 => {
            label(dc, T_EXT_LBL, geo::EXT_LBL_X, geo::ROW2_Y);
        }
        2 => {
            label(dc, T_FIND_LBL, geo::FIND_LBL_X, geo::ROW2_Y);
            label(dc, T_REPL_LBL, st(|s| s.repl_lbl_x), geo::ROW2_Y);
        }
        3 => {
            label(dc, T_PREFIX_LBL, st(|s| s.pfx_lbl_x), geo::ROW1_Y);
            label(dc, T_SUFFIX_LBL, st(|s| s.suf_lbl_x), geo::ROW1_Y);
            let lb = st(|s| s.seq_lbl);
            label(dc, T_SEQ_START, lb[0], geo::ROW2_Y);
            label(dc, T_SEQ_STEP, lb[2], geo::ROW2_Y);
            label(dc, T_SEQ_DIGITS, lb[4], geo::ROW2_Y);
        }
        4 => {
            label(dc, T_DEL_TEXT, geo::DEL_LBL_X, geo::ROW2_Y);
        }
        _ => {}
    }

    // 页底格式勾选（右边界与命中判定同一口径）
    let all = st(|s| s.fmt_all.clone());
    let on = st(|s| s.fmt_on.clone());
    let bs = sc(ui::CHECK_BOX);
    let by = sc(geo::BOT_Y) + (sc(geo::BOT_H) - bs) / 2;
    let mut fx = sc(geo::FMT_X);
    for e in all {
        let right = fx + sc(geo::FMT_W);
        if right > sc(geo::FMT_END) {
            break;
        }
        let checked = on.is_empty() || on.iter().any(|x| x == &e);
        draw_box(dc, fx, by, bs, checked, false, false);
        let f = st(|s| s.font);
        let mut tr = RECT {
            left: fx + bs + sc(4),
            top: sc(geo::BOT_Y),
            right: fx + sc(geo::FMT_W),
            bottom: sc(geo::BOT_Y + geo::BOT_H),
        };
        draw_str(dc, &format!(".{}", e), &mut tr, ui::C_TEXT, f, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        fx += sc(geo::FMT_PITCH);
    }

    // 右下角文件计数： 只在有文件时出现
    let n = st(|s| s.rows.len());
    if n > 0 {
        let mut tr = RECT {
            left: sc(ui::WINDOW_W - ui::PAGE_MARGIN - 56),
            top: sc(geo::BOT_Y),
            right: sc(ui::WINDOW_W - ui::PAGE_MARGIN),
            bottom: sc(geo::BOT_Y + geo::BOT_H),
        };
        draw_str(dc, &format!("{} 个", n), &mut tr, ui::C_TEXT_MUTED, st(|s| s.font), DT_SINGLELINE | DT_RIGHT | DT_VCENTER);
    }
}

/// 页面上的自绘命中：压线标签 + 页底格式框（按钮走 WM_COMMAND）
pub fn on_lbutton_down(x: i32, y: i32) {
    let p = tab_hit(x, y);
    if p >= 0 {
        set_panel(p);
        return;
    }
    if let Some(e) = fmt_hit(x, y) {
        toggle_fmt(&e);
    }
}
