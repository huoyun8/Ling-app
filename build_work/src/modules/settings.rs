//! 设置页：热键读写与录制、开机自启、截图/录屏目录、勾选项读写。

use crate::prelude::*;
use crate::*;

pub(crate) const RUN_KEY: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
pub(crate) const RUN_VALUE: &str = APP_NAME;

pub(crate) const SETTINGS_KEY: &str = concat!("Software\\", env!("APP_NAME"));
pub(crate) const HOTKEY_REC_VALUE: &str = "HotkeyRec";   // 识别（跟随当前页）  默认 F1
pub(crate) const HOTKEY_SHOT_VALUE: &str = "HotkeyShot"; // 截图（ScreenCapture）默认 Ctrl+Q
pub(crate) const HOTKEY_ZIP_VALUE: &str = "HotkeyZip";   // 关压缩软件           默认空

// 默认热键的唯一来源：注册、设置页显示、录入回退都取这里，避免四处各写一份字面值。
pub(crate) const DEF_MOD_REC: u32 = 0;
pub(crate) const DEF_VK_REC: u32 = 0x70;              // F1
pub(crate) const DEF_MOD_SHOT: u32 = MOD_CONTROL as u32;
pub(crate) const DEF_VK_SHOT: u32 = b'Q' as u32;      // Ctrl+Q
pub(crate) const DEF_MOD_ZIP: u32 = 0;
pub(crate) const DEF_VK_ZIP: u32 = 0;                 // 空：默认不注册

// 旧默认：识别 F2、截图 F1、关压缩软件 F9。
// 只在注册表里仍是这几个旧值时迁到新默认，用户自己改过的组合不覆盖。
// 关压缩软件默认置空
const OLD_DEF: [(&str, u32, u32, u32, u32); 3] = [
    (HOTKEY_REC_VALUE, 0, 0x71, DEF_MOD_REC, DEF_VK_REC),
    (HOTKEY_SHOT_VALUE, 0, 0x70, DEF_MOD_SHOT, DEF_VK_SHOT),
    (HOTKEY_ZIP_VALUE, 0, 0x78, DEF_MOD_ZIP, DEF_VK_ZIP),
];
pub(crate) const WM_HOTKEY_REC_ID: i32 = 104;
pub(crate) const WM_HOTKEY_SHOT_ID: i32 = 103;
pub(crate) const WM_HOTKEY_ZIP_ID: i32 = 105;

pub(crate) const SET_CHK_AUTO: usize = 5230;  // 开机自动启动
pub(crate) const SET_CHK_TRAY: usize = 5231;  // 关闭按钮最小化到托盘
pub(crate) const SET_CHK_HIDE: usize = 5232;  // 截图时隐藏主界面
pub(crate) const SET_CHK_MULTI: usize = 5234; // 压缩包防多开：同一个压缩包只留一个窗口
pub(crate) const SET_CHK_SINGLE: usize = 5235; // 只允许单实例
pub(crate) const SET_CHK_SHOWMAIN: usize = 5236; // 启动显示主界面
pub(crate) const ANTI_MULTI_VALUE: &str = "AntiMulti"; // 0=关 1=开（默认开）
pub(crate) const SINGLE_INSTANCE_VALUE: &str = "SingleInstance";
pub(crate) const SHOW_MAIN_VALUE: &str = "ShowMainOnStart"; // 0=落托盘 1=显示

pub(crate) const SET_LBL_SHOTDIR: usize = 5240;  // “截图保存到”标签
pub(crate) const SET_ED_SHOTDIR: usize = 5241;   // 截图目录显示框（只读）
pub(crate) const SET_BTN_SHOTDIR: usize = 5242;  // 截图目录“浏览…”
pub(crate) const SET_LBL_VIDDIR: usize = 5243;   // “录屏保存到”标签
pub(crate) const SET_ED_VIDDIR: usize = 5244;    // 录屏目录显示框（只读）
pub(crate) const SET_BTN_VIDDIR: usize = 5245;   // 录屏目录“浏览…”
pub(crate) const SHOT_DIR_VALUE: &str = "ShotDir";   // 截图保存目录（REG_SZ，空=桌面）
pub(crate) const VIDEO_DIR_VALUE: &str = "VideoDir"; // 录屏保存目录（REG_SZ，空=桌面）
pub(crate) const CLOSE_MODE_VALUE: &str = "CloseMode"; // 0=退出 1=到托盘
pub(crate) const HIDE_ON_REC_VALUE: &str = "HideOnRec"; // 0=不隐藏 1=隐藏(默认)
pub(crate) const SEL_SCOPE_VALUE: &str = "SelScope";

pub(crate) const SET_LBL_REC: usize = 5221;
pub(crate) const SET_LBL_SHOT: usize = 5225;
pub(crate) const SET_LBL_ZIP: usize = 5226;
pub(crate) const SET_COMBO_REC: usize = 5211;
pub(crate) const SET_COMBO_SHOT: usize = 5210;
pub(crate) const SET_COMBO_ZIP: usize = 5227;
pub(crate) const SET_RECORD_REC: usize = 5213;
pub(crate) const SET_RECORD_SHOT: usize = 5206;
pub(crate) const SET_RECORD_ZIP: usize = 5228;
pub(crate) const SET_TIMER_ID: usize = 99;
pub(crate) const SET_TIMER_SAVED: usize = 97;    // “已保存”按钮文字的复原定时器（一次性）

pub(crate) static CAPTURING: AtomicBool = AtomicBool::new(false);
pub(crate) static CAPTURE_TARGET: AtomicUsize = AtomicUsize::new(0); // 0=识别, 2=截图, 3=关压缩软件
pub(crate) static CUSTOM_REC: Mutex<Option<(u32, u32, String)>> = Mutex::new(None);
pub(crate) static CUSTOM_SHOT: Mutex<Option<(u32, u32, String)>> = Mutex::new(None);
pub(crate) static CUSTOM_ZIP: Mutex<Option<(u32, u32, String)>> = Mutex::new(None);

pub(crate) static CAP_MODS: AtomicI32 = AtomicI32::new(0);
/// 进入录制的 tick（毫秒）：用来吞掉同一行的连点
static CAP_START_TICK: AtomicI32 = AtomicI32::new(0);

