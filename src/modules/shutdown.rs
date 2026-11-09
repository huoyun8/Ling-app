//
// 版面（96dpi）：标题 56..112；两行设定共用三列网格 154/274/394、格 72×44：
//   指定时刻 140（右侧挂「每天」514..566）
//   倒计时   212
// 三颗按钮 300 起、178×50 @ 14/204/394；进度条 374..414，剩余时间叠在条内。
// 两行只有上限与落盘口径不同；生效模式只靠两行强弱与行首 2px 竖标记区分。
//
// 只碰本页摆放、观感与排定状态，关机调用主线的 sd_execute
// 颜色/字号/间距走 ui::*；几何在 ui::layout（数值清单见交付说明），模块里不另起常量。
//
// 主线接线：create_page / relayout / show_page / draw / on_command 五处同设置页；
// 另需四处：WM_DRAWITEM -> draw_item、WM_COMMAND 通知码 -> on_notify、
// WM_TIMER(wp == SD_TIMER_ID) -> on_timer、WM_CTLCOLORSTATIC/EDIT -> ctl_color。
// crate 内 sd_execute / show_toast / load_dword / save_dword / wide / LockExt 需 pub(crate)。

use std::mem::size_of;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicI64, AtomicU64, AtomicUsize, Ordering};
use std::sync::Mutex;

use winapi::shared::minwindef::{FILETIME, LPARAM, LRESULT, UINT, WPARAM};
use winapi::shared::windef::{HBRUSH, HDC, HMENU, HWND, POINT, RECT, SIZE};
use winapi::um::fileapi::CompareFileTime;
use winapi::um::minwinbase::SYSTEMTIME;
use winapi::um::sysinfoapi::{GetLocalTime, GetTickCount64};
use winapi::um::timezoneapi::SystemTimeToFileTime;
use winapi::um::wingdi::{
    CreateSolidBrush, DeleteObject, GetStockObject, GetTextExtentPoint32W,
    GetTextMetricsW, Polygon, SelectObject, SetBkMode, SetTextColor, NULL_PEN,
    TEXTMETRICW, TRANSPARENT,
};
use winapi::um::winuser::{
    CallWindowProcW, CreateWindowExW, DefWindowProcW, DrawTextW, EnableWindow, FillRect, FlashWindowEx,
    FrameRect, GetDC, GetDlgCtrlID, GetDlgItem, GetFocus, GetKeyState, GetParent, GetWindowLongW,
    GetWindowTextW, InvalidateRect, IsIconic, IsWindowEnabled, KillTimer, RedrawWindow, ReleaseDC,
    SendMessageW, SetFocus, SetForegroundWindow, SetTimer, SetWindowLongPtrW, SetWindowPos,
    SetWindowTextW, ShowWindow, BS_OWNERDRAW, DRAWITEMSTRUCT, DT_CENTER, DT_LEFT,
    DT_NOPREFIX, DT_RIGHT, DT_SINGLELINE, DT_VCENTER, FLASHWINFO, FLASHW_ALL, FLASHW_TIMERNOFG,
    GWLP_WNDPROC, GWL_STYLE, RDW_ALLCHILDREN, RDW_INVALIDATE, SWP_NOZORDER, SW_HIDE,
    SW_RESTORE, SW_SHOW, WM_MOUSELEAVE, WM_MOUSEMOVE, WM_SETFONT, WS_CHILD,
    WS_VISIBLE,
};

use crate::ui::{self, layout::shutdown as layout};
use crate::{load_dword, save_dword, show_toast, svg_icon_blit, track_leave, wide, LockExt};

// ——————————————— 页号 / 页签 / 定时器 ———————————————
pub const PAGE_SHUTDOWN: i32 = 7;
pub const ID_TAB_SHUT: usize = 4126;
pub const SD_TIMER_ID: usize = 98;

// ——————————————— 控件 ID（5600–5649）———————————————
pub const ID_AT_HOUR: usize = 5601;
pub const ID_AT_MIN: usize = 5602;
pub const ID_AT_SEC: usize = 5603;
pub const ID_CD_HOUR: usize = 5604;
pub const ID_CD_MIN: usize = 5605;
pub const ID_CD_SEC: usize = 5606;
pub const ID_UNIT_AT_HOUR: usize = 5607;
pub const ID_UNIT_AT_MIN: usize = 5608;
pub const ID_UNIT_AT_SEC: usize = 5609;
pub const ID_DAILY: usize = 5610;
pub const ID_UNIT_CD_HOUR: usize = 5611;
pub const ID_UNIT_CD_MIN: usize = 5612;
pub const ID_UNIT_CD_SEC: usize = 5613;
pub const ID_AT_UP_HOUR: usize = 5614;
pub const ID_AT_DN_HOUR: usize = 5615;
pub const ID_AT_UP_MIN: usize = 5616;
pub const ID_AT_DN_MIN: usize = 5617;
pub const ID_AT_UP_SEC: usize = 5618;
pub const ID_AT_DN_SEC: usize = 5619;
pub const ID_CD_UP_HOUR: usize = 5620;
pub const ID_CD_DN_HOUR: usize = 5621;
pub const ID_CD_UP_MIN: usize = 5622;
pub const ID_CD_DN_MIN: usize = 5623;
pub const ID_CD_UP_SEC: usize = 5624;
pub const ID_CD_DN_SEC: usize = 5625;
pub const ID_START: usize = 5640;
pub const ID_PAUSE: usize = 5641;
pub const ID_CLEAR: usize = 5642;

const ED_AT: [usize; 3] = [ID_AT_HOUR, ID_AT_MIN, ID_AT_SEC];
const ED_CD: [usize; 3] = [ID_CD_HOUR, ID_CD_MIN, ID_CD_SEC];
const UNIT_AT: [usize; 3] = [ID_UNIT_AT_HOUR, ID_UNIT_AT_MIN, ID_UNIT_AT_SEC];
const UNIT_CD: [usize; 3] = [ID_UNIT_CD_HOUR, ID_UNIT_CD_MIN, ID_UNIT_CD_SEC];
const UP_AT: [usize; 3] = [ID_AT_UP_HOUR, ID_AT_UP_MIN, ID_AT_UP_SEC];
const DN_AT: [usize; 3] = [ID_AT_DN_HOUR, ID_AT_DN_MIN, ID_AT_DN_SEC];
const UP_CD: [usize; 3] = [ID_CD_UP_HOUR, ID_CD_UP_MIN, ID_CD_UP_SEC];
const DN_CD: [usize; 3] = [ID_CD_DN_HOUR, ID_CD_DN_MIN, ID_CD_DN_SEC];
// 倒计时 99/59/59；指定时刻 23/59/59
const MAX_AT: [i64; 3] = [23, 59, 59];
const MAX_CD: [i64; 3] = [99, 59, 59];

pub const ALL_IDS: [usize; 28] = [
    ID_AT_HOUR, ID_AT_MIN, ID_AT_SEC, ID_CD_HOUR, ID_CD_MIN, ID_CD_SEC,
    ID_UNIT_AT_HOUR, ID_UNIT_AT_MIN, ID_UNIT_AT_SEC, ID_DAILY,
    ID_UNIT_CD_HOUR, ID_UNIT_CD_MIN, ID_UNIT_CD_SEC,
    ID_AT_UP_HOUR, ID_AT_DN_HOUR, ID_AT_UP_MIN, ID_AT_DN_MIN, ID_AT_UP_SEC, ID_AT_DN_SEC,
    ID_CD_UP_HOUR, ID_CD_DN_HOUR, ID_CD_UP_MIN, ID_CD_DN_MIN, ID_CD_UP_SEC, ID_CD_DN_SEC,
    ID_START, ID_PAUSE, ID_CLEAR,
];

