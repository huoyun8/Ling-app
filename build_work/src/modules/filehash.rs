// 文件校验的算法层：只管"算出摘要"，不管界面。
//
// 与界面完全解耦：主窗口只负责把结果摆到控件上。
// MD5 / SHA256 走 bcrypt（系统自带，不必自己实现密码学）；
// CRC32 自己算 —— CryptoAPI 的 CALG_CRC32 在部分 CSP 上不提供，
// 而这张表只有 256 项，自己算反而最稳。
//
// 为什么不自己写 MD5/SHA256：这两位是安全哈希，实现错一位就静默产出
// 错误摘要 —— 用户拿它比对发行版完整性，错摘要比没摘要更坏。
// 系统 bcrypt 有 FIPS 认证，比手写实现可靠。

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use winapi::shared::bcrypt::{
    BCryptCloseAlgorithmProvider, BCryptCreateHash, BCryptDestroyHash,
    BCryptFinishHash, BCryptGetProperty, BCryptHashData, BCryptOpenAlgorithmProvider,
    BCRYPT_MD5_ALGORITHM, BCRYPT_SHA256_ALGORITHM,
};
use winapi::shared::minwindef::{UCHAR, ULONG, FILETIME};
use winapi::um::fileapi::{
    FileTimeToLocalFileTime, GetFileAttributesExW, WIN32_FILE_ATTRIBUTE_DATA,
};
use winapi::um::minwinbase::GetFileExInfoStandard;
use winapi::um::timezoneapi::FileTimeToSystemTime;

/// 勾选了哪几种算法。位掩码，便于一路传到工作线程。
pub const HASH_MD5: u32 = 1 << 0;
pub const HASH_CRC32: u32 = 1 << 1;
/// SHA-256（32 字节 / 64 位十六进制）。发行方基本只提供这一种校验值，
/// SHA-1/MD5 早已因碰撞攻击不适合做完整性校验 —— 界面用 SHA-256 顶替 SHA-1。
pub const HASH_SHA256: u32 = 1 << 2;

/// 日期不参与哈希，借用掩码的高位当"要不要打印"的开关
pub const SHOW_DATE: u32 = 1 << 9;

/// 一个文件的输出块。字段名，便于和别的工具对照。
#[derive(Clone, Default)]
pub struct FileDigest {
    pub name: String,      // 文件名（不含路径）
    /// 完整路径。只写不读
    /// （结果框只显示文件名，路径太长会把版面撑坏）。
    /// 保留该字段，将来要显示路径时可直接取用。
    #[allow(dead_code)]
    pub path: String,
    pub size: u64,         // 文件大小（字节）
    pub modified: String,  // 修改时间，yyyy-MM-dd HH:mm:ss
    pub md5: String,       // 32 位小写十六进制
    pub sha256: String,    // 64 位
    pub crc32: String,     // 8 位
}

/// 进度计数：工作线程写、UI 线程读，全是原子的，不用锁。
/// file_done/file_total 是当前这一个文件，all_done/all_total 是整批文件。
#[derive(Default)]
pub struct HashProgress {
    pub file_done: AtomicU64,
    pub file_total: AtomicU64,
    pub all_done: AtomicU64,
    pub all_total: AtomicU64,
}

// —————————————————— CRC32（IEEE 802.3，反转多项式 0xEDB88320） ——————————————————

/// 256 项查表。一次性算好，之后每字节一次查表 + 一次异或。
struct Crc32Table([u32; 256]);

impl Crc32Table {
    const fn new() -> Crc32Table {
        let mut t = [0u32; 256];
        let mut i = 0usize;
        while i < 256 {
            let mut c = i as u32;
            let mut k = 0;
            while k < 8 {
                c = if c & 1 != 0 { 0xEDB8_8320u32 ^ (c >> 1) } else { c >> 1 };
                k += 1;
            }
            t[i] = c;
            i += 1;
        }
        Crc32Table(t)
    }
}

static CRC_TABLE: Crc32Table = Crc32Table::new();

