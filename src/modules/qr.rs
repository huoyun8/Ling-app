//! 二维码页：生成 / 识别（rqrr）/ LOGO 合成 / 页面控件与交互。
//!
//! 识别用 rqrr：1~4 倍最近邻放大 + Otsu 二值化预处理，再交给 rqrr 定位与纠错，
//! 取第一个有效结果。150px 小图与带 LOGO 的码都能解；
//! 极端码可切到 `reference/qr_rxing.rs.txt` 里的备用实现。
//!
//! 页面接口与 `modules/verify.rs` 同构：`create_page` / `relayout` / `show_page` /
//! `on_command` / `draw`，另有 `on_notify`（EN_CHANGE / EN_SETFOCUS）供 WM_COMMAND 转发。
//! 颜色 / 尺寸 / 字号 / 圆角 / 间距一律引用 `ui::*`，本模块不出现裸值。

use crate::prelude::*;
use crate::*;

use qrcode::bits::Bits;
use qrcode::Version;

/// 页面控件 ID： 4100 段（4108 / 4109 已空出）
// 号段保留不复用，免得新旧 ID 在合并时撞车。
pub(crate) const QR_EDIT: usize = 4101;
pub(crate) const QR_BTN_DOWNLOAD: usize = 4102; // 下载二维码
pub(crate) const QR_BTN_SCREEN: usize = 4103;   // 识别二维码
pub(crate) const QR_BTN_COPY: usize = 4105;     // 复制二维码
pub(crate) const QR_BTN_UPLOAD: usize = 4106;   // 上传 LOGO
pub(crate) const QR_BTN_CLEAR: usize = 4107;    // 清空

/// 按钮自上而下的顺序。
const QR_BTNS: [(usize, &str); 5] = [
    (QR_BTN_CLEAR, "清空"),
    (QR_BTN_SCREEN, "识别二维码"),
    (QR_BTN_COPY, "复制二维码"),
    (QR_BTN_DOWNLOAD, "下载二维码"),
    (QR_BTN_UPLOAD, "上传 LOGO"),
];

/// 空输入时卡片上的示例码内容（与输入框占位提示同一句）。
pub(crate) const QR_PLACEHOLDER: &str = "请输入文本内容！";

/// 静区模块数：kQuietModule 一致
const QR_QUIET: u32 = 1;

/// 复制 / 下载用的清晰渲染边长：kQrPixels 一致
const QR_EXPORT_PX: u32 = 600;

pub(crate) const WM_QR_RESULT: UINT = 0x0400 + 100;
/// 解码未命中且有 OCR 兜底时先弹提示。
pub(crate) const WM_QR_OCR_FALLBACK: UINT = 0x0400 + 101;
/// 解码未命中且没有 OCR 兜底时的固定提示。
pub(crate) const WM_QR_NOT_FOUND: UINT = 0x0400 + 102;

/// 当前生成的二维码（BGRA，预乘无关，纯不透明）。
pub(crate) static QR_IMG: Mutex<Option<(u32, u32, Vec<u8>)>> = Mutex::new(None);
/// 与 `QR_IMG` 对应的文本：复制 / 下载时按同一内容高清重渲染。
pub(crate) static QR_TEXT: Mutex<String> = Mutex::new(String::new());
/// 叠加到二维码中央的 LOGO（BGRA）。
pub(crate) static LOGO_IMG: Mutex<Option<(u32, u32, Vec<u8>)>> = Mutex::new(None);

// -------------------------------------------------------------------------------
// 页面入口
// -------------------------------------------------------------------------------

/// 建页：输入框 + 左侧一列按钮。
pub(crate) unsafe fn create_page(parent: HWND) {
    let hinstance = GetModuleHandleW(std::ptr::null());
    let edit = CreateWindowExW(
        0, wide("EDIT").as_ptr(), wide("").as_ptr(),
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        sc(ui::metric::SPACE_6), sc(ui::layout::main_page::EDIT_Y),
        sc(ui::layout::main_page::EDIT_W), sc(ui::layout::main_page::EDIT_H),
        parent, QR_EDIT as HMENU, hinstance, std::ptr::null_mut(),
    );
    if !edit.is_null() {
        // 空时显示灰色占位提示，一输入就消失
        SendMessageW(edit, 0x1501 /*EM_SETCUEBANNER*/, 0,
            wide(QR_PLACEHOLDER).as_ptr() as LPARAM);
    }
    for (i, (id, label)) in QR_BTNS.iter().enumerate() {
        CreateWindowExW(
            0, wide("BUTTON").as_ptr(), wide(label).as_ptr(),
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            sc(ui::layout::qr::BTN_X),
            sc(ui::layout::qr::BTN_Y0 + ui::layout::qr::BTN_PITCH * i as i32),
            sc(ui::layout::qr::BTN_W), sc(ui::layout::qr::BTN_H),
            parent, *id as HMENU, hinstance, std::ptr::null_mut(),
        );
    }
}

