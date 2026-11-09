#![windows_subsystem = "windows"]

mod prelude;
mod modules;
mod recvideo;
mod smart_erase;
mod ui;
mod ocr_layout;

use prelude::*;
use modules::settings;

pub(crate) use modules::capture::*;
pub(crate) use modules::config::*;
pub(crate) use modules::editor::canvas::*;
pub(crate) use modules::editor::options::*;
pub(crate) use modules::editor::paint::*;
pub(crate) use modules::editor::shapes::*;
pub(crate) use modules::editor::text::*;
pub(crate) use modules::filehash;
pub(crate) use modules::finalize::*;
pub(crate) use modules::icons::*;
pub(crate) use modules::ocr::*;
pub(crate) use modules::pin::*;
pub(crate) use modules::qr::*;
pub(crate) use modules::record::bar::*;
pub(crate) use modules::selection::*;
pub(crate) use modules::settings::*;
pub(crate) use modules::shutdown::*;
pub(crate) use modules::toolbar::*;
pub(crate) use modules::tooltip::*;
use modules::verify;

/// 逻辑像素 → 物理像素：唯一实现在 `ui::dpi`，这里只做转出。
pub(crate) use ui::dpi::sc;

/// 文件校验页的工作线程 → UI 线程的两条消息
const WM_VF_PROGRESS: UINT = winapi::um::winuser::WM_APP + 20;
const WM_VF_DONE: UINT = winapi::um::winuser::WM_APP + 21;

// DWM 合成接口：DwmFlush 让合成器立即应用隐藏/移动的窗口，避免抓屏还带旧画面
#[link(name = "dwmapi")]
extern "system" {
    fn DwmFlush() -> i32;
    // 9 = DWMWA_EXTENDED_FRAME_BOUNDS（不含 Win10 那圈看不见的窗口阴影）
    // 14 = DWMWA_CLOAKED（UWP 挂起 / 别的虚拟桌面时窗口还在但看不见）
    fn DwmGetWindowAttribute(hwnd: winapi::shared::windef::HWND, attr: u32, pv: *mut winapi::ctypes::c_void, cb: u32) -> i32;
}

// 选择文件夹（设置页“浏览…”。winapi 0.3.9 没包 SHBrowseForFolderW，这里自己声明）
#[link(name = "shell32")]
extern "system" {
    fn SHBrowseForFolderW(bi: *mut BROWSEINFOW) -> *mut winapi::ctypes::c_void;
    fn SHGetPathFromIDListW(pidl: *const winapi::ctypes::c_void, path: *mut u16) -> i32;
}

#[repr(C)]
struct BROWSEINFOW {
    hwnd_owner: HWND,
    pidl_root: *mut winapi::ctypes::c_void,
    psz_display_name: *mut u16,
    lpsz_title: *const u16,
    ul_flags: u32,
    lpfn: isize,
    l_param: LPARAM,
    i_image: i32,
}

// ——————————————————————— 全局兜底：锁毒化 / 崩溃落盘 ———————————————————————

/// 锁毒化的兜底。
///
/// `panic = "abort"` 下，某线程持锁时 panic 会让这把锁永久处于毒化状态，
/// 之后每个 `.lock_ok()` 都直接 panic —— 一次意外就变成整进程连锁消失。
/// 这里统一取回内部值（数据可能不完整，但程序还能继续跑），把"必崩"降级为"可能画错一帧"。
pub trait LockExt<T> {
    fn lock_ok(&self) -> std::sync::MutexGuard<'_, T>;
}

impl<T> LockExt<T> for std::sync::Mutex<T> {
    fn lock_ok(&self) -> std::sync::MutexGuard<'_, T> {
        self.lock().unwrap_or_else(|e| e.into_inner())
    }
}

/// 崩溃日志路径：%APPDATA%\<APP_NAME>\crash.log
fn crash_log_path() -> std::path::PathBuf {
    let base = std::env::var("APPDATA").unwrap_or_else(|_| ".".to_string());
    std::path::PathBuf::from(base).join(APP_NAME).join("crash.log")
}

/// 追加一行到崩溃日志（单文件超 256KB 轮转为 crash.1.log）
fn crash_log_line(s: &str) {
    let p = crash_log_path();
    if let Some(dir) = p.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    if let Ok(mt) = std::fs::metadata(&p) {
        if mt.len() > 256 * 1024 {
            let _ = std::fs::rename(&p, p.with_file_name("crash.1.log"));
        }
    }
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(&p) {
        use std::io::Write;
        let _ = f.write_all(s.as_bytes());
        let _ = f.write_all(b"\r\n");
        let _ = f.flush();
    }
}

/// 本地时间串（不引入 chrono）：yyyy-MM-dd HH:mm:ss.mmm
unsafe fn crash_stamp() -> String {
    use winapi::um::minwinbase::SYSTEMTIME;
    use winapi::um::sysinfoapi::GetLocalTime;
    let mut st: SYSTEMTIME = std::mem::zeroed();
    GetLocalTime(&mut st);
    format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds)
}

/// 未处理异常（访问违例 0xC0000005、栈溢出 0xC00000FD 等）回调。
/// panic hook 抓不到原生异常，而这两类在 `windows_subsystem = "windows"` 下完全静默。
unsafe extern "system" fn crash_exception_filter(
    info: *mut winapi::um::winnt::EXCEPTION_POINTERS) -> i32 {
    let mut code = 0u32;
    let mut addr = 0usize;
    if !info.is_null() {
        let rec = (*info).ExceptionRecord;
        if !rec.is_null() {
            code = (*rec).ExceptionCode as u32;
            addr = (*rec).ExceptionAddress as usize;
        }
    }
    crash_log_line(&format!("[{}] EXCEPTION code=0x{:08X} addr=0x{:X} thread={}",
        crash_stamp(), code, addr, GetCurrentThreadId()));
    1 /* EXCEPTION_CONTINUE_SEARCH：交给系统的默认处理，不改变行为 */
}

/// 崩溃可观测性：panic hook + 未处理异常回调。启动时调用一次，之后任何
/// panic / 访问违例会先落盘再退出，不会静默消失。
fn crash_log_init() {
    std::panic::set_hook(Box::new(|info| {
        let payload = if let Some(s) = info.payload().downcast_ref::<&str>() { (*s).to_string() }
            else if let Some(s) = info.payload().downcast_ref::<String>() { s.clone() }
            else { "unknown".to_string() };
        let loc = info.location()
            .map(|l| format!("{}:{}", l.file(), l.line())).unwrap_or_default();
        let bt = std::backtrace::Backtrace::force_capture();
        crash_log_line(&format!("[{}] PANIC {} @ {}\n{}",
            unsafe { crash_stamp() }, payload, loc, bt));
        std::process::abort();
    }));
    unsafe {
        winapi::um::errhandlingapi::SetUnhandledExceptionFilter(Some(crash_exception_filter));
    }
}


const WM_TRAYICON: UINT = WM_USER + 1;
/// 文件拖入消息（winuser.h 的 WM_DROPFILES = 0x0233）
const WM_DROPFILES_MSG: UINT = 0x0233;
const ID_TRAY_SHOW: usize = 2001;
const ID_TRAY_EXIT: usize = 2002;
const ID_TRAY_SETTINGS: usize = 2005;

// ───────────────────────────── 品牌标识 ─────────────────────────────
// 由 build.rs 按 Cargo feature 注入：不带 OCR 的构建 = SnapCore，带 OCR 的构建 = SnapOCR。
// 同一份源码出两个产物；窗口类名、单实例互斥体、注册表项、配置目录全部由它派生，
// 所以两个版本可以同时装、同时跑 —— 不会互抢实例，也不会互相覆盖配置。
const APP_NAME: &str = env!("APP_NAME");

/// 主窗口类
const CLS_MAIN: &str = concat!(env!("APP_NAME"), "_Main");
/// 框选遮罩
const CLS_SELWND: &str = concat!(env!("APP_NAME"), "_SelWnd");
/// 悬浮提示
const CLS_TIP: &str = concat!(env!("APP_NAME"), "_Tip");
/// 贴图窗口
const CLS_PIN: &str = concat!(env!("APP_NAME"), "_PinWnd");
/// 录制工具条
const CLS_RECBAR: &str = concat!(env!("APP_NAME"), "_RecBar");
/// 标注主窗 / 画布 / 子窗
const CLS_ANNO_MAIN: &str = concat!(env!("APP_NAME"), "_AnnoMain");
const CLS_ANNO_IMG: &str = concat!(env!("APP_NAME"), "_AnnoImg");
const CLS_ANNO_SUB: &str = concat!(env!("APP_NAME"), "_AnnoSub");
/// 文件校验结果框（vfverify 模块也用）
pub(crate) const CLS_VFBOX: &str = concat!(env!("APP_NAME"), "_VfBox");


// 页面切换标签（顶部，共5个：文字识别 / 二维码 / 文件校验 / 定时关机 / 设置）
const QR_TAB_QR: usize = 4120;    // “二维码”页
const QR_TAB_VF: usize = 4124;    // “文件校验”页（排在“定时关机”之后）
const QR_TAB_SHUT: usize = modules::shutdown::ID_TAB_SHUT; // “定时关机”页（4126，紧跟“二维码”）
const QR_TAB_SET: usize = 4122;   // “设置”页
const QR_TAB_OCR: usize = 4119;
const QR_TAB_RENAME: usize = modules::rename::TAB_ID; // “文件改名”页（4125）
const SEL_MODE_OCR: i32 = 1;          // 纯文字 OCR
const SEL_MODE_QR_THEN_OCR: i32 = 2; // 先二维码，无则文字 OCR
const PAGE_OCR: i32 = 0;
const PAGE_QR: i32 = 1;
const PAGE_SET: i32 = 3;
const PAGE_VF: i32 = verify::PAGE_VF;   // 「文件校验」页（= 5）
const PAGE_RENAME: i32 = modules::rename::PAGE_RENAME as i32; // 「文件改名」页（= 6）

// 顶部页面标签表（顺序即显示顺序）：控件 id 与标签文字一一对应。
// 增加页签只需往这张表里加一行，创建 / 摆位 / 自绘都从这里取。
const PAGE_TABS: [(usize, &str); 6] = [
    (QR_TAB_OCR, "文字识别"),
    (QR_TAB_QR, "二维码"),
    (QR_TAB_SHUT, "定时关机"),
    (QR_TAB_VF, "文件校验"),
    (QR_TAB_RENAME, modules::rename::TAB_LABEL),
    (QR_TAB_SET, "设置"),
];


// 当前悬停的按钮（1001=隐藏，1002=关闭，0=无）
static BTN_HOVER: AtomicI32 = AtomicI32::new(0);

// 主窗口句柄
static MAIN_HWND: AtomicIsize = AtomicIsize::new(0);

// 托盘重挂所需的最小状态。NOTIFYICONDATAW 里装的是裸窗口/图标句柄，不是 Send，
// 塞不进 static Mutex；这里只留重建它需要的三样纯值，重挂时现组一份出来。
static TRAY_HWND: AtomicIsize = AtomicIsize::new(0);
static TRAY_HICON: AtomicIsize = AtomicIsize::new(0);
static TRAY_TIP: Mutex<Vec<u16>> = Mutex::new(Vec::new());
// 任务栏重建消息 id（RegisterWindowMessageW("TaskbarCreated")，0 = 没注册上）
static TASKBAR_CREATED: AtomicU32 = AtomicU32::new(0);
// 托盘图标此刻是否真的挂在任务栏上（挂不上时关窗不能藏窗口，否则没有入口）
static TRAY_OK: AtomicBool = AtomicBool::new(false);


// 截图识别的一次性识别结果（与二维码当前内容 QR_TEXT 分开，避免污染）
static RECOG_RESULT: Mutex<String> = Mutex::new(String::new());

// DPI 感知：启动时按屏幕缩放系数把布局坐标等比放大，避免高分屏拉伸变形
static DPI_CACHE: AtomicI32 = AtomicI32::new(96);

// 输入框字体句柄
static EDIT_FONT: AtomicIsize = AtomicIsize::new(0);

// 输入框当前高度（随内容自动增高），用于动态布局
static INPUT_H: AtomicI32 = AtomicI32::new(101);

// 当前页面：0=文字识别, 1=二维码, 3=设置, 5=文件校验, 6=文件改名, 7=定时关机
// 启动固定停在二维码页
static PAGE_CUR: AtomicI32 = AtomicI32::new(PAGE_QR);
/// 上一帧显示的页面（用来判断"是不是刚离开文件校验页"）
static PAGE_LAST: AtomicI32 = AtomicI32::new(PAGE_QR);

fn dpi() -> i32 {
    let d = DPI_CACHE.load(Ordering::Relaxed);
    if d <= 0 { 96 } else { d }
}

pub(crate) fn wide(s: &str) -> Vec<u16> {
    OsStr::new(s).encode_wide().chain(std::iter::once(0)).collect()
}


// RGB 转 COLORREF（0xBBGGRR）
fn rgb(r: u32, g: u32, b: u32) -> u32 {
    (b << 16) | (g << 8) | r
}

/// 建一个字体（微软雅黑 + ClearType）。界面控件用。
/// 用完必须调 delete_font 释放，否则每次重画都泄漏一个 GDI 对象。
///
/// 即 lfHeight 取正 —— 正的 lfHeight 是"字符单元高"，负的才是"字形高"（后者画出来大一号）。
pub(crate) fn vf_font(height: i32, weight: i32) -> usize {
    unsafe {
        CreateFontW(height, 0, 0, 0, weight, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS,
            wide(ui::font::UI).as_ptr()) as usize
    }
}

/// 文件校验结果框的正文字体：等宽（Consolas）
///
/// 必须等宽而不是雅黑：64 位 SHA256 用雅黑量出来比等宽宽 20% 左右，
/// 一行放不下就会触发缩字号
pub(crate) fn vf_mono_font(height: i32, weight: i32) -> usize {
    unsafe {
        // 同样取正值（字符单元高）
        CreateFontW(height, 0, 0, 0, weight, 0, 0, 0,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, winapi::um::wingdi::FIXED_PITCH | winapi::um::wingdi::FF_MODERN,
            wide(ui::font::MONO).as_ptr()) as usize
    }
}

/// 释放 vf_font 建出来的字体
pub(crate) fn delete_font(f: usize) {
    unsafe {
        if f != 0 { DeleteObject(f as *mut winapi::ctypes::c_void); }
    }
}

