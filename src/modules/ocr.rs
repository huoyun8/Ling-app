//! 文字识别：外部 ocr-process 引擎驱动、排版、页辅助（搜索/计数/空闲回收）。

use crate::prelude::*;
use crate::*;
use crate::ocr_layout;

#[cfg(feature = "ocr")]
pub(crate) use ocr_impl::run_ocr_bgra;

pub(crate) const OCR_EDIT: usize = 4190;      // 大大的识别结果框（多行）
pub(crate) const OCR_SEARCH_EDIT: usize = 4194; // 顶部搜索框（快速定位）
pub(crate) const OCR_BTN_SEARCH: usize = 4195;  // 搜索框右侧“查找”按钮
pub(crate) const OCR_LBL_COUNT: usize = 4196;   // 识别字符数显示（“N 字符”，仅文字识别页显示）

pub(crate) const OCR_BTN_REC: usize = 4191;   // 识别（框选位置→OCR）
pub(crate) const OCR_BTN_COPY: usize = 4192;  // 复制
pub(crate) const OCR_BTN_CLEAR: usize = 4193; // 清空
pub(crate) const OCR_BTN_LAYOUT: usize = 4197; // 排版档位循环切换（自动/按行/整段/原始）

/// 仅在 ocr feature 关闭时由占位的 run_ocr_bgra 使用；开启 feature 时该占位被编译掉
#[allow(dead_code)]
pub(crate) const OCR_OFF_MSG: &str = "文字识别功能尚未接入（页面已保留，后续版本开放）";

pub(crate) const OCR_IDLE_TIMER: usize = 96;  // OCR 会话空闲回收定时器（30s 检查一次；仅 ocr feature 下设）

pub(crate) static OCR_TEXT: Mutex<String> = Mutex::new(String::new());

pub(crate) static OCR_LINES: Mutex<Vec<ocr_layout::OcrLine>> = Mutex::new(Vec::new());

pub(crate) static OCR_LAYOUT_MODE: AtomicI32 = AtomicI32::new(0);

pub(crate) fn trim16(v: &[u16]) -> &[u16] {
    let mut s = 0;
    while s < v.len() && v[s] == b' ' as u16 { s += 1; }
    let mut e = v.len();
    while e > s && v[e-1] == b' ' as u16 { e -= 1; }
    &v[s..e]
}

pub(crate) unsafe fn ocr_search_jump(hwnd: HWND) {
    let sbox = GetDlgItem(hwnd, OCR_SEARCH_EDIT as i32);
    let oedit = GetDlgItem(hwnd, OCR_EDIT as i32);
    if sbox.is_null() || oedit.is_null() { return; }
    let mut sb = [0u16; 256];
    let sn = GetWindowTextW(sbox, sb.as_mut_ptr(), 256);
    let needle = trim16(&sb[..sn as usize]);
    if needle.is_empty() { return; }
    let mut tb = [0u16; 65536];
    let tn = GetWindowTextW(oedit, tb.as_mut_ptr(), 65536);
    let hay = &tb[..tn as usize];
    if hay.len() >= needle.len() {
        if let Some(pos) = (0..=hay.len()-needle.len()).find(|&i| &hay[i..i+needle.len()] == needle) {
            SendMessageW(oedit, 0x00B1 /*EM_SETSEL*/, pos as WPARAM, (pos + needle.len()) as LPARAM);
            SendMessageW(oedit, 0x00B7 /*EM_SCROLLCARET*/, 0, 0);
            return;
        }
    }
    // 未匹配：取消选中，避免误导
    SendMessageW(oedit, 0x00B1 /*EM_SETSEL*/, 0, 0);
}

