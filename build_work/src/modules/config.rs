//! 配置读写：极简 JSON（JVal/JParser）与 %APPDATA%/config.json 存取。

use crate::prelude::*;
use crate::*;

#[derive(Clone, Debug)]
pub(crate) enum JVal {
    Obj(Vec<(String, JVal)>),
    Arr(Vec<JVal>),
    Str(String),
    Num(f64),
    Bool(bool),
    Null,
}

pub(crate) static CFG: Mutex<Option<JVal>> = Mutex::new(None);
pub(crate) static CFG_PATH: Mutex<Option<std::path::PathBuf>> = Mutex::new(None);

impl JVal {
    fn obj_get(&self, key: &str) -> Option<&JVal> {
        if let JVal::Obj(m) = self {
            m.iter().find(|(k, _)| k == key).map(|(_, v)| v)
        } else {
            None
        }
    }
    /// SetNamedValue：键已存在就地替换（保持键序），不存在追加到末尾
    fn obj_set(&mut self, key: &str, v: JVal) {
        if let JVal::Obj(m) = self {
            if let Some(e) = m.iter_mut().find(|(k, _)| k == key) {
                e.1 = v;
            } else {
                m.push((key.to_string(), v));
            }
        }
    }
    fn obj_entry(&mut self, key: &str) -> Option<&mut JVal> {
        if let JVal::Obj(m) = self {
            let idx = m.iter().position(|(k, _)| k == key);
            let i = match idx {
                Some(i) => i,
                None => {
                    m.push((key.to_string(), JVal::Null));
                    m.len() - 1
                }
            };
            Some(&mut m[i].1)
        } else {
            None
        }
    }
    /// getToolObj：缺哪一层就现建一层，已有的不是对象也照样换掉
    fn obj_ensure(&mut self, key: &str) -> Option<&mut JVal> {
        if !matches!(self.obj_get(key), Some(JVal::Obj(_))) {
            self.obj_set(key, JVal::Obj(Vec::new()));
        }
        self.obj_entry(key)
    }
    /// JSON 输出：紧凑、无空格、键序即插入序
    fn stringify(&self, out: &mut String) {
        match self {
            JVal::Obj(m) => {
                out.push('{');
                for (i, (k, v)) in m.iter().enumerate() {
                    if i > 0 {
                        out.push(',');
                    }
                    push_json_str(out, k);
                    out.push(':');
                    v.stringify(out);
                }
                out.push('}');
            }
            JVal::Arr(a) => {
                out.push('[');
                for (i, v) in a.iter().enumerate() {
                    if i > 0 {
                        out.push(',');
                    }
                    v.stringify(out);
                }
                out.push(']');
            }
            JVal::Str(s) => push_json_str(out, s),
            // 双精度序列化：整数值不带小数点（7 而不是 7.0）
            JVal::Num(n) => out.push_str(&format!("{}", n)),
            JVal::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
            JVal::Null => out.push_str("null"),
        }
    }
}

pub(crate) fn push_json_str(out: &mut String, s: &str) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{08}' => out.push_str("\\b"),
            '\u{0C}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
}

pub(crate) struct JParser {
    c: Vec<char>,
    i: usize,
    /// 嵌套深度。配置是用户可手改的文件，一串 "[[[[…" 会让 value/array 互相递归到
    /// 栈溢出（0xC00000FD，windows_subsystem 下连日志都没有）。上限取 64 层，
    /// 真实的配置文件连 6 层都用不到。
    depth: u32,
}