/// 写开机自启。返回注册表的实际写入结果：被组策略 / 安全软件拦下时返回 false，
/// 调用方据此保持原勾选态并提示（，不做"写完再读"的自欺）。
pub(crate) fn set_autostart(on: bool) -> bool {
    unsafe {
        let key = wide(RUN_KEY);
        let mut h: HKEY = std::ptr::null_mut();
        let mut disp: u32 = 0;
        if RegCreateKeyExW(HKEY_CURRENT_USER, key.as_ptr(), 0, std::ptr::null_mut(),
            0, KEY_SET_VALUE, std::ptr::null_mut(), &mut h, &mut disp) != 0 {
            return false;
        }
        let r = if on {
            let val = wide(RUN_VALUE);
            let path = wide(&format!("\"{}\"", exe_path()));
            RegSetValueExW(h, val.as_ptr(), 0, REG_SZ,
                path.as_ptr() as *const u8, (path.len() * 2) as u32)
        } else {
            let val = wide(RUN_VALUE);
            let r = RegDeleteValueW(h, val.as_ptr());
            // 删除成功（0）或本来就没有这个值（2）都算成功：目标状态就是"不开机自启"，
            // 其余错误码原样交给下面判定
            if r == 0 || r == 2 { 0 } else { r }
        };
        RegCloseKey(h);
        r == 0
    }
}

/// 出厂设置落地标记：只写一次，之后用户在设置页怎么改都不覆盖
const DEFAULTS_DONE_VALUE: &str = "DefaultsV8";

/// 出厂设置：开机自启 / 压缩包防多开 / 关闭到托盘 / 只允许单实例 = 开；
/// 截图时隐藏 / 启动显示主界面 = 关。
///
/// 开机自启不能只让勾选框显示成勾——那样注册表里没有 Run 值，等于骗用户；
/// 这里真写一次 Run 键（被组策略/安全软件拦下时 set_autostart 返回 false，
/// 设置页仍按 is_autostart() 的真实结果显示未勾，保持"所见即所得"）。
pub(crate) fn ensure_default_settings() {
    // 「显示/隐藏」热键不再使用，注册表里遗留的那个值要清掉（幂等，每次启动一次）。
    delete_value("HotkeyShow");
    if load_dword(DEFAULTS_DONE_VALUE, 0) == 1 { return; }
    save_dword(ANTI_MULTI_VALUE, 1);
    save_dword(CLOSE_MODE_VALUE, 1);
    save_dword(SINGLE_INSTANCE_VALUE, 1);
    save_dword(HIDE_ON_REC_VALUE, 0);
    save_dword(SHOW_MAIN_VALUE, 0);
    // 热键一次性落到指定组合：截图 Ctrl+Q、识别 F1、关压缩软件置空。
    // 之后用户在设置页改什么都不再覆盖（DefaultsV8 已置 1）。
    save_hotkey(HOTKEY_SHOT_VALUE, DEF_MOD_SHOT, DEF_VK_SHOT);
    save_hotkey(HOTKEY_REC_VALUE, DEF_MOD_REC, DEF_VK_REC);
    save_hotkey(HOTKEY_ZIP_VALUE, DEF_MOD_ZIP, DEF_VK_ZIP);
    set_autostart(true);
    save_dword(DEFAULTS_DONE_VALUE, 1);
}

/// 目录能否真正写入：就地建临时文件再删，不猜权限位（。）
pub(crate) fn is_dir_writable(dir: &str) -> bool {
    let p = std::path::Path::new(dir);
    if dir.trim().is_empty() || !p.is_dir() { return false; }
    let tmp = p.join(format!(".snapwrite_{}.tmp", std::process::id()));
    match std::fs::File::create(&tmp) {
        Ok(_) => { let _ = std::fs::remove_file(&tmp); true }
        Err(_) => false,
    }
}

pub(crate) fn is_autostart() -> bool {
    unsafe {
        let key = wide(RUN_KEY);
        let mut h: HKEY = std::ptr::null_mut();
        if RegOpenKeyExW(HKEY_CURRENT_USER, key.as_ptr(), 0, KEY_QUERY_VALUE, &mut h) == 0 {
            let val = wide(RUN_VALUE);
            let mut ty: u32 = 0;
            let mut sz: u32 = 0;
            let r = RegQueryValueExW(h, val.as_ptr(), std::ptr::null_mut(), &mut ty,
                std::ptr::null_mut(), &mut sz);
            RegCloseKey(h);
            r == 0
        } else {
            false
        }
    }
}

pub(crate) fn load_hotkey(value_name: &str, def_mod: u32, def_vk: u32) -> (u32, u32) {
    load_hotkey_raw(value_name).unwrap_or((def_mod, def_vk))
}

/// 读热键的原始值；键不存在返回 None（区分"没存过"和"存了 0/0"）。
fn load_hotkey_raw(value_name: &str) -> Option<(u32, u32)> {
    unsafe {
        let key = wide(SETTINGS_KEY);
        let mut h: HKEY = std::ptr::null_mut();
        if RegOpenKeyExW(HKEY_CURRENT_USER, key.as_ptr(), 0, KEY_QUERY_VALUE, &mut h) != 0 {
            return None;
        }
        let name = wide(value_name);
        let mut v: u32 = 0;
        let mut sz: u32 = 4;
        let r = RegQueryValueExW(h, name.as_ptr(), std::ptr::null_mut(), std::ptr::null_mut(),
            &mut v as *mut u32 as *mut u8, &mut sz);
        RegCloseKey(h);
        if r != 0 || sz != 4 { return None; }
        Some(((v >> 16) & 0xFFFF, v & 0xFFFF))
    }
}

/// 旧默认只迁一次的标记（写 1 后不再按旧值覆盖，用户后来手动清空的 (0,0) 才留得住）。
/// 关压缩软件默认空，旧配置里是 F9 的机器要迁一次。
const MIGRATED_VALUE: &str = "HotkeyDefsV3";

/// 默认热键落地：没存过的写入新默认；仍停在上版默认的覆盖一次；
/// 用户改过的不动。写回后值已等于新默认，重复调用不会再次改动。
fn ensure_hotkey_defaults() {
    let migrated = load_dword(MIGRATED_VALUE, 0) == 1;
    for (name, old_m, old_v, new_m, new_v) in OLD_DEF {
        match load_hotkey_raw(name) {
            None => save_hotkey(name, new_m, new_v),
            // 只在首次升级时覆盖旧默认；否则用户把"关压缩软件"清空成 (0,0) 后
            // 每次 apply_hotkeys 都会被重新写回 F9
            Some(cur) if !migrated && cur == (old_m, old_v) => save_hotkey(name, new_m, new_v),
            Some(_) => {}
        }
    }
    if !migrated { save_dword(MIGRATED_VALUE, 1); }
}