// 配置键（「每天」与指定时刻三格）
const CFG_H: &str = "SdAtHour";
const CFG_M: &str = "SdAtMin";
const CFG_S: &str = "SdAtSec";
const CFG_DAILY: &str = "SdDaily";

// 控件样式位（winapi 0.3.9 未导出，按 winuser.h 取值）
const SS_RIGHT: u32 = 0x0002;
const SS_CENTERIMAGE: u32 = 0x0200;
const ES_RIGHT: u32 = 0x0002;
const ES_NUMBER: u32 = 0x2000;
const ES_READONLY: u32 = 0x0800;
const EM_SETLIMITTEXT: u32 = 0x00C5;
const EM_SETSEL: u32 = 0x00B1;
const EM_GETSEL: u32 = 0x00B0;
const EM_SETREADONLY: u32 = 0x00CF;
const DLGC_WANTALLKEYS: u32 = 0x0004;
const ODS_SELECTED: u32 = 0x0001;
const ODS_DISABLED: u32 = 0x0004;
const WM_GETDLGCODE: u32 = 0x0087;
const WM_KEYDOWN: u32 = 0x0100;
const WM_CHAR: u32 = 0x0102;
const WM_SETFOCUS: u32 = 0x0007;
const WM_PASTE: u32 = 0x0302;
const WM_CUT: u32 = 0x0300;
const WM_UNDO: u32 = 0x0304;
const WM_LBUTTONDOWN: u32 = 0x0201;
const WM_COMMAND: u32 = 0x0111;
const VK_TAB: i32 = 0x09;
const VK_SHIFT: i32 = 0x10;
const VK_CONTROL: i32 = 0x11;
const VK_UP: i32 = 0x26;
const VK_DOWN: i32 = 0x28;
const VK_RETURN: i32 = 0x0D;
const EN_SETFOCUS: u16 = 0x0100;
const EN_CHANGE: u16 = 0x0300;
const EN_KILLFOCUS: u16 = 0x0102;

// 页面状态（全部自持；模块外不读）
static RUNNING: AtomicBool = AtomicBool::new(false);
static PAUSED: AtomicBool = AtomicBool::new(false);
static DONE: AtomicBool = AtomicBool::new(false);
static WARNED: AtomicBool = AtomicBool::new(false);
static AT_MODE: AtomicBool = AtomicBool::new(false);    // true=指定时刻，false=倒计时
static AT_DEFAULT: AtomicBool = AtomicBool::new(true);  // 指定时刻仍跟随本机时间（灰色默认态）
static DAILY: AtomicBool = AtomicBool::new(false);
static TIMER_ON: AtomicBool = AtomicBool::new(false);
static UI_BUSY: AtomicBool = AtomicBool::new(false);
static VISIBLE: AtomicBool = AtomicBool::new(false);
static PUSHING: AtomicBool = AtomicBool::new(false);    // 程序回写（不是用户手敲）
static REMAIN: AtomicI64 = AtomicI64::new(0);
static MAX: AtomicI64 = AtomicI64::new(1);
static DEADLINE: AtomicU64 = AtomicU64::new(0);         // GetTickCount64 毫秒
static AT_TARGET: AtomicU64 = AtomicU64::new(0);        // 本机时间折成的 FILETIME 计数
static TYPED: Mutex<[u32; 6]> = Mutex::new([0; 6]);     // 本次聚焦后手敲的位数
static EDIT_OLD: Mutex<[usize; 6]> = Mutex::new([0; 6]);
static HOVER: AtomicI32 = AtomicI32::new(0);
static BTN_OLD: AtomicUsize = AtomicUsize::new(0);
static CELL_BG: AtomicUsize = AtomicUsize::new(0);

fn edit_index(id: usize) -> Option<usize> {
    if let Some(i) = ED_AT.iter().position(|&x| x == id) { return Some(i); }
    ED_CD.iter().position(|&x| x == id).map(|i| 3 + i)
}

fn arrow_target(id: usize) -> Option<(usize, i64)> {
    for i in 0..3 {
        if id == UP_CD[i] { return Some((ED_CD[i], 1)); }
        if id == DN_CD[i] { return Some((ED_CD[i], -1)); }
        if id == UP_AT[i] { return Some((ED_AT[i], 1)); }
        if id == DN_AT[i] { return Some((ED_AT[i], -1)); }
    }
    None
}

fn arrow_up(id: usize) -> bool {
    UP_CD.iter().chain(UP_AT.iter()).any(|&x| x == id)
}

fn max_of(id: usize) -> i64 {
    if let Some(i) = ED_AT.iter().position(|&x| x == id) { MAX_AT[i] }
    else if let Some(i) = ED_CD.iter().position(|&x| x == id) { MAX_CD[i] }
    else { 0 }
}

/// 文字色是否降一档：非生效行、以及指定时刻还停在预读默认态
fn dim_of(id: usize) -> Option<bool> {
    let at = AT_MODE.load(Ordering::Relaxed);
    if ED_AT.contains(&id) || UNIT_AT.contains(&id) { return Some(!at || AT_DEFAULT.load(Ordering::Relaxed)); }
    if ED_CD.contains(&id) || UNIT_CD.contains(&id) { return Some(at); }
    None
}

fn cell_bg() -> HBRUSH {
    unsafe {
        let mut h = CELL_BG.load(Ordering::Relaxed) as HBRUSH;
        if h.is_null() {
            h = CreateSolidBrush(ui::rgb(ui::C_WHITE));
            CELL_BG.store(h as usize, Ordering::Relaxed);
        }
        h
    }
}

unsafe fn text_of(h: HWND) -> String {
    let mut b = [0u16; 32];
    let n = GetWindowTextW(h, b.as_mut_ptr(), 32);
    String::from_utf16_lossy(&b[..n.max(0) as usize])
}

unsafe fn raw(hwnd: HWND, id: usize) -> i64 {
    let c = GetDlgItem(hwnd, id as i32);
    if c.is_null() { return 0; }
    let t = text_of(c);
    t.chars().filter(|c| c.is_ascii_digit()).collect::<String>().parse().unwrap_or(0)
}

unsafe fn value(hwnd: HWND, id: usize) -> i64 { raw(hwnd, id).min(max_of(id)).max(0) }

/// 程序回写一格：两位数、值没变不写；回写等于换掉用户手敲内容 → 位数计数清零
unsafe fn set_value(hwnd: HWND, id: usize, v: i64) {
    let c = GetDlgItem(hwnd, id as i32);
    if c.is_null() { return; }
    if let Some(i) = edit_index(id) { TYPED.lock_ok()[i] = 0; }
    let t = format!("{:02}", v.max(0));
    if text_of(c) == t { return; }
    let prev = PUSHING.swap(true, Ordering::Relaxed);
    SetWindowTextW(c, wide(&t).as_ptr());
    PUSHING.store(prev, Ordering::Relaxed);
}

unsafe fn total_seconds(hwnd: HWND) -> i64 {
    value(hwnd, ED_CD[0]) * 3600 + value(hwnd, ED_CD[1]) * 60 + value(hwnd, ED_CD[2])
}

/// 外部（WM_TIMER）入口：只认本页那只定时器
pub unsafe fn on_timer(id: usize, hwnd: HWND) -> bool {
    if id != SD_TIMER_ID { return false; }
    tick(hwnd);
    true
}

fn ft_quad(f: &FILETIME) -> u64 {
    (f.dwHighDateTime as u64) << 32 | f.dwLowDateTime as u64
}