/// 自绘进度条：轨道 + 已完成段 + 条内文字。
/// 文字分两段，都在条子内部：左边是「文件:」「总计:」标签，正中是百分比。
/// 两段都要跨"已填充 / 未填充"两种底色，所以各自按填充边界裁两次。
///
/// 标签贴左边不动、数字居中
/// 两行条子的数字上下对齐成一条竖线。
pub(crate) fn draw_progress_bar(dc: winapi::shared::windef::HDC, bar: &RECT, pct: f64,
    left: &str, center: &str) {
    unsafe {
        let fill_gr = CreateSolidBrush(ui::color::cref(ui::color::SURFACE_ALT));
        FillRect(dc, bar, fill_gr as HBRUSH);
        DeleteObject(fill_gr as *mut winapi::ctypes::c_void);

        let mut fill = *bar;
        fill.right = bar.left + (((bar.right - bar.left) as f64) * pct / 100.0) as i32;
        if fill.right > bar.right { fill.right = bar.right; }
        let has_fill = fill.right > bar.left;
        if has_fill {
            let blue = CreateSolidBrush(ui::color::cref(ui::color::ACCENT_DEEP));
            FillRect(dc, &fill, blue as HBRUSH);
            DeleteObject(blue as *mut winapi::ctypes::c_void);
        }

        SetBkMode(dc, TRANSPARENT as i32);
        let f = ui_font(sc(ui::font::SIZE_18), ui::font::W_SEMIBOLD);
        let old = SelectObject(dc, f as *mut winapi::ctypes::c_void);
        let pad = sc(ui::metric::SPACE_5);

        // 文字横跨两种底色：已填充段用白字、未填充段用深字，各自裁一次再画，
        // 看起来就是一个字被进度条"淹"过去。
        // 裁之前必须判空：pct=0 时填充段宽度为 0，IntersectClipRect 得到一个空区，
        // 两段都画不出来 —— 校验刚开始时两条进度条不能是整条空白（连 0% 都要显示）
        let seg = |s: &str, align: u32, l: i32, r: i32, color: u32| {
            if s.is_empty() || r <= l { return; } // 空裁剪区画不出东西，直接跳过
            let saved = winapi::um::wingdi::SaveDC(dc);
            winapi::um::wingdi::IntersectClipRect(dc, l, bar.top, r, bar.bottom);
            SetTextColor(dc, color);
            let mut t = *bar;
            // DT_LEFT 的值就是 0，"align & DT_LEFT != 0" 恒为假 —— 靠左的标签
            // 一直没拿到内缩，字是贴着条子左边缘画的。这里改判"不是居中"。
            if align & DT_CENTER == 0 { t.left += pad; }
            DrawTextW(dc, wide(s).as_ptr(), -1, &mut t,
                align | DT_VCENTER | DT_SINGLELINE);
            winapi::um::wingdi::RestoreDC(dc, saved);
        };
        // 每段都画两遍：深字段（填充终点 → 右边界，没填充就是整条）+ 白字段（左边 → 填充终点）
        let paint = |s: &str, align: u32| {
            seg(s, align, if has_fill { fill.right } else { bar.left }, bar.right,
                ui::color::cref(ui::color::TEXT));
            if has_fill {
                seg(s, align, bar.left, fill.right,
                    ui::color::cref(ui::color::SURFACE));
            }
        };
        paint(left, DT_LEFT);
        paint(center, DT_CENTER);

        SelectObject(dc, old);
    }
}

/// 界面字体缓存：(字族, 字高, 字重, 斜体) -> HFONT。
///
/// 若每帧 WM_DRAWITEM / WM_PAINT 都现建一次 CreateFontW 再 DeleteObject：
/// 主界面十来颗按钮，鼠标在按钮上划一下就是几十次字体创建/销毁，GDI 句柄表跟着抖，
/// 字体的字模还得反复解析。字号只跟 DPI 走、进程内不变，缓存起来常驻即可。
/// 返回的 HFONT 归缓存所有，调用方**不要** DeleteObject。
/// 三类字族共用一张表（界面雅黑 / 等宽 Consolas / 标注文本框雅黑），
/// 退出时 WM_DESTROY 一次性全还，不会留下没人管的句柄。
static FONT_CACHE: Mutex<Vec<(u8, i32, i32, bool, usize)>> = Mutex::new(Vec::new());

/// 界面文字：雅黑，**正**字高（字符单元高），ClearType
const FK_UI: u8 = 0;
/// 等宽：Consolas，正字高
const FK_MONO: u8 = 1;
/// 标注文本框正文：雅黑，**负**字高（字形高），灰度 AA
const FK_TX: u8 = 2;

fn cached_font(kind: u8, height: i32, weight: i32, italic: bool) -> usize {
    let mut c = FONT_CACHE.lock_ok();
    if let Some(&(_, _, _, _, f)) = c.iter()
        .find(|(k, h, w, i, _)| *k == kind && *h == height && *w == weight && *i == italic)
    {
        return f;
    }
    let f = match kind {
        FK_MONO => vf_mono_font(height, weight),
        FK_TX => unsafe { tx_font_new(height, weight, italic) },
        _ => vf_font(height, weight),
    };
    c.push((kind, height, weight, italic, f));
    f
}

/// 界面字体（微软雅黑）。返回的 HFONT 归缓存所有，调用方**不要** DeleteObject。
pub(crate) fn ui_font(height: i32, weight: i32) -> usize {
    cached_font(FK_UI, height, weight, false)
}

/// 等宽字体（Consolas）。vfverify 的结果框正文用它，与 ui_font 同表同生命周期。
pub(crate) fn box_font(height: i32, weight: i32) -> usize {
    cached_font(FK_MONO, height, weight, false)
}

/// 按**字形高**给的界面字体（px 传正数，内部取负）。
/// 副条 / 录制条 / 框选提示这类要跟像素对位的地方用它，别再每帧现建现删。
pub(crate) fn ui_font_px(px: i32, weight: i32) -> usize {
    cached_font(FK_UI, -(px.max(1)), weight, false)
}

/// 量字体用的常驻内存 DC。
///
/// anno_tx_metrics / anno_tx_advances 在文本框绘制时每行都要调一次，
/// 每次 CreateCompatibleDC + DeleteDC 是纯句柄池开销，常驻复用即可。
/// 退出时随 WM_DESTROY 一起释放。
static TX_MDC: AtomicIsize = AtomicIsize::new(0);

unsafe fn tx_mdc() -> winapi::shared::windef::HDC {
    let d = TX_MDC.load(Ordering::Relaxed) as winapi::shared::windef::HDC;
    if !d.is_null() { return d; }
    let d = CreateCompatibleDC(std::ptr::null_mut());
    TX_MDC.store(d as isize, Ordering::Relaxed);
    d
}

// ————————————————————— GDI 共用样板 —————————————————————
//
// 下面几件是全程序反复出现的样板：常驻缓冲、一次性双缓冲、纯色填充、
// 鼠标离开登记、窗口类注册。各处自己抄一份（三份缓冲实现、八份双缓冲、
// 七份类注册），漏一次还原/删除就是一个常驻 GDI 泄漏。统一收到这里。

/// 常驻内存 DC + 位图（按尺寸复用，尺寸变了才重建）。
///
/// 主界面 / 框选遮罩 / 标注画布三处高频重绘都用它：主界面每帧重建一张
/// 585×445 位图，画布在 4K 下每帧 33MB，重建成本全落在帧率上。
/// `dib = true` 用 32bpp top-down DIB 段（遮罩要按 BGRA 直接贴整屏原图）。
pub(crate) struct BufDc {
    dc: AtomicIsize,
    bmp: AtomicIsize,
    /// DIB 段的像素指针（只有 dib=true 时有值）：调用方可以直接按行 memcpy，
    /// 省掉每次绘制把整张 CPU 图交给 GDI 转换
    bits: AtomicIsize,
    w: AtomicI32,
    h: AtomicI32,
    /// 最近一次取用的时刻（`GetTickCount64`，毫秒）。闲置超时据此释放。
    use_tick: AtomicI64,
    dib: bool,
}

impl BufDc {
    pub(crate) const fn new(dib: bool) -> Self {
        BufDc {
            dc: AtomicIsize::new(0), bmp: AtomicIsize::new(0), bits: AtomicIsize::new(0),
            w: AtomicI32::new(0), h: AtomicI32::new(0),
            use_tick: AtomicI64::new(0), dib,
        }
    }

    /// DIB 段的像素起始地址（top-down、32bpp、stride = 宽 × 4）。
    /// 非 DIB 或没建时为 null
    pub(crate) unsafe fn bits(&self, w: i32, h: i32) -> *mut u8 {
        if self.get(w, h).is_none() { return std::ptr::null_mut(); }
        self.bits.load(Ordering::Relaxed) as *mut u8
    }

    /// 闲置超过 `idle_ms` 就把位图还给系统，下次 `get` 自动重建。
    /// 只改释放时机，不改绘制内容：绘制期间缓冲一定处于"刚取用"状态。
    pub(crate) unsafe fn trim(&self, idle_ms: i64) {
        if self.dc.load(Ordering::Relaxed) == 0 { return; }
        if tick_ms() - self.use_tick.load(Ordering::Relaxed) < idle_ms { return; }
        self.free();
    }

    pub(crate) unsafe fn free(&self) {
        let bmp = self.bmp.swap(0, Ordering::Relaxed) as *mut winapi::ctypes::c_void;
        let dc = self.dc.swap(0, Ordering::Relaxed) as winapi::shared::windef::HDC;
        self.bits.store(0, Ordering::Relaxed);
        if !dc.is_null() { DeleteDC(dc); }       // DC 只能用 DeleteDC 还
        if !bmp.is_null() { DeleteObject(bmp); }
        self.w.store(0, Ordering::Relaxed);
        self.h.store(0, Ordering::Relaxed);
    }

    pub(crate) unsafe fn get(&self, w: i32, h: i32) -> Option<winapi::shared::windef::HDC> {
        if w <= 0 || h <= 0 { return None; }
        let dc = self.dc.load(Ordering::Relaxed) as winapi::shared::windef::HDC;
        if !dc.is_null()
            && self.w.load(Ordering::Relaxed) == w
            && self.h.load(Ordering::Relaxed) == h
        {
            self.use_tick.store(tick_ms(), Ordering::Relaxed);
            return Some(dc);
        }
        self.free();
        let sdc = GetDC(std::ptr::null_mut());
        if sdc.is_null() { return None; }
        let mut bits_ptr: *mut winapi::ctypes::c_void = std::ptr::null_mut();
        let bmp = if self.dib {
            let mut bmi: BITMAPINFO = std::mem::zeroed();
            bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
            bmi.bmiHeader.biWidth = w;
            bmi.bmiHeader.biHeight = -h;   // top-down，与整屏 BGRA 副本同一朝向
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            CreateDIBSection(sdc, &mut bmi, DIB_RGB_COLORS, &mut bits_ptr, std::ptr::null_mut(), 0)
        } else {
            CreateCompatibleBitmap(sdc, w, h)
        };
        ReleaseDC(std::ptr::null_mut(), sdc);
        if bmp.is_null() { return None; }
        let mdc = CreateCompatibleDC(std::ptr::null_mut());
        if mdc.is_null() {
            DeleteObject(bmp as *mut winapi::ctypes::c_void);
            return None;
        }
        SelectObject(mdc, bmp as *mut winapi::ctypes::c_void);
        self.dc.store(mdc as isize, Ordering::Relaxed);
        self.bmp.store(bmp as isize, Ordering::Relaxed);
        self.bits.store(if self.dib { bits_ptr as isize } else { 0 }, Ordering::Relaxed);
        self.w.store(w, Ordering::Relaxed);
        self.h.store(h, Ordering::Relaxed);
        self.use_tick.store(tick_ms(), Ordering::Relaxed);
        Some(mdc)
    }
}

/// 单调毫秒时钟（`GetTickCount64` 不受系统时间调整影响）
pub(crate) fn tick_ms() -> i64 {
    unsafe { winapi::um::sysinfoapi::GetTickCount64() as i64 }
}

/// 主界面重绘缓冲（第一次绘制时建，闲置 3 秒释放）
static MAIN_BUF: BufDc = BufDc::new(false);
/// 标注画布的全屏合成缓冲（DIB 段：像素可由我们自己按行写，刷新时只 BitBlt 脏矩形）
static ANNO_BUF: BufDc = BufDc::new(true);

unsafe fn main_buf_dc(w: i32, h: i32) -> Option<winapi::shared::windef::HDC> { MAIN_BUF.get(w, h) }
unsafe fn main_buf_free() { MAIN_BUF.free() }
unsafe fn anno_buf_dc(w: i32, h: i32) -> Option<winapi::shared::windef::HDC> { ANNO_BUF.get(w, h) }
/// 合成缓冲的 DIB 像素指针（top-down、32bpp、stride = 宽 × 4）
pub(crate) unsafe fn anno_buf_bits(w: i32, h: i32) -> *mut u8 { ANNO_BUF.bits(w, h) }
unsafe fn anno_buf_free() { ANNO_BUF.free() }

/// 整屏 / 大块绘制缓冲的闲置回收定时器（主窗口，1 秒一跳）
pub(crate) const TRIM_TIMER: usize = 95;
/// 闲置多久算"没人用了"：3 秒。重建一张 1080p 位图是毫秒级，比常驻 8MB 划算。
const BUF_IDLE_MS: i64 = 3000;

/// 把所有按需缓冲里闲置超时的还回系统；下次绘制自动重建，画面不变。
pub(crate) unsafe fn bufs_trim_idle() {
    MAIN_BUF.trim(BUF_IDLE_MS);
    // 标注窗开着就一直留着这张整屏缓冲：停手超过 3 秒就回收的话，
    // 下一笔的第一帧要先重建一整张 4K 位图（33MB），手感上就是"卡一下"
    if ANNO_HWND.load(Ordering::Relaxed) == 0 { ANNO_BUF.trim(BUF_IDLE_MS); }
    sel_buf_trim(BUF_IDLE_MS);
}

/// WM_PAINT 用的一次性双缓冲：建 DC + 兼容位图，`present` 贴屏，离开作用域自动还原/释放。
///
/// 每个窗口过程各写一遍"建 → 选 → 画 → BitBlt → 还原 → 删"，
/// 中途 return 就必然漏一组句柄；这里把建与还配成一对，Drop 兜底。
pub(crate) struct PaintBuf {
    dc: winapi::shared::windef::HDC,
    bmp: *mut winapi::ctypes::c_void,
    old: *mut winapi::ctypes::c_void,
    w: i32,
    h: i32,
}

impl PaintBuf {
    /// 建不出来（尺寸为 0 / GDI 耗尽）返回 None，调用方退回直接画窗口 DC。
    pub(crate) unsafe fn new(hdc: winapi::shared::windef::HDC, w: i32, h: i32) -> Option<PaintBuf> {
        if w <= 0 || h <= 0 { return None; }
        let dc = CreateCompatibleDC(hdc);
        if dc.is_null() { return None; }
        let bmp = CreateCompatibleBitmap(hdc, w, h);
        if bmp.is_null() { DeleteDC(dc); return None; }
        let old = SelectObject(dc, bmp as *mut winapi::ctypes::c_void);
        Some(PaintBuf { dc, bmp: bmp as *mut winapi::ctypes::c_void, old, w, h })
    }

    pub(crate) fn dc(&self) -> winapi::shared::windef::HDC { self.dc }

    /// 把整块缓冲贴回窗口 DC
    pub(crate) unsafe fn present(&self, hdc: winapi::shared::windef::HDC) {
        BitBlt(hdc, 0, 0, self.w, self.h, self.dc, 0, 0, SRCCOPY);
    }
}

impl Drop for PaintBuf {
    fn drop(&mut self) {
        unsafe {
            if !self.old.is_null() { SelectObject(self.dc, self.old); }
            if !self.bmp.is_null() { DeleteObject(self.bmp); }
            if !self.dc.is_null() { DeleteDC(self.dc); }
        }
    }
}

/// 纯色填一块矩形（刷子建完立刻还）。传 GDI COLORREF（`ui::color::cref`）。
pub(crate) unsafe fn fill_solid(dc: winapi::shared::windef::HDC, rc: &RECT, cref: u32) {
    let b = CreateSolidBrush(cref);
    FillRect(dc, rc, b as HBRUSH);
    DeleteObject(b as *mut winapi::ctypes::c_void);
}