pub(crate) unsafe fn update_ocr_count(hwnd: HWND) {
    let cnt = GetDlgItem(hwnd, OCR_LBL_COUNT as i32);
    if cnt.is_null() { return; }
    let n = OCR_TEXT.lock_ok().chars().count();
    if n > 0 && PAGE_CUR.load(Ordering::Relaxed) == PAGE_OCR {
        let s = format!("{} 字符", n);
        SetWindowTextW(cnt, wide(s.as_str()).as_ptr());
        ShowWindow(cnt, SW_SHOW);
    } else {
        ShowWindow(cnt, SW_HIDE);
    }
}

#[cfg(feature = "ocr")]
pub(crate) fn ocr_idle_tick() {
    ocr_impl::shutdown_if_idle(180_000);
}
#[cfg(not(feature = "ocr"))]
pub(crate) fn ocr_idle_tick() {}

#[cfg(feature = "ocr")]
pub(crate) fn ocr_shutdown_tick() {
    ocr_impl::shutdown();
}
#[cfg(not(feature = "ocr"))]
pub(crate) fn ocr_shutdown_tick() {}

#[cfg(feature = "ocr")]
pub(crate) mod ocr_impl {
    use super::*;
    use super::ocr_layout::{self, LayoutMode, OcrLine};
    use std::io::{self, Write};
    use std::os::windows::io::AsRawHandle;
    use std::os::windows::process::CommandExt;
    use std::path::PathBuf;
    use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
    use std::sync::atomic::AtomicU64;
    use std::sync::Mutex;
    use winapi::shared::ntdef::HANDLE;
    use winapi::um::fileapi::ReadFile;
    use winapi::um::namedpipeapi::PeekNamedPipe;
    use winapi::um::synchapi::{Sleep, WaitForSingleObject};
    use winapi::um::sysinfoapi::GetTickCount64;

    const OCR_MAGIC: [u8; 4] = *b"SOCR";
    const OCR_VER: u16 = 5;
    const K_HELLO: u16 = 1;
    const K_READY: u16 = 2;
    const K_SUBMIT: u16 = 3;
    const K_COMPLETE: u16 = 5;
    const K_SHUTDOWN: u16 = 6;
    const K_PREPARE: u16 = 8;
    const K_SESSION_READY: u16 = 9;
    const K_ATTACH: u16 = 12;
    const K_BUFFER_ATTACHED: u16 = 13;
    const K_RECOGNIZE: u16 = 14;
    const K_IMAGE_CONSUMED: u16 = 15;
    const K_DETACH: u16 = 16;

    const TIMEOUT_SMALL: u64 = 5000;
    const TIMEOUT_PREPARE: u64 = 20000;
    const TIMEOUT_RECOGNIZE: u64 = 30000;
    const MAX_PIXELS: u64 = 4096 * 2304;

    fn ocr_put_string(out: &mut Vec<u8>, s: &str) {
        out.extend_from_slice(&(s.len() as u32).to_le_bytes());
        out.extend_from_slice(s.as_bytes());
    }

    fn ocr_write_frame(w: &mut impl Write, kind: u16, req: u64, payload: &[u8]) -> io::Result<()> {
        let mut h = [0u8; 20];
        h[..4].copy_from_slice(&OCR_MAGIC);
        h[4..6].copy_from_slice(&OCR_VER.to_le_bytes());
        h[6..8].copy_from_slice(&kind.to_le_bytes());
        h[8..16].copy_from_slice(&req.to_le_bytes());
        h[16..20].copy_from_slice(&(payload.len() as u32).to_le_bytes());
        w.write_all(&h)?;
        w.write_all(payload)?;
        w.flush()
    }