pub(crate) fn save_hotkey(value_name: &str, reg_mod: u32, vk: u32) {
    unsafe {
        let key = wide(SETTINGS_KEY);
        let mut h: HKEY = std::ptr::null_mut();
        if RegCreateKeyExW(HKEY_CURRENT_USER, key.as_ptr(), 0, std::ptr::null_mut(),
            0, KEY_SET_VALUE, std::ptr::null_mut(), &mut h, std::ptr::null_mut()) == 0 {
            let name = wide(value_name);
            let v = ((reg_mod & 0xFFFF) << 16) | (vk & 0xFFFF);
            RegSetValueExW(h, name.as_ptr(), 0, REG_DWORD,
                &v as *const u32 as *const u8, 4);
            RegCloseKey(h);
        }
    }
}

pub(crate) fn load_dword(value_name: &str, def: u32) -> u32 {
    unsafe {
        let key = wide(SETTINGS_KEY);
        let mut h: HKEY = std::ptr::null_mut();
        if RegOpenKeyExW(HKEY_CURRENT_USER, key.as_ptr(), 0, KEY_QUERY_VALUE, &mut h) != 0 {
            return def;
        }
        let name = wide(value_name);
        let mut v: u32 = 0;
        let mut sz: u32 = 4;
        let r = RegQueryValueExW(h, name.as_ptr(), std::ptr::null_mut(), std::ptr::null_mut(),
            &mut v as *mut u32 as *mut u8, &mut sz);
        RegCloseKey(h);
        if r != 0 || sz != 4 { return def; }
        v
    }
}

/// 删一个配置值（不存在也当成功）。用于清理已经整体移除的功能留下的注册表项。
pub(crate) fn delete_value(value_name: &str) {
    unsafe {
        let key = wide(SETTINGS_KEY);
        let mut h: HKEY = std::ptr::null_mut();
        if RegOpenKeyExW(HKEY_CURRENT_USER, key.as_ptr(), 0, KEY_SET_VALUE, &mut h) == 0 {
            let name = wide(value_name);
            RegDeleteValueW(h, name.as_ptr());
            RegCloseKey(h);
        }
    }
}

pub(crate) fn save_dword(value_name: &str, v: u32) {
    unsafe {
        let key = wide(SETTINGS_KEY);
        let mut h: HKEY = std::ptr::null_mut();
        if RegCreateKeyExW(HKEY_CURRENT_USER, key.as_ptr(), 0, std::ptr::null_mut(),
            0, KEY_SET_VALUE, std::ptr::null_mut(), &mut h, std::ptr::null_mut()) == 0 {
            let name = wide(value_name);
            RegSetValueExW(h, name.as_ptr(), 0, REG_DWORD,
                &v as *const u32 as *const u8, 4);
            RegCloseKey(h);
        }
    }
}

pub(crate) fn load_str(value_name: &str) -> String {
    unsafe {
        let key = wide(SETTINGS_KEY);
        let mut h: HKEY = std::ptr::null_mut();
        if RegOpenKeyExW(HKEY_CURRENT_USER, key.as_ptr(), 0, KEY_QUERY_VALUE, &mut h) != 0 {
            return String::new();
        }
        let name = wide(value_name);
        let mut sz: u32 = 0;
        let mut ty: u32 = 0;
        let mut out = String::new();
        if RegQueryValueExW(h, name.as_ptr(), std::ptr::null_mut(), &mut ty,
            std::ptr::null_mut(), &mut sz) == 0 && sz >= 2
        {
            let mut buf = vec![0u16; (sz as usize / 2) + 1];
            if RegQueryValueExW(h, name.as_ptr(), std::ptr::null_mut(), &mut ty,
                buf.as_mut_ptr() as *mut u8, &mut sz) == 0
            {
                let n = buf.iter().position(|c| *c == 0).unwrap_or(buf.len());
                out = String::from_utf16_lossy(&buf[..n]);
            }
        }
        RegCloseKey(h);
        out
    }
}

pub(crate) fn save_str(value_name: &str, val: &str) {
    unsafe {
        let key = wide(SETTINGS_KEY);
        let mut h: HKEY = std::ptr::null_mut();
        if RegCreateKeyExW(HKEY_CURRENT_USER, key.as_ptr(), 0, std::ptr::null_mut(),
            0, KEY_SET_VALUE, std::ptr::null_mut(), &mut h, std::ptr::null_mut()) == 0 {
            let name = wide(value_name);
            if val.trim().is_empty() {
                RegDeleteValueW(h, name.as_ptr());
            } else {
                let w = wide(val);
                RegSetValueExW(h, name.as_ptr(), 0, REG_SZ,
                    w.as_ptr() as *const u8, (w.len() * 2) as u32);
            }
            RegCloseKey(h);
        }
    }
}

pub(crate) fn pick_folder(hwnd: HWND, title: &str) -> Option<String> {
    unsafe {
        let hr = CoInitializeEx(std::ptr::null_mut(), COINIT_APARTMENTTHREADED);
        let t = wide(title);
        let mut name = vec![0u16; 260];
        let mut bi: BROWSEINFOW = std::mem::zeroed();
        bi.hwnd_owner = hwnd;
        bi.pidl_root = std::ptr::null_mut();
        bi.psz_display_name = name.as_mut_ptr();
        bi.lpsz_title = t.as_ptr();
        // BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX
        bi.ul_flags = 0x0001 | 0x0040 | 0x0010;
        let pidl = SHBrowseForFolderW(&mut bi);
        let mut out = None;
        if !pidl.is_null() {
            let mut p = vec![0u16; 1024];
            if SHGetPathFromIDListW(pidl, p.as_mut_ptr()) != 0 {
                let n = p.iter().position(|c| *c == 0).unwrap_or(0);
                if n > 0 { out = Some(String::from_utf16_lossy(&p[..n])); }
            }
            CoTaskMemFree(pidl);
        }
        if hr == 0 || hr == 1 { CoUninitialize(); }
        out
    }
}

/// 四个热键统一注销：录制进出、重建、退出都走这里，避免上一轮的注册残留
pub(crate) unsafe fn unregister_all(hwnd: HWND) {
    if hwnd.is_null() { return; }
    for id in [WM_HOTKEY_REC_ID, WM_HOTKEY_SHOT_ID, WM_HOTKEY_ZIP_ID] {
        UnregisterHotKey(hwnd, id);
    }
}