impl JParser {
    fn ws(&mut self) {
        while self.i < self.c.len() && matches!(self.c[self.i], ' ' | '\t' | '\r' | '\n') {
            self.i += 1;
        }
    }
    fn value(&mut self) -> Option<JVal> {
        if self.depth >= 64 { return None; }
        self.ws();
        let ch = *self.c.get(self.i)?;
        match ch {
            '{' => {
                self.depth += 1;
                let v = self.object();
                self.depth -= 1;
                v
            }
            '[' => {
                self.depth += 1;
                let v = self.array();
                self.depth -= 1;
                v
            }
            '"' => Some(JVal::Str(self.string()?)),
            't' => self.lit("true", JVal::Bool(true)),
            'f' => self.lit("false", JVal::Bool(false)),
            'n' => self.lit("null", JVal::Null),
            _ => self.number(),
        }
    }
    fn lit(&mut self, word: &str, v: JVal) -> Option<JVal> {
        for w in word.chars() {
            if self.c.get(self.i) != Some(&w) {
                return None;
            }
            self.i += 1;
        }
        Some(v)
    }
    fn object(&mut self) -> Option<JVal> {
        self.i += 1; // {
        let mut m = Vec::new();
        self.ws();
        if self.c.get(self.i) == Some(&'}') {
            self.i += 1;
            return Some(JVal::Obj(m));
        }
        loop {
            self.ws();
            let k = self.string()?;
            self.ws();
            if self.c.get(self.i) != Some(&':') {
                return None;
            }
            self.i += 1;
            let v = self.value()?;
            m.push((k, v));
            self.ws();
            match self.c.get(self.i) {
                Some(&',') => {
                    self.i += 1;
                }
                Some(&'}') => {
                    self.i += 1;
                    return Some(JVal::Obj(m));
                }
                _ => return None,
            }
        }
    }
    fn array(&mut self) -> Option<JVal> {
        self.i += 1; // [
        let mut a = Vec::new();
        self.ws();
        if self.c.get(self.i) == Some(&']') {
            self.i += 1;
            return Some(JVal::Arr(a));
        }
        loop {
            let v = self.value()?;
            a.push(v);
            self.ws();
            match self.c.get(self.i) {
                Some(&',') => {
                    self.i += 1;
                }
                Some(&']') => {
                    self.i += 1;
                    return Some(JVal::Arr(a));
                }
                _ => return None,
            }
        }
    }
    fn string(&mut self) -> Option<String> {
        if self.c.get(self.i) != Some(&'"') {
            return None;
        }
        self.i += 1;
        let mut s = String::new();
        loop {
            let ch = *self.c.get(self.i)?;
            self.i += 1;
            match ch {
                '"' => return Some(s),
                '\\' => {
                    let e = *self.c.get(self.i)?;
                    self.i += 1;
                    match e {
                        '"' => s.push('"'),
                        '\\' => s.push('\\'),
                        '/' => s.push('/'),
                        'b' => s.push('\u{08}'),
                        'f' => s.push('\u{0C}'),
                        'n' => s.push('\n'),
                        'r' => s.push('\r'),
                        't' => s.push('\t'),
                        'u' => {
                            let mut cp: u32 = 0;
                            for _ in 0..4 {
                                let h = *self.c.get(self.i)?;
                                self.i += 1;
                                cp = cp * 16 + h.to_digit(16)?;
                            }
                            // 代理对：高代理 + 低代理拼成一个码点
                            if (0xD800..0xDC00).contains(&cp)
                                && self.c.get(self.i) == Some(&'\\')
                                && self.c.get(self.i + 1) == Some(&'u')
                            {
                                // 先确认后四位是合法低代理；不是就整段解析失败，
                                // 避免 lo - 0xDC00 下溢后在 release 里绕成一个"看似合法"的字符
                                let mut lo: u32 = 0;
                                let mut ok = true;
                                for k in 0..4 {
                                    match self.c.get(self.i + 2 + k).and_then(|h| h.to_digit(16)) {
                                        Some(d) => lo = lo * 16 + d,
                                        None => { ok = false; break; }
                                    }
                                }
                                if !ok || !(0xDC00..0xE000).contains(&lo) {
                                    return None;
                                }
                                self.i += 6;
                                let c = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                s.push(char::from_u32(c)?);
                            } else {
                                s.push(char::from_u32(cp)?);
                            }
                        }
                        _ => return None,
                    }
                }
                c => s.push(c),
            }
        }
    }
    fn number(&mut self) -> Option<JVal> {
        let start = self.i;
        if matches!(self.c.get(self.i), Some(&'-')) {
            self.i += 1;
        }
        while let Some(&ch) = self.c.get(self.i) {
            if ch.is_ascii_digit() || matches!(ch, '.' | 'e' | 'E' | '+' | '-') {
                self.i += 1;
            } else {
                break;
            }
        }
        let txt: String = self.c[start..self.i].iter().collect();
        txt.parse::<f64>().ok().map(JVal::Num)
    }
}

pub(crate) fn json_parse(s: &str) -> Option<JVal> {
    let mut p = JParser { c: s.chars().collect(), i: 0, depth: 0 };
    let v = p.value()?;
    p.ws();
    if p.i == p.c.len() {
        Some(v)
    } else {
        None
    }
}

/// Util::readTextFromBytes：UTF-16LE BOM → 直接按 u16 解释，UTF-8 BOM → 跳过
/// 其余一律按 UTF-8 认
pub(crate) fn cfg_decode(bytes: &[u8]) -> String {
    if bytes.len() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE {
        let u16s: Vec<u16> = bytes[2..]
            .chunks_exact(2)
            .map(|c| u16::from_le_bytes([c[0], c[1]]))
            .collect();
        return String::from_utf16_lossy(&u16s);
    }
    let off = if bytes.len() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF {
        3
    } else {
        0
    };
    String::from_utf8_lossy(&bytes[off..]).to_string()
}

