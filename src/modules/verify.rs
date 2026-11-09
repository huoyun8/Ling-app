// 「文件校验」页：上方一整块结果框；下面一行操作按钮；右下角一列算法勾选；
// 底部"文件 / 总计"两条进度条。
//
// 本文件负责「文件校验」页的：坐标、尺寸、
// 颜色、字号、绘制顺序、命中判定（96dpi 基准，用 sc 等比放大）
//
// 结果框是自绘的，不用多行只读 EDIT：
// EDIT 在"流式 SetWindowText + 用户滚动"这个组合下会把新行画在旧像素上，越滚越糊。
//
// 记录只留最新两条：新的顶掉旧的，框里永远不会堆成一片。
// 算法与读盘都在 filehash.rs 里，这里只做三件事：摆控件、起工作线程、把结果贴上去。
// 大文件算哈希是秒级到分钟级的活，放在 UI 线程里转会让整个窗口假死（连"停止"都点不动）。

use std::sync::atomic::{AtomicBool, AtomicI32, AtomicIsize, Ordering};
use std::sync::{Arc, Mutex};

use winapi::shared::minwindef::{LPARAM, LRESULT, UINT, WPARAM};
use winapi::shared::windef::{HBRUSH, HDC, HMENU, HWND, RECT, SIZE};
use winapi::um::winuser::{
    BeginPaint, CreateWindowExW, DefWindowProcW, DrawTextW, EndPaint, EnableWindow,
    GetClientRect, GetDC, GetKeyState, GetScrollInfo, GetSystemMetrics, GetWindowLongPtrW,
    InvalidateRect, IsWindow, LoadCursorW, PostMessageW, ReleaseCapture,
    ReleaseDC, SetCapture, SetFocus, SetScrollInfo, SetCursor, SetWindowLongPtrW, SetWindowPos,
    ShowWindow,
    BS_OWNERDRAW, CREATESTRUCTW, DLGC_WANTARROWS, DLGC_WANTCHARS, DT_CALCRECT, DT_END_ELLIPSIS,
    DT_LEFT, DT_NOPREFIX, DT_SINGLELINE, DT_TOP, GET_WHEEL_DELTA_WPARAM, GWLP_USERDATA, GWL_STYLE,
    HTCLIENT, IDC_ARROW, IDC_IBEAM, PAINTSTRUCT,
    SB_BOTTOM, SB_LINEDOWN, SB_LINEUP, SB_PAGEDOWN, SB_PAGEUP, SB_THUMBPOSITION, SB_THUMBTRACK,
    SB_TOP, SB_VERT, SCROLLINFO, SIF_PAGE, SIF_POS, SIF_RANGE, SIF_TRACKPOS, SM_CXVSCROLL,
    SWP_FRAMECHANGED, SWP_NOACTIVATE, SWP_NOMOVE, SWP_NOSIZE, SWP_NOZORDER, SW_HIDE, SW_SHOW,
    VK_CONTROL, VK_ESCAPE, VK_SHIFT, WHEEL_DELTA, WM_CAPTURECHANGED, WM_CHAR, WM_ERASEBKGND,
    WM_GETDLGCODE, WM_KEYDOWN, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MOUSEMOVE, WM_MOUSEWHEEL,
    WM_NCCREATE, WM_NCDESTROY, WM_PAINT, WM_SETCURSOR, WM_SIZE, WM_VSCROLL,
    WS_CHILD, WS_CLIPSIBLINGS, WS_VISIBLE, WS_VSCROLL,
};
use winapi::um::wingdi::{
    CreatePen, CreateSolidBrush, DeleteObject, GetTextExtentPoint32W, GetTextMetricsW,
    IntersectClipRect, LineTo, MoveToEx, PS_SOLID, RestoreDC, SaveDC, SelectObject, SetBkMode,
    SetTextColor, TEXTMETRICW,
    TRANSPARENT,
};
use winapi::um::winbase::{GlobalLock, GlobalSize, GlobalUnlock};

use crate::filehash::{self, FileDigest, HashProgress, HASH_CRC32, HASH_MD5, HASH_SHA256,
                      SHOW_DATE};
// 锁毒化兜底（定义在 main.rs 根模块）
use crate::LockExt;
use crate::{reg_class, PaintBuf};
use crate::ui::layout::vf as vfl;

/// WM_PASTE / WM_PRINTCLIENT：winapi 0.3.9 的 winuser 里没有导出，按 winuser.h 取值
const WM_PASTE_MSG: UINT = 0x0302;
const WM_PRINTCLIENT_MSG: UINT = 0x0318;

// ——————————————————————— 控件 ID ———————————————————————
// 号段 53xx，避免与既有控件（41xx/45xx/52xx）撞号
pub const VF_LIST: usize = 5301;
pub const VF_BROWSE: usize = 5302;
pub const VF_CLEAR: usize = 5303;
pub const VF_COPY: usize = 5304;
pub const VF_SAVE: usize = 5305;
pub const VF_STOP: usize = 5306;
pub const VF_CHK_DATE: usize = 5311;
pub const VF_CHK_MD5: usize = 5312;
/// SHA-256 勾选：占用原 SHA-1 的槽位（四个勾选，SHA-256 顶替 SHA-1）
pub const VF_CHK_SHA256: usize = 5313;
pub const VF_CHK_CRC: usize = 5314;

/// 页面 ID（对应 Rust 侧 PAGE_* 编号）
pub const PAGE_VF: i32 = 5;

/// ——————————————————————— 布局网格（96dpi 基准） ———————————————————————
// 数字的唯一来源在这里：主窗口画进度条、结果框自己排版，两边必须看同一份数字。
//
// 底部这一条带子 321（按钮行顶边）到 421（进度条底边）：
//   - 右边：四个勾选竖排，行距 25 / 行高 20，四行 321..416（行距=行高+5，均匀分布）；
//   - 左边：按钮 321..351，剩 70 高给两条进度条（24 高），
//     top 362 / 397，第二条 397..421 —— 两栏顶边对齐，右栏末行距底边留 5。
// 页面底 431，两栏下面各留 10 的页边。
// 网格数字与配色统一放在 ui 层：ui::layout::vf / ui::color

/// 勾选状态不放在按钮里：自绘（BS_OWNERDRAW）按钮对 BM_SETCHECK 的支持不可靠，
/// 实测勾上再画出来还是空的。状态由窗口自己拿，按钮只当"可点的矩形"用。
/// 顺序固定 0=日期 1=MD5 2=SHA256 3=CRC32
static VF_CHK: [AtomicBool; 4] = [
    AtomicBool::new(true), AtomicBool::new(true),
    AtomicBool::new(true), AtomicBool::new(true),
];

/// 勾选框 ID -> 状态下标；不是勾选返回 -1
pub fn vf_chk_index(id: usize) -> i32 {
    match id {
        VF_CHK_DATE => 0,
        VF_CHK_MD5 => 1,
        VF_CHK_SHA256 => 2,
        VF_CHK_CRC => 3,
        _ => -1,
    }
}

/// 读某个勾选的当前状态
pub fn vf_chk_state(idx: usize) -> bool {
    VF_CHK.get(idx).is_some_and(|b| b.load(Ordering::Relaxed))
}

/// 勾选状态切换
pub fn vf_toggle_chk(id: usize) {
    let i = vf_chk_index(id);
    if i < 0 { return; }
    let i = i as usize;
    VF_CHK[i].store(!VF_CHK[i].load(Ordering::Relaxed), Ordering::Relaxed);
}

/// 当前勾选的算法掩码（日期借用高位当"要不要打印"的开关）
fn vf_algo_mask() -> u32 {
    let mut mask = 0u32;
    if VF_CHK[1].load(Ordering::Relaxed) { mask |= HASH_MD5; }
    if VF_CHK[2].load(Ordering::Relaxed) { mask |= HASH_SHA256; }
    if VF_CHK[3].load(Ordering::Relaxed) { mask |= HASH_CRC32; }
    if VF_CHK[0].load(Ordering::Relaxed) { mask |= SHOW_DATE; }
    mask
}

/// ——————————————————————— 结论状态 ———————————————————————
///   enum VfVerdict { VfNone = 0, VfWaiting = 1, VfFirst = 2, VfSame = 3, VfDiff = 4 }
/// 逐值对应 —— 跨线程状态里存的就是这个序号。
#[derive(Clone, Copy, PartialEq)]
#[repr(i32)]
enum VfVerdict {
    None = 0,
    Waiting = 1,
    First = 2,
    Same = 3,
    Diff = 4,
}

impl VfVerdict {
    /// 序号 → 枚举。越界的值一律当"无结论"，不让脏数据把界面画花
    fn from_i32(v: i32) -> VfVerdict {
        match v {
            1 => VfVerdict::Waiting,
            2 => VfVerdict::First,
            3 => VfVerdict::Same,
            4 => VfVerdict::Diff,
            _ => VfVerdict::None,
        }
    }
}

/// 结论只在框上显颜色，不写字（用户口径：不新增任何提示文案）：
/// 相同 = 整圈绿框，不同 = 整圈红框，中性态（首次 / 校验中 / 已清除）回到浅灰边框。
fn verdict_frame(v: VfVerdict) -> u32 {
    match v {
        VfVerdict::Same => crate::ui::color::SUCCESS,
        VfVerdict::Diff => crate::ui::color::WARNING,
        _ => crate::ui::color::BORDER,
    }
}

/// 一份摘要组：下标 0/1/2 = MD5 / SHA256 / CRC32（与 vfPrevDig 同序）
type Dig3 = [String; 3];

fn dig3() -> Dig3 {
    [String::new(), String::new(), String::new()]
}