    // 带超时的管道精确读：PeekNamedPipe 轮询 + 进程存活检查，卡住就失败
    unsafe fn pipe_read_exact(h: HANDLE, dst: &mut [u8], timeout_ms: u64, proc: HANDLE) -> bool {
        let start = GetTickCount64();
        let mut got = 0usize;
        while got < dst.len() {
            let mut avail: u32 = 0;
            if PeekNamedPipe(h, std::ptr::null_mut(), 0, std::ptr::null_mut(), &mut avail, std::ptr::null_mut()) == 0 {
                return false;
            }
            if avail > 0 {
                let want = (avail as usize).min(dst.len() - got) as u32;
                let mut read: u32 = 0;
                let ok = ReadFile(
                    h,
                    dst[got..].as_mut_ptr() as *mut winapi::ctypes::c_void,
                    want,
                    &mut read,
                    std::ptr::null_mut(),
                );
                if ok == 0 || read == 0 {
                    return false;
                }
                got += read as usize;
                continue;
            }
            if GetTickCount64() - start > timeout_ms {
                return false;
            }
            if !proc.is_null() && WaitForSingleObject(proc, 0) == 0 {
                return false; // 进程没了（WAIT_OBJECT_0）
            }
            Sleep(4);
        }
        true
    }

    fn ocr_read_frame_h(h: HANDLE, proc: HANDLE, timeout_ms: u64) -> io::Result<(u16, u64, Vec<u8>)> {
        let mut hdr = [0u8; 20];
        if !unsafe { pipe_read_exact(h, &mut hdr, timeout_ms, proc) } {
            return Err(io::Error::new(io::ErrorKind::Other, "OCR 读帧超时或进程退出"));
        }
        if &hdr[..4] != &OCR_MAGIC {
            return Err(io::Error::new(io::ErrorKind::Other, "OCR 协议魔数错误"));
        }
        let kind = u16::from_le_bytes([hdr[6], hdr[7]]);
        let req = u64::from_le_bytes(hdr[8..16].try_into().unwrap());
        let len = u32::from_le_bytes(hdr[16..20].try_into().unwrap()) as usize;
        // 长度是管道里送来的，不能无条件照着分配：头里一个垃圾值就能让
        // `vec![0u8; 4GB]` 直接 abort（panic=abort 下没有任何提示）。
        // Complete 帧最大也就是几千行文字，512MB 已经是极宽松的上限。
        if len > 512 * 1024 * 1024 {
            return Err(io::Error::new(io::ErrorKind::Other, "OCR 帧过大"));
        }
        let mut p = vec![0u8; len];
        if len > 0 && !unsafe { pipe_read_exact(h, &mut p, timeout_ms, proc) } {
            return Err(io::Error::new(io::ErrorKind::Other, "OCR 读载荷超时"));
        }
        Ok((kind, req, p))
    }

    // 一套可用的资产路径
    struct Assets {
        proc: PathBuf,
        det: PathBuf,
        rec: PathBuf,
        dict: PathBuf,
        dir: PathBuf,
    }

    static OCR_EXTRACT_LOCK: Mutex<()> = Mutex::new(());

    fn ocr_cache_dir() -> PathBuf {
        if let Ok(p) = std::env::var("LOCALAPPDATA") {
            let d = PathBuf::from(p).join(concat!(env!("APP_NAME"), "_ocr"));
            return d;
        }
        std::env::temp_dir().join(concat!(env!("APP_NAME"), "_ocr"))
    }

    fn ensure_ocr_assets() -> Option<Assets> {
        let _g = OCR_EXTRACT_LOCK.lock_ok();
        let dir = ocr_cache_dir();
        let proc = dir.join("ocr-process.exe");
        let det = dir.join("PP-OCRv6_det_small.onnx");
        let rec = dir.join("PP-OCRv6_rec_small.onnx");
        let dict = dir.join("ppocrv6_dict.txt");
        let marker = dir.join(".v1");
        // DirectML.dll 不在此释放（可选加速；放一份到该目录即切 GPU）
        if !marker.exists() || !proc.exists() || !det.exists() || !rec.exists() || !dict.exists() {
            let _ = std::fs::create_dir_all(&dir);
            let ok = write_gz(&proc, include_bytes!(concat!(env!("OUT_DIR"), "/ocr-process.exe.sz"))).is_ok()
                && write_gz(&det, include_bytes!(concat!(env!("OUT_DIR"), "/PP-OCRv6_det_small.onnx.sz"))).is_ok()
                && write_gz(&rec, include_bytes!(concat!(env!("OUT_DIR"), "/PP-OCRv6_rec_small.onnx.sz"))).is_ok()
                && write_gz(&dict, include_bytes!(concat!(env!("OUT_DIR"), "/ppocrv6_dict.txt.sz"))).is_ok();
            if !ok {
                return None;
            }
            let _ = std::fs::write(&marker, b"1");
        }
        if proc.exists() && det.exists() && rec.exists() && dict.exists() {
            Some(Assets { proc, det, rec, dict, dir })
        } else {
            None
        }
    }