fn crc32_update(mut crc: u32, data: &[u8]) -> u32 {
    for &b in data {
        crc = CRC_TABLE.0[((crc ^ b as u32) & 0xFF) as usize] ^ (crc >> 8);
    }
    crc
}

// —————————————————— bcrypt 增量摘要器 ——————————————————

/// bcrypt 的一个哈希上下文。构造失败（系统太老 / 算法不可用）时 ready 为 false，
/// 调用方据此跳过这种算法，不会把整个校验拖垮。
struct BcryptHash {
    alg: *mut winapi::ctypes::c_void,
    hash: *mut winapi::ctypes::c_void,
    /// BCryptCreateHash 的"哈希对象"缓冲区。它必须活得和 hash 句柄一样久 ——
    /// bcrypt 会往这块内存里写中间状态，提前释放就是野指针写入。
    /// 所以它只写不读，但绝对不能删。
    #[allow(dead_code)]
    obj: Vec<u8>,
    digest: Vec<u8>,
    digest_len: u32,
    ready: bool,
}

impl BcryptHash {
    /// known_len 是该算法摘要的固定字节数（MD5=16、SHA256=32）。
    ///
    /// 为什么不只靠 BCryptGetProperty("HashLength")：实测本机 CNG 对该属性一律
    /// 返回 STATUS_NOT_SUPPORTED（连 SHA256 都查不到），拿到的长度是 0，
    /// 之后 BCryptFinishHash 必然以 STATUS_INVALID_PARAMETER 失败 —— 结果就是
    /// 界面上 MD5/SHA256 两行整个不出现。摘要长度本来就是算法定义里写死的，
    /// 属性查得到就用查到的，查不到就退回已知值，两种情况都能算对。
    fn new(alg_id: &[u16], known_len: u32) -> BcryptHash {
        unsafe {
            let mut alg: *mut winapi::ctypes::c_void = std::ptr::null_mut();
            let mut hash: *mut winapi::ctypes::c_void = std::ptr::null_mut();
            let mut obj_len: ULONG = 0;
            let mut got: ULONG = 0;

            // 属性名是 UTF-16 串，不是字节串
            let prop_obj = crate::wide("ObjectLength");
            let prop_hash = crate::wide("HashLength");

            if BCryptOpenAlgorithmProvider(&mut alg, alg_id.as_ptr(), std::ptr::null(), 0) < 0 {
                return BcryptHash::dead();
            }
            if BCryptGetProperty(alg, prop_obj.as_ptr(),
                &mut obj_len as *mut ULONG as *mut UCHAR, std::mem::size_of::<ULONG>() as ULONG, &mut got, 0) < 0 {
                BCryptCloseAlgorithmProvider(alg, 0);
                return BcryptHash::dead();
            }

            let mut digest_len: ULONG = known_len;
            let mut queried: ULONG = 0;
            if BCryptGetProperty(alg, prop_hash.as_ptr(),
                &mut queried as *mut ULONG as *mut UCHAR, std::mem::size_of::<ULONG>() as ULONG, &mut got, 0) >= 0
                && queried > 0
            {
                digest_len = queried;
            }

            let mut obj = vec![0u8; obj_len as usize];
            let digest = vec![0u8; digest_len as usize];
            if BCryptCreateHash(alg, &mut hash, obj.as_mut_ptr(), obj_len,
                std::ptr::null_mut(), 0, 0) < 0 {
                BCryptCloseAlgorithmProvider(alg, 0);
                return BcryptHash::dead();
            }
            BcryptHash { alg, hash, obj, digest, digest_len, ready: true }
        }
    }

    fn dead() -> BcryptHash {
        BcryptHash {
            alg: std::ptr::null_mut(),
            hash: std::ptr::null_mut(),
            obj: Vec::new(),
            digest: Vec::new(),
            digest_len: 0,
            ready: false,
        }
    }

    fn update(&mut self, data: &[u8]) -> bool {
        if !self.ready || data.is_empty() { return true; }
        unsafe {
            BCryptHashData(self.hash, data.as_ptr() as *mut UCHAR, data.len() as ULONG, 0) >= 0
        }
    }