pub(crate) fn apply_hotkeys() -> bool {
    unsafe {
        let hwnd = MAIN_HWND.load(Ordering::Relaxed) as HWND;
        if hwnd.is_null() { return true; }
        unregister_all(hwnd);
        ensure_hotkey_defaults();
        let mut failed: Vec<&'static str> = Vec::new();
        let mut reg = |id: i32, def_mod: u32, def_vk: u32, name: &'static str, label: &'static str| {
            let (m, v) = load_hotkey(name, def_mod, def_vk);
            // 空组合不注册
            if (m != 0 || v != 0) && RegisterHotKey(hwnd, id, m, v) == 0 {
                failed.push(label);
            }
        };
        reg(WM_HOTKEY_REC_ID, DEF_MOD_REC, DEF_VK_REC, HOTKEY_REC_VALUE, "识别");            // F1
        reg(WM_HOTKEY_SHOT_ID, DEF_MOD_SHOT, DEF_VK_SHOT, HOTKEY_SHOT_VALUE, "截图");        // Ctrl+Q
        reg(WM_HOTKEY_ZIP_ID, DEF_MOD_ZIP, DEF_VK_ZIP, HOTKEY_ZIP_VALUE, "关压缩软件");      // 默认空
        if !failed.is_empty() {
            let msg = format!("快捷键被其他程序占用：{}", failed.join("、"));
            crate::show_toast(&msg);
        }
        failed.is_empty()
    }
}

//
// 版面：页底色上一张白卡片（42,40,500,382），卡里三组「主蓝竖条 + 组标题 + 1px 分隔线」：
//   快捷键 y=56（三行 92 起、行距 34） / 保存位置 y=200（两行 236 起、行距 36） /
//   常规 y=306（勾选 2 行 3 列，342 起、行距 40、列距 160）。
// 三条贯穿全页的竖线：标签 58 宽 132；值 194 宽 246；动作 448 宽 78。行高 28，勾选框 148×24。
// 本模块只碰摆放、观感与勾选状态；热键录制 / 目录选择 / 注册表键名原样不动。

use winapi::shared::windef::HDC;

// 设置页只读显示框（热键三格 + 两个目录框）共用的白底刷
static VAL_BG: AtomicUsize = AtomicUsize::new(0);

fn val_bg() -> winapi::shared::windef::HBRUSH {
    unsafe {
        let mut h = VAL_BG.load(Ordering::Relaxed) as winapi::shared::windef::HBRUSH;
        if h.is_null() {
            h = CreateSolidBrush(ui::rgb(ui::C_SURFACE));
            VAL_BG.store(h as usize, Ordering::Relaxed);
        }
        h
    }
}

/// 标准只读 EDIT 默认拿「灰面」当底、STATIC 标签也是灰底黑字
/// 文字统一正文色。热键三格 + 目录两格 + 六条行标签都走这里。
pub(crate) unsafe fn ctl_color(dc: HDC, id: usize) -> Option<winapi::shared::windef::HBRUSH> {
    let hit = matches!(
        id,
        SET_COMBO_REC | SET_COMBO_SHOT | SET_COMBO_ZIP
            | SET_ED_SHOTDIR | SET_ED_VIDDIR
            | SET_LBL_REC | SET_LBL_SHOT | SET_LBL_ZIP
            | SET_LBL_SHOTDIR | SET_LBL_VIDDIR
    );
    if !hit {
        return None;
    }
    SetTextColor(dc, ui::rgb(ui::C_TEXT));
    SetBkColor(dc, ui::rgb(ui::C_SURFACE));
    Some(val_bg())
}

// winapi 0.3.9 未导出的几个样式位（取值同 winuser.h）
const SS_LEFTNOWORDWRAP: u32 = 0x0100;
const ES_AUTOHSCROLL: u32 = 0x0080;
const ES_READONLY: u32 = 0x0800;
const EM_SETMARGINS: u32 = 0x00D3;
const EC_LEFTMARGIN: u32 = 0x0001;
const EC_RIGHTMARGIN: u32 = 0x0002;

// 快捷键显示行：显示顺序（截图 → 识别 → 关压缩软件），target 决定取哪组值
const ROW_TARGET: [usize; 3] = [2, 0, 3];
// 常规组勾选：顺序固定（「只截光标所在的屏幕」 没有，不显示）
const CHK_ORDER: [usize; 6] = [SET_CHK_AUTO, SET_CHK_MULTI, SET_CHK_HIDE,
                                SET_CHK_TRAY, SET_CHK_SINGLE, SET_CHK_SHOWMAIN];
static CHK_STATE: Mutex<[bool; 6]> = Mutex::new([false; 6]);
// 本页自绘控件的悬停项（勾选框 / 录入 / 更改…），由子类窗口过程维护
static HOVER_ID: AtomicI32 = AtomicI32::new(0);
static BTN_OLD_PROC: AtomicUsize = AtomicUsize::new(0);

fn chk_index(id: usize) -> Option<usize> {
    CHK_ORDER.iter().position(|x| *x == id)
}

/// 录入按钮是否处于「等待按键」态（复用 CAPTURING / CAPTURE_TARGET，不改其语义）
fn rec_live(id: usize) -> bool {
    if !CAPTURING.load(Ordering::Relaxed) { return false; }
    let t = CAPTURE_TARGET.load(Ordering::Relaxed);
    (id == SET_RECORD_REC && t == 0)
        || (id == SET_RECORD_SHOT && t == 2)
        || (id == SET_RECORD_ZIP && t == 3)
}

/// 快捷键行：显示框 id、录入按钮 id、标签 id、值名、默认修饰键 / 虚拟键、文案
fn row(target: usize) -> (usize, usize, usize, &'static str, u32, u32, &'static str) {
    match target {
        0 => (SET_COMBO_REC, SET_RECORD_REC, SET_LBL_REC, HOTKEY_REC_VALUE, DEF_MOD_REC, DEF_VK_REC, "识别："),
        3 => (SET_COMBO_ZIP, SET_RECORD_ZIP, SET_LBL_ZIP, HOTKEY_ZIP_VALUE, DEF_MOD_ZIP, DEF_VK_ZIP, "关压缩软件："),
        _ => (SET_COMBO_SHOT, SET_RECORD_SHOT, SET_LBL_SHOT, HOTKEY_SHOT_VALUE, DEF_MOD_SHOT, DEF_VK_SHOT, "截图："),
    }
}

/// 自绘控件统一的悬停跟踪：鼠标进出发一次失效，重绘由 draw_item 读 HOVER_ID
unsafe extern "system" fn hover_proc(h: HWND, msg: UINT, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_MOUSEMOVE => {
            let id = GetDlgCtrlID(h);
            if HOVER_ID.swap(id, Ordering::Relaxed) != id {
                InvalidateRect(h, std::ptr::null_mut(), 0);
            }
            track_leave(h);
        }
        WM_MOUSELEAVE => {
            if HOVER_ID.swap(0, Ordering::Relaxed) != 0 {
                InvalidateRect(h, std::ptr::null_mut(), 0);
            }
        }
        _ => {}
    }
    let old = BTN_OLD_PROC.load(Ordering::Relaxed);
    CallWindowProcW(std::mem::transmute::<usize, Option<unsafe extern "system" fn(HWND, UINT, WPARAM, LPARAM) -> LRESULT>>(old),
        h, msg, wp, lp)
}