/// 目标时刻 = 今天这个钟点；已过（含正好这一秒）就取次日同一时刻
unsafe fn at_set_target(hwnd: HWND) {
    let mut now: SYSTEMTIME = std::mem::zeroed();
    GetLocalTime(&mut now);
    let mut t = now;
    t.wHour = value(hwnd, ED_AT[0]) as u16;
    t.wMinute = value(hwnd, ED_AT[1]) as u16;
    t.wSecond = value(hwnd, ED_AT[2]) as u16;
    t.wMilliseconds = 0;
    let mut ft: FILETIME = std::mem::zeroed();
    let mut fn_: FILETIME = std::mem::zeroed();
    if SystemTimeToFileTime(&t, &mut ft) == 0 || SystemTimeToFileTime(&now, &mut fn_) == 0 {
        AT_TARGET.store(0, Ordering::Relaxed);
        return;
    }
    if CompareFileTime(&ft, &fn_) <= 0 {
        // 纯日历进位 24h；抛给系统算月末/闰年/跨年
        let q = ft_quad(&ft) + 864_000_000_000u64;
        ft.dwLowDateTime = q as u32;
        ft.dwHighDateTime = (q >> 32) as u32;
    }
    AT_TARGET.store(ft_quad(&ft), Ordering::Relaxed);
}

/// 时刻模式还能等多久（秒），每拍现算本机时钟
unsafe fn at_remain() -> i64 {
    let a = AT_TARGET.load(Ordering::Relaxed);
    if a == 0 { return 0; }
    let mut now: SYSTEMTIME = std::mem::zeroed();
    GetLocalTime(&mut now);
    let mut fn_: FILETIME = std::mem::zeroed();
    if SystemTimeToFileTime(&now, &mut fn_) == 0 { return 0; }
    let b = ft_quad(&fn_);
    if a <= b { 0 } else { ((a - b + 9_999_999) / 10_000_000) as i64 }
}

/// 模式一变：两行的数字色、格线、单位字、「每天」都要跟着换主次。
/// 只置脏本页（含子控件）
unsafe fn apply_mode_style(hwnd: HWND) {
    if hwnd.is_null() || !VISIBLE.load(Ordering::Relaxed) { return; }
    RedrawWindow(hwnd, std::ptr::null_mut(), std::ptr::null_mut(),
        RDW_INVALIDATE | RDW_ALLCHILDREN);
}

/// 生效模式只有一路：换组输入即作废已排定那一次
unsafe fn set_mode(hwnd: HWND, clock: bool) {
    if AT_MODE.load(Ordering::Relaxed) == clock { return; }
    AT_MODE.store(clock, Ordering::Relaxed);
    if RUNNING.load(Ordering::Relaxed) {
        RUNNING.store(false, Ordering::Relaxed);
        PAUSED.store(false, Ordering::Relaxed);
        DONE.store(false, Ordering::Relaxed);
        WARNED.store(false, Ordering::Relaxed);
        REMAIN.store(0, Ordering::Relaxed);
        MAX.store(1, Ordering::Relaxed);
    }
    apply_mode_style(hwnd);
    update_status(hwnd);
}

unsafe fn save_cfg(hwnd: HWND) {
    save_dword(CFG_H, value(hwnd, ED_AT[0]) as u32);
    save_dword(CFG_M, value(hwnd, ED_AT[1]) as u32);
    save_dword(CFG_S, value(hwnd, ED_AT[2]) as u32);
    save_dword(CFG_DAILY, DAILY.load(Ordering::Relaxed) as u32);
}

unsafe fn load_cfg(hwnd: HWND) {
    for (i, id) in ED_AT.iter().enumerate() {
        set_value(hwnd, *id, MAX_AT[i].min(load_dword([CFG_H, CFG_M, CFG_S][i], 0) as i64));
    }
    DAILY.store(load_dword(CFG_DAILY, 0) == 1, Ordering::Relaxed);
}

/// 秒数拆成时/分/秒写进倒计时三格（时上限 99，不按 24 取模）
unsafe fn write_remain(hwnd: HWND, secs: i64) {
    let s = secs.max(0);
    set_value(hwnd, ED_CD[0], (s / 3600).min(MAX_CD[0]));
    set_value(hwnd, ED_CD[1], (s % 3600) / 60);
    set_value(hwnd, ED_CD[2], s % 60);
}

/// 指定时刻默认预读：本机当前时分秒，回到灰色默认态
unsafe fn at_prefill(hwnd: HWND) {
    let mut now: SYSTEMTIME = std::mem::zeroed();
    GetLocalTime(&mut now);
    let prev = PUSHING.swap(true, Ordering::Relaxed);
    set_value(hwnd, ED_AT[0], now.wHour as i64);
    set_value(hwnd, ED_AT[1], now.wMinute as i64);
    set_value(hwnd, ED_AT[2], now.wSecond as i64);
    PUSHING.store(prev, Ordering::Relaxed);
    AT_DEFAULT.store(true, Ordering::Relaxed);
}

/// 每秒一拍：只碰该动的那三格（倒计时运行读剩余；空闲默认态的三格跟本机钟）
unsafe fn sync_cells(hwnd: HWND) {
    if !AT_MODE.load(Ordering::Relaxed) && RUNNING.load(Ordering::Relaxed) && !PAUSED.load(Ordering::Relaxed) {
        write_remain(hwnd, REMAIN.load(Ordering::Relaxed));
        return;
    }
    // 空闲且默认态：不管当前是不是"指定时刻"模式，三格都跟随本机钟
    if RUNNING.load(Ordering::Relaxed) || !AT_DEFAULT.load(Ordering::Relaxed) { return; }
    // 正在这一格里打字时别回写（会把刚敲的字和选区顶掉）
    for id in ED_AT.iter() {
        let c = GetDlgItem(hwnd, *id as i32);
        if !c.is_null() && GetFocus() == c { return; }
    }
    let mut now: SYSTEMTIME = std::mem::zeroed();
    GetLocalTime(&mut now);
    set_value(hwnd, ED_AT[0], now.wHour as i64);
    set_value(hwnd, ED_AT[1], now.wMinute as i64);
    set_value(hwnd, ED_AT[2], now.wSecond as i64);
}

/// 这只定时器的开/关只认这里：倒计时在跑要它，空闲时默认态跟随钟点也要它
unsafe fn sync_timer(hwnd: HWND) {
    if hwnd.is_null() { return; }
    let want = (RUNNING.load(Ordering::Relaxed) && !PAUSED.load(Ordering::Relaxed))
        || (!RUNNING.load(Ordering::Relaxed) && AT_DEFAULT.load(Ordering::Relaxed) && VISIBLE.load(Ordering::Relaxed));
    if want == TIMER_ON.load(Ordering::Relaxed) { return; }
    if want {
        // SetTimer 失败（句柄耗尽等）时不能记为已开，否则之后 want 恒等、
        // 永不重试，倒计时看着在走但永远不触发
        let ok = SetTimer(hwnd, SD_TIMER_ID, 1000, None) != 0;
        TIMER_ON.store(ok, Ordering::Relaxed);
    } else {
        KillTimer(hwnd, SD_TIMER_ID);
        TIMER_ON.store(false, Ordering::Relaxed);
    }
}