    fn finish_hex(&mut self) -> String {
        if !self.ready { return String::new(); }
        unsafe {
            let ok = BCryptFinishHash(self.hash, self.digest.as_mut_ptr(), self.digest_len, 0) >= 0;
            // 收尾后句柄不再可用，重复调用直接返回空
            self.ready = false;
            if !ok { return String::new(); }
        }
        to_hex(&self.digest)
    }
}

impl Drop for BcryptHash {
    fn drop(&mut self) {
        unsafe {
            if !self.hash.is_null() { BCryptDestroyHash(self.hash); }
            if !self.alg.is_null() { BCryptCloseAlgorithmProvider(self.alg, 0); }
        }
    }
}

fn to_hex(data: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(data.len() * 2);
    for &b in data {
        s.push(HEX[(b >> 4) as usize] as char);
        s.push(HEX[(b & 0x0F) as usize] as char);
    }
    s
}

/// 增量摘要器：喂多少算多少，最后一次性取结果。
/// 一个文件一只实例（各算法并排累加）。
pub struct HashCalc {
    mask: u32,
    md5: Option<BcryptHash>,
    sha256: Option<BcryptHash>,
    /// CRC32 的累积值。初值 0xFFFFFFFF，收尾时再异或一次 0xFFFFFFFF 输出
    crc: u32,
}

impl HashCalc {
    pub fn new(mask: u32) -> HashCalc {
        // 算法 ID 是以 L'\0' 结尾的 UTF-16 串
        // （winapi 里的 BCRYPT_*_ALGORITHM 是 &str，要自己转成 UTF-16）
        let md5_id: Vec<u16> = crate::wide(BCRYPT_MD5_ALGORITHM);
        let sha256_id: Vec<u16> = crate::wide(BCRYPT_SHA256_ALGORITHM);
        HashCalc {
            mask,
            md5: if mask & HASH_MD5 != 0 { Some(BcryptHash::new(&md5_id, 16)) } else { None },
            sha256: if mask & HASH_SHA256 != 0 { Some(BcryptHash::new(&sha256_id, 32)) } else { None },
            crc: 0xFFFF_FFFFu32,
        }
    }

    /// 至少有一种算法可用（系统不支持 bcrypt 时也不至于整页瘫掉）。
    pub fn usable(&self) -> bool {
        if self.md5.as_ref().map_or(false, |h| h.ready) { return true; }
        if self.sha256.as_ref().map_or(false, |h| h.ready) { return true; }
        self.mask & HASH_CRC32 != 0
    }

    pub fn update(&mut self, data: &[u8]) -> bool {
        if data.is_empty() { return true; }
        let mut ok = true;
        if let Some(h) = self.md5.as_mut() { ok = h.update(data) && ok; }
        if let Some(h) = self.sha256.as_mut() { ok = h.update(data) && ok; }
        if self.mask & HASH_CRC32 != 0 {
            self.crc = crc32_update(self.crc, data);
        }
        ok
    }

    /// 收尾并取十六进制结果。未勾选的算法返回空串。只能调用一次。
    pub fn finish(&mut self) -> (String, String, String) {
        let mut md5 = String::new();
        let mut sha256 = String::new();
        let mut crc32 = String::new();
        if let Some(h) = self.md5.as_mut() { md5 = h.finish_hex(); }
        if let Some(h) = self.sha256.as_mut() { sha256 = h.finish_hex(); }
        if self.mask & HASH_CRC32 != 0 {
            // CRC32 的惯例是最终异或 0xFFFFFFFF 再输出；空文件也一样
            crc32 = format!("{:08x}", self.crc ^ 0xFFFF_FFFFu32);
        }
        (md5, sha256, crc32)
    }
}

// —————————————————— 文件元信息 ——————————————————

/// 本地修改时间，yyyy-MM-dd HH:mm:ss
pub fn file_modified_time(path: &str) -> String {
    unsafe {
        let wpath = crate::wide(path);
        let mut fad: WIN32_FILE_ATTRIBUTE_DATA = std::mem::zeroed();
        if GetFileAttributesExW(wpath.as_ptr(), GetFileExInfoStandard,
            &mut fad as *mut _ as *mut winapi::ctypes::c_void) == 0 {
            return String::new();
        }
        let mut local: FILETIME = std::mem::zeroed();
        if FileTimeToLocalFileTime(&fad.ftLastWriteTime, &mut local) == 0 { return String::new(); }
        let mut st: winapi::um::minwinbase::SYSTEMTIME = std::mem::zeroed();
        if FileTimeToSystemTime(&local, &mut st) == 0 { return String::new(); }
        format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond)
    }
}