unsafe fn hook(h: HWND) {
    if h.is_null() { return; }
    let p = SetWindowLongPtrW(h, GWLP_WNDPROC, hover_proc as *const () as usize as isize);
    if BTN_OLD_PROC.load(Ordering::Relaxed) == 0 {
        BTN_OLD_PROC.store(p as usize, Ordering::Relaxed);
    }
}

fn mk(parent: HWND, class: &str, text: &str, style: u32, id: usize,
      x: i32, y: i32, w: i32, h: i32) -> HWND {
    unsafe {
        CreateWindowExW(0, wide(class).as_ptr(), wide(text).as_ptr(),
            WS_CHILD | WS_VISIBLE | style, sc(x), sc(y), sc(w), sc(h),
            parent, id as HMENU, std::ptr::null_mut(), std::ptr::null_mut())
    }
}

pub(crate) unsafe fn create_page(parent: HWND) {
    let f = crate::ui_font(sc(ui::font::SIZE_18), ui::font::W_MEDIUM);

    // 快捷键三行：标签 / 当前键显示框（只读居中）/ 自绘「录入」
    for (i, &target) in ROW_TARGET.iter().enumerate() {
        let (combo, rec, lbl, vname, dm, dv, label) = row(target);
        let y = ui::layout::settings::G1_ROW_Y + ui::layout::settings::G1_PITCH * i as i32;
        let lc = mk(parent, "STATIC", label, SS_CENTERIMAGE | SS_LEFTNOWORDWRAP, lbl,
            ui::layout::settings::LBL_X, y, ui::layout::settings::LBL_W, ui::layout::settings::ROW_H);
        SendMessageW(lc, WM_SETFONT, f as WPARAM, 1);
        let (m, v) = load_hotkey(vname, dm, dv);
        let disp = mk(parent, "EDIT", &hotkey_name(m, v),
            WS_BORDER | ES_READONLY | ES_CENTER, combo,
            ui::layout::settings::VAL_X, y, ui::layout::settings::VAL_W, ui::layout::settings::ROW_H);
        SendMessageW(disp, WM_SETFONT, f as WPARAM, 1);
        hook(mk(parent, "BUTTON", "录入", BS_OWNERDRAW, rec,
            ui::layout::settings::ACT_X, y, ui::layout::settings::ACT_W, ui::layout::settings::ROW_H));
    }

    // 保存位置两行：标签 / 只读路径框 / 自绘「更改…」
    for (i, (lbl, eid, bid, vname, text)) in [
        (SET_LBL_SHOTDIR, SET_ED_SHOTDIR, SET_BTN_SHOTDIR, SHOT_DIR_VALUE, "截图保存："),
        (SET_LBL_VIDDIR, SET_ED_VIDDIR, SET_BTN_VIDDIR, VIDEO_DIR_VALUE, "视频保存："),
    ].iter().enumerate() {
        let y = ui::layout::settings::G2_ROW_Y + ui::layout::settings::G2_PITCH * i as i32;
        let lc = mk(parent, "STATIC", text, SS_CENTERIMAGE | SS_LEFTNOWORDWRAP, *lbl,
            ui::layout::settings::LBL_X, y, ui::layout::settings::LBL_W, ui::layout::settings::ROW_H);
        SendMessageW(lc, WM_SETFONT, f as WPARAM, 1);
        let cur = load_str(vname);
        let cur = if cur.trim().is_empty() { crate::desktop_dir() } else { cur };
        let ec = mk(parent, "EDIT", &cur, WS_BORDER | ES_READONLY | ES_AUTOHSCROLL, *eid,
            ui::layout::settings::VAL_X, y, ui::layout::settings::VAL_W, ui::layout::settings::ROW_H);
        SendMessageW(ec, WM_SETFONT, f as WPARAM, 1);
        // 路径两边留白，贴边起笔像被裁掉一截
        let m6 = (sc(ui::layout::settings::EDIT_PAD_X) & 0xFFFF)
            | ((sc(ui::layout::settings::EDIT_PAD_X) & 0xFFFF) << 16);
        SendMessageW(ec, EM_SETMARGINS, (EC_LEFTMARGIN | EC_RIGHTMARGIN) as WPARAM, m6 as LPARAM);
        hook(mk(parent, "BUTTON", "更改…", BS_OWNERDRAW, *bid,
            ui::layout::settings::ACT_X, y, ui::layout::settings::ACT_W, ui::layout::settings::ROW_H));
    }

    // 常规组六颗勾选：自绘（BS_OWNERDRAW 存不住 check 位，状态由 CHK_STATE 持有）
    // 出厂态：开机自启 / 压缩包防多开 / 关闭到托盘 / 只允许单实例 四个开；
    // 截图时隐藏 / 启动显示主界面 两个关（与出厂设置写下的值一致）
    *CHK_STATE.lock_ok() = [
        is_autostart(),
        load_dword(ANTI_MULTI_VALUE, 1) == 1,
        load_dword(HIDE_ON_REC_VALUE, 0) == 1,
        load_dword(CLOSE_MODE_VALUE, 1) == 1,
        load_dword(SINGLE_INSTANCE_VALUE, 1) == 1,
        load_dword(SHOW_MAIN_VALUE, 0) == 1,
    ];
    for (i, (id, text)) in [
        (SET_CHK_AUTO, "开机自启"),
        (SET_CHK_MULTI, "压缩包防多开"),
        (SET_CHK_HIDE, "截图时隐藏"),
        (SET_CHK_TRAY, "关闭到托盘"),
        (SET_CHK_SINGLE, "只允许单实例"),
        (SET_CHK_SHOWMAIN, "启动显示主界面"),
    ].iter().enumerate() {
        let c = mk(parent, "BUTTON", text, BS_OWNERDRAW, *id,
            ui::layout::settings::LBL_X + ui::layout::settings::CHK_PITCH_X * (i % 3) as i32,
            ui::layout::settings::G3_ROW_Y + ui::layout::settings::CHK_PITCH * (i / 3) as i32,
            ui::layout::settings::CHK_W, ui::layout::settings::CHK_H);
        SendMessageW(c, WM_SETFONT, f as WPARAM, 1);
        hook(c);
    }
}