/// 状态收口：定时器、清空按钮、六个格只读、十二颗箭头启用、进度条重画
unsafe fn update_status(hwnd: HWND) {
    if hwnd.is_null() { return; }
    sync_timer(hwnd);
    let busy = RUNNING.load(Ordering::Relaxed) && !PAUSED.load(Ordering::Relaxed);

    let c = GetDlgItem(hwnd, ID_CLEAR as i32);
    if !c.is_null() {
        let want = if busy { 0 } else { 1 };
        if IsWindowEnabled(c) != want {
            EnableWindow(c, want);
            InvalidateRect(c, std::ptr::null_mut(), 0);
        }
    }

    // 跑起来（且没暂停）之后设定格只读；用 EM_SETREADONLY，不禁用（禁用会把字画灰）
    for id in ED_AT.iter().chain(ED_CD.iter()) {
        let e = GetDlgItem(hwnd, *id as i32);
        if e.is_null() { continue; }
        let ro = (GetWindowLongW(e, GWL_STYLE) & ES_READONLY as i32) != 0;
        if ro != busy { SendMessageW(e, EM_SETREADONLY, busy as WPARAM, 0); }
    }
    // 箭头是自绘子窗口：启停后必须自己置脏
    for id in UP_CD.iter().chain(DN_CD.iter()).chain(UP_AT.iter()).chain(DN_AT.iter()) {
        let a = GetDlgItem(hwnd, *id as i32);
        if a.is_null() { continue; }
        let want = if busy { 0 } else { 1 };
        if IsWindowEnabled(a) != want {
            EnableWindow(a, want);
            // 只置脏、不 RDW_UPDATENOW：切页时整棵窗口树正在冻结统一刷新，
            // 这里抢着同步画会把「其它页还没藏完」的半切换状态写屏。下一轮 WM_PAINT 一样是即时可见。
            RedrawWindow(a, std::ptr::null_mut(), std::ptr::null_mut(), RDW_INVALIDATE);
        }
    }

    if !VISIBLE.load(Ordering::Relaxed) { return; }
    // 只置脏本页正文区（含子控件由下面的按钮重画兜住）
    let r = RECT {
        left: ui::sc(layout::SD_MARGIN), top: ui::sc(layout::SD_Y_TITLE),
        right: ui::sc(layout::SD_RIGHT), bottom: ui::sc(layout::SD_Y_BAR + layout::SD_BAR_H),
    };
    InvalidateRect(hwnd, &r, 0);

    // 三颗按钮颜色按 sdRunning/sdPaused 现算：状态真变了才逐个重画
    if UI_BUSY.swap(busy, Ordering::Relaxed) != busy {
        for id in [ID_START, ID_PAUSE, ID_CLEAR] {
            let b = GetDlgItem(hwnd, id as i32);
            if !b.is_null() {
                RedrawWindow(b, std::ptr::null_mut(), std::ptr::null_mut(), RDW_INVALIDATE);
            }
        }
    }
}

unsafe fn nudge(hwnd: HWND, id: usize, dir: i64) {
    let Some(idx) = edit_index(id) else { return };
    let at = idx < 3;
    let i = if at { idx } else { idx - 3 };
    let max = if at { MAX_AT[i] } else { MAX_CD[i] };
    set_mode(hwnd, at);
    let mut v = value(hwnd, id) + dir;
    if v < 0 { v = max; } else if v > max { v = 0; }
    set_value(hwnd, id, v);
    if at { AT_DEFAULT.store(false, Ordering::Relaxed); }
    // 暂停中改时长/钟点：排定立即按新值重算
    if RUNNING.load(Ordering::Relaxed) && PAUSED.load(Ordering::Relaxed) {
        let r = if at { at_set_target(hwnd); at_remain() } else { total_seconds(hwnd) };
        REMAIN.store(r, Ordering::Relaxed);
        MAX.store(r.max(1), Ordering::Relaxed);
    }
    if at { save_cfg(hwnd); }
    update_status(hwnd);
}

unsafe fn resume(hwnd: HWND) {
    PAUSED.store(false, Ordering::Relaxed);
    WARNED.store(false, Ordering::Relaxed);
    if AT_MODE.load(Ordering::Relaxed) {
        // 时刻模式不按暂停时的剩余续：目标钉在钟点上，恢复这一拍就从本机时间重算
        sync_timer(hwnd);
        tick(hwnd);
        return;
    }
    let left = REMAIN.load(Ordering::Relaxed);
    DEADLINE.store(GetTickCount64() + left.max(0) as u64 * 1000, Ordering::Relaxed);
    sync_timer(hwnd);
    update_status(hwnd);
}

unsafe fn start(hwnd: HWND) {
    if RUNNING.load(Ordering::Relaxed) && !PAUSED.load(Ordering::Relaxed) { return; }
    if RUNNING.load(Ordering::Relaxed) { resume(hwnd); return; }

    let total = if AT_MODE.load(Ordering::Relaxed) {
        // 三格先规范化（"7" → "07"），再立目标；今天已过自动落到次日
        for id in ED_AT.iter() { set_value(hwnd, *id, value(hwnd, *id)); }
        at_set_target(hwnd);
        save_cfg(hwnd);
        at_remain()
    } else {
        total_seconds(hwnd)
    };
    if total <= 0 {
        show_toast("请输入有效的时间");
        return;
    }
    RUNNING.store(true, Ordering::Relaxed);
    PAUSED.store(false, Ordering::Relaxed);
    DONE.store(false, Ordering::Relaxed);
    WARNED.store(false, Ordering::Relaxed);
    REMAIN.store(total, Ordering::Relaxed);
    MAX.store(total, Ordering::Relaxed);
    if !AT_MODE.load(Ordering::Relaxed) {
        DEADLINE.store(GetTickCount64() + total as u64 * 1000, Ordering::Relaxed);
        for id in ED_CD.iter() { set_value(hwnd, *id, value(hwnd, *id)); }
    }
    sync_timer(hwnd);
    update_status(hwnd);
}

unsafe fn pause(hwnd: HWND) {
    if !RUNNING.load(Ordering::Relaxed) { return; }
    if PAUSED.load(Ordering::Relaxed) { resume(hwnd); return; }
    // 暂停时把剩余量现算一遍（tick 是整秒跳的，拿上一次读数会平白多出近一秒）
    let r = if AT_MODE.load(Ordering::Relaxed) {
        at_remain()
    } else {
        let now = GetTickCount64();
        let d = DEADLINE.load(Ordering::Relaxed);
        if d > now { ((d - now + 999) / 1000) as i64 } else { 0 }
    };
    REMAIN.store(r, Ordering::Relaxed);
    PAUSED.store(true, Ordering::Relaxed);
    if !AT_MODE.load(Ordering::Relaxed) { write_remain(hwnd, r); }
    sync_timer(hwnd);
    update_status(hwnd);
}

unsafe fn clear(hwnd: HWND) {
    // 倒计时正跑着（未暂停）时「清空」禁用：先暂停或等结束
    if RUNNING.load(Ordering::Relaxed) && !PAUSED.load(Ordering::Relaxed) { return; }
    RUNNING.store(false, Ordering::Relaxed);
    PAUSED.store(false, Ordering::Relaxed);
    DONE.store(false, Ordering::Relaxed);
    WARNED.store(false, Ordering::Relaxed);
    REMAIN.store(0, Ordering::Relaxed);
    MAX.store(1, Ordering::Relaxed);
    DEADLINE.store(0, Ordering::Relaxed);
    AT_MODE.store(false, Ordering::Relaxed);
    DAILY.store(false, Ordering::Relaxed);
    let d = GetDlgItem(hwnd, ID_DAILY as i32);
    if !d.is_null() { InvalidateRect(d, std::ptr::null_mut(), 0); }
    write_remain(hwnd, 0);
    at_prefill(hwnd);
    save_cfg(hwnd);
    apply_mode_style(hwnd);
    update_status(hwnd);
}

/// 最后 10 秒：还原/顶出主界面并切到本页，让用户能临时暂停或改时间
unsafe fn warn_window(hwnd: HWND) {
    if IsIconic(hwnd) != 0 { ShowWindow(hwnd, SW_RESTORE); }
    ShowWindow(hwnd, SW_SHOW);
    SendMessageW(hwnd, WM_COMMAND, ID_TAB_SHUT as WPARAM, 0);
    if SetForegroundWindow(hwnd) == 0 {
        let mut fi: FLASHWINFO = std::mem::zeroed();
        fi.cbSize = size_of::<FLASHWINFO>() as u32;
        fi.hwnd = hwnd;
        fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
        FlashWindowEx(&mut fi);
    }
}