    // 解压 build.rs 压好的 gzip 载荷并落盘（内嵌压缩、运行期解压，省 exe 体积）
    fn write_gz(dst: &std::path::Path, gz: &[u8]) -> io::Result<()> {
        use flate2::read::GzDecoder;
        use std::io::Read;
        let mut d = GzDecoder::new(gz);
        let mut buf = Vec::new();
        d.read_to_end(&mut buf)?;
        std::fs::write(dst, &buf)
    }

    // 常驻引擎会话（复用进程，省每次约 1~1.5 秒模型加载；空闲超时由主窗口定时器收回）
    #[allow(dead_code)]
    struct Session {
        child: Child,
        stdin: ChildStdin,
        stdout: ChildStdout, // 持有以保活读端管道
        // 句柄以 isize 存，确保 Session: Send（HANDLE=*mut c_void 非 Send）；用时转回 HANDLE
        out_h: isize,
        proc_h: isize,
    }

    impl Session {
        fn read_frame(&self, timeout_ms: u64) -> io::Result<(u16, u64, Vec<u8>)> {
            ocr_read_frame_h(self.out_h as HANDLE, self.proc_h as HANDLE, timeout_ms)
        }
    }

    static SESSION: Mutex<Option<Session>> = Mutex::new(None);
    static LAST_USE: AtomicU64 = AtomicU64::new(0);
    /// 当前引擎进程句柄（isize 存，0 = 无）。退出时如果拿不到会话锁（正在识别），
    /// 就用它硬杀 —— 否则那个进程会留在后台。
    static SESSION_PROC: AtomicIsize = AtomicIsize::new(0);

    fn start_session(a: &Assets) -> Option<Session> {
        let mut child = Command::new(&a.proc)
            .creation_flags(0x08000000) // CREATE_NO_WINDOW
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .ok()?;
        let Some(stdin) = child.stdin.take() else {
            let _ = child.kill();
            let _ = child.wait();
            return None;
        };
        let Some(stdout) = child.stdout.take() else {
            let _ = child.kill();
            let _ = child.wait();
            return None;
        };
        let out_h = stdout.as_raw_handle() as isize;
        let proc_h = child.as_raw_handle() as isize;

        let mut s = Session { child, stdin, stdout, out_h, proc_h };

        // 握手放到单独一步：任何一步失败都必须把子进程收掉。`Child` 的 Drop **不会**
        // kill，直接 return 会留下一个吃着模型内存、永远等在管道上的
        // ocr-process.exe —— 用户看不见，只能去任务管理器里找。
        let ok = handshake(&mut s, a);
        if !ok {
            let _ = s.child.kill();
            let _ = s.child.wait();
            return None;
        }
        SESSION_PROC.store(proc_h, Ordering::Relaxed);
        Some(s)
    }