/// 登记"鼠标离开"通知（WM_MOUSEMOVE 里第一次进入时调）。
pub(crate) unsafe fn track_leave(hwnd: HWND) {
    let mut t: TRACKMOUSEEVENT = std::mem::zeroed();
    t.cbSize = std::mem::size_of::<TRACKMOUSEEVENT>() as u32;
    t.dwFlags = TME_LEAVE;
    t.hwndTrack = hwnd;
    TrackMouseEvent(&mut t);
}

/// 注册窗口类（9 字段样板）。已注册时忽略返回值。
/// `cursor` 传 IDC_* 的整数值，`icon` = 是否用本程序图标（主窗用）。
pub(crate) unsafe fn reg_class(
    name: &str,
    proc: Option<unsafe extern "system" fn(HWND, UINT, WPARAM, LPARAM) -> LRESULT>,
    style: u32, cursor: usize, icon: bool,
) {
    let hinstance = GetModuleHandleW(std::ptr::null_mut());
    let cls = wide(name);
    let hicon = if icon { LoadIconW(hinstance, 1 as *const u16) } else { std::ptr::null_mut() };
    let wc = WNDCLASSEXW {
        cbSize: std::mem::size_of::<WNDCLASSEXW>() as u32,
        style,
        lpfnWndProc: proc,
        cbClsExtra: 0, cbWndExtra: 0,
        hInstance: hinstance,
        hIcon: hicon,
        hCursor: LoadCursorW(std::ptr::null_mut(), cursor as *const u16),
        hbrBackground: std::ptr::null_mut(),
        lpszMenuName: std::ptr::null(),
        lpszClassName: cls.as_ptr(),
        hIconSm: hicon,
    };
    RegisterClassExW(&wc);
}

// 本程序 exe 的完整路径
fn exe_path() -> String {
    unsafe {
        let mut buf = [0u16; 1024];
        let n = GetModuleFileNameW(std::ptr::null_mut(), buf.as_mut_ptr(), 1024);
        String::from_utf16_lossy(&buf[..n as usize]).trim().to_string()
    }
}


// 文件拖入：一律送到「文件校验」页（二维码页不再接图片——问题/文件统一回落到校验）
unsafe fn handle_drop(hwnd: HWND, drop: HDROP) {
    let n = DragQueryFileW(drop, 0xFFFF_FFFF, std::ptr::null_mut(), 0);
    let mut files: Vec<String> = Vec::new();
    for i in 0..n {
        let len = DragQueryFileW(drop, i, std::ptr::null_mut(), 0) as usize;
        if len == 0 { continue; }
        let mut buf = vec![0u16; len + 1];
        DragQueryFileW(drop, i, buf.as_mut_ptr(), len as u32 + 1);
        files.push(String::from_utf16_lossy(&buf[..len]));
    }
    DragFinish(drop); // 句柄用完立刻释放，后面的耗时操作不占着它
    if files.is_empty() { return; }

    switch_page(hwnd, PAGE_VF);
    verify::vf_start(hwnd, files);
}

/// 拖入路由：改名页收下加入列表，其余页面一律走文件校验。
unsafe fn route_drop(hwnd: HWND, drop: HDROP) {
    if PAGE_CUR.load(Ordering::Relaxed) == PAGE_RENAME {
        modules::rename::on_drop_files(drop);
    } else {
        handle_drop(hwnd, drop);
    }
}

// ---- 子控件拖放转发 ----
// WM_DROPFILES 只发给「光标下那个登记过接收的窗口」，不会冒泡到父窗口。
// 主窗口里几乎每块地方都是子控件（页签按钮、输入框、结果框…），它们没登记就整片拒收，
// 表现就是「只有窗口空白处能拖、拖到框里没反应」。这里给每个子控件登记接收，
// 再挂子类化把 WM_DROPFILES 统一转给主窗口的 handle_drop。
static DROP_SUBCLASS: Mutex<Vec<(isize, isize)>> = Mutex::new(Vec::new());

/// 让一个窗口真正能接收文件拖入。三件事缺一不可：
///   1) WS_EX_ACCEPTFILES + DragAcceptFiles —— 传统 WM_DROPFILES 的前提；
///   2) ChangeWindowMessageFilterEx 放行 WM_DROPFILES —— 本程序以管理员权限运行时，
///      UIPI 会把普通权限资源管理器发来的这条消息整条过滤掉，看起来就是「怎么拖都没反应」；
///   3) 放行 WM_COPYGLOBALDATA(0x0049) —— 拖拽过程中传数据用的，不放行连拖拽造型都不出现。
/// 这几步对普通权限运行没有任何副作用（本来就允许的消息再放行一次是空操作）。
unsafe fn drop_allow(hwnd: HWND) {
    if hwnd.is_null() { return; }
    // MSGFLT_ALLOW = 1（winapi 0.3.9 没导出这个常量，直接按 winuser.h 的值给）
    const MSGFLT_ALLOW: u32 = 1;
    ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES_MSG, MSGFLT_ALLOW, std::ptr::null_mut());
    ChangeWindowMessageFilterEx(hwnd, 0x0049 /*WM_COPYGLOBALDATA*/, MSGFLT_ALLOW, std::ptr::null_mut());
    let st = GetWindowLongPtrW(hwnd, winapi::um::winuser::GWL_EXSTYLE);
    if st & (WS_EX_ACCEPTFILES as isize) == 0 {
        SetWindowLongPtrW(hwnd, winapi::um::winuser::GWL_EXSTYLE, st | WS_EX_ACCEPTFILES as isize);
    }
    DragAcceptFiles(hwnd, 1);
}

unsafe extern "system" fn drop_fwd_proc(hwnd: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    if msg == WM_DROPFILES_MSG {
        let mut root = MAIN_HWND.load(Ordering::Relaxed) as HWND;
        if root.is_null() { root = GetParent(hwnd); }
        // HDROP 在 wParam（不是 lParam）
        if !root.is_null() { route_drop(root, wp as HDROP); }
        return 0;
    }
    let old = {
        let v = DROP_SUBCLASS.lock_ok();
        v.iter().rev().find(|(h, _)| *h == hwnd as isize).map(|(_, p)| *p).unwrap_or(0)
    };
    if old == 0 { return DefWindowProcW(hwnd, msg, wp, lp); }
    CallWindowProcW(std::mem::transmute(old), hwnd, msg, wp, lp)
}

unsafe extern "system" fn drop_enum_child(child: HWND, _lp: LPARAM) -> i32 {
    // 先查重（短暂持锁），绝不能把锁跨着下面几个调用：
    // SetWindowLongPtrW(GWL_EXSTYLE) 会给窗口发 WM_STYLECHANGED，那时钩子已生效，
    // 钩子里再取同一把锁就自锁死——表现是启动时卡在 WM_CREATE，窗口一直不显示。
    {
        let v = DROP_SUBCLASS.lock_ok();
        if v.iter().any(|(h, _)| *h == child as isize) { return 1; }
    }
    let old = GetWindowLongPtrW(child, GWLP_WNDPROC);
    if old == drop_fwd_proc as *const () as isize { return 1; }
    DROP_SUBCLASS.lock_ok().push((child as isize, old));
    drop_allow(child);
    SetWindowLongPtrW(child, GWLP_WNDPROC, drop_fwd_proc as *const () as isize);
    1
}

/// 主窗口所有子控件登记文件拖放（控件建完后调；重复调用自动跳过已挂的）
unsafe fn enable_child_drop(hwnd: HWND) {
    EnumChildWindows(hwnd, Some(drop_enum_child), 0);
}


// 顶部页签排布：每个页签宽度 = 文字实测宽度 + 左右内边距，从左依次排开。
// 以后要加页签，只往下面的表里加一行即可，位置/宽度自动算，不用手改坐标。
unsafe fn place_tabs(hwnd: HWND) {
    let hdc = GetDC(hwnd);
    if hdc.is_null() { return; }
    let f = CreateFontW(sc(ui::font::SIZE_18), 0, 0, 0, ui::font::W_MEDIUM, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS,
        wide(ui::font::UI).as_ptr());
    let oldf = SelectObject(hdc, f as *mut winapi::ctypes::c_void);
    let mut x = sc(ui::metric::SPACE_6);
    for (id, label) in PAGE_TABS {
        let w = wide(label);
        let mut sz: SIZE = std::mem::zeroed();
        GetTextExtentPoint32W(hdc, w.as_ptr(), label.encode_utf16().count() as i32, &mut sz);
        let bw = sz.cx + sc(ui::layout::main_page::TAB_PAD_X);
        let b = GetDlgItem(hwnd, id as i32);
        if !b.is_null() {
            SetWindowPos(b, std::ptr::null_mut(), x,
                sc(ui::layout::main_page::TAB_Y), bw, sc(ui::layout::main_page::TAB_H), SWP_NOZORDER);
            x += bw + sc(ui::layout::main_page::TAB_GAP);
        }
    }
    SelectObject(hdc, oldf);
    DeleteObject(f as *mut winapi::ctypes::c_void);
    ReleaseDC(hwnd, hdc);
}

// 统一布局：窗口固定 585×445，边距 14，纵向间隙 10，按钮 132×46、按钮间间隙 12
unsafe fn relayout(hwnd: HWND) {
    let p = PAGE_CUR.load(Ordering::Relaxed);
    // 顶部四页签：宽度按文字实际宽度自适应，从左依次排开（以后加页签只需往 place_tabs 的表里加一行）
    place_tabs(hwnd);

    if p == PAGE_QR {
        // 二维码页排版全部归 modules/qr.rs（输入框 + 左侧一列按钮）
        modules::qr::relayout(hwnd);
    } else if p == PAGE_OCR {
        let sbox = GetDlgItem(hwnd, OCR_SEARCH_EDIT as i32);
        if !sbox.is_null() {
            SetWindowPos(sbox, std::ptr::null_mut(),
                sc(ui::layout::ocr::SEARCH_X), sc(ui::layout::ocr::SEARCH_Y),
                sc(ui::layout::ocr::SEARCH_W), sc(ui::layout::ocr::ROW_H), SWP_NOZORDER);
        }
        let sb = GetDlgItem(hwnd, OCR_BTN_SEARCH as i32);
        if !sb.is_null() {
            SetWindowPos(sb, std::ptr::null_mut(),
                sc(ui::layout::ocr::FIND_X), sc(ui::layout::ocr::SEARCH_Y),
                sc(ui::layout::ocr::FIND_W), sc(ui::layout::ocr::ROW_H), SWP_NOZORDER);
        }
        // 排版档位切换
        let lay = GetDlgItem(hwnd, OCR_BTN_LAYOUT as i32);
        if !lay.is_null() {
            SetWindowPos(lay, std::ptr::null_mut(),
                sc(ui::layout::ocr::LAYOUT_X), sc(ui::layout::ocr::SEARCH_Y),
                sc(ui::layout::ocr::LAYOUT_W), sc(ui::layout::ocr::ROW_H), SWP_NOZORDER);
        }
        let ocr = GetDlgItem(hwnd, OCR_EDIT as i32);
        if !ocr.is_null() {
            SetWindowPos(ocr, std::ptr::null_mut(),
                sc(ui::layout::ocr::SEARCH_X), sc(ui::layout::ocr::EDIT_Y),
                sc(ui::layout::ocr::EDIT_W), sc(ui::layout::ocr::EDIT_H), SWP_NOZORDER);
        }
        // 三个按钮彼此独立（宽132，间隙12）
        for (id, x) in [(OCR_BTN_REC, ui::layout::ocr::BTN_X[0]),
                        (OCR_BTN_COPY, ui::layout::ocr::BTN_X[1]),
                        (OCR_BTN_CLEAR, ui::layout::ocr::BTN_X[2])] {
            let bd = GetDlgItem(hwnd, id as i32);
            if !bd.is_null() {
                SetWindowPos(bd, std::ptr::null_mut(), sc(x), sc(ui::layout::ocr::BTN_Y),
                    sc(ui::layout::ocr::BTN_W), sc(ui::layout::ocr::BTN_H), SWP_NOZORDER);
            }
        }
        // “识别 N 字符”显示：紧贴清空右侧
        let cnt = GetDlgItem(hwnd, OCR_LBL_COUNT as i32);
        if !cnt.is_null() {
            SetWindowPos(cnt, std::ptr::null_mut(), sc(ui::layout::ocr::COUNT_X),
                sc(ui::layout::ocr::BTN_Y), sc(ui::layout::ocr::COUNT_W),
                sc(ui::layout::ocr::BTN_H), SWP_NOZORDER);
        }
    } else if p == PAGE_SHUTDOWN {
        modules::shutdown::relayout(hwnd);
    } else if p == PAGE_VF {
        // 文件校验页：结果框 + 按钮行 + 右侧四个算法勾选（网格在 vfverify 模块里）
        verify::vf_relayout(hwnd);
    } else if p == PAGE_SET {
        settings::relayout(hwnd);
    } else if p == PAGE_RENAME {
        // 文件改名页：工具条 / 五组面板 / 列表 / 页底格式条（动态列按实测字宽现算）
        modules::rename::relayout(hwnd);
    }
    // 所有页面统一窗口尺寸：客户区随 DPI 缩放（585×445 @100%），与控件同比例，避免高分屏控件溢出。
    // 只有尺寸真变了才调 SetWindowPos：尺寸相同也会派发 WM_SIZE，而 WM_SIZE 里改名页要
    // 再跑一遍 relayout（几十次 WM_SETFONT 强制重画），切页时等于把整页重画两遍。
    let style = GetWindowLongPtrW(hwnd, GWL_STYLE) as u32;
    let mut rc = RECT { left: 0, top: 0, right: sc(ui::metric::WIN_W), bottom: sc(ui::metric::WIN_H) };
    AdjustWindowRect(&mut rc, style, 0);
    let mut now: RECT = std::mem::zeroed();
    GetWindowRect(hwnd, &mut now);
    if now.right - now.left != rc.right - rc.left || now.bottom - now.top != rc.bottom - rc.top {
        SetWindowPos(hwnd, std::ptr::null_mut(), 0, 0,
            rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
    }
    // bErase=0：父窗口 WM_ERASEBKGND 本就返回 1 不擦，这里再让标准子控件擦一层
    // 默认灰底就纯属多一帧白/灰闪。整页底色由 WM_PAINT 的缓冲一次铺满。
    InvalidateRect(hwnd, std::ptr::null_mut(), 0);
}

// ===================== 定时关机页 =====================
// 控件布局 / 绘制 / 状态 / 到点执行全部在 modules/shutdown.rs；主线只保留关机调用。

/// 到点执行关机：启用关机权限后调用 `ExitWindowsEx`。
/// Err 的文案由模块弹提示（privilegeFail / shutdownFail）
pub(crate) fn sd_execute(_hwnd: HWND) -> Result<(), &'static str> {
    unsafe {
        let cur = winapi::um::processthreadsapi::GetCurrentProcess();
        let mut htok: winapi::shared::ntdef::HANDLE = std::ptr::null_mut();
        let ok = winapi::um::processthreadsapi::OpenProcessToken(
            cur,
            winapi::um::winnt::TOKEN_ADJUST_PRIVILEGES | winapi::um::winnt::TOKEN_QUERY,
            &mut htok);
        if ok == 0 || htok.is_null() { return Err("权限不足，无法关机"); }
        let mut luid: winapi::um::winnt::LUID = std::mem::zeroed();
        if winapi::um::winbase::LookupPrivilegeValueW(
            std::ptr::null_mut(), wide("SeShutdownPrivilege").as_ptr(), &mut luid) == 0
        {
            winapi::um::handleapi::CloseHandle(htok);
            return Err("权限不足，无法关机");
        }
        let mut tp: winapi::um::winnt::TOKEN_PRIVILEGES = std::mem::zeroed();
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = winapi::um::winnt::SE_PRIVILEGE_ENABLED;
        winapi::um::errhandlingapi::SetLastError(0);
        let adj = winapi::um::securitybaseapi::AdjustTokenPrivileges(
            htok, 0, &mut tp, 0, std::ptr::null_mut(), std::ptr::null_mut());
        let assigned = winapi::um::errhandlingapi::GetLastError() == 0;
        winapi::um::handleapi::CloseHandle(htok);
        if adj == 0 || !assigned { return Err("权限不足，无法关机"); }
        if ExitWindowsEx(EWX_POWEROFF | EWX_FORCE, SHTDN_REASON_MAJOR_OTHER) == 0 {
            return Err("关机调用失败，请检查系统权限");
        }
        Ok(())
    }
}