/// 关机没成：机器还开着，界面退回「未开始」，进度条分母一并归零
unsafe fn abort(hwnd: HWND, msg: &str) {
    DONE.store(false, Ordering::Relaxed);
    let r = REMAIN.load(Ordering::Relaxed);
    MAX.store(if RUNNING.load(Ordering::Relaxed) && r > 1 { r } else { 1 }, Ordering::Relaxed);
    update_status(hwnd);
    show_toast(msg);
}

unsafe fn tick(hwnd: HWND) {
    if !RUNNING.load(Ordering::Relaxed) || PAUSED.load(Ordering::Relaxed) {
        sync_cells(hwnd);
        return;
    }
    // 用截止时刻反算剩余（不逐秒减一）：定时器被合并到相邻调度片也不会跑偏
    let remain = if AT_MODE.load(Ordering::Relaxed) {
        at_remain()
    } else {
        let now = GetTickCount64();
        let d = DEADLINE.load(Ordering::Relaxed);
        if d > now { ((d - now + 999) / 1000) as i64 } else { 0 }
    };
    REMAIN.store(remain, Ordering::Relaxed);
    sync_cells(hwnd);
    if !WARNED.load(Ordering::Relaxed) && remain <= 10 {
        WARNED.store(true, Ordering::Relaxed);
        warn_window(hwnd);
    }
    if remain > 0 {
        update_status(hwnd);
        return;
    }

    // 到点：一次性收尾；「每天」在时刻模式下把排定续到次日
    if AT_MODE.load(Ordering::Relaxed) && DAILY.load(Ordering::Relaxed) {
        at_set_target(hwnd);
        let mut r = at_remain();
        // 正好落在目标那一秒可能是 86400：读数上限就是 23:59:59
        if r <= 0 || r >= 24 * 3600 { r = 24 * 3600 - 1; }
        REMAIN.store(r, Ordering::Relaxed);
        MAX.store(r, Ordering::Relaxed);
        PAUSED.store(false, Ordering::Relaxed);
        WARNED.store(false, Ordering::Relaxed);
        DONE.store(false, Ordering::Relaxed);
        update_status(hwnd);
    } else {
        REMAIN.store(0, Ordering::Relaxed);
        RUNNING.store(false, Ordering::Relaxed);
        DONE.store(true, Ordering::Relaxed);
        write_remain(hwnd, 0);
        sync_timer(hwnd);
        update_status(hwnd);
    }
    if let Err(msg) = crate::sd_execute(hwnd) { abort(hwnd, msg); }
}

// ——————————————— 悬停（模块内子类化，不新增主线接线） ———————————————
unsafe extern "system" fn hover_proc(h: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_MOUSEMOVE => {
            let id = GetDlgCtrlID(h);
            if HOVER.swap(id, Ordering::Relaxed) != id {
                InvalidateRect(h, std::ptr::null_mut(), 0);
            }
            track_leave(h);
        }
        WM_MOUSELEAVE => {
            if HOVER.swap(0, Ordering::Relaxed) != 0 {
                InvalidateRect(h, std::ptr::null_mut(), 0);
            }
        }
        _ => {}
    }
    let old = BTN_OLD.load(Ordering::Relaxed);
    CallWindowProcW(
        std::mem::transmute::<usize, Option<unsafe extern "system" fn(HWND, UINT, WPARAM, LPARAM) -> LRESULT>>(old),
        h, msg, wp, lp)
}

unsafe fn hook(h: HWND) {
    if h.is_null() { return; }
    let p = SetWindowLongPtrW(h, GWLP_WNDPROC, hover_proc as *const () as usize as isize);
    if BTN_OLD.load(Ordering::Relaxed) == 0 { BTN_OLD.store(p as usize, Ordering::Relaxed); }
}

// ——————————————— 六个数字格的键盘 ———————————————
unsafe fn call_old(h: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    let p = edit_index(GetDlgCtrlID(h) as usize)
        .map(|i| EDIT_OLD.lock_ok()[i])
        .unwrap_or(0);
    let f = std::mem::transmute::<usize, Option<unsafe extern "system" fn(HWND, UINT, WPARAM, LPARAM) -> LRESULT>>(p);
    match f {
        Some(f) => CallWindowProcW(Some(f), h, msg, wp, lp),
        None => DefWindowProcW(h, msg, wp, lp),
    }
}

unsafe extern "system" fn edit_proc(h: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    let id = GetDlgCtrlID(h) as usize;
    let idx = match edit_index(id) { Some(i) => i, None => return call_old(h, msg, wp, lp) };
    match msg {
        WM_GETDLGCODE => return DLGC_WANTALLKEYS as LRESULT,
        WM_KEYDOWN => {
            // 撤销/全选会整体换掉框里的字，之后那一键是替换而不是接着数位数
            if (GetKeyState(VK_CONTROL) as u16 & 0x8000) != 0
                && (wp as u32 == b'Z' as u32 || wp as u32 == b'A' as u32) {
                TYPED.lock_ok()[idx] = 0;
            }
            if wp == VK_TAB as WPARAM {
                // 视觉顺序：指定时刻 时/分/秒 → 倒计时 时/分/秒，Shift+Tab 反向
                let order = [ED_AT[0], ED_AT[1], ED_AT[2], ED_CD[0], ED_CD[1], ED_CD[2]];
                if let Some(at) = order.iter().position(|&x| x == id) {
                    let back = (GetKeyState(VK_SHIFT) as u16 & 0x8000) != 0;
                    let next = if back { (at + 5) % 6 } else { (at + 1) % 6 };
                    let c = GetDlgItem(GetParent(h), order[next] as i32);
                    if !c.is_null() {
                        SetFocus(c);
                        SendMessageW(c, EM_SETSEL, 0, -1);
                    }
                }
                return 0;
            }
            if wp == VK_UP as WPARAM || wp == VK_DOWN as WPARAM {
                if !RUNNING.load(Ordering::Relaxed) || PAUSED.load(Ordering::Relaxed) {
                    nudge(GetParent(h), id, if wp == VK_UP as WPARAM { 1 } else { -1 });
                }
                return 0;
            }
            if wp == VK_RETURN as WPARAM { start(GetParent(h)); return 0; }
        }
        WM_CHAR => {
            if wp as u32 == '\r' as u32 || wp as u32 == '\n' as u32 { return 0; }
            // 有选区（全选 / Ctrl+A）时这一键是替换整格，不再累加位数
            let sel = SendMessageW(h, EM_GETSEL, 0, 0) as u32;
            let replacing = (sel & 0xFFFF) != (sel >> 16);
            let n = TYPED.lock_ok()[idx];
            if (wp as u32) >= b'0' as u32 && (wp as u32) <= b'9' as u32 {
                if n >= 2 && !replacing {
                    // 满两位再敲数字 → 直接顶到该格上限（EM_SETLIMITTEXT 会把第三位挡在门外）
                    let max = max_of(id);
                    let buf = format!("{:02}", max);
                    if text_of(h) != buf { SetWindowTextW(h, wide(&buf).as_ptr()); }
                    TYPED.lock_ok()[idx] = 0;
                    SendMessageW(h, EM_SETSEL, -1isize as WPARAM, -1);
                    return 0;
                }
                TYPED.lock_ok()[idx] = if replacing { 1 } else { n + 1 };
            } else if wp as u32 == 8 {
                TYPED.lock_ok()[idx] = if replacing { 0 } else { n.saturating_sub(1) };
            }
        }
        WM_SETFOCUS => {
            // 每次聚焦重新计数：框里预填的 00/07 不算用户已敲的位数
            TYPED.lock_ok()[idx] = 0;
        }
        WM_PASTE | WM_CUT | WM_UNDO => {
            // 剪贴板/撤销整段换字：位数清零，并剔掉混进来的非数字（ES_NUMBER 挡不住剪贴板）
            let r = call_old(h, msg, wp, lp);
            TYPED.lock_ok()[idx] = 0;
            let cur = text_of(h);
            let dig: String = cur.chars().filter(|c| c.is_ascii_digit()).take(2).collect();
            if dig.chars().count() != cur.chars().count() {
                SetWindowTextW(h, wide(&dig).as_ptr());
                SendMessageW(h, EM_SETSEL, -1isize as WPARAM, -1);
            }
            return r;
        }
        WM_LBUTTONDOWN => {
            // 首次点击由这里吃掉：只拿焦点 + 整格全选，不交给默认过程按点击位置摆光标
            if GetFocus() != h && (GetWindowLongW(h, GWL_STYLE) & ES_READONLY as i32) == 0 {
                SetFocus(h);
                SendMessageW(h, EM_SETSEL, 0, -1);
                return 0;
            }
        }
        _ => {}
    }
    call_old(h, msg, wp, lp)
}