    /// Hello → Ready → Prepare → SessionReady。全部成功才返回 true。
    fn handshake(s: &mut Session, a: &Assets) -> bool {
        let mut payload = Vec::new();
        ocr_put_string(&mut payload, "");
        if ocr_write_frame(&mut s.stdin, K_HELLO, 0, &payload).is_err() {
            return false;
        }
        if !matches!(s.read_frame(TIMEOUT_SMALL), Ok((K_READY, _, _))) {
            return false;
        }
        // Prepare → SessionReady：后端按 ocr 目录是否有 DirectML.dll 决定 GPU/CPU
        let has_dml = a.dir.join("DirectML.dll").exists();
        let mut p = Vec::new();
        p.push(if has_dml { 1 } else { 0 });
        p.push(0); // 检测 resize = max
        p.push(0); // 处理顺序 = 精度优先
        ocr_put_string(&mut p, &a.det.to_string_lossy());
        ocr_put_string(&mut p, &a.rec.to_string_lossy());
        ocr_put_string(&mut p, &a.dict.to_string_lossy());
        if ocr_write_frame(&mut s.stdin, K_PREPARE, 1, &p).is_err() {
            return false;
        }
        matches!(s.read_frame(TIMEOUT_PREPARE), Ok((K_SESSION_READY, _, pl)) if pl.first() == Some(&1))
    }

    // Complete 载荷：[ok:1] + 空字符串 + 行数 + 每行{ 文本, score:4, quad:32 }
    fn parse_complete(p: &[u8]) -> Vec<OcrLine> {
        let mut lines: Vec<OcrLine> = Vec::new();
        let mut off = 1usize;
        if p.len() >= off + 4 {
            let l = u32::from_le_bytes(p[off..off + 4].try_into().unwrap()) as usize;
            off += 4 + l; // 跳过那个空字符串
        }
        let n = if p.len() >= off + 4 {
            u32::from_le_bytes(p[off..off + 4].try_into().unwrap()) as usize
        } else {
            0
        };
        off += 4;
        for _ in 0..n {
            if off + 4 > p.len() {
                break;
            }
            let sl = u32::from_le_bytes(p[off..off + 4].try_into().unwrap()) as usize;
            off += 4;
            if off + sl + 36 > p.len() {
                break; // 36 = score(4) + quad(32)
            }
            let text = String::from_utf8_lossy(&p[off..off + sl]).into_owned();
            let score = f32::from_le_bytes(p[off + sl..off + sl + 4].try_into().unwrap());
            let mut quad = [0f32; 8];
            for (k, q) in quad.iter_mut().enumerate() {
                let s = off + sl + 4 + k * 4;
                *q = f32::from_le_bytes(p[s..s + 4].try_into().unwrap());
            }
            off += sl + 36;
            // 四点顺序不做假设，取包围盒（排版只需外接矩形）
            let (mut x1, mut x2, mut y1, mut y2) = (quad[0], quad[0], quad[1], quad[1]);
            for k in 1..4 {
                let x = quad[k * 2];
                let y = quad[k * 2 + 1];
                if x < x1 { x1 = x; }
                if x > x2 { x2 = x; }
                if y < y1 { y1 = y; }
                if y > y2 { y2 = y; }
            }
            lines.push(OcrLine { text, score, x1, y1, x2, y2 });
        }
        lines
    }