/// ——————————————————————— 粘贴对照：从文本里认摘要 ———————————————————————
fn clipboard_text() -> String {
    unsafe {
        if winapi::um::winuser::OpenClipboard(std::ptr::null_mut()) == 0 { return String::new(); }
        let h = winapi::um::winuser::GetClipboardData(13 /*CF_UNICODETEXT*/);
        let mut out = String::new();
        if !h.is_null() {
            // 先问这块内存有多大，再按上限读。剪贴板里的字节数不保证带终止符，
            // 只靠"读到 0 为止"遇到没有 0 的块就是一路裸读越界（0xC0000005）
            let cap = GlobalSize(h) as usize / 2; // UTF-16 单元数
            if cap > 0 {
                let p = GlobalLock(h) as *const u16;
                if !p.is_null() {
                    let mut n = 0usize;
                    while n < cap && n < 1_000_000 && *p.add(n) != 0 { n += 1; }
                    out = String::from_utf16_lossy(std::slice::from_raw_parts(p, n));
                    GlobalUnlock(h);
                }
            }
        }
        winapi::um::winuser::CloseClipboard();
        out
    }
}

/// 一行里所有连续的十六进制串，统一成小写
fn hex_runs(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    for c in text.chars() {
        if c.is_ascii_hexdigit() {
            cur.push(c.to_ascii_lowercase());
        } else if !cur.is_empty() {
            out.push(std::mem::take(&mut cur));
        }
    }
    if !cur.is_empty() { out.push(cur); }
    out
}

/// 从粘贴的文字里认出三种摘要：下标 0/1/2 = MD5 / SHA256 / CRC32。
/// 先按行看标签（"MD5:" "SHA256:" "CRC32:"），认不到再按长度兜底：
/// 32 位当 MD5、64 位当 SHA256、8 位当 CRC32（8 位要求带字母，
/// 否则 "1331712" 这类文件大小会被当成 CRC32）。
fn parse_digests(text: &str) -> Dig3 {
    let mut out = dig3();
    for line in text.split('\n') {
        let low = line.to_lowercase();
        let want = if low.contains("crc32") { 2 }
            else if low.contains("sha256") { 1 }
            else if low.contains("md5") { 0 }
            else { continue };
        let len = match want { 0 => 32usize, 1 => 64, _ => 8 };
        for r in hex_runs(line) {
            if r.len() == len { out[want] = r; break; }
        }
    }
    if !out[0].is_empty() && !out[1].is_empty() && !out[2].is_empty() { return out; }
    for r in hex_runs(text) {
        match r.len() {
            64 if out[1].is_empty() => out[1] = r,
            32 if out[0].is_empty() => out[0] = r,
            8 if out[2].is_empty() && r.chars().any(|c| ('a'..='f').contains(&c)) => out[2] = r,
            _ => {}
        }
    }
    out
}

/// 两轮摘要比较：只比双方都有的算法，缺一项不算不一致；
/// 但共同项必须全部一致才判"相同"——任一算法不同即判"不同"。
/// 摘要都是同一份字节的确定性结果，出现分歧只可能是内容不同或对照值贴错，
/// 按"任一相同即相同"会把最弱的一项当成结论，容易误报"一致"。
fn judge(cur: &Dig3, prev: &Dig3) -> Option<bool> {
    let mut cmp = false;
    for i in 0..3 {
        if cur[i].is_empty() || prev[i].is_empty() { continue; }
        cmp = true;
        if !cur[i].eq_ignore_ascii_case(&prev[i]) { return Some(false); }
    }
    if cmp { Some(true) } else { None }
}

/// ——————————————————————— 跨线程状态 ———————————————————————
struct VfState {
    /// 正在处理的批
    cancel: Arc<AtomicBool>,
    busy: AtomicBool,
    /// 结论序号（VfVerdict）
    verdict: AtomicI32,
    /// 上一轮文件批量指纹（不带文件名、不带路径、不带时间）
    sig: Mutex<String>,
    has_sig: AtomicBool,
    /// 上一份摘要基线（文件算的或粘贴的，谁在后算谁）
    dig: Mutex<Dig3>,
    has_dig: AtomicBool,
    progress: Arc<HashProgress>,
}

impl Default for VfState {
    fn default() -> VfState {
        VfState {
            cancel: Arc::new(AtomicBool::new(false)),
            busy: AtomicBool::new(false),
            verdict: AtomicI32::new(VfVerdict::None as i32),
            sig: Mutex::new(String::new()),
            has_sig: AtomicBool::new(false),
            dig: Mutex::new(dig3()),
            has_dig: AtomicBool::new(false),
            progress: Arc::new(HashProgress::default()),
        }
    }
}

static VF_STATE: Mutex<Option<Arc<VfState>>> = Mutex::new(None);

fn vf_state() -> Arc<VfState> {
    let mut g = VF_STATE.lock_ok();
    if g.is_none() {
        *g = Some(Arc::new(VfState::default()));
    }
    g.as_ref().map(|s| s.clone()).unwrap_or_else(|| Arc::new(VfState::default()))
}

/// 工作线程攒好、等着贴到结果框的文本
static VF_PENDING: Mutex<String> = Mutex::new(String::new());

/// 本轮文件校验的结果：整批指纹 + 最后一份摘要（工作线程写、UI 在 vf_on_done 里读）
struct RunData {
    sig: String,
    dig: Dig3,
    valid: bool,
    /// 本轮成功算出摘要的文件数（用于判断能否跟粘贴的单个对照值比较）
    files: usize,
}
static VF_RUN: Mutex<Option<RunData>> = Mutex::new(None);

/// 还没跑完的这一轮作废后，跑完也别再往空框里补内容
static VF_SUPPRESS: AtomicBool = AtomicBool::new(false);

/// 当前是否正在校验（决定「停止」按钮的红/灰两态）
pub fn vf_busy() -> bool {
    vf_state().busy.load(Ordering::Relaxed)
}

// ——————————————————————— 结果框（自绘控件） ———————————————————————
const K_MAX_RECORDS: usize = 2;

/// 结果框的自绘控件
/// 行高由当前字体实测的 tmHeight 算，每帧整块重绘 + 裁剪，滚动只改一个像素偏移量；
/// 结论不占框内任何地方，只把整圈外框染色（相同绿 / 不同红 / 其余灰）。
struct Rec {
    lines: Vec<String>,
    /// 这一条是不是"用户输入 / 粘贴的对照值"块。
    /// 记录满两条时优先顶掉它，把哈希结果块留在框里。
    input: bool,
}

pub struct VfBox {
    /// 窗口句柄。存 isize 而不是裸指针：HWND 是 *mut HWND__，
    /// 裸指针不满足 Send，而这个结构要跨线程传给工作线程的 Arc
    hwnd: isize,
    records: Vec<Rec>,
    /// 所有记录的行摊平（选中与绘制都以它为准）
    flat: Vec<String>,
    sel_anchor: (usize, usize),
    sel_caret: (usize, usize),
    selecting: bool,
    /// 外框颜色：中性（无结论 / 首次 / 校验中）就是边框那条浅灰，相同绿、不同红
    frame: u32,
    scroll_y: i32,
    /// 末尾那条输入块是否还开着（继续手输就往它后面接字符，不另起一块）
    input_open: bool,
}

impl VfBox {
    fn new() -> VfBox {
        VfBox {
            hwnd: 0,
            records: Vec::new(),
            flat: Vec::new(),
            sel_anchor: (0, 0),
            sel_caret: (0, 0),
            selecting: false,
            frame: crate::ui::color::BORDER,
            scroll_y: 0,
            input_open: false,
        }
    }

    fn sc(&self, v: i32) -> i32 { crate::sc(v) }

    /// 取窗口句柄（存的是 isize，用时转回 HWND）
    fn hwnd(&self) -> HWND { self.hwnd as HWND }

    fn invalidate(&self) {
        unsafe {
            if self.hwnd != 0 { InvalidateRect(self.hwnd(), std::ptr::null_mut(), 0); }
        }
    }

    // —————— 记录 ——————

    /// 记录满两条时先顶掉最旧的一条：优先顶输入块，结果块（哈希值）尽量留在框里
    fn make_room(&mut self) {
        while self.records.len() >= K_MAX_RECORDS {
            if let Some(i) = self.records.iter().position(|r| r.input) {
                self.records.remove(i);
            } else if !self.records.is_empty() {
                self.records.remove(0);
            }
        }
    }

    /// 追加一条记录（每个元素是一行文字）。只保留最新两条，新的顶掉最旧的
    fn push_record(&mut self, mut lines: Vec<String>, input: bool) {
        while lines.last().is_some_and(|l| l.is_empty()) { lines.pop(); }
        if lines.is_empty() { return; }
        self.make_room();
        self.records.push(Rec { lines, input });
        self.input_open = input;
        self.rebuild_flat();
        // 新结果进来看最新那条：直接滚到底
        self.sync_scrollbar(true);
    }

    fn clear(&mut self) {
        self.records.clear();
        self.frame = crate::ui::color::BORDER;
        self.scroll_y = 0;
        self.input_open = false;
        self.rebuild_flat();
        self.sync_scrollbar(false);
    }

    /// 复制 / 另存用：把当前记录按显示顺序拼成纯文本（不含结论）
    fn all_text(&self) -> String {
        let mut out = String::new();
        for (r, rec) in self.records.iter().enumerate() {
            if r > 0 { out.push_str("--------------------------------\r\n"); }
            for line in &rec.lines {
                out.push_str(line);
                out.push_str("\r\n");
            }
        }
        out
    }