unsafe fn mk(parent: HWND, class: &str, text: &str, style: u32, id: usize,
            x: i32, y: i32, w: i32, h: i32) -> HWND {
    CreateWindowExW(0, wide(class).as_ptr(), wide(text).as_ptr(),
        WS_CHILD | WS_VISIBLE | style, ui::sc(x), ui::sc(y), ui::sc(w), ui::sc(h),
        parent, id as HMENU, std::ptr::null_mut(), std::ptr::null_mut())
}

pub unsafe fn create_page(parent: HWND) {
    let fnum = ui::font(28, 700);
    let funit = ui::font(22, 700);

    // 六个数字格：不带 WS_BORDER（边框父窗口画），无初始文字（避创建期误发 EN_CHANGE）
    for i in 0..3 {
        for id in [ED_AT[i], ED_CD[i]] {
            let c = mk(parent, "EDIT", "", ES_RIGHT | ES_NUMBER, id, 0, 0, 0, 0);
            if c.is_null() { continue; }
            SendMessageW(c, WM_SETFONT, fnum as WPARAM, 1);
            SendMessageW(c, EM_SETLIMITTEXT, 2, 0);
            let idx = edit_index(id).unwrap_or(0);
            EDIT_OLD.lock_ok()[idx] = SetWindowLongPtrW(c, GWLP_WNDPROC,
                edit_proc as *const () as usize as isize) as usize;
        }
        for (unit, txt) in [(UNIT_AT[i], ["时", "分", "秒"][i]), (UNIT_CD[i], ["时", "分", "秒"][i])] {
            let c = mk(parent, "STATIC", txt, SS_RIGHT | SS_CENTERIMAGE, unit, 0, 0, 0, 0);
            SendMessageW(c, WM_SETFONT, funit as WPARAM, 1);
        }
        hook(mk(parent, "BUTTON", "", BS_OWNERDRAW, UP_AT[i], 0, 0, 0, 0));
        hook(mk(parent, "BUTTON", "", BS_OWNERDRAW, DN_AT[i], 0, 0, 0, 0));
        hook(mk(parent, "BUTTON", "", BS_OWNERDRAW, UP_CD[i], 0, 0, 0, 0));
        hook(mk(parent, "BUTTON", "", BS_OWNERDRAW, DN_CD[i], 0, 0, 0, 0));
    }

    hook(mk(parent, "BUTTON", "每天", BS_OWNERDRAW, ID_DAILY, 0, 0, 0, 0));
    let fbody = ui::font(18, 500);
    let daily = GetDlgItem(parent, ID_DAILY as i32);
    if !daily.is_null() { SendMessageW(daily, WM_SETFONT, fbody as WPARAM, 1); }
    hook(mk(parent, "BUTTON", "开始定时器", BS_OWNERDRAW, ID_START, 0, 0, 0, 0));
    hook(mk(parent, "BUTTON", "暂停定时器", BS_OWNERDRAW, ID_PAUSE, 0, 0, 0, 0));
    hook(mk(parent, "BUTTON", "清空", BS_OWNERDRAW, ID_CLEAR, 0, 0, 0, 0));

    relayout(parent);
    // 重启后恢复「每天」，但停在未启动；用户点开始才真正排定
    load_cfg(parent);
    write_remain(parent, 0);
    at_prefill(parent);
    update_status(parent);
}

unsafe fn line_h(hwnd: HWND, px: i32, weight: i32) -> i32 {
    let f = ui::font(px, weight as u32);
    let mut lh = ui::sc(px) + ui::sc(4);
    let dc = GetDC(hwnd);
    if !dc.is_null() {
        if f != 0 {
            let old = SelectObject(dc, f as *mut winapi::ctypes::c_void);
            let mut tm: TEXTMETRICW = std::mem::zeroed();
            if GetTextMetricsW(dc, &mut tm) != 0 && tm.tmHeight > 0 { lh = tm.tmHeight; }
            SelectObject(dc, old);
        }
        ReleaseDC(hwnd, dc);
    }
    lh
}

pub unsafe fn relayout(parent: HWND) {
    let ed_h = line_h(parent, 28, 700) + ui::sc(layout::SD_ED_AIR);
    let unit_h = line_h(parent, 22, 700);
    let unit_h = if unit_h <= 0 { ui::sc(layout::SD_ED_H / 2) } else { unit_h };

    let rows: [(i32, [usize; 3], [usize; 3], [usize; 3], [usize; 3]); 2] = [
        (layout::SD_Y_AT, ED_AT, UNIT_AT, UP_AT, DN_AT),
        (layout::SD_Y_ED, ED_CD, UNIT_CD, UP_CD, DN_CD),
    ];
    for (y, eds, units, ups, dns) in rows.iter() {
        let ed_y = *y + (layout::SD_ED_H - ed_h) / 2;
        for i in 0..3 {
            let gx = layout::SD_GX[i];
            // 单位字贴格内右下角，下边离底 3px（STATIC 会把自己那块刷白，贴边会压掉一条线）
            for (id, x, yy, w, h) in [
                (units[i], gx + layout::SD_ED_W - layout::SD_UNIT_W - layout::SD_UNIT_GAP,
                    *y + layout::SD_ED_H - layout::SD_UNIT_BOTTOM - unit_h, layout::SD_UNIT_W, unit_h),
                (eds[i], gx + layout::SD_ED_PAD, ed_y, layout::SD_EDIT_W, ed_h),
                (ups[i], gx + layout::SD_ED_W, *y, layout::SD_ARROW_W, layout::SD_ARROW_H),
                (dns[i], gx + layout::SD_ED_W, *y + layout::SD_ARROW_DY, layout::SD_ARROW_W, layout::SD_ARROW_H),
            ] {
                let c = GetDlgItem(parent, id as i32);
                if !c.is_null() {
                    SetWindowPos(c, std::ptr::null_mut(), ui::sc(x), ui::sc(yy),
                        ui::sc(w), ui::sc(h), SWP_NOZORDER);
                }
            }
        }
    }
    let daily = GetDlgItem(parent, ID_DAILY as i32);
    if !daily.is_null() {
        SetWindowPos(daily, std::ptr::null_mut(), ui::sc(layout::SD_CHK_X), ui::sc(layout::SD_Y_AT),
            ui::sc(layout::SD_CHK_W), ui::sc(layout::SD_ED_H), SWP_NOZORDER);
    }
    for (i, id) in [ID_START, ID_PAUSE, ID_CLEAR].iter().enumerate() {
        let c = GetDlgItem(parent, *id as i32);
        if !c.is_null() {
            SetWindowPos(c, std::ptr::null_mut(), ui::sc(layout::SD_BTN_X[i]), ui::sc(layout::SD_Y_BTN),
                ui::sc(layout::SD_BTN_W), ui::sc(layout::SD_BTN_H), SWP_NOZORDER);
        }
    }
}

