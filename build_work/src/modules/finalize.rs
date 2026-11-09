use crate::prelude::*;
use crate::*;

pub(crate) unsafe fn anno_close() {
    // 录制条是独立顶层窗，录制线程也是另一条线程：只关编辑器不收它们的话，
    // "录制条开着但还没开录"会在屏幕上留下一片点不动的幽灵条（主条已经没了，没人再能关它），
    // "正在录"则是后台线程继续往临时文件里写。录制中关窗一律丢弃这段录制
    if REC_PHASE.load(Ordering::Relaxed) == 1 { rec_abort(); }
    rec_bar_close();
    tip_destroy(&TIP_MAIN);
    tip_destroy(&TIP_SUB);
    let sub = ANNO_SUB_HWND.load(Ordering::Relaxed) as HWND;
    if !sub.is_null() { DestroyWindow(sub); }
    let main = ANNO_MAIN_HWND.load(Ordering::Relaxed) as HWND;
    if !main.is_null() { DestroyWindow(main); }
    let img = ANNO_HWND.load(Ordering::Relaxed) as HWND;
    if !img.is_null() { DestroyWindow(img); }
    ANNO_SUB_HWND.store(0, Ordering::Relaxed);
    ANNO_MAIN_HWND.store(0, Ordering::Relaxed);
    ANNO_HWND.store(0, Ordering::Relaxed);
    ANNO_PIN.store(false, Ordering::Relaxed);
    ANNO_TOOL.store(-1, Ordering::Relaxed);
    ANNO_PREVIEW.store(-1, Ordering::Relaxed);
    ANNO_PENDING.store(-1, Ordering::Relaxed);
    // clear() 只把长度清零，容量还挂在 Vec 上：一张 1080p 选区就是 8MB，撤销栈里可能还有几份。
    // 这里直接换成空 Vec，让这几块缓冲立刻还给系统（截图关掉后内存能回到空闲水位）。
    anno_tx_cancel();               // 先收掉编辑态（内部会重放一次），再释放缓冲
    *ANNO_IMG.lock_ok() = Vec::new();
    *ANNO_ORIG.lock_ok() = Vec::new();
    // 拖动笔画时的"拖动前快照"也是一整张图，和正文一起还给系统
    *ANNO_PRE.lock_ok() = Vec::new();
    // 拖动最后一条时的前缀底图同理（正常抬手会释放；这里兜住"拖到一半就关窗"）
    anno_base_clear();
    ANNO_PTS.lock_ok().clear();
    *ANNO_UNDO.lock_ok() = Vec::new();
    *ANNO_REDO.lock_ok() = Vec::new();
    *ANNO_SHAPES.lock_ok() = Vec::new();
    anno_last_clear();          // 关窗：最后一条的几何也一起丢掉
    // 全屏合成缓冲（4K 下 33MB）也一起还给系统
    anno_buf_free();
    // 编辑器背景用的整屏副本（见 WM_LBUTTONUP 的 mode==3 分支），编辑结束就还给系统
    let _ = SEL_BGRA.lock_ok().take();
    // 截图全程保持主程序在托盘，不跳回主界面
}

/// 桌面上的 PNG 路径
///
/// 桌面目录必须走 Known Folder API（可能被重定向到 OneDrive），不能拼 %USERPROFILE%。
/// SHGetKnownFolderPath 要求调用线程初始化过 COM；若外层已初始化，CoInitializeEx 返回
/// S_FALSE，只有 S_OK/S_FALSE 才配对 CoUninitialize。
pub(crate) unsafe fn desktop_png_path() -> String { desktop_media_path("png") }

/// 桌面目录（Known Folder API，可能被重定向到 OneDrive）
pub(crate) unsafe fn desktop_dir() -> String {
    let hr = CoInitializeEx(std::ptr::null_mut(), COINIT_APARTMENTTHREADED);
    let mut raw: *mut u16 = std::ptr::null_mut();
    let r = SHGetKnownFolderPath(&FOLDERID_Desktop as *const _, 0, std::ptr::null_mut(), &mut raw);
    let mut dir = String::new();
    if r == 0 && !raw.is_null() {
        let mut n = 0usize;
        while *raw.add(n) != 0 { n += 1; }
        dir = String::from_utf16_lossy(std::slice::from_raw_parts(raw, n));
        CoTaskMemFree(raw as *mut winapi::ctypes::c_void);
    }
    if hr == 0 || hr == 1 { CoUninitialize(); }
    dir
}

/// 当前实际用的保存目录：设置页里填过且目录还在就用它，否则回落到桌面
pub(crate) unsafe fn media_dir(ext: &str) -> String {
    let conf = load_str(if ext.eq_ignore_ascii_case("png") { SHOT_DIR_VALUE } else { VIDEO_DIR_VALUE });
    let conf = conf.trim().trim_end_matches('\\').to_string();
    if !conf.is_empty() && std::path::Path::new(&conf).is_dir() { return conf; }
    desktop_dir()
}