// 显示/隐藏各页面控件
fn apply_page(hwnd: HWND) {
    unsafe {
        let p = PAGE_CUR.load(Ordering::Relaxed);
        // 没编入 OCR 引擎时这一页根本不存在：万一从旧配置/残留状态恢复到 PAGE_OCR，
        // 就地纠正回二维码页，否则会停在一个空白的客户区上。
        let p = if p == PAGE_OCR && !cfg!(feature = "ocr") {
            PAGE_CUR.store(PAGE_QR, Ordering::Relaxed);
            PAGE_QR
        } else {
            p
        };
        // 文字识别页已重新接入：允许停留在 PAGE_OCR，不再强弹回二维码页
        // 离开「文件校验」页就整页清空：结果、粘贴值、对比基线、结论、进度都不留，
        // 下次进来是从"首次校验"开始
        let last = PAGE_LAST.load(Ordering::Relaxed);
        if last == PAGE_VF && p != PAGE_VF {
            verify::vf_leave(hwnd);
        }
        PAGE_LAST.store(p, Ordering::Relaxed);
        // 文字识别页（已重新接入：控件按页显隐，结果回填由 WM_QR_RESULT 处理）
        for id in [OCR_EDIT, OCR_SEARCH_EDIT, OCR_BTN_SEARCH, OCR_BTN_LAYOUT, OCR_BTN_REC, OCR_BTN_COPY, OCR_BTN_CLEAR, OCR_LBL_COUNT] {
            let c = GetDlgItem(hwnd, id as i32);
            if !c.is_null() { ShowWindow(c, if p == PAGE_OCR { SW_SHOW } else { SW_HIDE }); }
        }
        // 二维码页：本页控件显隐 + 卡片重绘（modules/qr.rs）
        modules::qr::show_page(hwnd, p == PAGE_QR);
        // 定时关机页
        // 定时关机页：整页控件显隐归 modules/shutdown.rs
        modules::shutdown::show_page(hwnd, p == PAGE_SHUTDOWN);
        // 设置页：切走页面时若还在录制快捷键，先结束录制（并把全局热键注册回去）
        if p != PAGE_SET && CAPTURING.load(Ordering::Relaxed) { cap_cancel(hwnd); }
        // 设置页：摆位/显隐/自绘都归 modules/settings.rs
        settings::show_page(hwnd, p == PAGE_SET);
        // 文件校验页（结果框 + 按钮 + 四个算法勾选）
        verify::vf_show_page(hwnd, p == PAGE_VF);
        // 文件改名页：整页控件显隐（工具条 / 面板组 / 列表 / 编辑框）
        modules::rename::show_page(hwnd, p == PAGE_RENAME);
        // 文字识别页：按当前结果刷新“识别 N 字符”显示（空则隐藏）
        if p == PAGE_OCR { update_ocr_count(hwnd); }
        InvalidateRect(hwnd, std::ptr::null_mut(), 0);
        // 标签按钮是子窗口，父窗口失效不会重绘它们——逐个失效，保证选中高亮/未选不亮正确刷新
        for id in [QR_TAB_OCR, QR_TAB_QR, QR_TAB_SHUT, QR_TAB_VF, QR_TAB_RENAME, QR_TAB_SET] {
            let t = GetDlgItem(hwnd, id as i32);
            if !t.is_null() { InvalidateRect(t, std::ptr::null_mut(), 0); }
        }
        // 每次切页都把（可能刚建/刚显示出来的）子控件补登记一次文件拖放 ——
        // 只登记一次的话，后续动态创建的控件会整片拒收拖放
        enable_child_drop(hwnd);
    }
}