    // 单轮识别：Attach → Submit → Recognize → Detach，返回带坐标的行
    fn recognize_round(s: &mut Session, w: u32, h: u32, bgra: &[u8]) -> Result<Vec<OcrLine>, String> {
        let stride = w * 4;
        let size = (stride as usize) * (h as usize);
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        let slot = std::env::temp_dir().join(format!("qr_ocr_{}_{}.buf", std::process::id(), nanos));
        {
            let mut f = match std::fs::File::create(&slot) {
                Ok(f) => f,
                Err(_) => return Err("无法创建临时文件".into()),
            };
            let _ = f.write_all(&1u64.to_le_bytes()); // sequence
            let _ = f.write_all(&1u32.to_le_bytes()); // state = ready
            let _ = f.write_all(&w.to_le_bytes());
            let _ = f.write_all(&h.to_le_bytes());
            let _ = f.write_all(&stride.to_le_bytes());
            let _ = f.write_all(&(size as u32).to_le_bytes());
            let _ = f.write_all(&0x544f4c53u32.to_le_bytes()); // "SLOT"
            if f.write_all(bgra).is_err() || f.sync_all().is_err() {
                let _ = std::fs::remove_file(&slot);
                return Err("写临时文件失败".into());
            }
        }
        let total = 32u64 + size as u64;

        // Attach → BufferAttached
        let mut pay = Vec::new();
        ocr_put_string(&mut pay, &slot.to_string_lossy());
        pay.extend_from_slice(&total.to_le_bytes());
        if ocr_write_frame(&mut s.stdin, K_ATTACH, 2, &pay).is_err()
            || !matches!(s.read_frame(TIMEOUT_SMALL), Ok((K_BUFFER_ATTACHED, _, _)))
        {
            let _ = std::fs::remove_file(&slot);
            return Err("OCR 缓冲区错误".into());
        }
        // Submit → ImageConsumed（generation 必须等于 Attach 的请求号）
        let mut pay = Vec::new();
        pay.extend_from_slice(&2u64.to_le_bytes());
        pay.extend_from_slice(&w.to_le_bytes());
        pay.extend_from_slice(&h.to_le_bytes());
        pay.extend_from_slice(&stride.to_le_bytes());
        pay.extend_from_slice(&1u64.to_le_bytes());
        if ocr_write_frame(&mut s.stdin, K_SUBMIT, 3, &pay).is_err()
            || !matches!(s.read_frame(TIMEOUT_SMALL), Ok((K_IMAGE_CONSUMED, _, _)))
        {
            let _ = std::fs::remove_file(&slot);
            return Err("OCR 图像错误".into());
        }
        // Recognize → Complete
        let _ = ocr_write_frame(&mut s.stdin, K_RECOGNIZE, 3, &[]);
        let res = match s.read_frame(TIMEOUT_RECOGNIZE) {
            Ok((K_COMPLETE, _, p)) if p.first() == Some(&1) => {
                let lines = parse_complete(&p);
                if lines.is_empty() {
                    Err("未识别到文字".into())
                } else {
                    Ok(lines)
                }
            }
            Ok(_) => Err("未识别到文字".into()),
            Err(_) => Err("OCR 识别失败或超时".into()),
        };
        // Detach（失败不影响结果）
        let _ = ocr_write_frame(&mut s.stdin, K_DETACH, 2, &[]);
        let _ = s.read_frame(TIMEOUT_SMALL);
        let _ = std::fs::remove_file(&slot);
        res
    }

    /// 识别一张 BGRA 图：走常驻会话；出错则收尸重来，返回排版后的文本。
    /// 结果行另存到 super::OCR_LINES，供切换排版档位时免重跑引擎直接重排。
    pub fn run_ocr_bgra(w: u32, h: u32, bgra: &[u8]) -> String {
        if w == 0 || h == 0 || (w as u64 * h as u64) > MAX_PIXELS {
            return "图片尺寸超限".to_string();
        }
        // 像素数对得上、缓冲却短了，后面会把不满一帧的内容喂给引擎；
        // 更糟的是往临时文件里写短了，引擎按声明尺寸读就是越界。这里直接拒绝。
        if bgra.len() < (w as usize) * (h as usize) * 4 {
            return "图像缓冲区错误".to_string();
        }
        let Some(assets) = ensure_ocr_assets() else {
            return "OCR 引擎释放失败（无写入权限）".to_string();
        };
        let mut guard = SESSION.lock_ok();
        if guard.is_none() {
            match start_session(&assets) {
                Some(s) => *guard = Some(s),
                None => return "OCR 模型加载失败".into(),
            }
        }
        let s = guard.as_mut().unwrap();
        let res = recognize_round(s, w, h, bgra);
        // "未识别到文字"是正常结果，模型还在、会话还能用；只有协议/超时错误才收尸重来
        let fatal = matches!(&res, Err(e) if e != "未识别到文字");
        if fatal {
            if let Some(mut dead) = guard.take() {
                let _ = dead.child.kill();
                let _ = dead.child.wait();
                SESSION_PROC.store(0, Ordering::Relaxed);
            }
        }
        LAST_USE.store(unsafe { GetTickCount64() }, Ordering::Relaxed);
        match res {
            Ok(lines) => {
                let mode = LayoutMode::from_i32(super::OCR_LAYOUT_MODE.load(Ordering::Relaxed));
                // 先排版再存行：反过来的话每次识别都白拷一份全量行（结果框几千行时不小）
                let text = ocr_layout::format(&lines, mode);
                *super::OCR_LINES.lock_ok() = lines;
                text
            }
            Err(e) => e,
        }
    }