pub(crate) fn cfg_default() -> JVal {
    json_parse(r#"{"common":{"autoStart":false,"language":"zh-CN"},"shortcutKey":{"cap":"F1","qrcap":"F2","hide":"F10"}}"#)
        .unwrap_or(JVal::Obj(Vec::new()))
}

pub(crate) fn cfg_file_path() -> Option<std::path::PathBuf> {
    // 1) exe 同目录（只有本来就存在才认，不往程序目录里新建）
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let p = dir.join("config.json");
            if p.exists() {
                return Some(p);
            }
        }
    }
    // 2) %APPDATA%\<APP_NAME>\config.json（目录不存在就建）
    if let Ok(appdata) = std::env::var("APPDATA") {
        let dir = std::path::Path::new(&appdata).join(APP_NAME);
        let _ = std::fs::create_dir_all(&dir);
        return Some(dir.join("config.json"));
    }
    None
}

pub(crate) fn cfg_save_to(p: &std::path::Path, v: &JVal) {
    let mut s = String::new();
    v.stringify(&mut s);
    // UTF-16LE + BOM
    let mut bytes = vec![0xFFu8, 0xFEu8];
    for u in s.encode_utf16() {
        bytes.push((u & 0xFF) as u8);
        bytes.push((u >> 8) as u8);
    }
    let _ = std::fs::write(p, bytes);
}

/// Setting 构造：文件不存在 → 默认配置（不落盘），空文件 → 默认配置并存一份
/// 坏 JSON → 默认配置；能解析 → 就用读到的
pub(crate) fn cfg_init() {
    let path = cfg_file_path();
    let obj = match path.as_ref().and_then(|p| std::fs::read(p).ok()) {
        Some(bytes) => {
            let txt = cfg_decode(&bytes);
            if txt.trim().is_empty() {
                let d = cfg_default();
                if let Some(p) = path.as_ref() {
                    cfg_save_to(p, &d);
                }
                d
            } else {
                json_parse(&txt).unwrap_or_else(cfg_default)
            }
        }
        None => cfg_default(),
    };
    *CFG.lock_ok() = Some(obj);
    *CFG_PATH.lock_ok() = path;
}

pub(crate) fn cfg_save() {
    let path = CFG_PATH.lock_ok().clone();
    if let (Some(p), Some(v)) = (path, CFG.lock_ok().as_ref()) {
        cfg_save_to(&p, v);
    }
}

/// 标注工具条上的选项（颜色/线宽/开关）每点一下都同步落盘的话，
/// 一次点击就要序列化整份配置再写一遍 UTF-16 文件 —— 有杀软实时扫描时
/// 单次就是十几毫秒，点色板会明显"慢半拍"。这里只置脏标记，
/// 由主窗口 1s 的 TRIM_TIMER 合并写一次。
static CFG_DIRTY: AtomicBool = AtomicBool::new(false);

pub(crate) fn cfg_save_soon() {
    CFG_DIRTY.store(true, Ordering::Relaxed);
}

/// 把攒下的配置改动真正写盘（每 1s 由定时器调一次，改动最多晚 1s 落盘）
pub(crate) fn cfg_flush_if_dirty() {
    if CFG_DIRTY.swap(false, Ordering::Relaxed) { cfg_save(); }
}

pub(crate) fn cfg_tool_flag(tool: &str, key: &str, def: bool) -> bool {
    let g = CFG.lock_ok();
    if let Some(v) = g.as_ref() {
        if let Some(JVal::Bool(b)) = v.obj_get("toolPin").and_then(|t| t.obj_get(tool)).and_then(|o| o.obj_get(key)) {
            return *b;
        }
    }
    def
}

pub(crate) fn cfg_tool_num(tool: &str, key: &str, def: f32) -> f32 {
    let g = CFG.lock_ok();
    if let Some(v) = g.as_ref() {
        if let Some(JVal::Num(n)) = v.obj_get("toolPin").and_then(|t| t.obj_get(tool)).and_then(|o| o.obj_get(key)) {
            return *n as f32;
        }
    }
    def
}

pub(crate) fn cfg_set_tool_flag(tool: &str, key: &str, val: bool) {
    {
        let mut g = CFG.lock_ok();
        if let Some(v) = g.as_mut() {
            if let Some(tp) = v.obj_ensure("toolPin") {
                if let Some(to) = tp.obj_ensure(tool) {
                    to.obj_set(key, JVal::Bool(val));
                }
            }
        }
    }
    cfg_save_soon();
}

pub(crate) fn cfg_set_tool_num(tool: &str, key: &str, val: f32) {
    {
        let mut g = CFG.lock_ok();
        if let Some(v) = g.as_mut() {
            if let Some(tp) = v.obj_ensure("toolPin") {
                if let Some(to) = tp.obj_ensure(tool) {
                    to.obj_set(key, JVal::Num(val as f64));
                }
            }
        }
    }
    cfg_save_soon();
}