/// 1234567 -> "1,234,567"
///
/// 不倒着数一遍再 reverse 一次：那样要多一次分配 + 一遍 UTF-8 拷贝。
/// 正向数、用剩余位数判该不该插逗号，一次成型。
pub fn group_digits(v: u64) -> String {
    let digits = v.to_string();
    let n = digits.len();
    let mut out = String::with_capacity(n + n / 3);
    for (i, ch) in digits.chars().enumerate() {
        if i != 0 && (n - i) % 3 == 0 { out.push(','); }
        out.push(ch);
    }
    out
}

// —————————————————— 算一个文件 ——————————————————

/// 算一个文件。prog 非空时按块累加进度；tick 每读完一块调一次，
/// 线程那边自己做限流，免得每 1MB 就往 UI 线程 Post 一条消息。
/// 返回 None 表示读失败或被取消，此时 out 里的内容不完整。
pub fn hash_file(
    path: &str,
    mask: u32,
    cancel: &AtomicBool,
    prog: Option<&HashProgress>,
    tick: Option<&dyn Fn()>,
) -> Option<FileDigest> {
    let mut out = FileDigest {
        path: path.to_string(),
        ..Default::default()
    };
    out.name = match path.rfind(|c| c == '\\' || c == '/') {
        Some(i) => path[i + 1..].to_string(),
        None => path.to_string(),
    };
    out.modified = file_modified_time(path);

    // 打开文件读元数据 + 内容。用 std::fs 而不是 Win32 API：
    // Rust 标准库的文件 API 已经是跨平台的、且天然处理了长路径与句柄生命周期。
    let mut file = match std::fs::File::open(path) {
        Ok(f) => f,
        Err(_) => return None,
    };
    out.size = file.metadata().map(|m| m.len()).unwrap_or(0);
    if let Some(p) = prog {
        p.file_done.store(0, Ordering::Relaxed);
        p.file_total.store(out.size, Ordering::Relaxed);
    }

    let mut calc = HashCalc::new(mask);
    // 1MB 一块：足够摊薄系统调用，又不至于占内存。
    // 但小文件别硬按 1MB 分：几百个小文件连续校验时，每个都 touch 一整页 1MB
    // 就是一次清零 + page fault，按文件大小夹到 64KB~1MB 更省。
    let mut buf = vec![0u8; out.size.clamp(64 * 1024, 1 << 20) as usize];
    let mut done: u64 = 0;
    let mut ok = true;
    loop {
        if cancel.load(Ordering::Relaxed) { ok = false; break; }
        let n = match std::io::Read::read(&mut file, &mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(_) => { ok = false; break; }
        };
        // BCrypt/CRC 内部失败时不能继续：否则会把"部分数据"的摘要当成整文件摘要返回
        if !calc.update(&buf[..n]) { ok = false; break; }
        done += n as u64;
        if let Some(p) = prog {
            p.file_done.store(done, Ordering::Relaxed);
            p.all_done.fetch_add(n as u64, Ordering::Relaxed);
        }
        if let Some(t) = tick { t(); }
    }
    if !ok { return None; }

    let (md5, sha256, crc32) = calc.finish();
    out.md5 = md5;
    out.sha256 = sha256;
    out.crc32 = crc32;
    // 文件实际大小以读到的字节为准：读的过程中被改写时 GetFileSize 的值会不一致
    out.size = done;
    Some(out)
}

/// 批处理的总字节数（只读元数据，不碰内容，几十个文件也是毫秒级）
pub fn total_bytes(files: &[String]) -> u64 {
    let mut sum = 0u64;
    for f in files {
        if let Ok(md) = std::fs::metadata(f) {
            sum += md.len();
        }
    }
    sum
}