/// 切页统一入口：把「改页号 → 整页显隐 → 重排」压成**一次**重绘。
///
/// 这三步里每一步都在往屏幕上写：每个 ShowWindow / SetWindowPos 都让父窗口和
/// 子控件失效，apply_page 中途还有模块用 UpdateWindow / RDW_UPDATENOW 同步刷新，
/// 改名页 relayout 更会对几十个控件发 WM_SETFONT(重绘=TRUE) 强制同步重画；父窗口
/// 在设置 / 定时关机 / 文件校验页又是直接往窗口 DC 上画几十次。于是「隐藏一半 →
/// 画一半 → 再隐藏一半」的多个中间帧直接可见，就是切页闪烁；改名页控件最多，最明显。
///
/// 只冻结**父窗口**：父窗 WM_PAINT 里那几十次 FillRect 不会一帧帧写屏。
/// **绝不能对子控件逐个发 WM_SETREDRAW(FALSE)** —— 实测解冻时被隐藏的控件会恢复成
/// 「可见」，整页隐藏直接失效，结果是所有页的控件叠在一起。
unsafe fn switch_page(hwnd: HWND, page: i32) {
    use winapi::um::winuser::{RDW_ALLCHILDREN, RDW_INVALIDATE, RDW_NOERASE, RedrawWindow, WM_SETREDRAW};

    SendMessageW(hwnd, WM_SETREDRAW, 0, 0);

    PAGE_CUR.store(page, Ordering::Relaxed);
    apply_page(hwnd);
    relayout(hwnd);

    SendMessageW(hwnd, WM_SETREDRAW, 1, 0);
    // RDW_NOERASE：自绘按钮 / 编辑框都自己铺满矩形，不必先擦一层默认灰底。
    RedrawWindow(hwnd, std::ptr::null_mut(), std::ptr::null_mut(),
        RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
}

/// 把托盘图标登记回任务栏。两个时机会用到：首次登记失败后的补登记，
/// 以及 explorer 重建任务栏（重启/崩溃被拉起）之后的重新登记。
/// 句柄和图标都是本进程自己的，重建一次 NOTIFYICONDATAW 就能挂回去。
unsafe fn tray_attach() -> bool {
    let hwnd = TRAY_HWND.load(Ordering::Relaxed) as HWND;
    if hwnd.is_null() {
        return false;
    }
    let mut nid: NOTIFYICONDATAW = std::mem::zeroed();
    nid.cbSize = std::mem::size_of::<NOTIFYICONDATAW>() as u32;
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = TRAY_HICON.load(Ordering::Relaxed) as winapi::shared::windef::HICON;
    // 锁只圈住取字符串这一步：Shell_NotifyIconW 要跟 explorer 通信，
    // 不能把锁跨着系统调用持有，否则 explorer 卡住会连带卡死 UI 线程
    {
        let tip = TRAY_TIP.lock_ok();
        let n = tip.len().min(128);
        nid.szTip[..n].copy_from_slice(&tip[..n]);
    }
    let ok = Shell_NotifyIconW(NIM_ADD, &mut nid) != 0;
    TRAY_OK.store(ok, Ordering::Relaxed);
    ok
}

extern "system" fn wnd_proc(hwnd: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    // 任务栏重建（explorer 重启 / 崩溃后自动拉起）：托盘被清空，只能靠这条广播自己挂回去。
    // 消息 id 是运行时注册的，只能放在 match 之前比对；0 表示当初没注册上。
    let tb = TASKBAR_CREATED.load(Ordering::Relaxed);
    if tb != 0 && msg == tb {
        // 正常重建只重挂、不写日志；挂不上才留一笔，便于区分"没收到广播"和"挂不回去"
        unsafe {
            if !tray_attach() {
                crash_log_line(&format!("[{}] TASKBAR_CREATED re-add failed err={}",
                    crash_stamp(), GetLastError()));
            }
        }
        return 0;
    }
    match msg {
        WM_CREATE => unsafe {
            let hinstance = winapi::um::libloaderapi::GetModuleHandleW(std::ptr::null_mut());

            // 读取持久化的 OCR 排版档位（默认自动），并启动 OCR 会话空闲回收定时器
            OCR_LAYOUT_MODE.store(
                ocr_layout::LayoutMode::from_key(&load_str("ocrLayout")).as_i32(),
                Ordering::Relaxed,
            );
            // 没有引擎就没有会话可回收，定时器也不必设（省一次每 30 秒的空转唤醒）
            #[cfg(feature = "ocr")]
            SetTimer(hwnd, OCR_IDLE_TIMER, 30000, None);

            // 大块绘制缓冲按需建、闲置几秒回收：每秒看一眼有没有超时的
            SetTimer(hwnd, TRIM_TIMER, 1000, None);

            // 顶部页面标签（二维码 / 文件校验 / 定时关机 / 设置）：固定每页宽度
            // 「文字识别」页签只在编入 OCR 引擎时才存在 —— 没有引擎的包里露一个点了没用的
            // 页签，等于给用户摆一个死入口。关掉 feature 时整页连控件都不建（见下方）。
            for (id, label) in PAGE_TABS {
                if id == QR_TAB_OCR && !cfg!(feature = "ocr") { continue; }
                CreateWindowExW(0, wide("BUTTON").as_ptr(), wide(label).as_ptr(),
                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                    sc(ui::metric::SPACE_6), sc(ui::layout::main_page::TAB_Y),
                    sc(ui::layout::main_page::TAB_W_INIT), sc(ui::layout::main_page::TAB_H),
                    hwnd, id as HMENU,
                    hinstance, std::ptr::null_mut());
            }
            place_tabs(hwnd);

            // ===== 文件校验页控件（默认隐藏） =====
            verify::vf_create_page(hwnd);

            // ===== 设置页控件：摆放 / 显隐 / 自绘 / 勾选全部归 modules/settings.rs =====
            settings::create_page(hwnd);

            // ===== 文件改名页控件：工具条 / 五组规则 / 列表 / 底栏全部归 modules/rename.rs =====
            modules::rename::create_page(hwnd);

            // ===== 定时关机页控件：布局 / 状态 / 自绘全部归 modules/shutdown.rs =====
            modules::shutdown::create_page(hwnd);

            // ===== 文字识别页控件（默认隐藏） =====
            // 整块只在编入 OCR 引擎时创建：没有引擎时页面本身就没了（页签也不建），
            // 再建一堆永远隐藏的控件既占着 GDI 资源，又让 GetDlgItem 假装"控件还在"。
            #[cfg(feature = "ocr")]
            {
            // 顶部搜索框 + 右侧“查找”按钮（蓝色自绘，与其他按钮一致；按钮宽100）
            let sbox = CreateWindowExW(0, wide("EDIT").as_ptr(), wide("").as_ptr(),
                WS_CHILD | WS_VISIBLE | WS_BORDER | winapi::um::winuser::ES_AUTOHSCROLL,
                sc(ui::layout::ocr::SEARCH_X), sc(ui::layout::ocr::SEARCH_Y),
                sc(ui::layout::ocr::SEARCH_W), sc(ui::layout::ocr::ROW_H),
                hwnd, OCR_SEARCH_EDIT as HMENU,
                hinstance, std::ptr::null_mut());
            if !sbox.is_null() {
                SendMessageW(sbox, 0x1501 /*EM_SETCUEBANNER*/, 0,
                    wide("在识别结果中查找").as_ptr() as LPARAM);
            }
            CreateWindowExW(0, wide("BUTTON").as_ptr(), wide("查找").as_ptr(),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                sc(ui::layout::ocr::FIND_X), sc(ui::layout::ocr::SEARCH_Y),
                sc(ui::layout::ocr::FIND_W), sc(ui::layout::ocr::ROW_H),
                hwnd, OCR_BTN_SEARCH as HMENU,
                hinstance, std::ptr::null_mut());
            // 排版档位切换（搜索行最右）：点击循环 自动→按行→整段→原始
            CreateWindowExW(0, wide("BUTTON").as_ptr(), wide("排版").as_ptr(),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                sc(ui::layout::ocr::LAYOUT_X), sc(ui::layout::ocr::SEARCH_Y),
                sc(ui::layout::ocr::LAYOUT_W), sc(ui::layout::ocr::ROW_H),
                hwnd, OCR_BTN_LAYOUT as HMENU,
                hinstance, std::ptr::null_mut());
            let ocr = CreateWindowExW(0, wide("EDIT").as_ptr(), wide("").as_ptr(),
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | 0x0100 /*ES_NOHIDESEL 选中常显高亮*/,
                sc(ui::layout::ocr::SEARCH_X), sc(ui::layout::ocr::EDIT_Y),
                sc(ui::layout::ocr::EDIT_W), sc(ui::layout::ocr::EDIT_H),
                hwnd, OCR_EDIT as HMENU,
                hinstance, std::ptr::null_mut());
            if !ocr.is_null() {
                SendMessageW(ocr, 0x1501 /*EM_SETCUEBANNER*/, 0,
                    wide("框选屏幕上的文字，识别结果会显示在这里").as_ptr() as LPARAM);
            }
            // 三个按钮彼此独立（宽132，间隙12）
            for (i, (id, label)) in [(OCR_BTN_REC, "识别"), (OCR_BTN_COPY, "复制"),
                                     (OCR_BTN_CLEAR, "清空")].iter().enumerate() {
                CreateWindowExW(0, wide("BUTTON").as_ptr(), wide(label).as_ptr(),
                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                    sc(ui::layout::ocr::BTN_X[i]), sc(ui::layout::ocr::BTN_Y),
                    sc(ui::layout::ocr::BTN_W), sc(ui::layout::ocr::BTN_H),
                    hwnd, *id as HMENU, hinstance, std::ptr::null_mut());
            }
            // “清空”右侧的“识别 N 字符”显示（空则隐藏；字体在 efont 块里统一应用；水平居中）
            CreateWindowExW(0, wide("STATIC").as_ptr(), wide("").as_ptr(),
                WS_CHILD | winapi::um::winuser::SS_CENTER | 0x0200 /*SS_CENTERIMAGE*/,
                sc(ui::layout::ocr::COUNT_X), sc(ui::layout::ocr::BTN_Y),
                sc(ui::layout::ocr::COUNT_W), sc(ui::layout::ocr::BTN_H),
                hwnd, OCR_LBL_COUNT as HMENU,
                hinstance, std::ptr::null_mut());
            }

            // 二维码页控件（输入框 + 左侧 5 颗按钮）全部归 modules/qr.rs
            INPUT_H.store(ui::layout::main_page::EDIT_H, Ordering::Relaxed);
            modules::qr::create_page(hwnd);
            // 输入框字体：按 DPI 放大 + ClearType，文字清晰（字调大2px）
            let efont = CreateFontW(sc(ui::font::SIZE_22), 0, 0, 0, ui::font::W_REGULAR, 0, 0, 0,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS,
                wide(ui::font::UI).as_ptr());
            EDIT_FONT.store(efont as isize, Ordering::Relaxed);
            if !efont.is_null() {
                let qedit = GetDlgItem(hwnd, QR_EDIT as i32);
                if !qedit.is_null() { SendMessageW(qedit, WM_SETFONT, efont as WPARAM, 1); }
                // 文字识别页的大框、搜索框、查找按钮也用同一字体，保证文字一致清晰
                let ocr = GetDlgItem(hwnd, OCR_EDIT as i32);
                if !ocr.is_null() { SendMessageW(ocr, WM_SETFONT, efont as WPARAM, 1); }
                let sbox = GetDlgItem(hwnd, OCR_SEARCH_EDIT as i32);
                if !sbox.is_null() { SendMessageW(sbox, WM_SETFONT, efont as WPARAM, 1); }
                let sb = GetDlgItem(hwnd, OCR_BTN_SEARCH as i32);
                if !sb.is_null() { SendMessageW(sb, WM_SETFONT, efont as WPARAM, 1); }
                let cnt = GetDlgItem(hwnd, OCR_LBL_COUNT as i32);
                if !cnt.is_null() { SendMessageW(cnt, WM_SETFONT, efont as WPARAM, 1); }
            }

            // 按当前页面（默认文字识别页）隐藏其它页控件
            apply_page(hwnd);

            // 接受文件拖入（二维码图片 / 待校验文件）
            drop_allow(hwnd);

            // 默认显示“请输入文本内容！”的二维码
            if let Some((w, h, data)) = gen_qr_bgra("请输入文本内容！") {
                *QR_IMG.lock_ok() = Some((w, h, data));
                *QR_TEXT.lock_ok() = "请输入文本内容！".to_string();
            }

            // 托盘图标
            let tip = wide(APP_NAME);
            let mut nid: NOTIFYICONDATAW = std::mem::zeroed();
            nid.cbSize = std::mem::size_of::<NOTIFYICONDATAW>() as u32;
            nid.hWnd = hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
            nid.uCallbackMessage = WM_TRAYICON;
            nid.hIcon = LoadIconW(hinstance, 1 as *const u16);
            let n = tip.len().min(128);
            nid.szTip[..n].copy_from_slice(&tip[..n]);
            // 注册任务栏重建消息：explorer 重启后系统已经把托盘清空，只会广播这一条，
            // 不自己重新挂上去，图标就再也回不来（进程还在、入口没了）
            TASKBAR_CREATED.store(
                winapi::um::winuser::RegisterWindowMessageW(wide("TaskbarCreated").as_ptr()),
                Ordering::Relaxed,
            );
            // 留下重建 NOTIFYICONDATAW 需要的三样东西：explorer 重建任务栏后要再登记一次
            TRAY_HWND.store(hwnd as isize, Ordering::Relaxed);
            TRAY_HICON.store(nid.hIcon as isize, Ordering::Relaxed);
            *TRAY_TIP.lock_ok() = tip.clone();
            // 首次登记不上不再退进程：开机自启时 explorer 往往还没就绪，这时的失败是暂时的，
            // 等 TaskbarCreated 广播或每秒一次的重试，图标会自己回来。
            // （原来直接退出，等于把"暂时挂不上"变成"这次开机永远没有图标"）
            let added = Shell_NotifyIconW(NIM_ADD, &mut nid) != 0;
            TRAY_OK.store(added, Ordering::Relaxed);
            if !added {
                crash_log_line(&format!("[{}] TRAY add failed err={} hicon=0x{:X} hwnd=0x{:X}",
                    crash_stamp(), GetLastError(), nid.hIcon as usize, nid.hWnd as usize));
            }
            // 按当前输入框高度统一摆放控件
            relayout(hwnd);
            // 控件都建好了：全部登记文件拖放（拖到任何一块区域都落到文件校验）
            enable_child_drop(hwnd);
            0
        }

        WM_TRAYICON => unsafe {
            let ev = (lp & 0xFFFF) as u32;
            if ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK {
                // 单击切换：显示→隐藏，隐藏→显示（托盘行为保持原样）
                if IsWindowVisible(hwnd) != 0 {
                    ShowWindow(hwnd, SW_HIDE);
                } else {
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                    SetFocus(hwnd); // 焦点还给窗口本身，让输入框占位提示显示
                }
                0
            } else if ev == WM_RBUTTONUP {
                let mut pt: POINT = std::mem::zeroed();
                GetCursorPos(&mut pt);
                let hmenu = CreatePopupMenu();
                AppendMenuW(hmenu, MF_STRING, ID_TRAY_SHOW as usize, wide("打开主界面").as_ptr());
                AppendMenuW(hmenu, MF_STRING, ID_TRAY_SETTINGS as usize, wide("设置").as_ptr());
                AppendMenuW(hmenu, MF_STRING, ID_TRAY_EXIT as usize, wide("退出").as_ptr());
                SetForegroundWindow(hwnd);
                let cmd = TrackPopupMenu(hmenu,
                    TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD,
                    pt.x, pt.y, 0, hwnd, std::ptr::null_mut());
                DestroyMenu(hmenu);
                if cmd == ID_TRAY_SHOW as i32 {
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                    SetFocus(hwnd); // 焦点还给窗口本身，让输入框占位提示显示
                } else if cmd == ID_TRAY_EXIT as i32 {
                    // 同 WM_CLOSE：销毁窗口把 WM_DESTROY 的清理（托盘/热键/缓冲）走完再退出
                    DestroyWindow(hwnd);
                } else if cmd == ID_TRAY_SETTINGS as i32 {
                    // 切换到“设置”标签页并显示主窗口
                    switch_page(hwnd, PAGE_SET);
                    if IsWindowVisible(hwnd) == 0 {
                        ShowWindow(hwnd, SW_SHOW);
                        SetForegroundWindow(hwnd);
                    }
                }
                0
            } else {
                DefWindowProcW(hwnd, msg, wp, lp)
            }
        }

        // 文件拖入：一律送「文件校验」页
        // 注意 HDROP 在 wParam，lParam 恒为 0
        WM_DROPFILES_MSG => unsafe {
            route_drop(hwnd, wp as HDROP);
            0
        }

        // 全局快捷键：识别（跟随当前页）/ 截图 / 关压缩软件（默认空）
        WM_HOTKEY => unsafe {
            let wid = wp as i32;
            if wid == WM_HOTKEY_REC_ID {
                // 识别（跟随当前页）：二维码页=先二维码后文字，其它页=纯文字 OCR
                // 没编入 OCR 引擎时不存在"纯文字识别"这条路，一律按"先二维码"走 ——
                // 否则会切到一个连控件都不存在的空白页。
                if cfg!(feature = "ocr") && PAGE_CUR.load(Ordering::Relaxed) != PAGE_QR {
                    start_region_select(SEL_MODE_OCR);
                } else {
                    start_region_select(SEL_MODE_QR_THEN_OCR);
                }
                0
            } else if wid == WM_HOTKEY_ZIP_ID {
                let n = close_archiver_windows();
                let msg = if n > 0 { format!("已关闭 {} 个压缩软件窗口", n) }
                          else { "没有发现运行中的压缩软件".to_string() };
                show_toast(&msg);
                0
            } else if wid == WM_HOTKEY_SHOT_ID {
                // 截图快捷键（默认 Ctrl+Q）：编辑器未结束（未放弃/未保存复制）时，不开新截图
                if ANNO_HWND.load(Ordering::Relaxed) != 0 {
                    // 仍在编辑上一张：请先放弃、或保存/复制后再截图
                    0
                } else {
                    start_region_select(3);
                    0
                }
            } else {
                DefWindowProcW(hwnd, msg, wp, lp)
            }
        }

        // 点 X（关闭）：隐藏到托盘，不退出
        // 关闭按钮：默认退出软件；设置里可选“到托盘”
        WM_CLOSE => unsafe {
            // 图标没挂上时不能藏窗口：藏起来就彻底没有入口了（托盘点不到、任务栏也没窗口），
            // 只能去任务管理器结束进程 —— 这种情况直接退干净，下次启动重新挂
            if load_dword(CLOSE_MODE_VALUE, 1) == 1 && TRAY_OK.load(Ordering::Relaxed) {
                ShowWindow(hwnd, SW_HIDE);
            } else {
                // 必须走 DestroyWindow：直接 PostQuitMessage 会让消息循环一退进程就没了，
                // WM_DESTROY 永远不执行 —— 托盘图标留在那儿变成"点不动的幽灵"（要等鼠标
                // 划过才消失），热键、定时器、GDI 缓冲也都没被回收。
                DestroyWindow(hwnd);
            }
            0
        },
        // 原生标题栏按钮：只允许最小化到托盘；最大化/还原一律忽略（布局按固定尺寸设计，拉大会是空白）
        WM_SYSCOMMAND => unsafe {
            match wp as u32 & 0xFFF0 {
                0xF020 /*SC_MINIMIZE*/ => {
                    ShowWindow(hwnd, SW_HIDE);
                    return 0;
                }
                0xF030 /*SC_MAXIMIZE*/ => return 0,
                _ => {}
            }
            DefWindowProcW(hwnd, msg, wp, lp)
        },

        // 识别完成：结果回填到当前页面对应的框（工作线程通知）
        WM_QR_RESULT => unsafe {
            let t = RECOG_RESULT.lock_ok().clone();
            let mode = SEL_MODE.load(Ordering::Relaxed);
            // 纯文字 OCR（识别按钮 / 非二维码页快捷键）→ 回填文字识别页；
            // 其余走二维码页：命中填码文，兜底填文字识别结果并全选（modules/qr.rs::fill_result）
            if mode == SEL_MODE_OCR && cfg!(feature = "ocr") {
                if !t.is_empty() && !t.starts_with("未识别") {
                    *OCR_TEXT.lock_ok() = t.clone();
                    let ocr = GetDlgItem(hwnd, OCR_EDIT as i32);
                    if !ocr.is_null() { SetWindowTextW(ocr, wide(&t).as_ptr()); }
                    switch_page(hwnd, PAGE_OCR);
                    update_ocr_count(hwnd);
                }
            } else {
                modules::qr::fill_result(hwnd);
                switch_page(hwnd, PAGE_QR);
            }
            // 识别到内容：自动弹出主界面，让用户看到结果（不再藏在托盘）
            if IsWindowVisible(hwnd) == 0 {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
            }
            0
        },
        // 没解到码、转文字识别：先给提示（OCR 还在跑）
        WM_QR_OCR_FALLBACK => unsafe {
            modules::qr::on_ocr_fallback(hwnd);
            0
        },
        // 没解到码且无文字识别兜底：给出明确提示
        WM_QR_NOT_FOUND => unsafe {
            modules::qr::on_not_found(hwnd);
            0
        },

        // 禁止默认擦背景（配合双缓冲，避免闪烁）
        WM_ERASEBKGND => 1,

        // 定时关机页的数字格：运行中只读时是灰字（ctl_color 统一给色 + 刷底）
        // 设置页的只读显示框（热键三格 + 目录两格）：标准只读 EDIT 默认用“灰面”当底，
        // 这里统一给白底 + 正文色，和其它输入框保持一致
        WM_CTLCOLORSTATIC | winapi::um::winuser::WM_CTLCOLOREDIT => unsafe {
            let id = GetDlgCtrlID(lp as HWND) as usize;
            if let Some(b) = modules::shutdown::ctl_color(wp as winapi::shared::windef::HDC, id) {
                return b as usize as LRESULT;
            }
            if let Some(b) = settings::ctl_color(wp as winapi::shared::windef::HDC, id) {
                return b as usize as LRESULT;
            }
            DefWindowProcW(hwnd, msg, wp, lp)
        },

        WM_PAINT => unsafe {
            let mut ps: PAINTSTRUCT = std::mem::zeroed();
            let hdc = BeginPaint(hwnd, &mut ps);
            let mut cr: RECT = std::mem::zeroed();
            GetClientRect(hwnd, &mut cr);
            let cw = cr.right;
            let ch = cr.bottom;

            // 非二维码页：一律先整幅画进常驻内存 DC，最后一次 BitBlt 贴屏。
            // 若只有文件改名页走缓冲，设置 / 定时关机 / 文件校验页的几十次
            // FillRect / DrawText 是直接写窗口 DC 的，一次重绘就能看见
            // 「底色 → 一根竖条 → 一行字」逐笔出现，这正是切页闪烁的本体。
            // 二维码主页走下面自己的分支（同样双缓冲），绘制逻辑一行未动。
            let pc = PAGE_CUR.load(Ordering::Relaxed);
            if pc != PAGE_QR {
                let memdc = main_buf_dc(cw, ch).unwrap_or(hdc);
                let bg = CreateSolidBrush(ui::color::cref(ui::color::PAGE));
                FillRect(memdc, &cr, bg as HBRUSH);
                DeleteObject(bg as *mut winapi::ctypes::c_void);
                // 定时关机页：标题 / 两行设定 / 每天 / 进度条（全部归 modules/shutdown.rs）
                if pc == PAGE_SHUTDOWN {
                    modules::shutdown::draw(memdc);
                }
                // 文件校验页：底部「文件」「总计」两条进度条（纯绘制，无子控件）
                if pc == PAGE_VF {
                    verify::vf_draw_bars(memdc);
                }
                // 设置页：整页白卡 + 三组竖条/标题/分隔线（全部归 modules/settings.rs）
                if pc == PAGE_SET {
                    settings::draw(memdc);
                }
                // 文件改名页：页面底 + 分组卡 + 标签 + 页底格式条（全部归 modules/rename.rs）
                if pc == PAGE_RENAME {
                    modules::rename::draw(memdc);
                }
                if memdc != hdc {
                    BitBlt(hdc, 0, 0, cw, ch, memdc, 0, 0, SRCCOPY);
                }
                EndPaint(hwnd, &ps);
                return 0;
            }

            // 二维码页：双缓冲 + 纯色背景 + 卡片（控件为子窗口自行绘制）
            // 缓冲 DC/位图常驻复用：避免每次 WM_PAINT 都 CreateCompatibleDC + CreateCompatibleBitmap
            // 再删掉，鼠标划过按钮（悬停变色会整窗失效）就是每秒几十个 1MB 位图的创建/销毁
            let memdc = main_buf_dc(cw, ch).unwrap_or(hdc);

            // 纯净纯色背景（与其它页一致，彻底消除花纹）
            let bg = CreateSolidBrush(ui::color::cref(ui::color::PAGE));
            FillRect(memdc, &RECT { left: 0, top: 0, right: cw, bottom: ch }, bg as HBRUSH);
            DeleteObject(bg as *mut winapi::ctypes::c_void);

            // 右侧二维码显示区：正方形白色卡片 + 浅灰描边
            let area = qr_area();
            let border_brush = CreateSolidBrush(ui::color::cref(ui::color::BORDER));
            FillRect(memdc, &area, border_brush as HBRUSH);
            DeleteObject(border_brush as *mut winapi::ctypes::c_void);
            let white_brush = CreateSolidBrush(ui::color::cref(ui::color::SURFACE));
            let inset = ui::metric::SPACE_1;
            let inner = RECT { left: area.left + inset, top: area.top + inset,
                right: area.right - inset, bottom: area.bottom - inset };
            FillRect(memdc, &inner, white_brush as HBRUSH);
            DeleteObject(white_brush as *mut winapi::ctypes::c_void);

            // 二维码：整图缩放绘制（标准格式，一次到位，不会出现条纹）
            {
                let guard = QR_IMG.lock_ok();
                if let Some((w, h, buf)) = &*guard {
                    let avail = (inner.right - inner.left) as i64;
                    let availh = (inner.bottom - inner.top) as i64;
                    let s = (avail.min(availh) - ui::metric::SPACE_4 as i64)
                        .max(ui::layout::qr::IMG_MIN as i64) as u32;
                    let dx = (inner.left as i64 + (avail - s as i64) / 2) as i32;
                    let dy = (inner.top as i64 + (availh - s as i64) / 2) as i32;
                    // 用 BGRA 数据直接建 32bpp 位图
                    let qbmp = CreateBitmap(*w as i32, *h as i32, 1, 32,
                        buf.as_ptr() as *const winapi::ctypes::c_void);
                    if !qbmp.is_null() {
                        let qdc = CreateCompatibleDC(memdc);
                        let oldq = SelectObject(qdc, qbmp as *mut winapi::ctypes::c_void);
                        StretchBlt(memdc, dx, dy, s as i32, s as i32,
                            qdc, 0, 0, *w as i32, *h as i32, SRCCOPY);
                        SelectObject(qdc, oldq);
                        DeleteObject(qbmp as *mut winapi::ctypes::c_void);
                        // qdc 是 DC，只能 DeleteDC；DeleteObject 会失败，DC 每帧漏一个
                        DeleteDC(qdc);
                    }
                }
            }

            // 一次性呈现到屏幕（缓冲是复用的，不在这里释放）
            if memdc != hdc {
                BitBlt(hdc, 0, 0, cw, ch, memdc, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            0
        }

        // 鼠标移动：检测按钮悬停
        WM_MOUSEMOVE => unsafe {
            let mut pt: POINT = std::mem::zeroed();
            GetCursorPos(&mut pt);
            let mut over = 0i32;
            // 文件校验页的四个算法勾选是自绘按钮，悬停高亮靠 BTN_HOVER；
            // 它们只在「文件校验」页参与命中，否则会跟别的页控件矩形重叠
            let extra = if PAGE_CUR.load(Ordering::Relaxed) == PAGE_VF {
                [verify::VF_CHK_DATE as i32,
                 verify::VF_CHK_MD5 as i32, verify::VF_CHK_SHA256 as i32,
                 verify::VF_CHK_CRC as i32]
            } else {
                [0i32; 4]
            };
            for id in [QR_BTN_CLEAR as i32, QR_BTN_SCREEN as i32,
                       QR_BTN_COPY as i32, QR_BTN_DOWNLOAD as i32, QR_BTN_UPLOAD as i32,
                       OCR_BTN_REC as i32, OCR_BTN_COPY as i32, OCR_BTN_CLEAR as i32,
                       OCR_BTN_SEARCH as i32, OCR_BTN_LAYOUT as i32]
                .into_iter().chain(extra.into_iter())
            {
                if id == 0 { continue; }
                let btn = GetDlgItem(hwnd, id);
                if !btn.is_null() {
                    let mut wr: RECT = std::mem::zeroed();
                    GetWindowRect(btn, &mut wr);
                    if pt.x >= wr.left && pt.x < wr.right && pt.y >= wr.top && pt.y < wr.bottom {
                        over = id;
                        break;
                    }
                }
            }
            if BTN_HOVER.swap(over, Ordering::Relaxed) != over {
                InvalidateRect(hwnd, std::ptr::null_mut(), 1);
            }
            track_leave(hwnd);
            0
        }

        // 鼠标离开：清除悬停
        WM_MOUSELEAVE => unsafe {
            BTN_HOVER.store(0, Ordering::Relaxed);
            InvalidateRect(hwnd, std::ptr::null_mut(), 1);
            0
        }

        // 文件改名页：点页面空白处命中压线标签 / 页底格式框（按钮走 WM_COMMAND）
        WM_LBUTTONDOWN => unsafe {
            if PAGE_CUR.load(Ordering::Relaxed) == PAGE_RENAME {
                let x = (lp as i16) as i32;
                let y = ((lp as i32) >> 16) as i16 as i32;
                modules::rename::on_lbutton_down(x, y);
                return 0;
            }
            DefWindowProcW(hwnd, msg, wp, lp)
        }

        // 文件改名页：窗口尺寸变化时重排（只摆本页子控件，不再回头改窗口大小）
        winapi::um::winuser::WM_SIZE => {
            if PAGE_CUR.load(Ordering::Relaxed) == PAGE_RENAME {
                modules::rename::relayout(hwnd);
            }
            0
        }

        // 自定义圆角按钮绘制（蓝色）
        WM_DRAWITEM => unsafe {
            let dis = lp as *const DRAWITEMSTRUCT;
            if dis.is_null() { return 0; }
            let item = &*dis;
            // 设置页与二维码页自绘控件先接管
            if settings::draw_item(item) { return 1; }
            if modules::qr::draw(item) { return 1; }
            // 定时关机页：▲▼ 箭头 / 三颗按钮自绘（45xx / 456x）
            if modules::shutdown::draw_item(item) { return 1; }
            // 文件改名页：规则勾选 / 工具条实心钮 / 页底文字链（5500–5540）
            if (modules::rename::ID_ADD_FILES..=modules::rename::ID_AFFIX_ON)
                .contains(&(item.CtlID as usize)) {
                modules::rename::on_draw_item(dis);
                return 1;
            }
            let hdc = item.hDC;
            let mut r = item.rcItem;
            // 顶部页面标签：选中=蓝色底白字，未选=浅灰底深字
            if item.CtlID as usize == QR_TAB_OCR
                || item.CtlID as usize == QR_TAB_QR
                || item.CtlID as usize == QR_TAB_VF
                || item.CtlID as usize == QR_TAB_RENAME
                || item.CtlID as usize == QR_TAB_SHUT || item.CtlID as usize == QR_TAB_SET {
                let cur = PAGE_CUR.load(Ordering::Relaxed);
                let sel = (item.CtlID as usize == QR_TAB_OCR && cur == PAGE_OCR)
                    || (item.CtlID as usize == QR_TAB_QR && cur == PAGE_QR)
                    || (item.CtlID as usize == QR_TAB_VF && cur == PAGE_VF)
                    || (item.CtlID as usize == QR_TAB_RENAME && cur == PAGE_RENAME)
                    || (item.CtlID as usize == QR_TAB_SHUT && cur == PAGE_SHUTDOWN)
                    || (item.CtlID as usize == QR_TAB_SET && cur == PAGE_SET);
                // 四个标签用 41 版同款配色：浅灰底 + 深字；选中用蓝色底部条
                let brush = CreateSolidBrush(ui::color::cref(ui::color::TAB_BG));
                FillRect(hdc, &r, brush as HBRUSH);
                DeleteObject(brush as *mut winapi::ctypes::c_void);
                if sel {
                    let bar = CreateSolidBrush(ui::color::cref(ui::color::TAB_ACTIVE));
                    FillRect(hdc, &RECT { left: r.left,
                        top: r.bottom - ui::layout::main_page::TAB_UNDERLINE,
                        right: r.right, bottom: r.bottom },
                        bar as HBRUSH);
                    DeleteObject(bar as *mut winapi::ctypes::c_void);
                }
                let tcol = ui::color::cref(ui::color::TEXT_SECONDARY);
                let oldcol = SetTextColor(hdc, tcol);
                SetBkMode(hdc, TRANSPARENT as i32);
                let f = ui_font(sc(ui::font::SIZE_18), ui::font::W_MEDIUM);
                let oldf = SelectObject(hdc, f as *mut winapi::ctypes::c_void);
                let mut rc = r;
                let label = PAGE_TABS.iter()
                    .find(|(id, _)| *id == item.CtlID as usize)
                    .map(|(_, l)| *l).unwrap_or("设置");
                DrawTextW(hdc, wide(label).as_ptr(), -1, &mut rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdc, oldf);
                SetTextColor(hdc, oldcol);
                return 0;
            }

            // —— 文件校验页的四个算法勾选：自绘 ——
            // 统一自绘：整块填底 + 方框 + 对勾。
            // 用主题复选框（BS_AUTOCHECKBOX）会有两个问题：
            // 一是透明背景画字、自己不擦底，被页签/别的窗口盖过之后控件矩形里留着
            // 别人的旧像素（看起来像标签重叠/串字）；
            // 二是自绘按钮存不住 check 位，所以勾选状态由 vfverify 模块自己持有。
            if verify::vf_chk_index(item.CtlID as usize) >= 0 {
                let idx = verify::vf_chk_index(item.CtlID as usize) as usize;
                let on = verify::vf_chk_state(idx);
                let hot = BTN_HOVER.load(Ordering::Relaxed) == item.CtlID as i32;
                // 整块填底是这段存在的理由：不填就留别人的旧像素
                // 悬停走"压暗"：dim(ui::color::TOOL_SURFACE_TOP, 8%) = (227,229,231)
                // 浅底提亮只会更泛白，压一档反而能把整行从背景里拎出来
                let bg = if hot { ui::color::cref(ui::color::DIVIDER_SOFT) }
                         else { ui::color::cref(ui::color::PAGE) };
                let brush = CreateSolidBrush(bg);
                FillRect(hdc, &r, brush as HBRUSH);
                DeleteObject(brush as *mut winapi::ctypes::c_void);
                let bs = sc(ui::metric::SPACE_6);
                let by = r.top + (r.bottom - r.top - bs) / 2;
                let box_r = RECT { left: r.left + sc(ui::metric::STROKE), top: by,
                    right: r.left + sc(ui::metric::STROKE) + bs, bottom: by + bs };
                let bb = CreateSolidBrush(if on { ui::color::cref(ui::color::ACCENT_DEEP) }
                                         else { ui::color::cref(ui::color::SURFACE) });
                FillRect(hdc, &box_r, bb as HBRUSH);
                DeleteObject(bb as *mut winapi::ctypes::c_void);
                let fb = CreateSolidBrush(if on { ui::color::cref(ui::color::ACCENT_DEEP) }
                                         else { ui::color::cref(ui::color::BORDER) });
                FrameRect(hdc, &box_r, fb as HBRUSH);
                DeleteObject(fb as *mut winapi::ctypes::c_void);
                if on {
                    // 对勾走 ui-check 的 SVG 蒙版
                    // 不再手绘三条线：三个页面的勾从此只有 icons.rs 一处图形定义
                    svg_icon_blit(hdc, box_r.left, by, bs, "ui-check",
                        ui::color::ACCENT_DEEP, ui::color::TEXT_INVERSE);
                }
                let mut label_buf = [0u16; 64];
                let n = GetWindowTextW(item.hwndItem, label_buf.as_mut_ptr(), 64);
                SetBkMode(hdc, TRANSPARENT as i32);
                SetTextColor(hdc, ui::color::cref(ui::color::TEXT));
                let f = ui_font(sc(ui::font::SIZE_18), ui::font::W_MEDIUM);
                let oldf = SelectObject(hdc, f as *mut winapi::ctypes::c_void);
                let mut tr = RECT { left: box_r.right + sc(ui::metric::SPACE_3), top: r.top,
                    right: r.right, bottom: r.bottom };
                DrawTextW(hdc, label_buf.as_ptr(), n as i32, &mut tr,
                    winapi::um::winuser::DT_LEFT | DT_VCENTER | DT_SINGLELINE
                    | winapi::um::winuser::DT_NOPREFIX);
                SelectObject(hdc, oldf);
                return 0;
            }
            let pressed = (item.itemState & ODS_SELECTED) != 0;
            let hovering = BTN_HOVER.load(Ordering::Relaxed) == item.CtlID as i32;

            // 文件校验页的五个操作按钮：直角纯色 + 白字
            // 一律直角、不描边（Win32 的圆角只能靠区域硬裁，小尺寸上不是缺角就是发毛）
            // 静止态底色只三档：清除=红；停止=忙时红、闲时灰；其余=蓝。
            // 悬停 / 按下都在同一个底色上压暗（8% / 20%），不另维护亮色号。
            let vid = item.CtlID as usize;
            if vid == verify::VF_BROWSE || vid == verify::VF_CLEAR
                || vid == verify::VF_COPY || vid == verify::VF_SAVE
                || vid == verify::VF_STOP {
                let base = if vid == verify::VF_CLEAR {
                    ui::color::DANGER
                } else if vid == verify::VF_STOP {
                    // 进度条没在走时按"停止"没有意义，直接画成禁用灰
                    if verify::vf_busy() { ui::color::DANGER } else { ui::color::NEUTRAL }
                } else {
                    ui::color::ACCENT_DEEP
                };
                let keep = (100 - if pressed { 20 } else if hovering { 8 } else { 0 }) as u32;
                let bg = ui::color::cref(ui::color::dim(base, keep));
                let brush = CreateSolidBrush(bg);
                FillRect(hdc, &r, brush as HBRUSH);
                DeleteObject(brush as *mut winapi::ctypes::c_void);
                // 标签直接取按钮文本（"浏览…"/"清除"/"复制"/"保存…"/"停止"）
                let mut label_buf = [0u16; 64];
                let n = GetWindowTextW(item.hwndItem, label_buf.as_mut_ptr(), 64);
                SetBkMode(hdc, TRANSPARENT as i32);
                SetTextColor(hdc, ui::color::cref(ui::color::TEXT_INVERSE));
                // 按钮高 30，字号取 18 才能在框内垂直居中
                let f = ui_font(sc(ui::font::SIZE_18), ui::font::W_MEDIUM);
                let oldf = SelectObject(hdc, f as *mut winapi::ctypes::c_void);
                DrawTextW(hdc, label_buf.as_ptr(), n as i32, &mut r,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(hdc, oldf);
                return 0;
            }

            let is_clear = item.CtlID == QR_BTN_CLEAR as u32
                || item.CtlID == OCR_BTN_CLEAR as u32;
            let base = if is_clear {
                // 危险操作（清空）：红色系
                if pressed { ui::color::cref(ui::color::DANGER_PRESSED) }
                else if hovering { ui::color::cref(ui::color::DANGER_HOVER) }
                else { ui::color::cref(ui::color::DANGER) }
            } else {
                // 其余按钮统一用主题强调色（三态）
                if pressed { ui::color::cref(ui::color::ACCENT_PRESSED) }
                else if hovering { ui::color::cref(ui::color::ACCENT_HOVER) }
                else { ui::color::cref(ui::color::ACCENT_DEEP) }
            };
            let brush = CreateSolidBrush(base);
            FillRect(hdc, &r, brush as HBRUSH);
            let rad = ui::metric::RADIUS_BTN;
            let hrgn = CreateRoundRectRgn(r.left, r.top, r.right, r.bottom, rad, rad);
            FillRgn(hdc, hrgn, brush as HBRUSH);
            DeleteObject(brush as *mut winapi::ctypes::c_void);
            DeleteObject(hrgn as *mut winapi::ctypes::c_void);

            let label = btn_label(item.CtlID as u32);
            // 字体高度按 DPI 放大 + ClearType 渲染，保证文字清晰不虚
            let fsize = ui::font::SIZE_22;
            let font = ui_font(sc(fsize), ui::font::W_BOLD);
            let old = SelectObject(hdc, font as *mut winapi::ctypes::c_void);
            SetTextColor(hdc, ui::color::cref(ui::color::TEXT_INVERSE));
            SetBkMode(hdc, TRANSPARENT as i32);
            DrawTextW(hdc, wide(label).as_ptr(), -1, &mut r,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, old);
            0
        }

        WM_COMMAND => unsafe {
            let id = (wp & 0xFFFF) as usize;
            let notify = (wp >> 16) & 0xFFFF;
            // 定时关机页通知（勾选「每天」等）
            if modules::shutdown::on_notify(id, notify as u16, hwnd) { return 0; }
            // 二维码页 EN_CHANGE / EN_SETFOCUS 先接管；false 落回下面的页面切换
            if modules::qr::on_notify(id, notify, hwnd) { return 0; }
            // 设置页勾选；false 落回下面的录入/更改
            if settings::on_command(id, hwnd) { return 0; }
            // 定时关机页按钮/箭头
            if modules::shutdown::on_command(id, hwnd) { return 0; }
            // 二维码页按钮
            if modules::qr::on_command(id, hwnd) { return 0; }
            // 文件改名页：5500–5540 全部归模块（按钮 / 勾选 / 编辑框 / 下拉）
            if (modules::rename::ID_ADD_FILES..=modules::rename::ID_AFFIX_ON).contains(&id) {
                // 通知码要一起传：改名页靠它区分"下拉选了一项"和"手打了一个字"，
                // 丢掉通知码的话「改为 / 过滤」从列表里挑一项不生效
                modules::rename::on_command(id, notify as u16, hwnd);
                return 0;
            }
            // 文字识别页搜索框：输入即定位识别结果
            if id == OCR_SEARCH_EDIT && notify == 0x0300 /*EN_CHANGE*/ {
                ocr_search_jump(hwnd);
            }
            match id {
                QR_TAB_OCR => {
                    switch_page(hwnd, PAGE_OCR);
                    0
                }
                QR_TAB_QR => {
                    switch_page(hwnd, PAGE_QR);
                    0
                }
                QR_TAB_VF => {
                    switch_page(hwnd, PAGE_VF);
                    0
                }
                QR_TAB_RENAME => {
                    switch_page(hwnd, PAGE_RENAME);
                    0
                }
                QR_TAB_SHUT => {
                    switch_page(hwnd, PAGE_SHUTDOWN);
                    0
                }
                QR_TAB_SET => {
                    switch_page(hwnd, PAGE_SET);
                    0
                }
                verify::VF_BROWSE => {
                    verify::vf_browse(hwnd);
                    0
                }
                verify::VF_CLEAR => {
                    verify::vf_clear(hwnd);
                    0
                }
                verify::VF_COPY => {
                    verify::vf_copy(hwnd);
                    0
                }
                verify::VF_SAVE => {
                    verify::vf_save(hwnd);
                    0
                }
                verify::VF_STOP => {
                    verify::vf_stop();
                    0
                }
                // 四个算法勾选：自绘按钮存不住 check 位，状态由 vfverify 模块持有，
                // 这里只负责翻转并重画
                verify::VF_CHK_DATE | verify::VF_CHK_MD5
                    | verify::VF_CHK_SHA256 | verify::VF_CHK_CRC => {
                    verify::vf_toggle_chk(id);
                    let c = GetDlgItem(hwnd, id as i32);
                    if !c.is_null() { InvalidateRect(c, std::ptr::null_mut(), 1); }
                    0
                }
                // 保存位置：选目录 → 写注册表 → 回填显示框（取消选择就什么都不动）
                SET_BTN_SHOTDIR | SET_BTN_VIDDIR => {
                    let (ed, vname, title) = if id == SET_BTN_SHOTDIR {
                        (SET_ED_SHOTDIR, SHOT_DIR_VALUE, "选择截图保存目录")
                    } else {
                        (SET_ED_VIDDIR, VIDEO_DIR_VALUE, "选择录屏保存目录")
                    };
                    if let Some(dir) = pick_folder(hwnd, title) {
                        // 目录存在不等于能写：只读目录 / 写保护 U 盘 / 断连网络盘
                        // 都能过对话框的路径检查，但之后截图会静默落不了盘
                        if !settings::is_dir_writable(&dir) {
                            show_toast("目录不可写，请换一个");
                        } else {
                            save_str(vname, &dir);
                            let e = GetDlgItem(hwnd, ed as i32);
                            if !e.is_null() {
                                SendMessageW(e, 0x000C, 0, wide(&dir).as_ptr() as LPARAM);
                            }
                            show_toast(if id == SET_BTN_SHOTDIR {
                                "截图位置已更新"
                            } else {
                                "视频位置已更新"
                            });
                        }
                    }
                    0
                }
                OCR_BTN_SEARCH => {
                    ocr_search_jump(hwnd); // 手动“查找”：定位并高亮
                    0
                }
                SET_RECORD_REC | SET_RECORD_SHOT | SET_RECORD_ZIP => {
                    let target = match id {
                        SET_RECORD_REC => 0, SET_RECORD_ZIP => 3, _ => 2,
                    };
                    // 刚进录制就被连点/双击：当作同一次点击，别把自己取消掉
                    if CAPTURING.load(Ordering::Relaxed)
                        && CAPTURE_TARGET.load(Ordering::Relaxed) == target
                        && cap_recent()
                    {
                        return 0;
                    }
                    // 正在录这一行：按钮此刻是“取消”，再点一下就结束并复原
                    if CAPTURING.load(Ordering::Relaxed) {
                        cap_cancel(hwnd);
                        if CAPTURE_TARGET.load(Ordering::Relaxed) == target { return 0; }
                    }
                    cap_begin(hwnd, target);
                    0
                }
                OCR_BTN_REC => {
                    // 文字识别：框选位置→OCR（内部先隐藏主界面，松手识别完自动回文字识别页）
                    start_region_select(SEL_MODE_OCR);
                    0
                }
                OCR_BTN_COPY => {
                    let t = OCR_TEXT.lock_ok().clone();
                    if t.is_empty() {
                        set_status(hwnd, "没有可复制的文字");
                    } else if copy_text_clipboard(&t) {
                        set_status(hwnd, "文字已复制到剪贴板");
                    } else {
                        set_status(hwnd, "复制失败");
                    }
                    0
                }
                OCR_BTN_CLEAR => {
                    *OCR_TEXT.lock_ok() = String::new();
                    OCR_LINES.lock_ok().clear();
                    let ocr = GetDlgItem(hwnd, OCR_EDIT as i32);
                    if !ocr.is_null() { SetWindowTextW(ocr, wide("").as_ptr()); }
                    // 同时清空顶部搜索框
                    let sbox = GetDlgItem(hwnd, OCR_SEARCH_EDIT as i32);
                    if !sbox.is_null() { SetWindowTextW(sbox, wide("").as_ptr()); }
                    update_ocr_count(hwnd);
                    0
                }
                OCR_BTN_LAYOUT => {
                    // 循环排版档位并持久化；有缓存结果就立即重排（不重跑引擎）
                    let cur = ocr_layout::LayoutMode::from_i32(OCR_LAYOUT_MODE.load(Ordering::Relaxed));
                    let next = cur.next();
                    OCR_LAYOUT_MODE.store(next.as_i32(), Ordering::Relaxed);
                    save_str("ocrLayout", next.key());
                    set_status(hwnd, next.label());
                    let lines = OCR_LINES.lock_ok().clone();
                    if !lines.is_empty() {
                        let t = ocr_layout::format(&lines, next);
                        *OCR_TEXT.lock_ok() = t.clone();
                        let ocr = GetDlgItem(hwnd, OCR_EDIT as i32);
                        if !ocr.is_null() { SetWindowTextW(ocr, wide(&t).as_ptr()); }
                        update_ocr_count(hwnd);
                    }
                    0
                }
                _ => DefWindowProcW(hwnd, msg, wp, lp),
            }
        }

        // 文件校验：工作线程 → UI 线程的两条消息
        WM_VF_PROGRESS => {
            verify::vf_on_progress();
            // 进度条画在父窗口上，刷这一块就够了，不必整窗重绘
            if PAGE_CUR.load(Ordering::Relaxed) == PAGE_VF {
                verify::vf_invalidate_bars(hwnd);
            }
            0
        }
        WM_VF_DONE => {
            verify::vf_on_done();
            if PAGE_CUR.load(Ordering::Relaxed) == PAGE_VF {
                verify::vf_invalidate_bars(hwnd);
            }
            0
        }
        WM_TIMER => unsafe {
            // 大块绘制缓冲的闲置回收（每秒检查；闲置 3 秒就还给系统，下次绘制重建）
            if wp as usize == TRIM_TIMER {
                bufs_trim_idle();
                // 标注选项的改动是攒着的，这里合并写一次盘
                cfg_flush_if_dirty();
                // 顺带补挂托盘：开机自启时 explorer 可能还没就绪，或者广播来得比窗口早，
                // 这两种情况都靠这一秒一次的重试自己恢复，不留在"进程在、图标没了"的状态
                if !TRAY_OK.load(Ordering::Relaxed) {
                    tray_attach();
                }
                return 0;
            }
            // OCR 会话空闲回收（每 30s 检查；空闲超 3 分钟收掉，释放约 100MB）
            if wp as usize == OCR_IDLE_TIMER {
                ocr_idle_tick();
                return 0;
            }
            // 定时关机倒计时（走表 / 到点关机 / 最后 10 秒弹回主界面）
            if wp as usize == modules::shutdown::SD_TIMER_ID {
                modules::shutdown::on_timer(wp as usize, hwnd);
                return 0;
            }
            // “已保存”只亮一小会儿，随后三颗按钮文字统一回到“录入”
            if wp as usize == SET_TIMER_SAVED {
                KillTimer(hwnd, SET_TIMER_SAVED as usize);
                for id in [SET_RECORD_REC, SET_RECORD_SHOT, SET_RECORD_ZIP] {
                    let b = GetDlgItem(hwnd, id as i32);
                    if !b.is_null() { SetWindowTextW(b, wide("录入").as_ptr()); }
                }
                return 0;
            }
            // 设置页快捷键录制
            if wp as usize == SET_TIMER_ID {
                if CAPTURING.load(Ordering::Relaxed) {
                    if GetAsyncKeyState(VK_ESCAPE) as u16 & 0x8000 != 0 {
                        cap_cancel(hwnd);
                        return 0;
                    }
                    // DELETE：清空当前正在录制的快捷键。录制期间三条全局热键本就处于失效态
                    // （由 cap_begin 注销、apply_hotkeys 复位），这里把目标条存成 (0,0) 即"未设置"，
                    // 其余两条在下面 apply_hotkeys 时重新注册回来。
                    if GetAsyncKeyState(VK_DELETE) as u16 & 1 != 0 {
                        let target = CAPTURE_TARGET.load(Ordering::Relaxed);
                        let (combo_id, rec_id, store, vname, _, _, _, _) = hotkey_row(target);
                        *store.lock_ok() = None;
                        save_hotkey(vname, 0, 0); // (0,0) = 未设置，apply_hotkeys 不会再注册它
                        let disp = GetDlgItem(hwnd, combo_id as i32);
                        if !disp.is_null() {
                            SendMessageW(disp, 0x000C /*WM_SETTEXT*/, 0, wide("无").as_ptr() as LPARAM);
                        }
                        CAPTURING.store(false, Ordering::Relaxed);
                        CAP_MODS.store(0, Ordering::Relaxed);
                        KillTimer(hwnd, SET_TIMER_ID as usize);
                        // 重新注册其余三条（被清的那条 m=v=0 不再注册）；失败提示由 apply_hotkeys 统一发
                        apply_hotkeys();
                        let rec = GetDlgItem(hwnd, rec_id as i32);
                        if !rec.is_null() {
                            SetWindowTextW(rec, wide("已清空").as_ptr());
                            SetTimer(hwnd, SET_TIMER_SAVED as usize, 900, None);
                        }
                        return 0;
                    }
                    // 修饰键按"边沿"累计：低位=上次轮询后按下过，高位=此刻按着
                    let mut mods = CAP_MODS.load(Ordering::Relaxed) as u32;
                    if GetAsyncKeyState(VK_CONTROL) as u16 & 0x8001 != 0 { mods |= MOD_CONTROL as u32; }
                    if GetAsyncKeyState(VK_MENU) as u16 & 0x8001 != 0 { mods |= MOD_ALT as u32; }
                    if GetAsyncKeyState(VK_SHIFT) as u16 & 0x8001 != 0 { mods |= MOD_SHIFT as u32; }
                    if (GetAsyncKeyState(VK_LWIN) as u16 | GetAsyncKeyState(VK_RWIN) as u16) & 0x8001 != 0 {
                        mods |= MOD_WIN as u32;
                    }
                    CAP_MODS.store(mods as i32, Ordering::Relaxed);

                    // 候选主键：数字、字母、F1-F12、空格/回车、方向键、编辑键
                    let mut pressed: Option<u32> = None;
                    for vk in 0x30..=0x39 { if GetAsyncKeyState(vk) as u16 & 1 != 0 { pressed = Some(vk as u32); } }
                    for vk in 0x41..=0x5A { if GetAsyncKeyState(vk) as u16 & 1 != 0 { pressed = Some(vk as u32); } }
                    for vk in 0x70..=0x7B { if GetAsyncKeyState(vk) as u16 & 1 != 0 { pressed = Some(vk as u32); } }
                    // 注意：不再把 VK_DELETE(0x2E) 列为主键候选 —— Delete 改作"清空"指令（见下方）
                    for vk in [0x20, 0x0D, 0x25, 0x26, 0x27, 0x28, 0x21, 0x22, 0x23, 0x24, 0x2D] {
                        if GetAsyncKeyState(vk) as u16 & 1 != 0 { pressed = Some(vk as u32); }
                    }
                    // 单键热键允许的几个：PrintScreen / ScrollLock / Pause
                    for vk in [0x2C, 0x91, 0x13] {
                        if GetAsyncKeyState(vk) as u16 & 1 != 0 { pressed = Some(vk as u32); }
                    }
                    if let Some(vk) = pressed {
                        let target = CAPTURE_TARGET.load(Ordering::Relaxed);
                        let (combo_id, rec_id, store, vname, _, _, _, _) = hotkey_row(target);
                        let disp = GetDlgItem(hwnd, combo_id as i32);
                        let name = hotkey_name(mods, vk);
                        // 不配修饰键时只认 F1~F12 / PrintScreen / ScrollLock / Pause
                        // 字母数字方向键单设等于把打字和导航抢走；系统/编辑保留组合也拒绝。
                        // 拒绝时留在录入状态等用户换一个（不然热键整套注册一遍又白折腾）
                        let alone_ok = (0x70..=0x7B).contains(&vk)
                            || vk == 0x2C || vk == 0x91 || vk == 0x13;
                        if (mods == 0 && !alone_ok) || hotkey_reserved(&name) {
                            if !disp.is_null() {
                                SendMessageW(disp, 0x000C, 0, wide("组合不可用，请重按").as_ptr() as LPARAM);
                            }
                            return 0;
                        }
                        if !disp.is_null() {
                            SendMessageW(disp, 0x000C, 0, wide(&name).as_ptr() as LPARAM);
                        }
                        *store.lock_ok() = Some((mods, vk, format!("自定义：{}", name)));
                        save_hotkey(vname, mods, vk);
                        // 同一个组合只能挂在一个功能上
                        // 抢了别行的就顺手把别行清掉，否则两功能同时响应同一个键
                        // 只剩识别(0) / 截图(2) / 关压缩软件(3) 三行
                        for other in [0usize, 2, 3] {
                            if other == target { continue; }
                            let (oc, _orc, ost, ov, _, _, _, _) = hotkey_row(other);
                            if load_hotkey(ov, 0, 0) == (mods, vk) {
                                save_hotkey(ov, 0, 0);
                                *ost.lock_ok() = None;
                                let od = GetDlgItem(hwnd, oc as i32);
                                if !od.is_null() {
                                    SendMessageW(od, 0x000C, 0, wide("未设置").as_ptr() as LPARAM);
                                }
                            }
                        }
                        // 录制期间注销过，这里重新注册；失败提示由 apply_hotkeys 统一发
                        apply_hotkeys();
                        CAPTURING.store(false, Ordering::Relaxed);
                        CAP_MODS.store(0, Ordering::Relaxed);
                        KillTimer(hwnd, SET_TIMER_ID as usize);
                        let rec = GetDlgItem(hwnd, rec_id as i32);
                        if !rec.is_null() {
                            SetWindowTextW(rec, wide("已保存").as_ptr());
                            SetTimer(hwnd, SET_TIMER_SAVED as usize, 900, None);
                        }
                    }
                }
                return 0;
            }
            DefWindowProcW(hwnd, msg, wp, lp)
        }

        WM_DESTROY => unsafe {
            let mut nid: NOTIFYICONDATAW = std::mem::zeroed();
            nid.cbSize = std::mem::size_of::<NOTIFYICONDATAW>() as u32;
            nid.hWnd = hwnd;
            nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &mut nid);
            // 重挂用的状态一起清掉：窗口都没了，不该还留着能重建图标的副本
            TRAY_HWND.store(0, Ordering::Relaxed);
            TRAY_HICON.store(0, Ordering::Relaxed);
            TRAY_TIP.lock_ok().clear();
            TRAY_OK.store(false, Ordering::Relaxed);
            unregister_all(hwnd);
            KillTimer(hwnd, modules::shutdown::SD_TIMER_ID);
            KillTimer(hwnd, SET_TIMER_ID as usize);
            KillTimer(hwnd, SET_TIMER_SAVED);
            KillTimer(hwnd, OCR_IDLE_TIMER);
            KillTimer(hwnd, TRIM_TIMER);
            // 关闭常驻 OCR 引擎会话（释放进程与模型占用的内存）
            ocr_shutdown_tick();
            // 关掉全屏倒计时窗口
            let ef = EDIT_FONT.load(Ordering::Relaxed);
            if ef != 0 {
                DeleteObject(ef as *mut winapi::ctypes::c_void);
                EDIT_FONT.store(0, Ordering::Relaxed);
            }
            // 常驻的绘制资源：字体缓存与重绘缓冲都还给系统，别等进程退出才回收
            main_buf_free();
            shot_free();
            {
                let mdc = TX_MDC.swap(0, Ordering::Relaxed) as winapi::shared::windef::HDC;
                if !mdc.is_null() { DeleteDC(mdc); }
                let mut c = FONT_CACHE.lock_ok();
                for (_, _, _, _, f) in c.drain(..) {
                    delete_font(f);
                }
            }
            PostQuitMessage(0);
            0
        },
        _ => unsafe { DefWindowProcW(hwnd, msg, wp, lp) },
    }
}

// 复制/保存等结果用轻提示弹窗反馈，避免静默丢失
fn set_status(hwnd: HWND, s: &str) {
    if !hwnd.is_null() {
        unsafe { show_toast(s); }
    }
}


// 自绘按钮文字
fn btn_label(id: u32) -> &'static str {
    match id {
        4102 => "下载二维码",
        4103 => "识别二维码",
        4105 => "复制二维码",
        4106 => "上传 LOGO",
        4107 => "清空",
        4108 => "截图",
        4109 => "关闭压缩软件",
        4191 => "识别",
        4192 => "复制",
        4193 => "清空",
        4195 => "查找",
        4197 => "排版",
        4560 => "开始定时器",
        4561 => "暂停定时器",
        4563 => "清空",
        // 「文件校验」页操作按钮（id 见 verify::VF_*）
        5302 => "浏览…",
        5303 => "清除",
        5304 => "复制",
        5305 => "保存…",
        5306 => "停止",
        _ => "",
    }
}


fn main() {
    // 先装崩溃落盘：之后任何 panic / 访问违例都能在 %APPDATA%\<APP_NAME>\crash.log 里查到，
    // 而不是进程直接消失、连 WER 都没有记录
    crash_log_init();
    crash_log_line(&format!("[{}] START pid={}", unsafe { crash_stamp() }, std::process::id()));
    // 配置先读进来：副选项条的选中态（椭圆/矩形、填充、颜色、滑块值）全部由它决定
    cfg_init();
    // 出厂设置只落一次（四个开、两个关），必须在建窗口前完成：
    // 设置页的勾选态就是按这里的值初始化的
    settings::ensure_default_settings();
    unsafe {
        // 开启 DPI 感知：优先 per-monitor v2（Win10 1703+），失败退回 system-aware，
        // 保证高分屏按原生分辨率渲染，文字清晰不变形
        let user32 = GetModuleHandleW(wide("user32.dll").as_ptr());
        if !user32.is_null() {
            let f = GetProcAddress(user32, b"SetProcessDpiAwarenessContext\0".as_ptr() as *const i8);
            if !f.is_null() {
                let p: unsafe extern "system" fn(isize) -> i32 = std::mem::transmute(f);
                if p(-4 /*DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2*/) == 0 {
                    SetProcessDPIAware();
                }
            } else {
                SetProcessDPIAware();
            }
        } else {
            SetProcessDPIAware();
        }
        let hdc = GetDC(std::ptr::null_mut());
        if !hdc.is_null() {
            let d = GetDeviceCaps(hdc, LOGPIXELSX);
            if d > 0 { DPI_CACHE.store(d, Ordering::Relaxed); }
            ReleaseDC(std::ptr::null_mut(), hdc);
        }
    }

    // 单实例：再次打开时不弹提示，把之前打开的界面调到前台
    let single = load_dword(SINGLE_INSTANCE_VALUE, 1) == 1;
    let m = wide(concat!("Global\\", env!("APP_NAME"), "_Single"));
    unsafe {
        // CreateMutexW 在"新建成功"时**不会**把 last-error 清零，132 行之前随便哪个调用
        // 留下的 183（ERROR_ALREADY_EXISTS，例如建目录撞到已存在的目录）都会被下面当成
        // "已经有一个实例在跑"，于是双击图标后进程立刻无声退出 —— 偶发的
        // "点了没反应"就是这个。必须自己先清零，再判断。
        SetLastError(0);
        let h = CreateMutexW(std::ptr::null_mut(), 1, m.as_ptr());
        let existed = !h.is_null() && GetLastError() == 183;
        if single && existed {
            // 再次双击：调到前台显示（不再隐藏）
            let title = wide(APP_NAME);
            let hwnd = FindWindowW(std::ptr::null(), title.as_ptr());
            if !hwnd.is_null() {
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
                SetFocus(hwnd); // 焦点还给窗口本身，让输入框占位提示显示
            }
            return;
        }
        // 互斥体句柄要一直留着：一旦 CloseHandle，单实例保护就没了。
        // 进程退出时系统自动回收，不需要手工释放。
        let _keep = h;
    }

    // 监控线程
    let running = Arc::new(AtomicBool::new(true));
    let r = running.clone();
    std::thread::spawn(move || monitor_thread(r));

    unsafe {
        let hinstance = GetModuleHandleW(std::ptr::null_mut());
        let cls = wide(CLS_MAIN);
        reg_class(CLS_MAIN, Some(wnd_proc), CS_HREDRAW | CS_VREDRAW, IDC_ARROW as usize, true);

        // 固定在屏幕中心；客户区随 DPI 缩放（585×445 @100%），固定窗口（不可拖动拉伸）
        // WS_CAPTION|WS_SYSMENU = 无最大/可调边框；只留最小化，最大化会把页面拉出空白
        let style = 0x00C00000 /*WS_CAPTION*/ | 0x00080000 /*WS_SYSMENU*/ | WS_CLIPCHILDREN
            | 0x00020000 /*WS_MINIMIZEBOX*/;
        let sw = GetSystemMetrics(SM_CXSCREEN);
        let sh = GetSystemMetrics(SM_CYSCREEN);
        let mut rc: RECT = RECT {
            left: 0, top: 0, right: sc(ui::metric::WIN_W), bottom: sc(ui::metric::WIN_H) };
        AdjustWindowRect(&mut rc, style, 0);
        let win_w = rc.right - rc.left;
        let win_h = rc.bottom - rc.top;
        let x = (sw - win_w) / 2;
        let y = (sh - win_h) / 2;

        let hwnd = CreateWindowExW(
            0, cls.as_ptr(), wide(APP_NAME).as_ptr(),
            style,
            x, y, win_w, win_h,
            std::ptr::null_mut(), std::ptr::null_mut(), hinstance, std::ptr::null_mut());

        // 启动是否显示主界面：设置页可关；关掉则落托盘
        if load_dword(SHOW_MAIN_VALUE, 0) == 1 {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else {
            ShowWindow(hwnd, SW_HIDE);
        }

        // 记录主窗口句柄并应用全局快捷键
        MAIN_HWND.store(hwnd as isize, Ordering::Relaxed);
        apply_hotkeys();

        let mut msg: MSG = std::mem::zeroed();
        loop {
            let mr = GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0);
            // GetMessageW 的三种返回：>0 有消息、0 收到 WM_QUIT、-1 出错。
            // 不能把 -1 也当"退出"：一次取消息失败进程就没了；这里记一笔再继续。
            if mr == 0 { break; }
            if mr == -1 {
                crash_log_line(&format!("[{}] GETMESSAGE -1 err={}",
                    crash_stamp(), GetLastError()));
                std::thread::sleep(Duration::from_millis(50));
                continue;
            }
            // ESC 统一入口：编辑器/框选按一次即关闭，主界面按一次最小化到托盘（不再退进程）。
            // 拦截住就不再分发，避免各窗口各自处理导致「要按两次」。
            if msg.message == WM_KEYDOWN && msg.wParam as i32 == VK_ESCAPE
                && esc_exit_all(msg.hwnd)
            {
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    crash_log_line(&format!("[{}] EXIT clean", unsafe { crash_stamp() }));
    running.store(false, Ordering::Relaxed);
}

// ==================== 截图标注编辑器 ====================
// 底部按钮：矩形·圆形·箭头·标号·线条·文本·马赛克·橡皮擦 | 撤销·重做 | 关闭·保存·复制
// 图标取自源码 iconfont.ttf，Linux 端预渲染成 34×34 位图内嵌（见 anno_icons.bin），运行时直贴，不依赖 GDI 字体。

static ANNO_HWND: AtomicIsize = AtomicIsize::new(0);       // 原图窗（1:1 显示截图 + 1px 红框）
static ANNO_MAIN_HWND: AtomicIsize = AtomicIsize::new(0);  // 主工具条浮窗
static ANNO_SUB_HWND: AtomicIsize = AtomicIsize::new(0);   // 副选项条浮窗（选中绘制工具时出现）
static ANNO_PIN: AtomicBool = AtomicBool::new(false);      // 贴在屏幕上（图钉）— 仅编辑器内的临时标记
// 贴屏原图像素尺寸：窗口可以缩放（Ctrl+滚轮），但底图始终是这么大
static ANNO_IMG: Mutex<Vec<u8>> = Mutex::new(Vec::new());   // 当前工作位图 BGRA(top-down)
static ANNO_ORIG: Mutex<Vec<u8>> = Mutex::new(Vec::new());  // 当前选区的底图（未标注，橡皮擦/重放用）
// ANNO_ORIG 这张底图对应的选区原点（屏幕坐标）。拖选区内核时靠它算出"图元要不要跟着挪"
static ANNO_BASE_OX: AtomicI32 = AtomicI32::new(0);
static ANNO_BASE_OY: AtomicI32 = AtomicI32::new(0);
static ANNO_W: AtomicI32 = AtomicI32::new(0);
static ANNO_H: AtomicI32 = AtomicI32::new(0);
static ANNO_OX: AtomicI32 = AtomicI32::new(0); // 亮区在屏幕上的左上角 x（全屏编辑器：整屏压暗、亮区=选区）
static ANNO_OY: AtomicI32 = AtomicI32::new(0);
static ANNO_TOOL: AtomicI32 = AtomicI32::new(0);  // 0..7 绘制工具
static ANNO_HOVER: AtomicI32 = AtomicI32::new(-1); // 主工具条悬停按钮（-1 无）
// 副选项条悬停目标（编码同 Tip 归属：100+k 开关 / 200+i 色板 / 300 滑块，-1 无）。
// 我们副条是画出来的区域，自己记一份
static ANNO_SUB_HOVER: AtomicI32 = AtomicI32::new(-1);
// 主工具条 14 颗按钮的图标
const ANNO_BTN_ICON: [&str; 14] = [
    "tool-rect", "tool-arrow-straight", "tool-number", "tool-line", "tool-text", "tool-mosaic", "tool-eraser",
    "recording-start",
    "action-undo", "action-redo", "action-close", "action-pin", "quick-save", "action-copy",
];
// 按钮位 → 内部工具号。椭圆已并进矩形（工具 0 + ANNO_OPT_B），所以这里跳过 1：
// 0 矩形/椭圆 | 1 箭头 | 2 序号 | 3 线条 | 4 文字 | 5 马赛克 | 6 橡皮擦
const ANNO_BTN_TOOL: [i32; 7] = [0, 2, 3, 4, 5, 6, 7];
// 两个分隔条插在"橡皮擦之后"和"重做之后"（btnIds 里两个 L"|" 同位置）
const ANNO_SEP_AT: [usize; 2] = [8, 10];
// 色板：红 黄 绿 青 蓝 紫 粉 黑 白
const ANNO_COLORS: [(u8, u8, u8); 9] = [
    (0xCF, 0x13, 0x22), (0xD4, 0x88, 0x06), (0x38, 0x9E, 0x0D), (0x13, 0xC2, 0xC2), (0x09, 0x58, 0xD9),
    (0x72, 0x2E, 0xD1), (0xEB, 0x2F, 0x96), (0x00, 0x00, 0x00), (0xFF, 0xFF, 0xFF),
];
// 图标库（anno_icons.bin，20 个 34×34）：0..12 主工具栏；13=e602(fill矩形/马赛克/橡皮) 14=e600(ellipse填充)
// 15=e604(arrow填充) 16=e605(number填充) 17=e607(line半透明) 18=e634(text加粗) 19=e682(text斜体)
// 每工具滑块配置 (min,max,默认)
const ANNO_SLIDER_CFG: [[i32; 3]; 8] = [
    [1, 26, 2],    // rect   线宽
    [1, 26, 2],    // ellipse 线宽
    [1, 32, 5],    // arrow 线宽
    [6, 86, 9],    // number 圆半径
    [1, 60, 12],   // line   线宽
    [10, 72, 12],  // text   字号（滑块值域 10~72，默认 12）
    [18, 68, 28],  // mosaic 块大小
    [18, 68, 28],  // eraser 橡皮大小
];