pub unsafe fn show_page(parent: HWND, visible: bool) {
    VISIBLE.store(visible, Ordering::Relaxed);
    let show = if visible { SW_SHOW } else { SW_HIDE };
    for id in ALL_IDS.iter() {
        let c = GetDlgItem(parent, *id as i32);
        if !c.is_null() { ShowWindow(c, show); }
    }
    sync_timer(parent);
    if visible { update_status(parent); }
}

pub unsafe fn on_command(id: usize, hwnd: HWND) -> bool {
    if let Some((ed, dir)) = arrow_target(id) {
        nudge(hwnd, ed, dir);
        return true;
    }
    match id {
        ID_DAILY => {
            // 倒计时运行中不允许切模式：set_mode 会清空 REMAIN/MAX，
            // 等于静默取消已排好的关机
            if RUNNING.load(Ordering::Relaxed) && !PAUSED.load(Ordering::Relaxed) { return true; }
            // 点它就是"在设指定时刻这一组"，顺手把模式切过来
            set_mode(hwnd, true);
            DAILY.store(!DAILY.load(Ordering::Relaxed), Ordering::Relaxed);
            let c = GetDlgItem(hwnd, ID_DAILY as i32);
            if !c.is_null() { InvalidateRect(c, std::ptr::null_mut(), 0); }
            if RUNNING.load(Ordering::Relaxed) && !PAUSED.load(Ordering::Relaxed) { tick(hwnd); }
            save_cfg(hwnd);
            update_status(hwnd);
            true
        }
        ID_START => { start(hwnd); true }
        ID_PAUSE => { pause(hwnd); true }
        ID_CLEAR => { clear(hwnd); true }
        _ => false,
    }
}

/// WM_COMMAND 里六个格的 EN_* 通知（主线把通知码一起传进来）
pub unsafe fn on_notify(id: usize, code: u16, hwnd: HWND) -> bool {
    let idx = match edit_index(id) { Some(i) => i, None => return false };
    let at = idx < 3;
    let i = if at { idx } else { idx - 3 };
    match code {
        EN_SETFOCUS => {
            // 首次点进 / Tab 切到的格子一律全选：键入即替换
            let c = GetDlgItem(hwnd, id as i32);
            if !c.is_null() { SendMessageW(c, EM_SETSEL, 0, -1); }
            true
        }
        EN_CHANGE => {
            if !PUSHING.load(Ordering::Relaxed) {
                if at { AT_DEFAULT.store(false, Ordering::Relaxed); }
                set_mode(hwnd, at);
            }
            let max = if at { MAX_AT[i] } else { MAX_CD[i] };
            if raw(hwnd, id) > max {
                set_value(hwnd, id, max);
                // SetWindowText 会把插入点丢回开头，摆回末尾
                let c = GetDlgItem(hwnd, id as i32);
                if !c.is_null() { SendMessageW(c, EM_SETSEL, -1isize as WPARAM, -1); }
            }
            // 暂停中改格子 = 改了这次要等的时长/钟点：剩余跟着改
            if !PUSHING.load(Ordering::Relaxed)
                && RUNNING.load(Ordering::Relaxed) && PAUSED.load(Ordering::Relaxed) {
                let r = if at { at_set_target(hwnd); at_remain() } else { total_seconds(hwnd) };
                REMAIN.store(r, Ordering::Relaxed);
                MAX.store(r.max(1), Ordering::Relaxed);
                update_status(hwnd);
            }
            true
        }
        EN_KILLFOCUS => {
            // 离开输入框把 "5" 补成 "05"；指定时刻这组顺手落盘
            set_value(hwnd, id, value(hwnd, id));
            if at { save_cfg(hwnd); }
            update_status(hwnd);
            true
        }
        _ => false,
    }
}

/// WM_CTLCOLORSTATIC / WM_CTLCOLOREDIT：格内文字色按模式深浅，白底与格子一致
pub unsafe fn ctl_color(dc: HDC, id: usize) -> Option<HBRUSH> {
    let dim = dim_of(id)?;
    let is_edit = edit_index(id).is_some();
    SetBkMode(dc, TRANSPARENT as i32);
    SetTextColor(dc, ui::rgb(if dim {
        ui::C_TEXT_MUTED
    } else if is_edit {
        ui::C_TEXT
    } else {
        ui::C_TEXT_SEC
    }));
    Some(cell_bg())
}

unsafe fn fill(dc: HDC, r: &RECT, c: u32) {
    let b = CreateSolidBrush(ui::rgb(c));
    FillRect(dc, r, b as HBRUSH);
    DeleteObject(b as *mut winapi::ctypes::c_void);
}

unsafe fn frame(dc: HDC, r: &RECT, c: u32) {
    let b = CreateSolidBrush(ui::rgb(c));
    FrameRect(dc, r, b as HBRUSH);
    DeleteObject(b as *mut winapi::ctypes::c_void);
}