    // —————— 框内输入（粘贴 / 手输） ——————
    // 用户要求：哈希值下方可以粘/打对照值，每粘一次就是新的一块，
    // 块与块之间照旧画一条横线 —— 跟"连续拖入两个文件"长得一模一样，
    // 不额外加框、不加提示文字。

    /// 最近一条输入块的纯文本（抠摘要用）
    fn last_input_text(&self) -> String {
        match self.records.iter().rev().find(|r| r.input) {
            Some(rec) => rec.lines.join("\n"),
            None => String::new(),
        }
    }

    /// 粘贴：整段文字原样成为新的一块记录（换行拆行、去掉行尾 \r、末尾空行丢掉）
    fn paste_text(&mut self, text: &str) {
        let mut lines: Vec<String> = Vec::new();
        for l in text.split('\n') {
            lines.push(l.strip_suffix('\r').unwrap_or(l).to_string());
        }
        while lines.last().is_some_and(|l| l.is_empty()) { lines.pop(); }
        if lines.is_empty() { return; }
        self.push_record(lines, true);
        self.eval_input();
        self.invalidate();
    }

    /// 手输一个字符：接在当前这条输入块后面，不另起新记录
    fn type_char(&mut self, s: &str) {
        let open = self.input_open && self.records.last().is_some_and(|r| r.input);
        if !open {
            self.make_room();
            self.records.push(Rec { lines: vec![String::new()], input: true });
            self.input_open = true;
        }
        if let Some(rec) = self.records.last_mut() {
            if let Some(line) = rec.lines.last_mut() { line.push_str(s); }
        }
        self.after_edit();
    }

    /// 手输退格：删掉输入块里最后一个字符
    fn backspace(&mut self) {
        let mut hit = false;
        if let Some(rec) = self.records.last_mut() {
            if rec.input {
                if let Some(line) = rec.lines.last_mut() {
                    if line.pop().is_some() { hit = true; }
                }
                if !hit && rec.lines.len() > 1 { rec.lines.pop(); }
            }
        }
        self.after_edit();
    }

    /// 手输改动之后的统一收尾：空块撤掉、重排、滚到底、重新判结论
    fn after_edit(&mut self) {
        if self.records.last().is_some_and(
            |r| r.input && r.lines.len() == 1 && r.lines[0].is_empty())
        {
            self.records.pop();
            self.input_open = false;
        }
        self.rebuild_flat();
        self.sync_scrollbar(true);
        self.eval_input();
        self.invalidate();
    }

    /// 判定：抠出可比字段 → 跟上一份基线比，结论只体现在整圈外框颜色上。
    /// 认不出摘要就只显示记录，外框不动（与 vfPasteCompare 同口径）
    fn eval_input(&mut self) {
        let text = self.last_input_text();
        if text.trim().is_empty() {
            // 没有任何输入：外框回到中性灰（用户要求：无输入 = 中性灰）
            self.set_frame(crate::ui::color::BORDER);
            return;
        }
        let dig = parse_digests(&text);
        if dig.iter().all(|s| s.is_empty()) { return; }

        let st = vf_state();
        let v = {
            let mut prev = st.dig.lock_ok();
            let v = if st.has_dig.load(Ordering::Relaxed) { judge(&dig, &prev) } else { None };
            // 这一份成为下一次的基线：只覆盖认出来的算法
            for i in 0..3 {
                if !dig[i].is_empty() { prev[i] = dig[i].clone(); }
            }
            v
        };
        st.has_dig.store(true, Ordering::Relaxed);
        // 用户刚给了对照值：接下来校验文件必须跟它比，
        // 不能继续走上一轮的整批指纹，否则粘贴进来的值会被彻底忽略
        st.has_sig.store(false, Ordering::Relaxed);
        *st.sig.lock_ok() = String::new();
        let vv = match v {
            Some(true) => VfVerdict::Same,
            Some(false) => VfVerdict::Diff,
            None => VfVerdict::First,
        };
        st.verdict.store(vv as i32, Ordering::Relaxed);
        self.set_frame(verdict_frame(vv));
    }

    fn set_frame(&mut self, c: u32) {
        if self.frame == c { return; }
        self.frame = c;
        self.invalidate();
    }

    // —————— 文本选中 ——————

    fn rebuild_flat(&mut self) {
        self.flat.clear();
        for rec in &self.records {
            for line in &rec.lines { self.flat.push(line.clone()); }
        }
        // 记录被顶掉后行号会整体平移，越界的端点直接夹回来，别让它指到不存在的行
        if self.flat.len() <= self.sel_anchor.0 { self.sel_anchor = (0, 0); }
        if self.flat.len() <= self.sel_caret.0 { self.sel_caret = (0, 0); }
        if !self.flat.is_empty() {
            let n = self.flat[self.sel_anchor.0].chars().count();
            self.sel_anchor.1 = self.sel_anchor.1.min(n);
            let n = self.flat[self.sel_caret.0].chars().count();
            self.sel_caret.1 = self.sel_caret.1.min(n);
        }
    }

    fn has_selection(&self) -> bool {
        if self.flat.is_empty() { return false; }
        if self.sel_anchor.0 >= self.flat.len() || self.sel_caret.0 >= self.flat.len() { return false; }
        self.sel_anchor != self.sel_caret
    }

    fn ordered_sel(&self) -> ((usize, usize), (usize, usize)) {
        let a = self.sel_anchor;
        let b = self.sel_caret;
        if b.0 < a.0 || (b.0 == a.0 && b.1 < a.1) { (b, a) } else { (a, b) }
    }

    fn clear_selection(&mut self) {
        self.sel_anchor = (0, 0);
        self.sel_caret = (0, 0);
        self.selecting = false;
        self.invalidate();
    }

    fn select_all(&mut self) {
        if self.flat.is_empty() { self.clear_selection(); return; }
        self.sel_anchor = (0, 0);
        let last = self.flat.len() - 1;
        self.sel_caret = (last, self.flat[last].chars().count());
        self.invalidate();
    }

    fn selected_text(&self) -> String {
        if !self.has_selection() { return String::new(); }
        let (a, b) = self.ordered_sel();
        let mut out = String::new();
        for r in a.0..=b.0 {
            let s: Vec<char> = self.flat[r].chars().collect();
            let from = if r == a.0 { a.1 } else { 0 };
            let to = if r == b.0 { b.1 } else { s.len() };
            let from = from.min(s.len());
            let to = to.min(s.len());
            if to > from { out.extend(&s[from..to]); }
            if r < b.0 { out.push_str("\r\n"); }
        }
        out
    }

    fn copy_selection(&self) {
        let text = self.selected_text();
        if text.is_empty() { return; }
        crate::copy_text_clipboard(&text);
    }

    /// 正文左边界（与 paint 里 text_area.left 同一套算法）
    /// 这里统一取 10 —— 绘制在哪，命中就按哪算）
    fn text_left(&self) -> i32 { self.sc(vfl::TEXT_LEFT) }

    /// 行高 = 字身高度 + 1。字体建不出来时退回 sc(19)
    fn line_height(&self) -> i32 {
        unsafe {
            if self.hwnd != 0 {
                let f = box_font(self.sc(crate::ui::font::SIZE_15));
                let dc = if f != 0 { GetDC(self.hwnd()) } else { std::ptr::null_mut() };
                if !dc.is_null() {
                    let old = SelectObject(dc, f as *mut winapi::ctypes::c_void);
                    let mut tm: TEXTMETRICW = std::mem::zeroed();
                    let ok = GetTextMetricsW(dc, &mut tm) != 0;
                    SelectObject(dc, old);
                    ReleaseDC(self.hwnd(), dc);
                    // 行距 = 字身高度 + 1：多给一点，上一行的下缘不会压到下一行；
                    // 给太多两条记录就装不进框里了（271 高的框 ≈ 13 行）
                    if ok && tm.tmHeight > 0 {
                        return tm.tmHeight + self.sc(crate::ui::metric::STROKE);
                    }
                }
            }
            self.sc(crate::ui::font::SIZE_19)
        }
    }

    fn view_height(&self) -> i32 {
        unsafe {
            let mut rc: RECT = std::mem::zeroed();
            if self.hwnd == 0 || GetClientRect(self.hwnd(), &mut rc) == 0 { return 0; }
            rc.bottom - rc.top - self.sc(vfl::VIEW_PAD) // 上下各留 6
        }
    }

    fn content_height(&self) -> i32 {
        let lh = self.line_height();
        let mut h = self.sc(vfl::CONTENT_PAD);
        for (r, rec) in self.records.iter().enumerate() {
            h += rec.lines.len() as i32 * lh;
            if r + 1 < self.records.len() { h += lh; } // 记录之间的分隔线占一行高
        }
        h + self.sc(vfl::CONTENT_PAD)
    }

    /// 文字列可用的右边界：结论只染外框，整条宽度都归正文；只给系统滚动条让一条
    fn text_right(&self) -> i32 {
        unsafe {
            let mut rc: RECT = std::mem::zeroed();
            if self.hwnd == 0 || GetClientRect(self.hwnd(), &mut rc) == 0 { return 0; }
            // 结论边框最粗 5px，文字内缩也跟着放到 10，免得首尾字符压在色边上
            let mut right = rc.right - self.sc(vfl::TEXT_LEFT);
            let style = GetWindowLongPtrW(self.hwnd(), GWL_STYLE) as usize;
            if style & (WS_VSCROLL as usize) != 0
                && self.content_height() > self.view_height()
            {
                right -= GetSystemMetrics(SM_CXVSCROLL)
                    - self.sc(crate::ui::metric::SPACE_1);
            }
            right
        }
    }