    /// 关闭常驻会话（应用退出时调用）
    pub fn shutdown() {
        // 不能用 lock()：识别进行中锁被占着，最长 30 秒，退出流程会一直干等。
        // 先短等一会儿拿锁（正常空闲时立刻到手，可以走"先礼后兵"），拿不到就直接硬杀。
        let mut taken: Option<Session> = None;
        for _ in 0..20 {
            match SESSION.try_lock() {
                Ok(mut g) => {
                    taken = g.take();
                    break;
                }
                Err(_) => unsafe { Sleep(25) },
            }
        }
        let Some(s) = taken else {
            // 没拿到锁：要么识别还在跑，要么本来就空。用句柄兜底，别把引擎进程留在后台
            let h = SESSION_PROC.swap(0, Ordering::Relaxed) as HANDLE;
            if !h.is_null() {
                unsafe { TerminateProcess(h, 0) };
            }
            return;
        };
        SESSION_PROC.store(0, Ordering::Relaxed);
        reap_session(s);
    }

    /// 空闲超过 limit_ms 则回收会话（由主窗口定时器周期调用）
    ///
    /// 这里必须用 try_lock：本函数是在 **UI 线程**的 WM_TIMER 里跑的，而识别是在
    /// 工作线程里、握着 SESSION 锁跑满几秒。用阻塞的 lock() 就等于每做一次识别，
    /// 界面都可能被定时器冻住几秒。
    pub fn shutdown_if_idle(limit_ms: u64) {
        let Ok(mut g) = SESSION.try_lock() else { return }; // 正在识别：下次再说
        let now = unsafe { GetTickCount64() };
        let last = LAST_USE.load(Ordering::Relaxed);
        if last == 0 || now.saturating_sub(last) <= limit_ms {
            return;
        }
        // 在锁内直接取走会话，不能先 drop 再调 shutdown()：
        // 中间空隙里新识别会拿到锁，shutdown 抢锁失败后会把正在跑的引擎硬杀
        if let Some(s) = g.take() {
            SESSION_PROC.store(0, Ordering::Relaxed);
            drop(g);
            reap_session(s);
        }
    }

/// 先礼后兵地收掉一个已取出的会话：发 Shutdown + 关写端，1.5 秒不退再硬杀
fn reap_session(mut s: Session) {
    let _ = ocr_write_frame(&mut s.stdin, K_SHUTDOWN, 0, &[]);
    drop(s.stdin);
    let mut waited = 0u64;
    while waited < 1500 {
        if unsafe { WaitForSingleObject(s.proc_h as HANDLE, 100) } == 0 {
            break;
        }
        waited += 100;
    }
    let _ = s.child.kill();
    let _ = s.child.wait();
}
}

/// OCR 占位：签名与真实现一致，调用方无需改动。
/// 当前 OCR 未接入（OCR_ENABLED=false），没有任何调用点；保留签名供后续接回。
#[cfg(not(feature = "ocr"))]
#[allow(dead_code)]
pub(crate) fn run_ocr_bgra(_w: u32, _h: u32, _bgra: &[u8]) -> String {
    OCR_OFF_MSG.to_string()
}