pub(crate) unsafe fn desktop_media_path(ext: &str) -> String {
    let dir = media_dir(ext);
    if dir.is_empty() { return String::new(); }
    // 年4 月2 日2 时2 分2 秒2 毫秒3
    let mut st: winapi::um::minwinbase::SYSTEMTIME = std::mem::zeroed();
    winapi::um::sysinfoapi::GetLocalTime(&mut st);
    format!("{}\\{:04}{:02}{:02}{:02}{:02}{:02}{:03}.{}",
        dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ext)
}

/// 把一块 BGRA 像素落成桌面 PNG。
/// 成功返回落盘路径；桌面目录拿不到（极少数策略禁用账户）或编码失败返回 None。
/// 编辑器保存和贴屏窗口 Ctrl+S 共用这一份，不再各写一遍 BGRA→RGBA。
pub(crate) unsafe fn save_bgra_desktop(w: i32, h: i32, img: &[u8]) -> Option<String> {
    if w <= 0 || h <= 0 || img.len() < (w * h * 4) as usize { return None; }
    let path = desktop_png_path();
    if path.is_empty() { return None; }
    // image 用 RGBA，截图缓冲是 BGRA：逐像素交换 R/B，alpha 原样保留
    let mut rgba = Vec::with_capacity((w * h * 4) as usize);
    for i in 0..(w * h) as usize {
        let o = i * 4;
        rgba.push(img[o + 2]);
        rgba.push(img[o + 1]);
        rgba.push(img[o]);
        rgba.push(img[o + 3]);
    }
    let im = image::RgbaImage::from_raw(w as u32, h as u32, rgba)?;
    im.save(&path).ok()?;
    Some(path)
}


pub(crate) unsafe fn anno_save() {
    // 不弹"另存为"，直接落桌面 + 应用内轻提示 + 关窗
    let w = ANNO_W.load(Ordering::Relaxed);
    let h = ANNO_H.load(Ordering::Relaxed);
    if w <= 0 || h <= 0 { return; }
    anno_tx_finish();          // 先把编辑中的文字收笔，导出的才是所见
    let img = anno_tx_flat();  // 文字图元烘进副本
    if img.is_empty() { return; }
    if save_bgra_desktop(w, h, &img).is_some() {
        show_toast("已保存");
        anno_close();
    } else {
        // 写盘失败不关窗，只给一句提示，让用户换目录再存
        show_toast("保存失败：保存目录不可写或磁盘已满，请在「设置」里换一个目录");
    }
}