pub(crate) unsafe fn relayout(parent: HWND) {
    for (i, &target) in ROW_TARGET.iter().enumerate() {
        let (combo, rec, lbl, _, _, _, _) = row(target);
        let y = ui::layout::settings::G1_ROW_Y + ui::layout::settings::G1_PITCH * i as i32;
        for (id, x, w) in [(lbl, ui::layout::settings::LBL_X, ui::layout::settings::LBL_W),
                           (combo, ui::layout::settings::VAL_X, ui::layout::settings::VAL_W),
                           (rec, ui::layout::settings::ACT_X, ui::layout::settings::ACT_W)] {
            let c = GetDlgItem(parent, id as i32);
            if !c.is_null() {
                SetWindowPos(c, std::ptr::null_mut(), sc(x), sc(y), sc(w),
                    sc(ui::layout::settings::ROW_H), SWP_NOZORDER);
            }
        }
    }
    for (i, (lbl, eid, bid)) in [(SET_LBL_SHOTDIR, SET_ED_SHOTDIR, SET_BTN_SHOTDIR),
                                 (SET_LBL_VIDDIR, SET_ED_VIDDIR, SET_BTN_VIDDIR)].iter().enumerate() {
        let y = ui::layout::settings::G2_ROW_Y + ui::layout::settings::G2_PITCH * i as i32;
        for (id, x, w) in [(*lbl, ui::layout::settings::LBL_X, ui::layout::settings::LBL_W),
                           (*eid, ui::layout::settings::VAL_X, ui::layout::settings::VAL_W),
                           (*bid, ui::layout::settings::ACT_X, ui::layout::settings::ACT_W)] {
            let c = GetDlgItem(parent, id as i32);
            if !c.is_null() {
                SetWindowPos(c, std::ptr::null_mut(), sc(x), sc(y), sc(w),
                    sc(ui::layout::settings::ROW_H), SWP_NOZORDER);
            }
        }
    }
    for (i, id) in CHK_ORDER.iter().enumerate() {
        let c = GetDlgItem(parent, *id as i32);
        if !c.is_null() {
            SetWindowPos(c, std::ptr::null_mut(),
                sc(ui::layout::settings::LBL_X + ui::layout::settings::CHK_PITCH_X * (i % 3) as i32),
                sc(ui::layout::settings::G3_ROW_Y + ui::layout::settings::CHK_PITCH * (i / 3) as i32),
                sc(ui::layout::settings::CHK_W), sc(ui::layout::settings::CHK_H), SWP_NOZORDER);
        }
    }
}

pub(crate) unsafe fn show_page(parent: HWND, visible: bool) {
    let show = if visible { SW_SHOW } else { SW_HIDE };
    for id in [SET_LBL_REC, SET_COMBO_REC, SET_RECORD_REC,
               SET_LBL_SHOT, SET_COMBO_SHOT, SET_RECORD_SHOT,
               SET_LBL_ZIP, SET_COMBO_ZIP, SET_RECORD_ZIP,
               SET_LBL_SHOTDIR, SET_ED_SHOTDIR, SET_BTN_SHOTDIR,
               SET_LBL_VIDDIR, SET_ED_VIDDIR, SET_BTN_VIDDIR,
               SET_CHK_AUTO, SET_CHK_MULTI, SET_CHK_HIDE, SET_CHK_TRAY,
               SET_CHK_SINGLE, SET_CHK_SHOWMAIN] {
        let c = GetDlgItem(parent, id as i32);
        if !c.is_null() { ShowWindow(c, show); }
    }
}

/// 只接管常规组勾选；录入 / 更改… 交给主线原有处理（返回 false）
pub(crate) unsafe fn on_command(id: usize, hwnd: HWND) -> bool {
    let Some(idx) = chk_index(id) else { return false };
    let on = {
        let mut st = CHK_STATE.lock_ok();
        st[idx] = !st[idx];
        st[idx]
    };
    // 开机自启以注册表实际写入结果为准：没写进去就退回原勾选态并提示
    if id == SET_CHK_AUTO && !set_autostart(on) {
        CHK_STATE.lock_ok()[idx] = !on;
        let c = GetDlgItem(hwnd, id as i32);
        if !c.is_null() { InvalidateRect(c, std::ptr::null_mut(), 0); }
        crate::show_toast("开机自启设置失败");
        return true;
    }
    let toast = match id {
        SET_CHK_AUTO => if on { "开机自启已开启" } else { "开机自启已关闭" },
        SET_CHK_TRAY => {
            save_dword(CLOSE_MODE_VALUE, on as u32);
            if on { "关闭到托盘已开启" } else { "关闭到托盘已关闭" }
        }
        SET_CHK_HIDE => {
            save_dword(HIDE_ON_REC_VALUE, on as u32);
            if on { "截图隐藏已开启" } else { "截图隐藏已关闭" }
        }
        SET_CHK_SINGLE => {
            save_dword(SINGLE_INSTANCE_VALUE, on as u32);
            if on { "单实例已开启" } else { "单实例已关闭" }
        }
        SET_CHK_SHOWMAIN => {
            save_dword(SHOW_MAIN_VALUE, on as u32);
            if on { "启动显示已开启" } else { "启动显示已关闭" }
        }
        _ => {
            save_dword(ANTI_MULTI_VALUE, on as u32);
            if on { "防多开已开启" } else { "防多开已关闭" }
        }
    };
    let c = GetDlgItem(hwnd, id as i32);
    if !c.is_null() { InvalidateRect(c, std::ptr::null_mut(), 0); }
    crate::show_toast(toast);
    true
}