    fn clamp_scroll(&mut self) {
        let max_scroll = (self.content_height() - self.view_height()).max(0);
        if self.scroll_y > max_scroll { self.scroll_y = max_scroll; }
        if self.scroll_y < 0 { self.scroll_y = 0; }
    }

    /// 滚动条：内容不满一屏就把 WS_VSCROLL 摘掉，否则挂着一条灰槽
    fn sync_scrollbar(&mut self, follow_bottom: bool) {
        unsafe {
            if self.hwnd == 0 { return; }
            let content = self.content_height();
            let view = self.view_height();
            let need_bar = content > view;
            if follow_bottom { self.scroll_y = (content - view).max(0); }
            self.clamp_scroll();

            let mut si: SCROLLINFO = std::mem::zeroed();
            si.cbSize = std::mem::size_of::<SCROLLINFO>() as u32;
            si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            si.nMin = 0;
            si.nMax = (content - 1).max(0);
            si.nPage = view.max(0) as u32;
            si.nPos = self.scroll_y;
            SetScrollInfo(self.hwnd(), SB_VERT as i32, &si, 1);

            let style = GetWindowLongPtrW(self.hwnd(), GWL_STYLE);
            let has = style & (WS_VSCROLL as isize) != 0;
            if need_bar && !has {
                SetWindowLongPtrW(self.hwnd(), GWL_STYLE, style | (WS_VSCROLL as isize));
                SetWindowPos(self.hwnd(), std::ptr::null_mut(), 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            } else if !need_bar && has {
                SetWindowLongPtrW(self.hwnd(), GWL_STYLE, style & !(WS_VSCROLL as isize));
                SetWindowPos(self.hwnd(), std::ptr::null_mut(), 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            }
        }
    }

    /// 扁平行 -> 该行顶边的客户区 y（已含滚动量）。分隔线占的那一行不进结果，
    /// 但会在 y 上留出空档，所以必须和 paint 走同一套累加
    fn row_tops(&self) -> Vec<i32> {
        let mut tops = Vec::new();
        if self.flat.is_empty() { return tops; }
        let lh = self.line_height();
        // 起点和累加方式必须和 paint 完全一样：正文从 top+8 开始，滚动只是整体平移
        let mut y = self.sc(vfl::TEXT_TOP) - self.scroll_y;
        for (r, rec) in self.records.iter().enumerate() {
            for _ in &rec.lines {
                tops.push(y);
                y += lh;
            }
            if r + 1 < self.records.len() { y += lh; }
        }
        tops
    }

    /// 命中测试：找离鼠标最近的字符边界
    fn hit_test(&self, x: i32, y: i32) -> (usize, usize) {
        if self.flat.is_empty() { return (0, 0); }
        let tops = self.row_tops();
        if tops.len() != self.flat.len() { return (0, 0); }
        let lh = self.line_height();
        let mut row = 0usize;
        if y < tops[0] {
            row = 0;
        } else if y >= tops[tops.len() - 1] + lh {
            row = self.flat.len() - 1;
        } else {
            // 落在记录之间那条分隔线的空档里时仍算上一行 —— 拖选跨记录时手感才连续
            for (i, &t) in tops.iter().enumerate() {
                if y >= t {
                    row = i;
                    if y < t + lh { break; }
                }
            }
        }

        // 找离鼠标最近的字符边界；量宽度用正文字体，否则高 DPI 下会偏好几个字符
        let chars: Vec<char> = self.flat[row].chars().collect();
        let mut best = 0usize;
        let mut best_d = i32::MAX;
        unsafe {
            let dc = if self.hwnd != 0 { GetDC(self.hwnd()) } else { std::ptr::null_mut() };
            if dc.is_null() { return (row, 0); }
            let f = box_font(self.sc(crate::ui::font::SIZE_15));
            let old = SelectObject(dc, f as *mut winapi::ctypes::c_void);
            for c in 0..=chars.len() {
                let cx = self.text_left() + prefix_w(dc, &chars, c);
                let d = if cx > x { cx - x } else { x - cx };
                if d < best_d { best_d = d; best = c; }
            }
            SelectObject(dc, old);
            ReleaseDC(self.hwnd(), dc);
        }
        (row, best)
    }

    // —————— 绘制 ——————

    fn paint(&self, dc: HDC) {
        unsafe {
            let mut rc: RECT = std::mem::zeroed();
            GetClientRect(self.hwnd(), &mut rc);

            // 双缓冲：一次流式刷新只重绘这一块，直接怼窗口 DC 会闪
            let buf = PaintBuf::new(dc, rc.right, rc.bottom);
            let target = buf.as_ref().map_or(dc, |b| b.dc());

            self.paint_on(target, &rc);

            if let Some(b) = buf.as_ref() { b.present(dc); }
        }
    }

    fn paint_on(&self, target: HDC, rc: &RECT) {
        unsafe {
            // ——— 底色 + 边框 ———
            let white = CreateSolidBrush(crate::ui::color::cref(crate::ui::color::SURFACE));
            winapi::um::winuser::FillRect(target, rc, white as HBRUSH);
            DeleteObject(white as *mut winapi::ctypes::c_void);

            // 结论不外露文字，整圈外框染色就是"隐藏式提醒"：
            // 相同 = 绿框、不同 = 红框；中性态是 1px 浅灰。
            // 有结论时加粗到 5px —— 1px 彩线在小窗口里容易被白底吃掉
            let colored = self.frame != crate::ui::color::BORDER;
            let fw = if colored { self.sc(vfl::FRAME) }
                     else { self.sc(crate::ui::metric::STROKE) };
            let fb = CreateSolidBrush(crate::ui::color::cref(self.frame));
            let strips = [
                RECT { left: rc.left, top: rc.top, right: rc.right, bottom: rc.top + fw },
                RECT { left: rc.left, top: rc.bottom - fw, right: rc.right, bottom: rc.bottom },
                RECT { left: rc.left, top: rc.top, right: rc.left + fw, bottom: rc.bottom },
                RECT { left: rc.right - fw, top: rc.top, right: rc.right, bottom: rc.bottom },
            ];
            for s in strips.iter() {
                winapi::um::winuser::FillRect(target, s, fb as HBRUSH);
            }
            DeleteObject(fb as *mut winapi::ctypes::c_void);

            let lh = self.line_height();
            let right = self.text_right();
            // 正文：整块裁剪到文字区，滚动只改起始 y
            let area = RECT {
                left: rc.left + self.sc(vfl::TEXT_LEFT),
                top: rc.top + self.sc(vfl::TEXT_TOP),
                right,
                bottom: rc.bottom - self.sc(vfl::TEXT_BOTTOM),
            };
            // 窗口太窄（布局还没跑）时不要硬画
            if area.right <= area.left { return; }

            let saved = SaveDC(target);
            IntersectClipRect(target, area.left, area.top, area.right, area.bottom);
            SetBkMode(target, TRANSPARENT as i32);
            SetTextColor(target, crate::ui::color::cref(crate::ui::color::TEXT));

            let f_base = box_font(self.sc(crate::ui::font::SIZE_15));
            let f_small = box_font(self.sc(crate::ui::font::SIZE_13));
            let old_font = SelectObject(target, f_base as *mut winapi::ctypes::c_void);
            let avail = area.right - area.left;

            let mut y = area.top - self.scroll_y;
            // 选区端点先排好序，逐行判断本行落在选区的哪一段。
            // 底色必须画在字形之前 —— DrawText 用 TRANSPARENT 背景，先画字就被盖住了
            let (s0, s1) = self.ordered_sel();
            let show_sel = self.has_selection();
            let mut flat_row = 0usize;

            for (r, rec) in self.records.iter().enumerate() {
                for line in &rec.lines {
                    if y + lh >= area.top && y <= area.bottom {
                        // 一行放不下就先降一档字号：SHA256 那行是 "SHA256: " + 64 位十六进制，
                        // 共 72 个字符，窗口一窄（或 DPI 一变）15 号字就顶格了。
                        // 行高仍按统一值排，只是这行的字形小一号，整行宽度必然收回来；
                        // 降档还放不下才交给 DrawText 省略，不会出现半截值
                        let mut font = f_base;
                        if f_small != 0 && line_w(target, line, f_base) > avail {
                            font = f_small;
                        }
                        SelectObject(target, font as *mut winapi::ctypes::c_void);

                        if show_sel && flat_row >= s0.0 && flat_row <= s1.0 {
                            let chars: Vec<char> = line.chars().collect();
                            let a = if flat_row == s0.0 { s0.1.min(chars.len()) } else { 0 };
                            let b = if flat_row == s1.0 { s1.1.min(chars.len()) } else { chars.len() };
                            if b > a {
                                // 此刻 target 上选着这一行实际用的字体，量出来的和字形宽度一致
                                let x0 = area.left + prefix_w(target, &chars, a);
                                let x1 = area.left + prefix_w(target, &chars, b);
                                let hr = RECT { left: x0, top: y, right: x1, bottom: y + lh };
                                let sb = CreateSolidBrush(crate::ui::color::cref(crate::ui::color::SELECTION_SOFT));
                                winapi::um::winuser::FillRect(target, &hr, sb as HBRUSH);
                                DeleteObject(sb as *mut winapi::ctypes::c_void);
                            }
                        }
                        let mut lr = RECT { left: area.left, top: y, right: area.right, bottom: y + lh };
                        DrawTextW(target, crate::wide(line).as_ptr(), -1, &mut lr,
                            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                        SelectObject(target, f_base as *mut winapi::ctypes::c_void);
                    }
                    y += lh;
                    flat_row += 1;
                }
                if r + 1 < self.records.len() {
                    // 记录之间画一条细横线，而不是再打一串减号：
                    // 减号在等宽字体里是实线，看着像结果的一部分
                    if y + lh >= area.top && y <= area.bottom {
                        let pen = CreatePen(PS_SOLID as i32, crate::ui::metric::STROKE,
                            crate::ui::color::cref(crate::ui::color::DIVIDER_DEEP));
                        let op = SelectObject(target, pen as *mut winapi::ctypes::c_void);
                        let ly = y + lh / 2;
                        MoveToEx(target, area.left, ly, std::ptr::null_mut());
                        LineTo(target, area.right - self.sc(vfl::SEP_RIGHT_INSET), ly);
                        SelectObject(target, op);
                        DeleteObject(pen as *mut winapi::ctypes::c_void);
                    }
                    y += lh;
                }
            }

            SelectObject(target, old_font);
            RestoreDC(target, saved);
        }
    }
}

/// 结果框正文用的等宽字体（Consolas），fontOf 的 lfHeight 是**正**值
/// （字符单元高），所以这里同样传正高度。
///
/// 等宽字体句柄统一走主缓存表 `crate::box_font`（与界面字体同一张表），
/// 退出时随 WM_DESTROY 一并归还，不会留下没人管的句柄。
fn box_font(px: i32) -> usize {
    crate::box_font(px, 400)
}

/// 用与绘制完全同一条 DrawTextW（DT_CALCRECT）量整行宽度。
/// 不能用 GetTextExtentPoint32W 量，它比 DrawText 实际画出来宽一截，
/// 结果明明右边还空着一大片就把字缩了一级。
fn line_w(dc: HDC, text: &str, font: usize) -> i32 {
    unsafe {
        let old = SelectObject(dc, font as *mut winapi::ctypes::c_void);
        let mut lr = RECT { left: 0, top: 0, right: 0, bottom: 0 };
        DrawTextW(dc, crate::wide(text).as_ptr(), -1, &mut lr,
            DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_CALCRECT);
        SelectObject(dc, old);
        lr.right - lr.left
    }
}

/// 一行里第 n 个字符相对行首的偏移（第 0 个是 0）。选区端点必须按当前字体量，
/// 否则选区的边和字形对不齐。
fn prefix_w(dc: HDC, chars: &[char], n: usize) -> i32 {
    unsafe {
        if n == 0 || dc.is_null() { return 0; }
        let n = n.min(chars.len());
        let s: String = chars[..n].iter().collect();
        let w = crate::wide(&s);
        let mut sz: SIZE = std::mem::zeroed();
        if GetTextExtentPoint32W(dc, w.as_ptr(), (w.len() - 1) as i32, &mut sz) == 0 { 0 } else { sz.cx }
    }
}

/// 结果框实例指针（窗口私有）。
///
/// 不能用 `Arc<Mutex<VfBox>>`：窗口过程持锁期间 Windows 同步重入
/// （WM_SIZE → SetScrollInfo/SetWindowPos → 再进 WM_SIZE / WM_VSCROLL）
/// 就会自死锁；而且任何一处 panic 都会把锁毒化。因此用窗口私有裸指针：
/// 整个 UI 侧单线程、单引用地访问，没有锁。
static VF_BOX_PTR: AtomicIsize = AtomicIsize::new(0);

/// 结果框的窗口过程。
extern "system" fn vf_box_proc(hwnd: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    unsafe {
        if msg == WM_NCCREATE {
            let cs = &*(lp as *const CREATESTRUCTW);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, cs.lpCreateParams as isize);
        }
        let p = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if p == 0 { return DefWindowProcW(hwnd, msg, wp, lp); }
        let b = &mut *(p as *mut VfBox);

        match msg {
            WM_ERASEBKGND => return 1,
            WM_PAINT => {
                let mut ps: PAINTSTRUCT = std::mem::zeroed();
                let dc = BeginPaint(hwnd, &mut ps);
                if !dc.is_null() {
                    b.paint(dc);
                    EndPaint(hwnd, &ps);
                }
                return 0;
            }
            // 主窗口的 WM_PRINT（PrintWindow / 系统截图）会转成这一条：
            // 自绘控件不认 WM_PRINTCLIENT 的话，抓出来就是一块空白
            WM_PRINTCLIENT_MSG => {
                if wp != 0 { b.paint(wp as HDC); }
                return 0;
            }
            WM_SIZE => {
                b.clamp_scroll();
                b.sync_scrollbar(false);
                InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                return 0;
            }
            // 框内粘贴：值直接显示成新的一块记录，立刻出结论
            WM_PASTE_MSG => {
                let t = clipboard_text();
                b.paste_text(&t);
                return 0;
            }
            WM_MOUSEWHEEL => {
                let delta = GET_WHEEL_DELTA_WPARAM(wp) as i32;
                b.scroll_y -= delta * b.line_height() * 3 / WHEEL_DELTA as i32;
                b.clamp_scroll();
                b.sync_scrollbar(false);
                InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                return 0;
            }
            WM_VSCROLL => {
                let lh = b.line_height();
                let max_scroll = (b.content_height() - b.view_height()).max(0);
                let mut pos = b.scroll_y;
                match (wp & 0xFFFF) as isize {
                    SB_LINEUP => pos -= lh,
                    SB_LINEDOWN => pos += lh,
                    SB_PAGEUP => pos -= b.view_height(),
                    SB_PAGEDOWN => pos += b.view_height(),
                    SB_TOP => pos = 0,
                    SB_BOTTOM => pos = max_scroll,
                    SB_THUMBTRACK | SB_THUMBPOSITION => {
                        let mut si: SCROLLINFO = std::mem::zeroed();
                        si.cbSize = std::mem::size_of::<SCROLLINFO>() as u32;
                        si.fMask = SIF_TRACKPOS;
                        if GetScrollInfo(hwnd, SB_VERT as i32, &mut si) != 0 {
                            pos = si.nTrackPos;
                        }
                    }
                    _ => return 0,
                }
                b.scroll_y = pos.clamp(0, max_scroll);
                b.sync_scrollbar(false);
                InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                return 0;
            }
            WM_LBUTTONDOWN => {
                // 点一下把焦点拿过来，滚轮 / 手输消息才会落到这里
                SetFocus(hwnd);
                let pos = b.hit_test((lp & 0xFFFF) as i16 as i32, ((lp >> 16) & 0xFFFF) as i16 as i32);
                // Shift + 点击从原锚点续选，不按 Shift 就在点击处另起一段
                let shift = ((GetKeyState(VK_SHIFT) as u16) & 0x8000) != 0;
                if !shift { b.sel_anchor = pos; }
                b.sel_caret = pos;
                b.selecting = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                return 0;
            }
            WM_MOUSEMOVE => {
                if !b.selecting { return 0; }
                let mx = (lp & 0xFFFF) as i16 as i32;
                let my = ((lp >> 16) & 0xFFFF) as i16 as i32;
                let mut rc: RECT = std::mem::zeroed();
                GetClientRect(hwnd, &mut rc);
                let top = rc.top + b.sc(vfl::CONTENT_PAD);
                let bottom = rc.bottom - b.sc(vfl::CONTENT_PAD);
                // 拖到框外继续自动滚：长结果一屏放不下，不滚就选不到屏幕外的行
                let mut scrolled = false;
                if my < top { b.scroll_y -= b.line_height(); scrolled = true; }
                else if my > bottom { b.scroll_y += b.line_height(); scrolled = true; }
                if scrolled {
                    b.clamp_scroll();
                    b.sync_scrollbar(false);
                }
                let cy = if my < top { top } else if my > bottom - 1 { bottom - 1 } else { my };
                b.sel_caret = b.hit_test(mx, cy);
                InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                return 0;
            }
            WM_LBUTTONUP => {
                if b.selecting {
                    b.selecting = false;
                    ReleaseCapture();
                    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                }
                return 0;
            }
            // 捕获被系统拿走后不要再盯着鼠标，否则选区会跟着光标乱跑
            WM_CAPTURECHANGED => {
                if b.selecting {
                    b.selecting = false;
                    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
                }
                return 0;
            }
            WM_SETCURSOR => {
                if (lp & 0xFFFF) == HTCLIENT {
                    SetCursor(LoadCursorW(std::ptr::null_mut(), IDC_IBEAM));
                    return 1;
                }
            }
            WM_KEYDOWN => {
                let ctrl = ((GetKeyState(VK_CONTROL) as u16) & 0x8000) != 0;
                if ctrl && (wp as u32 == 'C' as u32 || wp as u32 == 'c' as u32) {
                    b.copy_selection();
                    return 0;
                }
                if ctrl && (wp as u32 == 'A' as u32 || wp as u32 == 'a' as u32) {
                    b.select_all();
                    return 0;
                }
                // 焦点在框上时按 Ctrl+V：值直接粘成新的一块记录
                if ctrl && (wp as u32 == 'V' as u32 || wp as u32 == 'v' as u32) {
                    let t = clipboard_text();
                    b.paste_text(&t);
                    return 0;
                }
                if wp as u32 == VK_ESCAPE as u32 {
                    b.clear_selection();
                    return 0;
                }
            }
            // 焦点在框上时直接打字：字符追加到框内的输入块
            WM_CHAR => {
                let ch = wp as u32;
                if ch == 8 /*VK_BACK*/ {
                    b.backspace();
                    return 0;
                }
                if ch >= 32 && ch != 127 {
                    let s = String::from_utf16_lossy(&[ch as u16]);
                    b.type_char(&s);
                    return 0;
                }
            }
            WM_GETDLGCODE => return DLGC_WANTARROWS | DLGC_WANTCHARS,
            WM_NCDESTROY => {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                // 只有"当前指针就是自己"才清：框重建时旧窗口的 NCDESTROY 晚到，
                // 无条件清零会把新框的指针抹掉，之后整个结果框静默失联。
                // 同时把 create 时 Box::leak 的那一份收回，避免每重建一次泄漏一份。
                let me = b as *const VfBox as isize;
                if VF_BOX_PTR.load(Ordering::Relaxed) == me {
                    VF_BOX_PTR.store(0, Ordering::Relaxed);
                    drop(Box::from_raw(b as *mut VfBox));
                }
                return 0;
            }
            _ => {}
        }
        DefWindowProcW(hwnd, msg, wp, lp)
    }
}

/// 建结果框子窗口
fn vf_box_create(parent: HWND, id: usize) -> HWND {
    unsafe {
        // 结果框整个进程只建一个：已经活着就直接复用，避免再新建一份泄漏
        let existing = VF_BOX_PTR.load(Ordering::Relaxed);
        if existing != 0 {
            let old = &*(existing as *const VfBox);
            if old.hwnd != 0 && IsWindow(old.hwnd as HWND) != 0 {
                return old.hwnd as HWND;
            }
            VF_BOX_PTR.store(0, Ordering::Relaxed);
        }
        let hinstance = winapi::um::libloaderapi::GetModuleHandleW(std::ptr::null_mut());
        let cls = crate::wide(crate::CLS_VFBOX);
        reg_class(crate::CLS_VFBOX, Some(vf_box_proc),
            winapi::um::winuser::CS_HREDRAW | winapi::um::winuser::CS_VREDRAW,
            IDC_ARROW as usize, false);

        // VfBox 交给窗口私有（故意泄漏一份：全应用只建这一个结果框，
        // 换来的是指针绝对稳定，窗口过程里可以放心取 &mut）
        let boxed: &'static mut VfBox = Box::leak(Box::new(VfBox::new()));
        let hwnd = CreateWindowExW(0, cls.as_ptr(), crate::wide("").as_ptr(),
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS,
            0, 0, 0, 0, parent, id as HMENU,
            hinstance, boxed as *mut VfBox as *mut winapi::ctypes::c_void);
        if !hwnd.is_null() {
            boxed.hwnd = hwnd as isize;
            VF_BOX_PTR.store(boxed as *mut VfBox as isize, Ordering::Relaxed);
        }
        hwnd
    }
}

/// 取结果框私有状态指针（未创建时为空）
fn vf_box_ptr() -> *mut VfBox {
    VF_BOX_PTR.load(Ordering::Relaxed) as *mut VfBox
}

/// 对结果框做一件事，做完重画一次
fn with_box(f: impl FnOnce(&mut VfBox)) {
    let p = vf_box_ptr();
    if p.is_null() { return; }
    let b = unsafe { &mut *p };
    f(b);
    b.invalidate();
}

fn vf_box_clear() { with_box(|b| b.clear()); }

fn vf_box_push(lines: Vec<String>) { with_box(|b| b.push_record(lines, false)); }

fn vf_box_all_text() -> String {
    let p = vf_box_ptr();
    if p.is_null() { return String::new(); }
    unsafe { (&*p).all_text() }
}

/// 把结论颜色推到结果框外框
fn vf_sync_verdict() {
    let v = VfVerdict::from_i32(vf_state().verdict.load(Ordering::Relaxed));
    with_box(|b| b.set_frame(verdict_frame(v)));
}

// ——————————————————————— 页面装配 ———————————————————————

/// 建文件校验页的控件
pub fn vf_create_page(parent: HWND) {
    unsafe {
        let hinstance = winapi::um::libloaderapi::GetModuleHandleW(std::ptr::null_mut());
        // 结果框：自绘控件，id VF_LIST，切页显隐 / GetDlgItem 都还按这个 id 走
        vf_box_create(parent, VF_LIST);

        for (id, label) in [
            (VF_BROWSE, "浏览…"),
            (VF_CLEAR, "清除"),
            (VF_COPY, "复制"),
            (VF_SAVE, "保存…"),
            (VF_STOP, "停止"),
        ] {
            CreateWindowExW(0, crate::wide("BUTTON").as_ptr(), crate::wide(label).as_ptr(),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 0, 0, parent, id as HMENU,
                hinstance, std::ptr::null_mut());
        }
        // 初始态就是禁用灰：没有进度条在走，停止按了也没有意义
        vf_sync_stop_btn(parent);

        // 四个勾选：自绘（BS_OWNERDRAW）而不是 BS_AUTOCHECKBOX。
        // 主题复选框是"透明背景画字"，自己不擦底，而主窗口是 WS_CLIPCHILDREN +
        // 整幅双缓冲，父窗口也画不到子控件底下去 —— 被别的窗口盖过之后会留旧像素。
        // 自绘每一帧先整块填底色，物理上不可能残留。
        for (id, label) in [
            (VF_CHK_DATE, "日期"),
            (VF_CHK_MD5, "MD5"),
            (VF_CHK_SHA256, "SHA256"),
            (VF_CHK_CRC, "CRC32"),
        ] {
            CreateWindowExW(0, crate::wide("BUTTON").as_ptr(), crate::wide(label).as_ptr(),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 0, 0, parent, id as HMENU,
                hinstance, std::ptr::null_mut());
        }
    }
}

/// 排布文件校验页的控件（坐标relayoutVerify）
pub fn vf_relayout(parent: HWND) {
    unsafe {
        let place = |id: usize, x: i32, y: i32, w: i32, h: i32| {
            let c = winapi::um::winuser::GetDlgItem(parent, id as i32);
            if !c.is_null() {
                SetWindowPos(c, std::ptr::null_mut(),
                    crate::sc(x), crate::sc(y), crate::sc(w), crate::sc(h),
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
        };
        place(VF_LIST, vfl::LIST_X, vfl::LIST_Y, vfl::LIST_W, vfl::LIST_H);
        // 按钮行：浏览 100 / 清除 70 / 复制 70 / 保存 88 / 停止 74
        for (i, id) in [VF_BROWSE, VF_CLEAR, VF_COPY, VF_SAVE, VF_STOP].iter().enumerate() {
            place(*id, vfl::BTN_X[i], vfl::BTN_Y, vfl::BTN_W[i], vfl::BTN_H);
        }
        // 勾选行：四个竖排，跟按钮行同一条顶边（321），行距 25 / 行高 20
        for (i, id) in [VF_CHK_DATE, VF_CHK_MD5, VF_CHK_SHA256, VF_CHK_CRC]
            .iter().enumerate()
        {
            place(*id, vfl::CHK_X, vfl::CHK_Y + vfl::CHK_PITCH * i as i32,
                vfl::CHK_W, vfl::CHK_H);
        }
        // 底部两条进度条是纯绘制的，没有控件要摆
    }
}

/// 切页显隐
pub fn vf_show_page(parent: HWND, visible: bool) {
    unsafe {
        for id in [VF_LIST, VF_BROWSE, VF_CLEAR, VF_COPY, VF_SAVE, VF_STOP,
                   VF_CHK_DATE, VF_CHK_MD5, VF_CHK_SHA256, VF_CHK_CRC] {
            let c = winapi::um::winuser::GetDlgItem(parent, id as i32);
            if !c.is_null() { ShowWindow(c, if visible { SW_SHOW } else { SW_HIDE }); }
        }
        // 切回来时按当前忙闲把停止按钮的两态对齐（在别的页时校验可能已经跑完）
        if visible { vf_sync_stop_btn(parent); }
    }
}

/// 停止按钮的两态：进度条在走（busy）= 红 + 可点；闲着 = 灰 + 禁用。
/// 自绘按钮不会因为状态变化自己重画，必须显式 invalidate 一次
fn vf_sync_stop_btn(parent: HWND) {
    unsafe {
        let btn = winapi::um::winuser::GetDlgItem(parent, VF_STOP as i32);
        if btn.is_null() { return; }
        let busy = vf_busy();
        EnableWindow(btn, if busy { 1 } else { 0 });
        // 只置脏、不同步重画：切页过程会冻结整棵窗口树并统一刷新一次，
        // 这里若用 UpdateWindow 抢着画，会把「其它页还没藏完」的半切换状态写屏。
        // 常规交互里按钮颜色在下一轮 WM_PAINT 就换过来，同样是即时可见。
        InvalidateRect(btn, std::ptr::null_mut(), 0);
    }
}

/// 一个文件的输出块，字段名与 digestLines 一致，便于和别的工具对照
/// 末尾不再留空行：记录之间的分隔线由结果框画
fn digest_lines(d: &FileDigest, mask: u32) -> Vec<String> {
    let mut lines = Vec::new();
    lines.push(format!("文件名: {}", d.name));
    lines.push(format!("大小: {} 字节", filehash::group_digits(d.size)));
    if mask & SHOW_DATE != 0 && !d.modified.is_empty() {
        lines.push(format!("修改日期: {}", d.modified));
    }
    if !d.md5.is_empty() { lines.push(format!("MD5: {}", d.md5)); }
    if !d.sha256.is_empty() { lines.push(format!("SHA256: {}", d.sha256)); }
    if !d.crc32.is_empty() { lines.push(format!("CRC32: {}", d.crc32)); }
    lines
}

fn main_hwnd() -> HWND {
    crate::MAIN_HWND.load(Ordering::Relaxed) as HWND
}

/// 启动一批校验（工作线程）
pub fn vf_start(parent: HWND, files: Vec<String>) {
    unsafe {
        if files.is_empty() { return; }
        let st = vf_state();
        if st.busy.load(Ordering::Relaxed) {
            crate::show_toast("正在校验，请稍候");
            return;
        }
        let mask = vf_algo_mask();
        if mask & (HASH_MD5 | HASH_SHA256 | HASH_CRC32) == 0 {
            crate::show_toast("请至少勾选一种算法");
            return;
        }
        // 勾了算法不等于这台机器跑得起：FIPS 策略下 bcrypt 常把 MD5 直接禁掉。
        // 不在这儿拦，每个文件都会落成一行"读取失败"，用户看不出是算法不可用
        if !filehash::HashCalc::new(mask).usable() {
            crate::show_toast("勾选的算法在本机不可用");
            return;
        }

        st.cancel.store(false, Ordering::Relaxed);
        st.busy.store(true, Ordering::Relaxed);
        // 新的一轮开始：上一轮"离开页面"的作废标记就此解除
        VF_SUPPRESS.store(false, Ordering::Relaxed);
        vf_sync_stop_btn(parent);   // 进度条开始走：停止按钮转红、转为可点
        VF_PENDING.lock_ok().clear();
        // 上一轮的结果留在框里，新一轮的记录进来后把最旧的顶掉（框内上限 2 条）
        st.progress.file_done.store(0, Ordering::Relaxed);
        st.progress.file_total.store(0, Ordering::Relaxed);
        st.progress.all_done.store(0, Ordering::Relaxed);
        st.progress.all_total.store(0, Ordering::Relaxed);
        // 第一次校验根本没有"上一次"可比，连"对比中…"也是假的，干脆整圈不染色。
        // 从第二次起（或已经粘贴过对照值）才提示中性态
        let has_prev = st.has_sig.load(Ordering::Relaxed) || st.has_dig.load(Ordering::Relaxed);
        st.verdict.store(if has_prev { VfVerdict::Waiting as i32 }
                         else { VfVerdict::None as i32 }, Ordering::Relaxed);
        vf_sync_verdict();

        let cancel = st.cancel.clone();
        let prog = st.progress.clone();
        // HWND 是裸指针，不满足 Send：先降成 isize 搬进线程，线程里再转回来
        let target = parent as isize;
        let r = std::thread::Builder::new().name("vf-hash".into()).spawn(move || {
            // 本轮指纹：拿它跟上一轮比，就知道两次结果一不一致。
            // 判据只看"内容"—— 大小 + 这次真算出来的哈希：
            //   · 不带文件名、不带路径：同一份内容改名、换目录、换盘仍然是同一份；
            //   · 不带修改时间：拷贝、解压、从网盘拉回来都会改时间，跟内容无关；
            //   · 只放真正算了的那几种算法：没勾的留空，不参与比较；
            //   · 多个文件不看拖入顺序：拼之前按内容排序。
            let target = target as HWND;
            let mut parts: Vec<String> = Vec::new();
            let mut run_dig = dig3();
            let mut run_valid = false;
            let mut run_files = 0usize;

            // 总计进度得先有分母。这里只读文件元数据，不碰内容，几十个文件也是毫秒级
            prog.all_total.store(filehash::total_bytes(&files), Ordering::Relaxed);

            // 每读一块就 Post 一次会把消息队列灌爆，按时间限流到 ~12fps 足够顺
            let last_post = Mutex::new(std::time::Instant::now() - std::time::Duration::from_secs(1));
            let tick = || {
                let mut lp = last_post.lock_ok();
                if lp.elapsed() >= std::time::Duration::from_millis(80) {
                    *lp = std::time::Instant::now();
                    PostMessageW(target, crate::WM_VF_PROGRESS, 0, 0);
                }
            };

            for f in &files {
                if cancel.load(Ordering::Relaxed) { break; }
                let d = filehash::hash_file(f, mask, &cancel, Some(&prog), Some(&tick));
                let mut chunk = String::new();
                if let Some(d) = d {
                    // 一个文件的"内容身份证"：大小兜底，勾了的算法逐个附上
                    let mut part = d.size.to_string();
                    if !d.md5.is_empty() { part.push('|'); part.push_str(&d.md5); }
                    if !d.sha256.is_empty() { part.push('|'); part.push_str(&d.sha256); }
                    if !d.crc32.is_empty() { part.push('|'); part.push_str(&d.crc32); }
                    parts.push(part);
                    for line in digest_lines(&d, mask) {
                        chunk.push_str(&line);
                        chunk.push_str("\r\n");
                    }
                    // 本轮算出来的摘要：多文件时最后一个算完的赢
                    run_dig = [d.md5, d.sha256, d.crc32];
                    run_valid = true;
                    run_files += 1;
                } else if !cancel.load(Ordering::Relaxed) {
                    // 取消不是失败：点了停止就只在该停的地方停，别往结果里写"读取失败" ——
                    // 真失败也只写四个字：不跟路径、不跟原因
                    chunk = "读取失败\r\n".to_string();
                }
                VF_PENDING.lock_ok().push_str(&chunk);
                PostMessageW(target, crate::WM_VF_PROGRESS, 0, 0);
            }

            // 排序后再拼：同一批文件换个拖入顺序，内容还是那一批
            parts.sort();
            let mut sig = String::new();
            for p in &parts {
                sig.push_str(p);
                sig.push('\n');
            }
            // 跑完就把两条进度条落到 100%：中途读失败的文件会让 all_done 差一点，
            // 停在 97% 会让人以为还没跑完
            if !cancel.load(Ordering::Relaxed) {
                prog.all_done.store(prog.all_total.load(Ordering::Relaxed), Ordering::Relaxed);
                prog.file_done.store(prog.file_total.load(Ordering::Relaxed), Ordering::Relaxed);
            }
            *VF_RUN.lock_ok() = Some(RunData { sig, dig: run_dig, valid: run_valid, files: run_files });
            PostMessageW(target, crate::WM_VF_DONE, 0, 0);
        });
        if r.is_err() {
            // 线程创建失败：复位 busy，否则完成消息永远不来，校验页从此拒绝一切操作
            st.busy.store(false, Ordering::Relaxed);
            vf_sync_stop_btn(parent);
            crate::show_toast("校验启动失败，请重试");
        }
    }
}

/// UI 线程：把工作线程攒好的文本贴到结果框
pub fn vf_on_progress() {
    if VF_SUPPRESS.load(Ordering::Relaxed) {
        // 已经离开校验页：这一批的结果全部丢掉
        VF_PENDING.lock_ok().clear();
        return;
    }
    let delta = std::mem::take(&mut *VF_PENDING.lock_ok());
    // 只在新结果到了才动结果框：进度刷新（12fps）不再反复重画
    if delta.is_empty() { return; }
    // 工作线程交上来的是"若干个文件的完整文本"，按行拆开凑成一条记录
    let mut lines: Vec<String> = delta.split("\r\n").map(|s| s.to_string()).collect();
    while lines.last().is_some_and(|l| l.is_empty()) { lines.pop(); }
    if !lines.is_empty() { vf_box_push(lines); }
}

/// UI 线程：校验结束（对比逻辑跑完）
pub fn vf_on_done() {
    let st = vf_state();
    if VF_SUPPRESS.swap(false, Ordering::Relaxed) {
        // 离开页面时把跑了一半的那轮作废：不收结果、不动基线
        VF_PENDING.lock_ok().clear();
        *VF_RUN.lock_ok() = None;
        st.cancel.store(false, Ordering::Relaxed);
        st.busy.store(false, Ordering::Relaxed);
        vf_sync_stop_btn(main_hwnd());
        return;
    }
    let run = VF_RUN.lock_ok().take();
    let canceled = st.cancel.swap(false, Ordering::Relaxed);
    st.busy.store(false, Ordering::Relaxed);
    vf_sync_stop_btn(main_hwnd());   // 进度条停了：停止按钮回灰、转为禁用

    // 对比：跟"上一次跑完的指纹"比。被取消的这一轮不算数，不参与对比，
    // 否则点一下停止就把历史顶掉了。
    if !canceled {
        if let Some(r) = run {
            let has_result = !r.sig.is_empty() || r.valid;
            if has_result {
                let has_sig = st.has_sig.load(Ordering::Relaxed);
                let has_dig = st.has_dig.load(Ordering::Relaxed);
                let v = if has_sig {
                    // 上一次也是文件校验：按整批指纹比（多文件、拖入顺序都不影响）
                    if r.sig == *st.sig.lock_ok() { VfVerdict::Same } else { VfVerdict::Diff }
                } else if has_dig {
                    // 上一次是粘贴进来的对照值：没有整批指纹，只能按摘要比 ——
                    // "先输入、后文件"就是走这一条。
                    // 但粘贴的对照值是"一个文件的一份摘要"，一批文件跟它没有对应关系：
                    // 拿最后一个文件的摘要去比，10 个里 9 个没比也照样亮绿框。
                    // 这种场合宁可不给结论（中性外框），也不给一个以偏概全的"一致"。
                    if r.files > 1 {
                        VfVerdict::None
                    } else {
                        match judge(&r.dig, &st.dig.lock_ok()) {
                            Some(true) => VfVerdict::Same,
                            Some(false) => VfVerdict::Diff,
                            None => VfVerdict::First,
                        }
                    }
                } else {
                    VfVerdict::First
                };
                // 本轮结果成为下一次的基线：摘要与整批指纹各留一份。
                // 摘要只覆盖本轮真算出来的算法，别把已有的其他算法基线抹掉
                if r.valid {
                    let mut d = st.dig.lock_ok();
                    for i in 0..3 {
                        if !r.dig[i].is_empty() { d[i] = r.dig[i].clone(); }
                    }
                    drop(d);
                    st.has_dig.store(true, Ordering::Relaxed);
                }
                *st.sig.lock_ok() = r.sig;
                st.has_sig.store(true, Ordering::Relaxed);
                st.verdict.store(v as i32, Ordering::Relaxed);
            }
        }
    }
    // 取消或一个文件都没读成功：别把"校验中…"留在那儿冒充结果
    if st.verdict.load(Ordering::Relaxed) == VfVerdict::Waiting as i32 {
        st.verdict.store(VfVerdict::None as i32, Ordering::Relaxed);
    }
    vf_sync_verdict();
    // 完成不弹提示：两条进度条落到 100%、外框颜色已写清楚，浮层只会压住进度条。
    // 只有"被停止"这种非正常结束才需要说一声
    if canceled {
        unsafe { crate::show_toast("用户取消"); }
    }
}

/// 清除：记录、输入、对比基线、结论一起丢掉，下一次校验回到"首次"
pub fn vf_clear(parent: HWND) {
    let st = vf_state();
    if st.busy.load(Ordering::Relaxed) { vf_stop(); }
    VF_PENDING.lock_ok().clear();
    *VF_RUN.lock_ok() = None;
    vf_box_clear();
    *st.sig.lock_ok() = String::new();
    st.has_sig.store(false, Ordering::Relaxed);
    *st.dig.lock_ok() = dig3();
    st.has_dig.store(false, Ordering::Relaxed);
    st.verdict.store(VfVerdict::None as i32, Ordering::Relaxed);
    vf_sync_verdict();
    st.progress.file_done.store(0, Ordering::Relaxed);
    st.progress.file_total.store(0, Ordering::Relaxed);
    st.progress.all_done.store(0, Ordering::Relaxed);
    st.progress.all_total.store(0, Ordering::Relaxed);
    vf_invalidate_bars(parent);
}

/// 离开「文件校验」页：整页回到初始状态（结果 / 输入 / 基线 / 结论 / 进度）。
/// 正在跑的校验直接作废 —— 否则切走再回来会看到半截记录。
pub fn vf_leave(parent: HWND) {
    VF_SUPPRESS.store(true, Ordering::Relaxed);
    VF_PENDING.lock_ok().clear();
    vf_clear(parent);
}

/// 停止：只打标记，工作线程每读一块（1MB）检查一次，所以最多一秒内就停
pub fn vf_stop() {
    vf_state().cancel.store(true, Ordering::Relaxed);
}

/// 重画两条进度条（只刷这一块，不必整窗重绘）
pub fn vf_invalidate_bars(parent: HWND) {
    unsafe {
        let r = RECT {
            left: crate::sc(vfl::BAR_X),
            top: crate::sc(vfl::ROW_FILE),
            right: crate::sc(vfl::BAR_X + vfl::BAR_W),
            bottom: crate::sc(vfl::ROW_TOTAL + vfl::BAR_TOP_OFFSET + vfl::BAR_H),
        };
        InvalidateRect(parent, &r, 0);
    }
}

/// 画两条进度条（父窗口 WM_PAINT 调用）
pub fn vf_draw_bars(dc: HDC) {
    let st = vf_state();
    let pct_of = |done: u64, total: u64| -> f64 {
        if total == 0 { return 0.0; }
        let v = done as f64 * 100.0 / total as f64;
        if v > 100.0 { 100.0 } else { v }
    };
    let p1 = pct_of(st.progress.file_done.load(Ordering::Relaxed),
                    st.progress.file_total.load(Ordering::Relaxed));
    let p2 = pct_of(st.progress.all_done.load(Ordering::Relaxed),
                    st.progress.all_total.load(Ordering::Relaxed));
    let r1 = RECT { left: crate::sc(vfl::BAR_X),
                    top: crate::sc(vfl::ROW_FILE + vfl::BAR_TOP_OFFSET),
                    right: crate::sc(vfl::BAR_X + vfl::BAR_W),
                    bottom: crate::sc(vfl::ROW_FILE + vfl::BAR_TOP_OFFSET + vfl::BAR_H) };
    let r2 = RECT { left: crate::sc(vfl::BAR_X),
                    top: crate::sc(vfl::ROW_TOTAL + vfl::BAR_TOP_OFFSET),
                    right: crate::sc(vfl::BAR_X + vfl::BAR_W),
                    bottom: crate::sc(vfl::ROW_TOTAL + vfl::BAR_TOP_OFFSET + vfl::BAR_H) };
    // 一条进度条里两块信息：左边「文件:」「总计:」标签，正中百分比。
    // 标签贴左边不动、数字居中 —— 两行条子的数字上下对齐成一条竖线
    crate::draw_progress_bar(dc, &r1, p1, "文件: ", &format!("{}%", p1.round() as i64));
    crate::draw_progress_bar(dc, &r2, p2, "总计: ", &format!("{}%", p2.round() as i64));
}

/// 浏览：多选文件对话框
pub fn vf_browse(parent: HWND) {
    unsafe {
        if vf_busy() {
            crate::show_toast("正在校验，请稍候");
            return;
        }
        let mut buf = vec![0u16; 64 * 1024];
        // 过滤串必须是 "描述\0通配\0描述\0通配\0\0" 这种带嵌入 0 的形式。
        // 注意不能用字面量拼接：按 wcslen 走，第一个 \0 就被截断了。
        // crate::wide() 自带结尾 0，所以这里每段只补中间的 0，最后再补一个收尾 0。
        // "描述\0通配\0\0"：wide() 已带每段结尾 0，这里只补最后的界尾
        let mut filter: Vec<u16> = crate::wide("所有文件");
        filter.extend(crate::wide("*.*"));
        filter.push(0);

        // 标题也要活到对话框关掉为止：临时 Vec 的指针不能直接塞进 OPENFILENAME
        let title: Vec<u16> = crate::wide("选择要校验的文件");
        let mut ofn: winapi::um::commdlg::OPENFILENAMEW = std::mem::zeroed();
        ofn.lStructSize = std::mem::size_of::<winapi::um::commdlg::OPENFILENAMEW>() as u32;
        ofn.hwndOwner = parent;
        ofn.lpstrFile = buf.as_mut_ptr();
        ofn.nMaxFile = buf.len() as u32;
        ofn.lpstrFilter = filter.as_ptr();
        ofn.lpstrTitle = title.as_ptr();
        ofn.Flags = winapi::um::commdlg::OFN_FILEMUSTEXIST
            | winapi::um::commdlg::OFN_PATHMUSTEXIST
            | winapi::um::commdlg::OFN_ALLOWMULTISELECT
            | winapi::um::commdlg::OFN_EXPLORER
            | winapi::um::commdlg::OFN_HIDEREADONLY;
        if winapi::um::commdlg::GetOpenFileNameW(&mut ofn) == 0 { return; }

        // 多选时 lpstrFile 是 "目录\0文件1\0文件2\0\0"；单选时整串就是完整路径
        let mut files: Vec<String> = Vec::new();
        let dir_len = buf.iter().position(|&c| c == 0).unwrap_or(0);
        let dir = String::from_utf16_lossy(&buf[..dir_len]);
        let mut p = dir_len + 1;
        if p >= buf.len() || buf[p] == 0 {
            files.push(dir);
        } else {
            let base = format!("{}\\", dir);
            while p < buf.len() && buf[p] != 0 {
                let end = buf[p..].iter().position(|&c| c == 0).map(|i| p + i).unwrap_or(buf.len());
                let name = String::from_utf16_lossy(&buf[p..end]);
                files.push(format!("{}{}", base, name));
                p = end + 1;
            }
        }
        vf_start(parent, files);
    }
}

/// 复制结果到剪贴板
pub fn vf_copy(_parent: HWND) {
    let text = vf_box_all_text();
    if text.is_empty() {
        unsafe { crate::show_toast("还没有校验结果"); }
        return;
    }
    unsafe {
        crate::show_toast(if crate::copy_text_clipboard(&text) {
            "已复制到剪贴板"
        } else {
            "复制失败，剪贴板被其他程序占用"
        });
    }
}

/// 另存为 txt
pub fn vf_save(parent: HWND) {
    let text = vf_box_all_text();
    if text.is_empty() {
        unsafe { crate::show_toast("还没有校验结果"); }
        return;
    }
    unsafe {
        let mut fbuf = [0u16; 1024];
        let def = crate::wide("校验结果.txt");
        let n = def.len() - 1;
        fbuf[..n].copy_from_slice(&def[..n]);
        let filter = crate::wide("文本文件\0*.txt\0所有文件\0*.*\0\0");
        let mut ofn: winapi::um::commdlg::OPENFILENAMEW = std::mem::zeroed();
        ofn.lStructSize = std::mem::size_of::<winapi::um::commdlg::OPENFILENAMEW>() as u32;
        ofn.hwndOwner = parent;
        ofn.lpstrFilter = filter.as_ptr();
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = fbuf.as_mut_ptr();
        ofn.nMaxFile = fbuf.len() as u32;
        ofn.Flags = winapi::um::commdlg::OFN_OVERWRITEPROMPT | winapi::um::commdlg::OFN_PATHMUSTEXIST
            | winapi::um::commdlg::OFN_EXPLORER;
        if winapi::um::commdlg::GetSaveFileNameW(&mut ofn) == 0 { return; }
        let end = fbuf.iter().position(|&c| c == 0).unwrap_or(0);
        let mut path = String::from_utf16_lossy(&fbuf[..end]);
        if !path.to_lowercase().ends_with(".txt") { path.push_str(".txt"); }
        let ok = std::fs::write(&path, text.as_bytes()).is_ok();
        crate::show_toast(if ok { "已保存" } else { "保存失败" });
    }
}