// 把一块 BGRA 图送进剪贴板。编辑器复制和贴屏双击复制共用这一份
//
// 谁认哪种就用哪种
//   1) CF_DIBV5  —— 32bpp、BI_BITFIELDS、带 alpha、top-down（Office / 现代软件）
//   2) CF_DIB    —— 24bpp、BI_RGB、自下而上（老软件只认最传统的这一种）
//   3) 注册格式 PNG —— 浏览器、即时通讯软件优先取它
pub(crate) unsafe fn anno_copy_bgra(w: i32, h: i32, img: &[u8]) -> bool {
    if img.is_empty() || w <= 0 || h <= 0 { return false; }
    let row_bytes = (w * 4) as usize;
    let img_bytes = row_bytes * h as usize;
    if img.len() < img_bytes { return false; }

    // ---------- 1) PNG ----------
    // encodePng 是 32bppBGRA 直出，alpha 原样保留，截图本来就不透明，两者结果一致
    let mut rgba = Vec::with_capacity(img_bytes);
    for px in img.chunks_exact(4) {
        rgba.extend_from_slice(&[px[2], px[1], px[0], px[3]]);
    }
    let mut png: Vec<u8> = Vec::new();
    if image::ImageEncoder::write_image(
        image::codecs::png::PngEncoder::new(&mut png),
        &rgba, w as u32, h as u32, image::ExtendedColorType::Rgba8,
    ).is_err() { return false; }
    let h_png = GlobalAlloc(GMEM_MOVEABLE, png.len());
    if h_png.is_null() { return false; }
    {
        let d = GlobalLock(h_png) as *mut u8;
        if d.is_null() { GlobalFree(h_png); return false; }
        std::ptr::copy_nonoverlapping(png.as_ptr(), d, png.len());
        GlobalUnlock(h_png);
    }

    // ---------- 2) CF_DIBV5（带 alpha） ----------
    let v5sz = std::mem::size_of::<winapi::um::wingdi::BITMAPV5HEADER>();
    let h_v5 = GlobalAlloc(GMEM_MOVEABLE, v5sz + img_bytes);
    if h_v5.is_null() { GlobalFree(h_png); return false; }
    {
        let p = GlobalLock(h_v5) as *mut u8;
        if p.is_null() { GlobalFree(h_v5); GlobalFree(h_png); return false; }
        let mut v5: winapi::um::wingdi::BITMAPV5HEADER = std::mem::zeroed();
        v5.bV5Size = v5sz as u32;
        v5.bV5Width = w;
        v5.bV5Height = -h;
        v5.bV5Planes = 1;
        v5.bV5BitCount = 32;
        v5.bV5Compression = winapi::um::wingdi::BI_BITFIELDS;
        v5.bV5SizeImage = img_bytes as u32;
        v5.bV5RedMask = 0x00FF_0000;
        v5.bV5GreenMask = 0x0000_FF00;
        v5.bV5BlueMask = 0x0000_00FF;
        v5.bV5AlphaMask = 0xFF00_0000;
        v5.bV5CSType = winapi::um::wingdi::LCS_sRGB as u32;
        v5.bV5Intent = winapi::um::wingdi::LCS_GM_GRAPHICS as u32;
        std::ptr::copy_nonoverlapping(&v5 as *const _ as *const u8, p, v5sz);
        std::ptr::copy_nonoverlapping(img.as_ptr(), p.add(v5sz), img_bytes);
        GlobalUnlock(h_v5);
    }

    // ---------- 3) CF_DIB（24bpp、自下而上、行 4 字节对齐） ----------
    let dib_row = ((w as usize * 3) + 3) & !3usize;
    let dib_bytes = dib_row * h as usize;
    let hdr = std::mem::size_of::<BITMAPINFOHEADER>();
    let h_dib = GlobalAlloc(GMEM_MOVEABLE, hdr + dib_bytes);
    if h_dib.is_null() { GlobalFree(h_v5); GlobalFree(h_png); return false; }
    {
        let p = GlobalLock(h_dib) as *mut u8;
        if p.is_null() { GlobalFree(h_dib); GlobalFree(h_v5); GlobalFree(h_png); return false; }
        let mut bi: BITMAPINFOHEADER = std::mem::zeroed();
        bi.biSize = hdr as u32;
        bi.biWidth = w;
        bi.biHeight = h;          // 正 = 自下而上
        bi.biPlanes = 1;
        bi.biBitCount = 24;
        bi.biCompression = BI_RGB;
        bi.biSizeImage = dib_bytes as u32;
        std::ptr::copy_nonoverlapping(&bi as *const _ as *const u8, p, hdr);
        let dst_base = p.add(hdr);
        for row in 0..h as usize {
            let src = img.as_ptr().add(row * row_bytes);
            let dst = dst_base.add((h as usize - 1 - row) * dib_row);
            for col in 0..w as usize {
                std::ptr::copy_nonoverlapping(src.add(col * 4), dst.add(col * 3), 3);
            }
        }
        GlobalUnlock(h_dib);
    }

    if OpenClipboard(std::ptr::null_mut()) == 0 {
        GlobalFree(h_dib); GlobalFree(h_v5); GlobalFree(h_png);
        return false;
    }
    EmptyClipboard();
    // 成功后 HGLOBAL 归剪贴板所有，失败才自己释放。
    // 成功口径取主格式 CF_DIBV5（其余都是兼容兜底）
    let cf_v5 = winapi::um::winuser::CF_DIBV5;
    let ok = !SetClipboardData(cf_v5, h_v5 as *mut winapi::ctypes::c_void).is_null();
    if !ok {
        GlobalFree(h_v5);
    }
    if SetClipboardData(8 /*CF_DIB*/, h_dib as *mut winapi::ctypes::c_void).is_null() {
        GlobalFree(h_dib);
    }
    let cf_png = winapi::um::winuser::RegisterClipboardFormatW(wide("PNG").as_ptr());
    if cf_png == 0 || SetClipboardData(cf_png, h_png as *mut winapi::ctypes::c_void).is_null() {
        GlobalFree(h_png);
    }
    CloseClipboard();
    ok
}

pub(crate) unsafe fn anno_copy() {
    let w = ANNO_W.load(Ordering::Relaxed);
    let h = ANNO_H.load(Ordering::Relaxed);
    anno_tx_finish();
    let img = anno_tx_flat();
    if anno_copy_bgra(w, h, &img) {
        anno_close(); // 复制成功后关闭编辑器（原版行为）
    } else {
        // cap.copyFail：剪贴板被别的程序占着 / 图片太大，给提示并留在编辑器里
        show_toast("复制到剪切板失败，请重试");
    }
}

/// 把窗口提到前台并拿到键盘焦点。
///
/// 全局热键唤起的窗口不属于当前前台进程，系统"前台锁"会直接让 SetForegroundWindow
/// 返回 0：窗口看着压在最上层，其实收不到键盘，Ctrl+S / Esc 这些快捷键全是哑的。
/// 标准解法是把本线程的输入队列临时附着到当前前台线程上，再夺一次前台。
pub(crate) unsafe fn force_foreground(hwnd: HWND) {
    if SetForegroundWindow(hwnd) != 0 {
        SetFocus(hwnd);
        return;
    }
    let fg = GetForegroundWindow();
    let fg_tid = if fg.is_null() { 0 } else { GetWindowThreadProcessId(fg, std::ptr::null_mut()) };
    let my_tid = GetCurrentThreadId();
    let attach = fg_tid != 0 && fg_tid != my_tid;
    if attach { AttachThreadInput(my_tid, fg_tid, 1); }
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if attach { AttachThreadInput(my_tid, fg_tid, 0); }
}