/// 页面底纹：白卡片 + 三组标题（主蓝竖条 3×14）+ 1px 分隔线
pub(crate) unsafe fn draw(dc: HDC) {
    let card = RECT {
        left: sc(ui::layout::settings::CARD_X), top: sc(ui::layout::settings::CARD_Y),
        right: sc(ui::layout::settings::CARD_X + ui::layout::settings::CARD_W),
        bottom: sc(ui::layout::settings::CARD_Y + ui::layout::settings::CARD_H),
    };
    let white = CreateSolidBrush(ui::color::cref(ui::color::SURFACE));
    FillRect(dc, &card, white as HBRUSH);
    DeleteObject(white as *mut winapi::ctypes::c_void);
    let border = CreateSolidBrush(ui::color::cref(ui::color::BORDER));
    FrameRect(dc, &card, border as HBRUSH);
    let accent = CreateSolidBrush(ui::color::cref(ui::color::GROUP_BAR));
    SetBkMode(dc, TRANSPARENT as i32);
    SetTextColor(dc, ui::color::cref(ui::color::TEXT));
    let old = SelectObject(dc,
        crate::ui_font(sc(ui::font::SIZE_18), ui::font::W_BOLD) as *mut winapi::ctypes::c_void);
    for (title, gy) in [("快捷键", ui::layout::settings::G1_Y),
                        ("保存位置", ui::layout::settings::G2_Y),
                        ("常规", ui::layout::settings::G3_Y)] {
        let ty = sc(gy);
        let th = sc(ui::layout::settings::TITLE_H);
        let bh = sc(ui::layout::settings::BAR_H);
        FillRect(dc, &RECT { left: sc(ui::layout::settings::LBL_X), top: ty + (th - bh) / 2,
            right: sc(ui::layout::settings::LBL_X) + sc(ui::layout::settings::BAR_W),
            bottom: ty + (th + bh) / 2 }, accent);
        let mut tr = RECT {
            left: sc(ui::layout::settings::LBL_X + ui::layout::settings::TITLE_DX), top: ty,
            right: sc(ui::layout::settings::CARD_X + ui::layout::settings::CARD_W
                - ui::layout::settings::PAD),
            bottom: ty + th };
        DrawTextW(dc, wide(title).as_ptr(), -1, &mut tr,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        let ly = sc(gy + ui::layout::settings::TITLE_H + ui::layout::settings::GAP_TITLE);
        FillRect(dc, &RECT { left: sc(ui::layout::settings::CARD_X + ui::layout::settings::PAD),
            top: ly,
            right: sc(ui::layout::settings::CARD_X + ui::layout::settings::CARD_W
                - ui::layout::settings::PAD),
            bottom: ly + sc(ui::layout::settings::LINE_H) }, border);
    }
    SelectObject(dc, old);
    DeleteObject(accent as *mut winapi::ctypes::c_void);
    DeleteObject(border as *mut winapi::ctypes::c_void);
}

/// 本页自绘控件的 WM_DRAWITEM：勾选框 / 「录入」/「更改…」。
/// 返回 true 表示已画（主线在 WM_DRAWITEM 里先问本模块，再走通用按钮画法）。
pub(crate) unsafe fn draw_item(item: &DRAWITEMSTRUCT) -> bool {
    let r = item.rcItem;
    let id = item.CtlID as usize;

    if let Some(idx) = chk_index(id) {
        let on = CHK_STATE.lock_ok()[idx];
        let hot = HOVER_ID.load(Ordering::Relaxed) == item.CtlID as i32;
        // 卡片白底：悬停压暗一档；整块填底，避免留下别的页的旧像素
        let bg = if hot { ui::color::cref(ui::color::dim(ui::color::SURFACE, 92)) }
                 else { ui::color::cref(ui::color::SURFACE) };
        let b = CreateSolidBrush(bg);
        FillRect(item.hDC, &r, b as HBRUSH);
        DeleteObject(b as *mut winapi::ctypes::c_void);
        let bs = sc(ui::layout::settings::CHK_BOX);
        let by = r.top + (r.bottom - r.top - bs) / 2;
        let box_r = RECT { left: r.left + sc(ui::layout::settings::CHK_BOX_INSET), top: by,
            right: r.left + sc(ui::layout::settings::CHK_BOX_INSET) + bs, bottom: by + bs };
        let face = if on { ui::color::cref(ui::color::ACCENT_DEEP) }
                   else { ui::color::cref(ui::color::SURFACE) };
        let f = CreateSolidBrush(face);
        FillRect(item.hDC, &box_r, f as HBRUSH);
        DeleteObject(f as *mut winapi::ctypes::c_void);
        let edge = if on { ui::color::cref(ui::color::ACCENT_DEEP) }
                   else { ui::color::cref(ui::color::BORDER) };
        let e = CreateSolidBrush(edge);
        FrameRect(item.hDC, &box_r, e as HBRUSH);
        DeleteObject(e as *mut winapi::ctypes::c_void);
        if on {
            // 白色对勾：复用 ui-check 的 SVG 蒙版（与主界面、定时关机页同一处图形定义）
            svg_icon_blit(item.hDC, box_r.left, by, bs, "ui-check",
                ui::color::ACCENT_DEEP, ui::color::TEXT_INVERSE);
        }
        let mut text = [0u16; 64];
        let n = GetWindowTextW(item.hwndItem, text.as_mut_ptr(), 64);
        SetBkMode(item.hDC, TRANSPARENT as i32);
        SetTextColor(item.hDC, ui::color::cref(ui::color::TEXT));
        let of = SelectObject(item.hDC,
            crate::ui_font(sc(ui::font::SIZE_18), ui::font::W_MEDIUM) as *mut winapi::ctypes::c_void);
        let mut tr = RECT { left: box_r.right + sc(ui::layout::settings::CHK_TEXT_GAP), top: r.top,
            right: r.right, bottom: r.bottom };
        DrawTextW(item.hDC, text.as_ptr(), n as i32, &mut tr,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(item.hDC, of);
        return true;
    }

    let is_rec = matches!(id, SET_RECORD_REC | SET_RECORD_SHOT | SET_RECORD_ZIP);
    if !(is_rec || id == SET_BTN_SHOTDIR || id == SET_BTN_VIDDIR) {
        return false;
    }
    let hot = HOVER_ID.load(Ordering::Relaxed) == item.CtlID as i32;
    let live = is_rec && rec_live(id);
    let bg = if hot { ui::color::cref(ui::color::dim(ui::color::SURFACE, 92)) }
             else { ui::color::cref(ui::color::SURFACE) };
    let b = CreateSolidBrush(bg);
    FillRect(item.hDC, &r, b as HBRUSH);
    DeleteObject(b as *mut winapi::ctypes::c_void);
    // 录入中描边转主蓝；「更改…」悬停同样转蓝；其余常态走卡片描边色
    let edge = if live || (!is_rec && hot) { ui::color::cref(ui::color::ACCENT) }
               else { ui::color::cref(ui::color::BORDER) };
    let e = CreateSolidBrush(edge);
    FrameRect(item.hDC, &r, e as HBRUSH);
    DeleteObject(e as *mut winapi::ctypes::c_void);
    let mut text = [0u16; 64];
    let n = GetWindowTextW(item.hwndItem, text.as_mut_ptr(), 64);
    SetBkMode(item.hDC, TRANSPARENT as i32);
    SetTextColor(item.hDC, if live { ui::color::cref(ui::color::ACCENT) }
                          else { ui::color::cref(ui::color::TEXT) });
    let of = SelectObject(item.hDC,
        crate::ui_font(sc(ui::font::SIZE_18), ui::font::W_MEDIUM) as *mut winapi::ctypes::c_void);
    let mut tr = r;
    DrawTextW(item.hDC, text.as_ptr(), n as i32, &mut tr,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, of);
    true
}

pub(crate) fn hotkey_reserved(name: &str) -> bool {
    const RESERVED: [&str; 18] = [
        "Ctrl+C", "Ctrl+V", "Ctrl+X", "Ctrl+Z", "Ctrl+Y", "Ctrl+A", "Ctrl+S",
        "Ctrl+Alt+Delete", "Ctrl+Shift+Esc", "Ctrl+Esc",
        "Alt+Tab", "Alt+Esc", "Alt+F4",
        "Win+L", "Win+D", "Win+E", "Win+R", "Win+Tab",
    ];
    RESERVED.iter().any(|r| r.eq_ignore_ascii_case(name))
}

pub(crate) fn hotkey_name(m: u32, vk: u32) -> String {
    if m == 0 && vk == 0 { return "无".to_string(); }
    let mut parts: Vec<&str> = Vec::new();
    if m & MOD_CONTROL as u32 != 0 { parts.push("Ctrl"); }
    if m & MOD_ALT as u32 != 0 { parts.push("Alt"); }
    if m & MOD_SHIFT as u32 != 0 { parts.push("Shift"); }
    if m & MOD_WIN as u32 != 0 { parts.push("Win"); }
    let key = if vk >= 0x30 && vk <= 0x39 {
        format!("{}", (b'0' + (vk - 0x30) as u8) as char)
    } else if vk >= 0x41 && vk <= 0x5A {
        format!("{}", (b'A' + (vk - 0x41) as u8) as char)
    } else if vk >= 0x70 && vk <= 0x7B {
        format!("F{}", vk - 0x6F)
    } else if vk == 0x20 { "Space".to_string() } else if vk == 0x0D { "Enter".to_string() }
    else if vk == 0x25 { "Left".to_string() } else if vk == 0x26 { "Up".to_string() }
    else if vk == 0x27 { "Right".to_string() } else if vk == 0x28 { "Down".to_string() }
    else if vk == 0x21 { "PgUp".to_string() } else if vk == 0x22 { "PgDn".to_string() }
    else if vk == 0x23 { "End".to_string() } else if vk == 0x24 { "Home".to_string() }
    else if vk == 0x2D { "Insert".to_string() } else if vk == 0x2E { "Delete".to_string() }
    else { format!("VK{:X}", vk) };
    parts.push(&key);
    parts.join("+")
}

/// 进入录制后 300ms 内视为同一次点击（防连点/双击立刻把自己取消掉）
pub(crate) fn cap_recent() -> bool {
    let t = CAP_START_TICK.load(Ordering::Relaxed);
    t != 0 && (unsafe { winapi::um::sysinfoapi::GetTickCount() } as i32).wrapping_sub(t) < 300
}

pub(crate) unsafe fn cap_begin(hwnd: HWND, target: usize) {
    CAPTURING.store(true, Ordering::Relaxed);
    CAPTURE_TARGET.store(target, Ordering::Relaxed);
    CAP_START_TICK.store(winapi::um::sysinfoapi::GetTickCount() as i32, Ordering::Relaxed);
    CAP_MODS.store(0, Ordering::Relaxed);
    // 上一轮「已保存」的复原定时器会把按钮文字改回「录入」，进入录制前先清掉
    KillTimer(hwnd, SET_TIMER_SAVED as usize);
    for id in [SET_RECORD_REC, SET_RECORD_SHOT, SET_RECORD_ZIP] {
        let b = GetDlgItem(hwnd, id as i32);
        if !b.is_null() { SetWindowTextW(b, wide("录入").as_ptr()); }
    }
    unregister_all(hwnd);
    // 排空候选键的"按下沿"：录制前按过的键会留下低位标记，不清掉一进录制就被当成本次输入
    for vk in (0x30..=0x39).chain(0x41..=0x5A).chain(0x70..=0x7B).chain(
        [0x20, 0x0D, 0x25, 0x26, 0x27, 0x28, 0x21, 0x22, 0x23, 0x24, 0x2D, 0x2C, 0x91, 0x13, 0x1B, 0x2E],
    ) {
        GetAsyncKeyState(vk);
    }
    let (combo_id, rec_id, _, _, _, _, _, _) = hotkey_row(target);
    let disp = GetDlgItem(hwnd, combo_id as i32);
    if !disp.is_null() { SendMessageW(disp, 0x000C /*WM_SETTEXT*/, 0, wide("请按键…(Esc 取消·Delete 清空)").as_ptr() as LPARAM); }
    let rec = GetDlgItem(hwnd, rec_id as i32);
    if !rec.is_null() {
        SetWindowTextW(rec, wide("取消").as_ptr());
        InvalidateRect(rec, std::ptr::null_mut(), 0);
    }
    SetTimer(hwnd, SET_TIMER_ID as usize, 40, None);
}

pub(crate) unsafe fn cap_cancel(hwnd: HWND) {
    let target = CAPTURE_TARGET.load(Ordering::Relaxed);
    let was = CAPTURING.swap(false, Ordering::Relaxed);
    CAP_MODS.store(0, Ordering::Relaxed);
    KillTimer(hwnd, SET_TIMER_ID as usize);
    if !was { return; }
    let (combo_id, rec_id, store, vname, dm, dv, _, _) = hotkey_row(target);
    let (m, v) = match *store.lock_ok() {
        Some((m, v, _)) => (m, v),
        None => load_hotkey(vname, dm, dv),
    };
    let disp = GetDlgItem(hwnd, combo_id as i32);
    if !disp.is_null() {
        SendMessageW(disp, 0x000C /*WM_SETTEXT*/, 0, wide(&hotkey_name(m, v)).as_ptr() as LPARAM);
    }
    let rec = GetDlgItem(hwnd, rec_id as i32);
    if !rec.is_null() {
        SetWindowTextW(rec, wide("录入").as_ptr());
        InvalidateRect(rec, std::ptr::null_mut(), 0);
    }
    // 取消录入也要把热键挂回去；失败提示由 apply_hotkeys 统一发一次
    apply_hotkeys();
}

pub(crate) fn hotkey_row(target: usize) -> (usize, usize, &'static Mutex<Option<(u32, u32, String)>>,
    &'static str, u32, u32, &'static str, &'static str) {
    match target {
        0 => (SET_COMBO_REC, SET_RECORD_REC, &CUSTOM_REC, HOTKEY_REC_VALUE, DEF_MOD_REC, DEF_VK_REC,
              "识别快捷键:", "请按“识别”的快捷键（Esc 取消）…"),
        3 => (SET_COMBO_ZIP, SET_RECORD_ZIP, &CUSTOM_ZIP, HOTKEY_ZIP_VALUE, DEF_MOD_ZIP, DEF_VK_ZIP,
              "关压缩软件:", "请按“关压缩软件”的快捷键（Esc 取消）…"),
        _ => (SET_COMBO_SHOT, SET_RECORD_SHOT, &CUSTOM_SHOT, HOTKEY_SHOT_VALUE, DEF_MOD_SHOT, DEF_VK_SHOT,
              "截图快捷键:", "请按“截图”的快捷键（Esc 取消）…"),
    }
}