pub unsafe fn draw(dc: HDC) {
    SetBkMode(dc, TRANSPARENT as i32);
    let l = ui::sc(layout::SD_MARGIN);
    let r = ui::sc(layout::SD_RIGHT);

    // 标题：与下方内容区同宽居中
    SetTextColor(dc, ui::rgb(ui::C_TEXT));
    let old = SelectObject(dc, ui::font(34, 700) as *mut winapi::ctypes::c_void);
    let mut tr = RECT { left: l, top: ui::sc(layout::SD_Y_TITLE), right: r,
        bottom: ui::sc(layout::SD_Y_TITLE + layout::SD_TITLE_H) };
    DrawTextW(dc, wide("电脑定时关机").as_ptr(), -1, &mut tr,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);

    // 两行行首标签：右对齐，右边界固定 146（与第一格 154 之间留 8）
    SetTextColor(dc, ui::rgb(ui::C_TEXT_SEC));
    let old = SelectObject(dc, ui::font(22, 700) as *mut winapi::ctypes::c_void);
    for (s, y) in [("指定时刻：", layout::SD_Y_AT), ("倒计时：", layout::SD_Y_ED)] {
        let mut lr = RECT { left: ui::sc(layout::SD_LBL_X), top: ui::sc(y),
            right: ui::sc(layout::SD_LBL_X + layout::SD_LBL_W), bottom: ui::sc(y + layout::SD_ED_H) };
        DrawTextW(dc, wide(s).as_ptr(), -1, &mut lr,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, old);

    // 数字格：白底 + 1px 边框；非生效行的格线降一档
    let at = AT_MODE.load(Ordering::Relaxed);
    for i in 0..3 {
        let gx = layout::SD_GX[i];
        for (y, active) in [(layout::SD_Y_AT, at), (layout::SD_Y_ED, !at)] {
            let cell = RECT { left: ui::sc(gx), top: ui::sc(y),
                right: ui::sc(gx + layout::SD_ED_W), bottom: ui::sc(y + layout::SD_ED_H) };
            fill(dc, &cell, ui::C_WHITE);
            frame(dc, &cell, if active { ui::C_BORDER } else { ui::C_SURFACE_SUBTLE });
        }
    }
    // 生效行行首 2px 竖标记（不加底块、不加阴影）
    let my = if at { layout::SD_Y_AT } else { layout::SD_Y_ED };
    fill(dc, &RECT { left: ui::sc(layout::SD_MARK_X), top: ui::sc(my + 2),
        right: ui::sc(layout::SD_MARK_X + layout::SD_MARK_W), bottom: ui::sc(my + layout::SD_ED_H - 2) },
        ui::C_PRIMARY);

    // 状态行兼进度条：轨道 + 已完成蓝段 + 白色读数（暂停也画已走过的比例）
    let bar = RECT { left: l, top: ui::sc(layout::SD_Y_BAR), right: r,
        bottom: ui::sc(layout::SD_Y_BAR + layout::SD_BAR_H) };
    fill(dc, &bar, ui::C_TRACK);
    let max = MAX.load(Ordering::Relaxed);
    if max > 1 {
        let frac = (1.0 - REMAIN.load(Ordering::Relaxed) as f64 / max as f64).clamp(0.0, 1.0);
        let fw = ((bar.right - bar.left) as f64 * frac) as i32;
        if fw > 2 {
            fill(dc, &RECT { left: bar.left, top: bar.top, right: bar.left + fw, bottom: bar.bottom },
                ui::C_PRIMARY);
        }
    }

    let (status, secs) = if RUNNING.load(Ordering::Relaxed) {
        (if PAUSED.load(Ordering::Relaxed) { "已暂停" } else { "运行中" }, REMAIN.load(Ordering::Relaxed))
    } else if DONE.load(Ordering::Relaxed) {
        ("已到点", REMAIN.load(Ordering::Relaxed))
    } else {
        ("未开始", 0)
    };
    let s = if (RUNNING.load(Ordering::Relaxed) || DONE.load(Ordering::Relaxed)) && secs > 0 { secs } else { 0 };
    let time = format!("{:02}:{:02}:{:02}", s / 3600, (s % 3600) / 60, s % 60);

    SetTextColor(dc, ui::rgb(ui::C_WHITE));
    let old = SelectObject(dc, ui::font(22, 700) as *mut winapi::ctypes::c_void);
    let mut sz: SIZE = std::mem::zeroed();
    let ws_s = wide(status);
    let ws_t = wide(&time);
    let ws_ref = wide("00:00:00");
    let mut ws = 0;
    let mut wt = 0;
    if GetTextExtentPoint32W(dc, ws_s.as_ptr(), ws_s.len() as i32 - 1, &mut sz) != 0 { ws = sz.cx; }
    // 时间按最宽的数字串量宽：秒跳时整串宽度不变，不会左右抖
    if GetTextExtentPoint32W(dc, ws_ref.as_ptr(), 8, &mut sz) != 0 { wt = sz.cx; }
    let gap = ui::sc(layout::SD_TIME_GAP);
    let mut x = (bar.left + bar.right - (ws + gap + wt)) / 2;
    let mut sr = RECT { left: x, top: bar.top, right: x + ws, bottom: bar.bottom };
    DrawTextW(dc, ws_s.as_ptr(), -1, &mut sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    x += ws + gap;
    let mut tr2 = RECT { left: x, top: bar.top, right: x + wt, bottom: bar.bottom };
    DrawTextW(dc, ws_t.as_ptr(), -1, &mut tr2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

pub unsafe fn draw_item(item: &DRAWITEMSTRUCT) -> bool {
    let id = item.CtlID as usize;
    let r = item.rcItem;
    let pressed = item.itemState & ODS_SELECTED != 0;
    let locked = item.itemState & ODS_DISABLED != 0;
    let hot = !locked && HOVER.load(Ordering::Relaxed) == item.CtlID as i32;

    // 「每天」：自绘勾选（14 方框 + 6 + 正文），底色跟页面
    if id == ID_DAILY {
        let bg = if hot { ui::dim(ui::C_BG_TOP, 92) } else { ui::C_BG_TOP };
        fill(item.hDC, &r, bg);
        let bs = ui::sc(14);
        let by = r.top + (r.bottom - r.top - bs) / 2;
        let bx = RECT { left: r.left + ui::sc(1), top: by, right: r.left + ui::sc(1) + bs, bottom: by + bs };
        let on = DAILY.load(Ordering::Relaxed);
        fill(item.hDC, &bx, if on { ui::C_BTN_BLUE } else { ui::C_WHITE });
        frame(item.hDC, &bx, if on { ui::C_BTN_BLUE } else { ui::C_BORDER });
        if on {
            // 对勾：复用 ui-check 的 SVG 蒙版（与主界面、设置页同一处图形定义）
            svg_icon_blit(item.hDC, bx.left, by, bs, "ui-check",
                ui::C_BTN_BLUE, ui::C_WHITE);
        }
        let mut text = [0u16; 32];
        let n = GetWindowTextW(item.hwndItem, text.as_mut_ptr(), 32);
        SetBkMode(item.hDC, TRANSPARENT as i32);
        SetTextColor(item.hDC, ui::rgb(ui::C_TEXT));
        let of = SelectObject(item.hDC, ui::font(18, 500) as *mut winapi::ctypes::c_void);
        let mut tr = RECT { left: bx.right + ui::sc(6), top: r.top, right: r.right, bottom: r.bottom };
        DrawTextW(item.hDC, text.as_ptr(), n, &mut tr,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(item.hDC, of);
        return true;
    }

    // ▲▼：灰底 + 几何三角形（不用符号字体，21px 高里基线才稳）
    if arrow_target(id).is_some() {
        let base = ui::C_BTN_GR;
        let bg = if pressed { ui::dim(base, 80) } else if hot { ui::dim(base, 92) } else { base };
        fill(item.hDC, &r, bg);
        let cx = (r.left + r.right) / 2;
        let cy = (r.top + r.bottom) / 2;
        let hw = ui::sc(5);
        let hh = ui::sc(4);
        let pts = if arrow_up(id) {
            [POINT { x: cx, y: cy - hh }, POINT { x: cx - hw, y: cy + hh }, POINT { x: cx + hw, y: cy + hh }]
        } else {
            [POINT { x: cx, y: cy + hh }, POINT { x: cx - hw, y: cy - hh }, POINT { x: cx + hw, y: cy - hh }]
        };
        let tri = CreateSolidBrush(ui::rgb(if locked { ui::C_TEXT_MUTED } else { ui::C_TEXT }));
        let ob = SelectObject(item.hDC, tri as *mut winapi::ctypes::c_void);
        let op = SelectObject(item.hDC, GetStockObject(NULL_PEN as i32));
        Polygon(item.hDC, pts.as_ptr(), 3);
        SelectObject(item.hDC, op);
        SelectObject(item.hDC, ob);
        DeleteObject(tri as *mut winapi::ctypes::c_void);
        return true;
    }

    if !matches!(id, ID_START | ID_PAUSE | ID_CLEAR) { return false; }
    let busy = RUNNING.load(Ordering::Relaxed) && !PAUSED.load(Ordering::Relaxed);
    let base = if id == ID_START {
        ui::C_BTN_BLUE
    } else if id == ID_PAUSE {
        if busy { ui::C_DANGER } else { ui::C_BTN_GR }
    } else if busy || locked {
        ui::C_BTN_GR
    } else {
        ui::C_DANGER
    };
    let bg = if pressed { ui::dim(base, 80) } else if hot { ui::dim(base, 92) } else { base };
    fill(item.hDC, &r, bg);
    let mut text = [0u16; 32];
    let n = GetWindowTextW(item.hwndItem, text.as_mut_ptr(), 32);
    SetBkMode(item.hDC, TRANSPARENT as i32);
    SetTextColor(item.hDC, ui::rgb(ui::C_WHITE));
    let of = SelectObject(item.hDC, ui::font(22, 700) as *mut winapi::ctypes::c_void);
    let mut tr = r;
    DrawTextW(item.hDC, text.as_ptr(), n, &mut tr,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item.hDC, of);
    true
}