/// 排版：输入框固定填满顶部，按钮列贴输入框下沿。
pub(crate) unsafe fn relayout(parent: HWND) {
    let edit = GetDlgItem(parent, QR_EDIT as i32);
    if !edit.is_null() {
        SetWindowPos(edit, std::ptr::null_mut(),
            sc(ui::metric::SPACE_6), sc(ui::layout::main_page::EDIT_Y),
            sc(ui::layout::main_page::EDIT_W), sc(INPUT_H.load(Ordering::Relaxed)),
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    let y0 = below_y();
    for (i, (id, _)) in QR_BTNS.iter().enumerate() {
        let b = GetDlgItem(parent, *id as i32);
        if b.is_null() { continue; }
        SetWindowPos(b, std::ptr::null_mut(),
            sc(ui::layout::qr::BTN_X),
            sc(y0 + ui::layout::qr::BTN_PITCH * i as i32),
            sc(ui::layout::qr::BTN_W), sc(ui::layout::qr::BTN_H),
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/// 显隐：本页所有控件 + 卡片区域重绘。
pub(crate) unsafe fn show_page(parent: HWND, visible: bool) {
    let sw = if visible { SW_SHOW } else { SW_HIDE };
    let edit = GetDlgItem(parent, QR_EDIT as i32);
    if !edit.is_null() { ShowWindow(edit, sw); }
    for (id, _) in QR_BTNS {
        let b = GetDlgItem(parent, id as i32);
        if !b.is_null() { ShowWindow(b, sw); }
    }
    let r = qr_area();
    InvalidateRect(parent, &r, 0);
}

/// 按钮点击（BN_CLICKED）。返回是否本页处理。
pub(crate) unsafe fn on_command(id: usize, hwnd: HWND) -> bool {
    match id {
        QR_BTN_CLEAR => clear_input(hwnd),
        // 的「识别二维码」：手动拖框，先解二维码，没解到再文字识别兜底
        QR_BTN_SCREEN => start_region_select(SEL_MODE_QR_THEN_OCR),
        QR_BTN_COPY => copy_qr(hwnd),
        QR_BTN_DOWNLOAD => download_qr(hwnd),
        QR_BTN_UPLOAD => upload_logo(hwnd),
        _ => return false,
    }
    true
}

/// 编辑框通知（EN_CHANGE / EN_SETFOCUS）。返回是否本页处理。
pub(crate) unsafe fn on_notify(id: usize, notify: usize, hwnd: HWND) -> bool {
    if id != QR_EDIT { return false; }
    let edit = GetDlgItem(hwnd, QR_EDIT as i32);
    match notify {
        0x0100 /*EN_SETFOCUS*/ => {
            // 点输入框默认全选，方便直接覆盖输入
            if !edit.is_null() { SendMessageW(edit, 0x00B1 /*EM_SETSEL*/, 0, -1); }
        }
        0x0300 /*EN_CHANGE*/ => generate_from_edit(hwnd),
        _ => return false,
    }
    true
}

/// 自绘：本页按钮的底色 / 圆角 / 白字，与其它页按钮同一套样式。
pub(crate) unsafe fn draw(item: &DRAWITEMSTRUCT) -> bool {
    let id = item.CtlID as usize;
    if !QR_BTNS.iter().any(|(bid, _)| *bid == id) { return false; }
    let r = item.rcItem;
    let pressed = (item.itemState & ODS_SELECTED) != 0;
    let hovering = BTN_HOVER.load(Ordering::Relaxed) == id as i32;
    // 清空是危险操作（红），其余主界面蓝
    let base = if id == QR_BTN_CLEAR {
        if pressed { ui::color::DANGER_PRESSED }
        else if hovering { ui::color::DANGER_HOVER }
        else { ui::color::DANGER }
    } else if pressed { ui::color::ACCENT_PRESSED }
    else if hovering { ui::color::ACCENT_HOVER }
    else { ui::color::ACCENT_DEEP };
    let brush = CreateSolidBrush(ui::color::cref(base));
    FillRect(item.hDC, &r, brush as HBRUSH);
    let rad = ui::metric::RADIUS_BTN;
    let hrgn = CreateRoundRectRgn(r.left, r.top, r.right, r.bottom, rad, rad);
    FillRgn(item.hDC, hrgn, brush as HBRUSH);
    DeleteObject(brush as *mut winapi::ctypes::c_void);
    DeleteObject(hrgn as *mut winapi::ctypes::c_void);

    let mut label = [0u16; 64];
    let n = GetWindowTextW(item.hwndItem, label.as_mut_ptr(), 64);
    let font = ui_font(sc(ui::font::SIZE_22), ui::font::W_BOLD);
    let old = SelectObject(item.hDC, font as *mut winapi::ctypes::c_void);
    SetTextColor(item.hDC, ui::color::cref(ui::color::TEXT_INVERSE));
    SetBkMode(item.hDC, TRANSPARENT as i32);
    let mut tr = r;
    DrawTextW(item.hDC, label.as_ptr(), n, &mut tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, old);
    true
}

// -------------------------------------------------------------------------------
// 页面动作
// -------------------------------------------------------------------------------

/// 清空：连同 LOGO 一起复位，卡片回到示例码。
unsafe fn clear_input(hwnd: HWND) {
    *LOGO_IMG.lock_ok() = None;
    let edit = GetDlgItem(hwnd, QR_EDIT as i32);
    if !edit.is_null() { SetWindowTextW(edit, wide("").as_ptr()); }
    // 文本本来就是空的时候不会触发 EN_CHANGE，这里补一次（LOGO 拿掉了码要重生成）
    generate_from_edit(hwnd);
}

/// 复制：按导出尺寸重渲染再进剪贴板。
unsafe fn copy_qr(hwnd: HWND) {
    let text = QR_TEXT.lock_ok().clone();
    let Some((w, h, buf)) = gen_qr_bgra_int(&text, QR_EXPORT_PX) else {
        set_status(hwnd, "复制失败，剪贴板被其他程序占用");
        return;
    };
    if copy_qr_full_clipboard(w, h, &buf) {
        set_status(hwnd, "已复制到剪贴板");
    } else {
        set_status(hwnd, "复制失败，剪贴板被其他程序占用");
    }
}

/// 下载：按导出尺寸重渲染并另存 PNG。
unsafe fn download_qr(hwnd: HWND) {
    // 不弹另存框，直接落桌面（二维码_日期时间.png）
    let text = QR_TEXT.lock_ok().clone();
    let Some((w, h, buf)) = gen_qr_bgra_int(&text, QR_EXPORT_PX) else {
        set_status(hwnd, "保存失败");
        return;
    };
    let ok = save_bgra_desktop(w as i32, h as i32, &buf).is_some();
    set_status(hwnd, if ok { "已保存到桌面" } else { "保存失败" });
}

/// 上传 LOGO：读进来叠到码上并立即重生成。
unsafe fn upload_logo(hwnd: HWND) {
    let Some(path) = pick_logo_file(hwnd) else { return };
    match load_logo_bgra(&path) {
        Some(img) => {
            *LOGO_IMG.lock_ok() = Some(img);
            generate_from_edit(hwnd);
            set_status(hwnd, "LOGO 已添加");
        }
        None => set_status(hwnd, "读取 LOGO 失败（仅支持 PNG）"),
    }
}

/// 按输入框内容重生成二维码。空输入不留白板，显示示例码。
pub(crate) unsafe fn generate_from_edit(hwnd: HWND) {
    let edit = GetDlgItem(hwnd, QR_EDIT as i32);
    if edit.is_null() { return; }
    let mut buf = [0u16; 4096];
    let n = GetWindowTextW(edit, buf.as_mut_ptr(), 4096);
    let mut text = String::from_utf16_lossy(&buf[..n as usize]);
    if text.is_empty() { text = QR_PLACEHOLDER.to_string(); }

    // 只记「本来有没有码」：输入超长时每敲一个键都弹提示太吵
    let had_code = QR_IMG.lock_ok().is_some();
    let encoded = gen_qr_bgra(&text);
    let ok = encoded.is_some();
    *QR_IMG.lock_ok() = encoded;
    *QR_TEXT.lock_ok() = text;
    invalidate_card(hwnd);
    if !ok && had_code {
        set_status(hwnd, "内容太长或含有无法编码的字符");
    }
}

/// 识别结果回填（WM_QR_RESULT 的二维码分支）：命中填码文，兜底填文字并全选聚焦。
pub(crate) unsafe fn fill_result(hwnd: HWND) {
    let t = RECOG_RESULT.lock_ok().clone();
    let edit = GetDlgItem(hwnd, QR_EDIT as i32);
    if edit.is_null() || t.is_empty() { return; }
    if SEL_QR_HIT.load(Ordering::Relaxed) == 1 {
        SetWindowTextW(edit, wide(&t).as_ptr());
        generate_from_edit(hwnd);
        set_status(hwnd, "识别成功，内容已填入输入框");
        return;
    }
    // 文字识别兜底：压成一行，全选 + 聚焦，方便直接改写
    let flat: String = t.chars()
        .map(|c| if c == '\r' || c == '\n' { ' ' } else { c })
        .collect();
    let flat = flat.trim().to_string();
    if flat.is_empty() { return; }
    SetWindowTextW(edit, wide(&flat).as_ptr());
    generate_from_edit(hwnd);
    SendMessageW(edit, 0x00B1 /*EM_SETSEL*/, 0, -1);
    SetFocus(edit);
}

/// 没解到码、准备转文字识别时的提示（在 OCR 起跑之前弹）。
pub(crate) unsafe fn on_ocr_fallback(hwnd: HWND) {
    set_status(hwnd, "没识别到二维码，已按文字识别填入");
}

/// 没解到码、也没有 OCR 兜底时的提示。
pub(crate) unsafe fn on_not_found(hwnd: HWND) {
    set_status(hwnd, "这一块里没有识别到二维码");
}

// -------------------------------------------------------------------------------
// 识别（rqrr + 放大 / Otsu 预处理）
// -------------------------------------------------------------------------------

/// 解码截图区域（BGRA）。1~4 倍最近邻放大 × Otsu 二值化，再交给 rqrr 定位纠错。
pub(crate) fn decode_qr(bgra: &[u8], w: u32, h: u32) -> Option<String> {
    if w == 0 || h == 0 || bgra.len() < (w as usize * h as usize * 4) { return None; }

    for s in 1..=4u32 {
        let (dw, dh) = (w * s, h * s);
        // 只限制放大档；1× 原图必须允许，否则 5000×300 这种超宽条带直接被跳光
        if s > 1 && (dw > 4096 || dh > 4096) { continue; }
        if let Some(t) = decode_at(bgra, w, h, dw, dh) { return Some(t); }
    }
    // 大图再加一次半尺寸尝试（QR 定位对缩放不敏感，密集码放大反而糊）
    if w >= 400 && h >= 400 {
        if let Some(t) = decode_at(bgra, w, h, w / 2, h / 2) { return Some(t); }
    }
    None
}

fn decode_at(bgra: &[u8], w: u32, h: u32, dw: u32, dh: u32) -> Option<String> {
    // 最近邻采样 + 整数亮度公式（r*306 + g*601 + b*117）>> 10
    let mut luma = vec![0u8; (dw * dh) as usize];
    for y in 0..dh {
        let sy = (y as usize * h as usize) / dh as usize;
        let row = y as usize * dw as usize;
        for x in 0..dw {
            let sx = (x as usize * w as usize) / dw as usize;
            let i = (sy * w as usize + sx) * 4;
            luma[row + x as usize] = ((bgra[i + 2] as u32 * 306
                + bgra[i + 1] as u32 * 601
                + bgra[i] as u32 * 117) >> 10) as u8;
        }
    }
    // 先 Otsu：小图与带 LOGO 的码灰度直方图仍是双峰，全局阈值能把被压暗的模块
    // 拉回纯黑纯白，rqrr 的定位更容易咬住三个角
    let t = otsu(&luma);
    let mut bin = luma.clone();
    for p in bin.iter_mut() { *p = if *p > t { 255 } else { 0 }; }
    if let Some(s) = rqrr_scan(bin, dw, dh) { return Some(s); }
    // 渐变背景上 Otsu 会把整块压成一色，原灰度再走一次 rqrr 自带阈值兜底
    rqrr_scan(luma, dw, dh)
}

fn rqrr_scan(gray: Vec<u8>, w: u32, h: u32) -> Option<String> {
    let img = image::GrayImage::from_raw(w, h, gray)?;
    let mut prepared = PreparedImage::prepare(img);
    for grid in prepared.detect_grids() {
        if let Ok((_meta, text)) = grid.decode() {
            if !text.is_empty() { return Some(text); }  // (_meta, text)
        }
    }
    None
}

/// 大津阈值：类间方差最大的灰度分界。
fn otsu(gray: &[u8]) -> u8 {
    let mut hist = [0u32; 256];
    for &v in gray { hist[v as usize] += 1; }
    let total = gray.len() as f64;
    let sum: f64 = hist.iter().enumerate().map(|(i, &c)| i as f64 * c as f64).sum();
    let (mut wb, mut sum_b, mut best_t, mut best_var) = (0.0f64, 0.0f64, 128u8, -1.0f64);
    for t in 0..256 {
        wb += hist[t] as f64;
        if wb == 0.0 { continue; }
        let wf = total - wb;
        if wf == 0.0 { break; }
        sum_b += t as f64 * hist[t] as f64;
        let mb = sum_b / wb;
        let mf = (sum - sum_b) / wf;
        let var = wb * wf * (mb - mf) * (mb - mf);
        if var > best_var { best_var = var; best_t = t as u8; }
    }
    best_t
}

// -------------------------------------------------------------------------------
// 生成（字节模式）
// -----------------------------------------------------------------------------------

/// 强制 8-bit 字节模式（QR_MODE_8），版本取最小可容纳。
fn encode_byte_mode(text: &str, ec: EcLevel) -> Option<QrCode> {
    let bytes = text.as_bytes();
    (1..=40i16).find_map(|v| {
        let mut bits = Bits::new(Version::Normal(v));
        bits.push_byte_data(bytes).ok()?;
        bits.push_terminator(ec).ok()?;
        QrCode::with_bits(bits, ec).ok()
    })
}

fn version_of(code: &QrCode) -> i16 {
    ((code.width() as i16) - 17) / 4
}

/// QrEcAuto：L 的最小版本为，Q 同版本则用 Q，M 同版本则用 M
/// 都涨版本就用 Q；有 LOGO 时优先 H，H 放不下退回上面这套。
fn encode_v5(text: &str) -> Option<QrCode> {
    if LOGO_IMG.lock_ok().is_some() {
        if let Some(c) = encode_byte_mode(text, EcLevel::H) { return Some(c); }
    }
    let l = encode_byte_mode(text, EcLevel::L)?;
    let v = version_of(&l);
    let q = encode_byte_mode(text, EcLevel::Q);
    let m = encode_byte_mode(text, EcLevel::M);
    if q.as_ref().map(version_of) == Some(v) { return q; }
    if m.as_ref().map(version_of) == Some(v) { return m; }
    q.or(m)
}

/// 按目标边长等比落格（预览用：模块大小可以不是整数像素）。
fn render_fit(code: &QrCode, dim: u32) -> Vec<u8> {
    let size = code.width() as u32;
    let colors = code.to_colors();
    let full = size + QR_QUIET * 2;
    let mut buf = vec![0xFFu8; (dim * dim * 4) as usize]; // 白底
    for y in 0..dim {
        let my = (y as u64 * full as u64) / dim as u64;
        if my < QR_QUIET as u64 || my >= (QR_QUIET + size) as u64 { continue; }
        let row = (my - QR_QUIET as u64) as usize;
        for x in 0..dim {
            let mx = (x as u64 * full as u64) / dim as u64;
            if mx < QR_QUIET as u64 || mx >= (QR_QUIET + size) as u64 { continue; }
            let col = (mx - QR_QUIET as u64) as usize;
            if colors[row * size as usize + col] == qrcode::Color::Dark {
                let i = ((y * dim + x) * 4) as usize;
                buf[i..i + 4].copy_from_slice(&[0, 0, 0, 255]);
            }
        }
    }
    buf
}

/// 整数倍落格（复制 / 下载用：模块边缘不发虚）。
fn render_crisp(code: &QrCode, target: u32) -> (u32, u32, Vec<u8>) {
    let size = code.width() as u32;
    let colors = code.to_colors();
    let full = size + QR_QUIET * 2;
    let scale = ((target + full / 2) / full).max(1);
    let dim = full * scale;
    let mut buf = vec![0xFFu8; (dim * dim * 4) as usize];
    for y in 0..size {
        for x in 0..size {
            if colors[(y * size + x) as usize] != qrcode::Color::Dark { continue; }
            let py = (y + QR_QUIET) * scale;
            let px = (x + QR_QUIET) * scale;
            for dy in 0..scale {
                let row = (py + dy) * dim;
                for dx in 0..scale {
                    let i = ((row + px + dx) * 4) as usize;
                    buf[i..i + 4].copy_from_slice(&[0, 0, 0, 255]);
                }
            }
        }
    }
    (dim, dim, buf)
}

/// 叠加 LOGO：正方形白底 + 居中拉伸。
fn overlay_logo(buf: &mut [u8], dim: u32) {
    // 借引用而不是 clone：每敲一个字都会重生成，clone 一份大图不划算
    let guard = LOGO_IMG.lock_ok();
    let Some((lw, lh, src)) = guard.as_ref() else { return };
    if *lw == 0 || *lh == 0 || dim == 0 { return; }
    let side = (dim as i64 * 22 / 100) as i32;
    if side <= 0 { return; }
    let pad = side / 10;
    let x0 = dim as i32 / 2 - side / 2;
    let y0 = dim as i32 / 2 - side / 2;
    let (bw, bh) = (side + pad * 2, side + pad * 2);
    for y in 0..bh {
        let py = y0 - pad + y;
        if py < 0 || py >= dim as i32 { continue; }
        for x in 0..bw {
            let px = x0 - pad + x;
            if px < 0 || px >= dim as i32 { continue; }
            let lx = x - pad;
            let ly = y - pad;
            let di = ((py as u32 * dim + px as u32) * 4) as usize;
            if lx >= 0 && ly >= 0 && lx < side && ly < side {
                // 拉伸成正方形
                let sx = (lx as u32 * *lw / side as u32) as usize;
                let sy = (ly as u32 * *lh / side as u32) as usize;
                let si = (sy * *lw as usize + sx) * 4;
                let a = src[si + 3];
                if a < 8 { continue; }
                if a >= 248 {
                    buf[di] = src[si]; buf[di + 1] = src[si + 1]; buf[di + 2] = src[si + 2];
                } else {
                    // 半透明：over 混合到白底上，消除硬边
                    let ia = 255 - a as u32;
                    for k in 0..3 {
                        buf[di + k] = ((src[si + k] as u32 * a as u32
                            + buf[di + k] as u32 * ia + 127) / 255) as u8;
                    }
                }
                buf[di + 3] = 255;
            } else {
                buf[di..di + 4].copy_from_slice(&[255, 255, 255, 255]); // 白边
            }
        }
    }
}

/// 预览尺寸的二维码：卡片内框再留一圈白边。
pub(crate) fn gen_qr_bgra(text: &str) -> Option<(u32, u32, Vec<u8>)> {
    let code = encode_v5(text)?;
    let dim = (qr_side() - sc(ui::metric::SPACE_1) * 2 - sc(ui::metric::SPACE_4))
        .max(ui::layout::qr::IMG_MIN) as u32;
    let mut buf = render_fit(&code, dim);
    overlay_logo(&mut buf, dim);
    Some((dim, dim, buf))
}

/// 导出尺寸的二维码：复制 / 下载用。
pub(crate) fn gen_qr_bgra_int(text: &str, target: u32) -> Option<(u32, u32, Vec<u8>)> {
    let code = encode_v5(text)?;
    let (w, h, mut buf) = render_crisp(&code, target);
    overlay_logo(&mut buf, w);
    Some((w, h, buf))
}

// -------------------------------------------------------------------------------
// LOGO / 剪贴板 / 落盘
// -------------------------------------------------------------------------------

pub(crate) fn pick_logo_file(hwnd: HWND) -> Option<String> {
    unsafe {
        let mut ofn: OPENFILENAMEW = std::mem::zeroed();
        let mut fbuf = [0u16; 1024];
        let filter = wide("图片文件\0*.png;*.jpg;*.jpeg;*.bmp\0所有文件\0*.*\0\0");
        ofn.lStructSize = std::mem::size_of::<OPENFILENAMEW>() as u32;
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = filter.as_ptr();
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = fbuf.as_mut_ptr();
        ofn.nMaxFile = fbuf.len() as u32;
        // OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_EXPLORER
        ofn.Flags = 0x00001000 | 0x00000800 | 0x00080000;
        if GetOpenFileNameW(&mut ofn) != 0 {
            let n = fbuf.iter().position(|&c| c == 0).unwrap_or(0);
            Some(String::from_utf16_lossy(&fbuf[..n]).trim().to_string())
        } else {
            None
        }
    }
}

pub(crate) fn load_logo_bgra(path: &str) -> Option<(u32, u32, Vec<u8>)> {
    let img = image::open(path).ok()?;
    let (w, h) = img.dimensions();
    let rgba = img.to_rgba8();
    let mut bgra = Vec::with_capacity((w * h * 4) as usize);
    for p in rgba.pixels() {
        bgra.extend_from_slice(&[p[2], p[1], p[0], p[3]]);
    }
    Some((w, h, bgra))
}

pub(crate) fn copy_text_clipboard(s: &str) -> bool {
    unsafe {
        let mut w: Vec<u16> = s.encode_utf16().collect();
        w.push(0);
        let n = w.len() * 2;
        let hmem = GlobalAlloc(GMEM_MOVEABLE, n);
        if hmem.is_null() { return false; }
        let ptr = GlobalLock(hmem) as *mut u16;
        if ptr.is_null() { GlobalFree(hmem); return false; }
        std::ptr::copy_nonoverlapping(w.as_ptr(), ptr, w.len());
        GlobalUnlock(hmem);
        if OpenClipboard(std::ptr::null_mut()) == 0 {
            GlobalFree(hmem);
            return false;
        }
        EmptyClipboard();
        let r = SetClipboardData(13 /*CF_UNICODETEXT*/, hmem);
        CloseClipboard();
        if r.is_null() { GlobalFree(hmem); return false; }
        true
    }
}

pub(crate) fn get_text_clipboard() -> Option<String> {
    unsafe {
        if OpenClipboard(std::ptr::null_mut()) == 0 { return None; }
        let mut out = None;
        let h = GetClipboardData(13 /*CF_UNICODETEXT*/);
        if !h.is_null() {
            let p = GlobalLock(h) as *const u16;
            if !p.is_null() {
                // 以分配长度为界扫终止符：剪贴板里的串不一定带 0，不能一路读出去
                let cap = (winapi::um::winbase::GlobalSize(h) / 2) as usize;
                let mut n = 0usize;
                while n < cap && *p.add(n) != 0 { n += 1; }
                if n > 0 {
                    let ws = std::slice::from_raw_parts(p, n);
                    out = Some(String::from_utf16_lossy(ws));
                }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        out
    }
}

pub(crate) fn copy_qr_full_clipboard(w: u32, h: u32, buf: &[u8]) -> bool {
    unsafe {
        let hbmp = CreateBitmap(w as i32, h as i32, 1, 32,
            buf.as_ptr() as *const winapi::ctypes::c_void);
        if hbmp.is_null() { return false; }
        if OpenClipboard(std::ptr::null_mut()) == 0 {
            DeleteObject(hbmp as *mut winapi::ctypes::c_void);
            return false;
        }
        if EmptyClipboard() == 0 {
            CloseClipboard();
            DeleteObject(hbmp as *mut winapi::ctypes::c_void);
            return false;
        }
        let r = SetClipboardData(2 /*CF_BITMAP*/, hbmp as *mut winapi::ctypes::c_void);
        CloseClipboard();
        // 成功时系统接管句柄；失败才需要释放
        if r.is_null() { DeleteObject(hbmp as *mut winapi::ctypes::c_void); return false; }
        true
    }
}

// -------------------------------------------------------------------------------
// 防多开（与二维码页无关，随模块保留）
// -------------------------------------------------------------------------------

pub(crate) const TARGET_PROCESSES: &[&str] = &[
    "7zFM.exe", "7z.exe", "7zz.exe",
    "WinRAR.exe", "Rar.exe", "UnRAR.exe",
    "Bandizip.exe", "HaoZip.exe", "360zip.exe",
    "PeaZip.exe", "NanaZip.exe", "WinZip.exe",
    "IZArc.exe", "PowerArchiver.exe", "BreeZip.exe",
    "2345Zip.exe", "Kuaizip.exe", "WinMount.exe",
    "uzip.exe", "Zipper.exe", "ExtractNow.exe",
];

pub(crate) fn proc_creation_time(pid: u32) -> u64 {
    unsafe {
        let h = OpenProcess(0x1000, 0, pid); // PROCESS_QUERY_LIMITED_INFORMATION
        if h.is_null() { return 0; }
        let mut ct: FILETIME = std::mem::zeroed();
        let mut et: FILETIME = std::mem::zeroed();
        let mut kt: FILETIME = std::mem::zeroed();
        let mut ut: FILETIME = std::mem::zeroed();
        let ok = GetProcessTimes(h, &mut ct, &mut et, &mut kt, &mut ut);
        winapi::um::handleapi::CloseHandle(h);
        if ok != 0 {
            ((ct.dwHighDateTime as u64) << 32) | (ct.dwLowDateTime as u64)
        } else {
            0
        }
    }
}

pub(crate) extern "system" fn enum_window_proc(hwnd: HWND, lparam: LPARAM) -> i32 {
    unsafe {
        let vec_ptr = lparam as *mut Vec<(u32, String)>;
        if vec_ptr.is_null() { return 1; }
        let mut pid: u32 = 0;
        GetWindowThreadProcessId(hwnd, &mut pid);
        if pid == 0 || IsWindowVisible(hwnd) == 0 { return 1; }
        let mut buf = [0u16; 512];
        let n = GetWindowTextW(hwnd, buf.as_mut_ptr(), 512);
        if n > 0 {
            let title = String::from_utf16_lossy(&buf[..n as usize]).trim().to_string();
            if !title.is_empty() {
                (*vec_ptr).push((pid, title));
            }
        }
        1
    }
}

pub(crate) fn archive_identity(title: &str) -> String {
    if let Some(idx) = title.find(" - ") {
        title[..idx].trim().to_string()
    } else {
        title.trim().to_string()
    }
}

pub(crate) fn is_target(sz: &[u16]) -> bool {
    let n = sz.len();
    if n == 0 { return false; }
    // 名单里全是 ASCII。非 ASCII 单元必须直接判否：`lo as u8` 截断会把
    // 中文/符号截成 'a' 之类而"命中" 7zFM.exe，后果是误杀无关进程。
    if sz.iter().any(|&u| u > 0x7F) { return false; }
    TARGET_PROCESSES.iter().any(|t| {
        let tb = t.as_bytes();
        if tb.len() != n { return false; }
        tb.iter().zip(sz).all(|(&b, &u)| {
            let lo = if (0x41u16..=0x5Au16).contains(&u) { u + 32 } else { u };
            let bl = if b.is_ascii_uppercase() { b + 32 } else { b };
            lo as u8 == bl
        })
    })
}

pub(crate) extern "system" fn close_archiver_enum(hwnd: HWND, lparam: LPARAM) -> i32 {
    unsafe {
        let pids = lparam as *mut Vec<u32>;
        if pids.is_null() { return 1; }
        if IsWindowVisible(hwnd) == 0 { return 1; }
        let mut pid: u32 = 0;
        GetWindowThreadProcessId(hwnd, &mut pid);
        if pid != 0 && (*pids).contains(&pid) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            CLOSED_N.fetch_add(1, Ordering::Relaxed);
        }
        1
    }
}

static CLOSED_N: AtomicUsize = AtomicUsize::new(0);

/// 关闭所有可见的压缩软件窗口，返回实际发出关闭的窗口数（热键提示用）
pub(crate) fn close_archiver_windows() -> usize {
    unsafe {
        let snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if snap.is_null() { return 0; }
        let mut entry: PROCESSENTRY32W = std::mem::zeroed();
        entry.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
        let mut pids: Vec<u32> = Vec::with_capacity(8);
        if Process32FirstW(snap, &mut entry) == 1 {
            loop {
                let end = entry.szExeFile.iter().position(|&c| c == 0).unwrap_or(0);
                if is_target(&entry.szExeFile[..end]) {
                    pids.push(entry.th32ProcessID);
                }
                if Process32NextW(snap, &mut entry) != 1 { break; }
            }
        }
        winapi::um::handleapi::CloseHandle(snap);
        if pids.is_empty() { return 0; }
        CLOSED_N.store(0, Ordering::Relaxed);
        EnumWindows(Some(close_archiver_enum), &mut pids as *mut Vec<u32> as LPARAM);
        CLOSED_N.load(Ordering::Relaxed)
    }
}

pub(crate) fn monitor_thread(running: Arc<AtomicBool>) {
    while running.load(Ordering::Relaxed) {
        let active;
        unsafe {
            let snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if snap.is_null() {
                std::thread::sleep(Duration::from_millis(800));
                continue;
            }

            let mut entry: PROCESSENTRY32W = std::mem::zeroed();
            entry.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
            let mut pids: Vec<u32> = Vec::with_capacity(8);

            if Process32FirstW(snap, &mut entry) == 1 {
                loop {
                    let end = entry.szExeFile.iter().position(|&c| c == 0).unwrap_or(0);
                    if is_target(&entry.szExeFile[..end]) {
                        pids.push(entry.th32ProcessID);
                    }
                    if Process32NextW(snap, &mut entry) != 1 { break; }
                }
            }
            winapi::um::handleapi::CloseHandle(snap);
            active = !pids.is_empty();

            if pids.len() > 1 && load_dword(ANTI_MULTI_VALUE, 1) == 1 {
                // 按"压缩包"分组：同包多开只保留 1 个（最新），不同包各自保留
                let mut wins: Vec<(u32, String)> = Vec::new();
                EnumWindows(Some(enum_window_proc),
                    &mut wins as *mut Vec<(u32, String)> as LPARAM);

                let mut pid_archive: std::collections::HashMap<u32, String> =
                    std::collections::HashMap::new();
                for (pid, title) in wins {
                    if pids.contains(&pid) {
                        pid_archive.entry(pid).or_insert_with(|| archive_identity(&title));
                    }
                }

                let mut groups: std::collections::HashMap<String, Vec<u32>> =
                    std::collections::HashMap::new();
                for &pid in &pids {
                    let ident = pid_archive.get(&pid).cloned()
                        .unwrap_or_else(|| format!("pid{}", pid));
                    groups.entry(ident).or_default().push(pid);
                }

                for group in groups.values() {
                    if group.len() < 2 { continue; }
                    let mut keep = group[0];
                    let mut keep_t = proc_creation_time(group[0]);
                    for &pid in &group[1..] {
                        let t = proc_creation_time(pid);
                        if t > keep_t { keep_t = t; keep = pid; }
                    }
                    for &pid in group {
                        if pid == keep { continue; }
                        let h = OpenProcess(0x0001, 0, pid);
                        if !h.is_null() {
                            TerminateProcess(h, 0);
                            winapi::um::handleapi::CloseHandle(h);
                        }
                    }
                }
            }
        }
        // 有压缩软件在跑 → 800ms 快扫；空闲 → 3000ms 省 CPU
        std::thread::sleep(Duration::from_millis(if active { 800 } else { 3000 }));
    }
}

// -------------------------------------------------------------------------------
// 几何
// -------------------------------------------------------------------------------

/// 输入框下沿 → 内容区，= 的 QR_TOP（153）
pub(crate) fn below_y() -> i32 {
    ui::layout::main_page::EDIT_Y
        + INPUT_H.load(Ordering::Relaxed)
        + ui::layout::main_page::GAP_BELOW_EDIT
}

/// 卡片逻辑边长。
pub(crate) fn qr_side() -> i32 {
    sc(ui::layout::qr::CARD_SIDE)
}

pub(crate) fn qr_area() -> RECT {
    let x = sc(ui::layout::qr::CARD_X);
    let y = sc(ui::layout::qr::CARD_Y);
    let side = sc(ui::layout::qr::CARD_SIDE);
    RECT { left: x, top: y, right: x + side, bottom: y + side }
}

fn invalidate_card(hwnd: HWND) {
    unsafe {
        let mut r = qr_area();
        let d = sc(ui::metric::SPACE_1);
        r.left -= d;
        r.top -= d;
        r.right += d;
        r.bottom += d;
        InvalidateRect(hwnd, &r, 0);
    }
}
