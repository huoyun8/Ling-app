//! 录屏的 MP4 通路
//! 画面：Media Foundation SinkWriter，H.264（起不来才退 HEVC）
//!   · 声音：WASAPI 抓默认设备。系统声走"渲染设备 + LOOPBACK"，麦克风走"捕获设备"，
//! 两路都转成 44.1k/双声道/16bit 再混在一起，交给 AAC 编码器
//!
//! 采集用 GDI BitBlt（与 GIF 通路同一份 screenshot_bgra），不走 DXGI 桌面复制：
//! 这样两条通路的画面口径完全一致，也不用替混合显卡笔记本处理"适配器选错"的坑。
//!
//! 整个模块只在采集线程里跑：COM/MF 的初始化与释放都在这一条线程上做。

// 媒体类型常量刻意 Windows SDK 的命名（MFVideoFormat_H264 等）
// 一眼能对上文档，不按 Rust 的 upper-case 规范改名
#![allow(non_upper_case_globals)]

use std::ffi::c_void;
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

type Hr = i32;

// ——————————————————————— GUID ———————————————————————
#[repr(C)]
#[derive(Clone, Copy)]
struct Guid {
    d1: u32,
    d2: u16,
    d3: u16,
    d4: [u8; 8],
}

const fn g(d1: u32, d2: u16, d3: u16, d4: [u8; 8]) -> Guid {
    Guid { d1, d2, d3, d4 }
}

// 媒体类型 / 属性键（mfapi.h）
const MF_MT_MAJOR_TYPE: Guid = g(0x48eba18e, 0xf8c9, 0x4687, [0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f]);
const MF_MT_SUBTYPE: Guid = g(0xf7e34c9a, 0x42e8, 0x4714, [0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5]);
const MF_MT_AVG_BITRATE: Guid = g(0x20332624, 0xfb0d, 0x4d9e, [0xbd, 0x0d, 0xcb, 0xf6, 0x78, 0x6c, 0x10, 0x2e]);
const MF_MT_FRAME_SIZE: Guid = g(0x1652c33d, 0xd6b2, 0x4012, [0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d]);
const MF_MT_FRAME_RATE: Guid = g(0xc459a2e8, 0x3d2c, 0x4e44, [0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0]);
const MF_MT_INTERLACE_MODE: Guid = g(0xe2724bb8, 0xe676, 0x4806, [0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd]);
const MF_MT_DEFAULT_STRIDE: Guid = g(0x644b4e48, 0x1e02, 0x4516, [0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6]);
const MF_MT_AUDIO_NUM_CHANNELS: Guid = g(0x37e48bf5, 0x645e, 0x4c5b, [0x89, 0xde, 0xad, 0xa9, 0xe2, 0x9b, 0x69, 0x6a]);
const MF_MT_AUDIO_SAMPLES_PER_SECOND: Guid = g(0x5faeeae7, 0x0290, 0x4c31, [0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba]);
const MF_MT_AUDIO_BITS_PER_SAMPLE: Guid = g(0xf2deb57f, 0x40fa, 0x4764, [0xaa, 0x33, 0xed, 0x4f, 0x2d, 0x1f, 0xf6, 0x69]);
const MF_MT_AUDIO_AVG_BYTES_PER_SECOND: Guid = g(0x1aab75c8, 0xcfef, 0x451c, [0xab, 0x95, 0xac, 0x03, 0x4b, 0x8e, 0x17, 0x31]);
const MF_MT_AUDIO_BLOCK_ALIGNMENT: Guid = g(0x322de230, 0x9eeb, 0x43bd, [0xab, 0x7a, 0xff, 0x41, 0x22, 0x51, 0x54, 0x1d]);

const MFMediaType_Video: Guid = g(0x73646976, 0x0000, 0x0010, [0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71]);
const MFMediaType_Audio: Guid = g(0x73647561, 0x0000, 0x0010, [0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71]);
const MFVideoFormat_H264: Guid = g(0x34363248, 0x0000, 0x0010, [0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71]);
const MFVideoFormat_HEVC: Guid = g(0x43564548, 0x0000, 0x0010, [0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71]);
const MFVideoFormat_RGB32: Guid = g(0x00000016, 0x0000, 0x0010, [0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71]);
const MFAudioFormat_AAC: Guid = g(0x00001610, 0x0000, 0x0010, [0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71]);
const MFAudioFormat_PCM: Guid = g(0x00000001, 0x0000, 0x0010, [0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71]);

const GUID_NULL: Guid = g(0, 0, 0, [0; 8]);
const IID_ICODEC_API: Guid = g(0x901DB4C7, 0x31CE, 0x41A2, [0x85, 0xDC, 0x8F, 0xA0, 0xBF, 0x41, 0xB8, 0xDA]);
const CODECAPI_AVENC_COMMON_RATE_CONTROL_MODE: Guid = g(0x1C0608E9, 0x370C, 0x4710, [0x8A, 0x58, 0xCB, 0x61, 0x81, 0xC4, 0x24, 0x23]);
const CODECAPI_AVENC_COMMON_QUALITY: Guid = g(0xFCBF57A3, 0x7EA5, 0x4B0C, [0x96, 0x44, 0x69, 0xB4, 0x0C, 0x39, 0xC3, 0x91]);
const CODECAPI_AVENC_COMMON_QUALITY_VS_SPEED: Guid = g(0x98332DF8, 0x03CD, 0x476B, [0x89, 0xFA, 0x3F, 0x9E, 0x44, 0x2D, 0xEC, 0x9F]);


// WASAPI
const CLSID_MMDEVICE_ENUMERATOR: Guid = g(0xBCDE0395, 0xE52F, 0x467C, [0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E]);
const IID_IMMDEVICE_ENUMERATOR: Guid = g(0xA95664D2, 0x9614, 0x4F35, [0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6]);
const IID_IAUDIO_CLIENT: Guid = g(0x1CB9AD4C, 0xDBFA, 0x4C32, [0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2]);
const IID_IAUDIO_CAPTURE_CLIENT: Guid = g(0xC8ADBD64, 0xE71E, 0x48A0, [0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17]);
const KSDATAFORMAT_SUBTYPE_IEEE_FLOAT: Guid = g(0x00000003, 0x0000, 0x0010, [0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71]);

const MF_VERSION: u32 = 0x0002_0070;      // MF_SDK_VERSION << 16 | MF_API_VERSION
const CLSCTX_INPROC_SERVER: u32 = 1;
const AUDCLNT_STREAMFLAGS_LOOPBACK: u32 = 0x0002_0000;
const AUDCLNT_BUFFERFLAGS_SILENT: u32 = 0x2;
const TARGET_RATE: u32 = 44100;
const TARGET_BLOCK: usize = 441;          // 10ms 一块（双声道 → 882 个 i16）
const FPS: u32 = 30;
// CapVideo::startMp4 没有按像素折算码率，dp.BR 保持默认 4000（kbps），写死 4 Mbps
const VIDEO_BITRATE: u32 = 4_000_000;

#[link(name = "ole32")]
extern "system" {
    fn CoCreateInstance(rclsid: *const Guid, outer: *mut c_void, ctx: u32, riid: *const Guid,
                        ppv: *mut *mut c_void) -> Hr;
    fn CoTaskMemFree(pv: *mut c_void);
    fn CoInitializeEx(pv: *mut c_void, coinit: u32) -> Hr;
    fn CoUninitialize();
}
const COINIT_MULTITHREADED: u32 = 0x0;

/// 采集线程的 COM 生命周期。MFStartup / WASAPI 都要求先初始化 COM，
/// 原来只在主线程初始化过，采集线程拿到的 CoCreateInstance 恒为
/// CO_E_NOTINITIALIZED —— 声音永远录不到。用 Drop 保证每条返回路径都配对释放。
struct ComInit(Hr);
impl Drop for ComInit {
    fn drop(&mut self) {
        if self.0 >= 0 { unsafe { CoUninitialize(); } }
    }
}

#[link(name = "mfplat")]
extern "system" {
    fn MFStartup(version: u32, flags: u32) -> Hr;
    fn MFShutdown() -> Hr;
    fn MFCreateMediaType(pp: *mut *mut c_void) -> Hr;
    fn MFCreateMemoryBuffer(max_len: u32, pp: *mut *mut c_void) -> Hr;
    fn MFCreateSample(pp: *mut *mut c_void) -> Hr;
}

#[link(name = "mfreadwrite")]
extern "system" {
    fn MFCreateSinkWriterFromURL(url: *const u16, stream: *mut c_void, attrs: *mut c_void,
                                 pp: *mut *mut c_void) -> Hr;
}

// ——————————————————————— COM 调用的薄封装 ———————————————————————
unsafe fn vt(obj: *mut c_void, slot: usize) -> usize {
    let table = *(obj as *mut *const usize);
    *table.add(slot)
}

unsafe fn com_release(obj: *mut c_void) {
    if obj.is_null() {
        return;
    }
    let f: extern "system" fn(*mut c_void) -> u32 = std::mem::transmute(vt(obj, 2));
    f(obj);
}

// IMFAttributes：SetUINT32=21 / SetUINT64=22 / SetGUID=24（前 3 个槽是 IUnknown）
unsafe fn attr_u32(obj: *mut c_void, key: &Guid, v: u32) -> Hr {
    let f: extern "system" fn(*mut c_void, *const Guid, u32) -> Hr = std::mem::transmute(vt(obj, 21));
    f(obj, key, v)
}
unsafe fn attr_u64(obj: *mut c_void, key: &Guid, v: u64) -> Hr {
    let f: extern "system" fn(*mut c_void, *const Guid, u64) -> Hr = std::mem::transmute(vt(obj, 22));
    f(obj, key, v)
}
unsafe fn attr_guid(obj: *mut c_void, key: &Guid, v: &Guid) -> Hr {
    let f: extern "system" fn(*mut c_void, *const Guid, *const Guid) -> Hr = std::mem::transmute(vt(obj, 24));
    f(obj, key, v)
}

use crate::wide;

// ——————————————————————— MP4 写出器 ———————————————————————
struct Mp4Writer {
    w: *mut c_void,
    has_audio: bool,
}

/// 编码器参数：VARIANT 在 x64 是 24 字节（vt + 3 个保留 + 16 字节联合）
#[repr(C)]
struct Variant {
    vt: u16,
    r1: u16,
    r2: u16,
    r3: u16,
    val: u64,
    pad: u64,
}

/// 把 CapVideo::startMp4 里那三个 CodecAPI 参数塞给编码器：
/// 质量模式（rate control = Quality）+ 质量 50 + 质量/速度 50。
/// 拿不到 ICodecAPI 就算了（也是 if (ca) 才设）
unsafe fn set_quality(w: *mut c_void, stream: u32) {
    let get_srv: extern "system" fn(*mut c_void, u32, *const Guid, *const Guid, *mut *mut c_void) -> Hr =
        std::mem::transmute(vt(w, 12));
    let mut ca: *mut c_void = std::ptr::null_mut();
    if get_srv(w, stream, &GUID_NULL, &IID_ICODEC_API, &mut ca) < 0 || ca.is_null() {
        return;
    }
    // ICodecAPI 继承 IUnknown：IsSupported=3 … SetValue=8、GetValue=9
    let set: extern "system" fn(*mut c_void, *const Guid, *mut Variant) -> Hr =
        std::mem::transmute(vt(ca, 8));
    let mk = |v: u32| Variant { vt: 19 /* VT_UI4 */, r1: 0, r2: 0, r3: 0, val: v as u64, pad: 0 };
    set(ca, &CODECAPI_AVENC_COMMON_QUALITY_VS_SPEED, &mut mk(50));
    set(ca, &CODECAPI_AVENC_COMMON_RATE_CONTROL_MODE, &mut mk(2));   // Quality
    set(ca, &CODECAPI_AVENC_COMMON_QUALITY, &mut mk(50));
    com_release(ca);
}

impl Mp4Writer {
    /// 建 SinkWriter：视频流必成，音频流失败就当"没有声音"继续录（不整段失败）
    /// codec 先 H.264，起不来再由调用方重开 HEVC
    unsafe fn open(path: &str, width: i32, height: i32, want_audio: bool, codec: &Guid) -> Option<Mp4Writer> {
        let mut w: *mut c_void = std::ptr::null_mut();
        if MFCreateSinkWriterFromURL(wide(path).as_ptr(), std::ptr::null_mut(),
                                     std::ptr::null_mut(), &mut w) < 0 || w.is_null() {
            return None;
        }

        // --- 输出类型：H.264 / HEVC ---
        let out = fmt_video(codec, width, height);
        if out.is_null() {
            com_release(w);
            return None;
        }
        attr_u32(out, &MF_MT_AVG_BITRATE, VIDEO_BITRATE);
        let mut stream: u32 = 0;
        let add: extern "system" fn(*mut c_void, *mut c_void, *mut u32) -> Hr = std::mem::transmute(vt(w, 3));
        let hr = add(w, out, &mut stream);
        com_release(out);
        if hr < 0 {
            com_release(w);
            return None;
        }
        // --- 输入类型：RGB32（top-down，步幅 = 宽*4），由 SinkWriter 自己转 NV12 ---
        let inp = fmt_video(&MFVideoFormat_RGB32, width, height);
        if inp.is_null() {
            com_release(w);
            return None;
        }
        attr_u32(inp, &MF_MT_DEFAULT_STRIDE, (width * 4) as u32);
        let set_in: extern "system" fn(*mut c_void, u32, *mut c_void, *mut c_void) -> Hr =
            std::mem::transmute(vt(w, 4));
        let hr = set_in(w, stream, inp, std::ptr::null_mut());
        com_release(inp);
        if hr < 0 {
            com_release(w);
            return None;
        }
        set_quality(w, stream);

        let mut has_audio = false;
        if want_audio {
            has_audio = Mp4Writer::add_audio(w).is_some();
        }
        // BeginWriting 必须在两条流都 SetInputMediaType 之后、任何 WriteSample 之前调用。
        // 漏掉它的话 WriteSample 会返回 MF_E_NOT_INITIALIZED，MF 一律丢弃样本 ——
        // 结果就是"录完得到一个 0 字节/没有画面的 mp4"，而且没有任何报错。
        let begin: extern "system" fn(*mut c_void) -> Hr = std::mem::transmute(vt(w, 5));
        if begin(w) < 0 {
            com_release(w);
            return None;
        }
        Some(Mp4Writer { w, has_audio })
    }

    /// 音频流：输出 AAC、输入 44.1k 双声道 16bit PCM。返回流号
    unsafe fn add_audio(w: *mut c_void) -> Option<u32> {
        let out: *mut c_void = media_type(&MFMediaType_Audio, &MFAudioFormat_AAC);
        if out.is_null() {
            return None;
        }
        attr_u32(out, &MF_MT_AUDIO_SAMPLES_PER_SECOND, TARGET_RATE);
        attr_u32(out, &MF_MT_AUDIO_NUM_CHANNELS, 2);
        attr_u32(out, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        attr_u32(out, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24_000);   // 192 kbps
        let mut stream: u32 = 0;
        let add: extern "system" fn(*mut c_void, *mut c_void, *mut u32) -> Hr = std::mem::transmute(vt(w, 3));
        let hr = add(w, out, &mut stream);
        com_release(out);
        if hr < 0 {
            return None;
        }
        let inp = media_type(&MFMediaType_Audio, &MFAudioFormat_PCM);
        if inp.is_null() {
            return None;
        }
        attr_u32(inp, &MF_MT_AUDIO_SAMPLES_PER_SECOND, TARGET_RATE);
        attr_u32(inp, &MF_MT_AUDIO_NUM_CHANNELS, 2);
        attr_u32(inp, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        attr_u32(inp, &MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        attr_u32(inp, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, TARGET_RATE * 4);
        let set_in: extern "system" fn(*mut c_void, u32, *mut c_void, *mut c_void) -> Hr =
            std::mem::transmute(vt(w, 4));
        let hr = set_in(w, stream, inp, std::ptr::null_mut());
        com_release(inp);
        if hr < 0 {
            return None;
        }
        Some(stream)
    }

    /// 写一帧画面（BGRA，top-down）。t100 是这一帧的时间戳（100ns 单位）
    /// 返回 false = WriteSample 失败
    unsafe fn write_video(&mut self, buf: &[u8], t100: i64) -> bool {
        if buf.is_empty() {
            return true;
        }
        let mut mb: *mut c_void = std::ptr::null_mut();
        if MFCreateMemoryBuffer(buf.len() as u32, &mut mb) < 0 || mb.is_null() {
            return false;
        }
        let lock: extern "system" fn(*mut c_void, *mut *mut u8, *mut u32, *mut u32) -> Hr =
            std::mem::transmute(vt(mb, 3));
        let mut p: *mut u8 = std::ptr::null_mut();
        let (mut maxlen, mut cur) = (0u32, 0u32);
        if lock(mb, &mut p, &mut maxlen, &mut cur) < 0 || p.is_null() {
            com_release(mb);
            return false;
        }
        std::ptr::copy_nonoverlapping(buf.as_ptr(), p, buf.len());
        let unlock: extern "system" fn(*mut c_void) -> Hr = std::mem::transmute(vt(mb, 4));
        unlock(mb);
        let set_len: extern "system" fn(*mut c_void, u32) -> Hr = std::mem::transmute(vt(mb, 6));
        set_len(mb, buf.len() as u32);

        let mut ok = false;
        let mut smp: *mut c_void = std::ptr::null_mut();
        if MFCreateSample(&mut smp) >= 0 && !smp.is_null() {
            let add_buf: extern "system" fn(*mut c_void, *mut c_void) -> Hr = std::mem::transmute(vt(smp, 42));
            add_buf(smp, mb);
            let set_time: extern "system" fn(*mut c_void, i64) -> Hr = std::mem::transmute(vt(smp, 36));
            let set_dur: extern "system" fn(*mut c_void, i64) -> Hr = std::mem::transmute(vt(smp, 38));
            set_time(smp, t100);
            set_dur(smp, 10_000_000 / FPS as i64);
            let write: extern "system" fn(*mut c_void, u32, *mut c_void) -> Hr = std::mem::transmute(vt(self.w, 6));
            ok = write(self.w, 0, smp) >= 0;
            com_release(smp);
        }
        com_release(mb);
        ok
    }

    /// 写一块音频（44.1k 双声道 16bit，交错）。frames 是采样帧数
    unsafe fn write_audio(&mut self, pcm: &[i16], frames: usize, t100: i64) {
        if !self.has_audio || frames == 0 {
            return;
        }
        let bytes = frames * 4;
        let mut mb: *mut c_void = std::ptr::null_mut();
        if MFCreateMemoryBuffer(bytes as u32, &mut mb) < 0 || mb.is_null() {
            return;
        }
        let lock: extern "system" fn(*mut c_void, *mut *mut u8, *mut u32, *mut u32) -> Hr =
            std::mem::transmute(vt(mb, 3));
        let mut p: *mut u8 = std::ptr::null_mut();
        let (mut maxlen, mut cur) = (0u32, 0u32);
        if lock(mb, &mut p, &mut maxlen, &mut cur) >= 0 && !p.is_null() {
            std::ptr::copy_nonoverlapping(pcm.as_ptr() as *const u8, p, bytes);
            let unlock: extern "system" fn(*mut c_void) -> Hr = std::mem::transmute(vt(mb, 4));
            unlock(mb);
            let set_len: extern "system" fn(*mut c_void, u32) -> Hr = std::mem::transmute(vt(mb, 6));
            set_len(mb, bytes as u32);
            let mut smp: *mut c_void = std::ptr::null_mut();
            if MFCreateSample(&mut smp) >= 0 && !smp.is_null() {
                let add_buf: extern "system" fn(*mut c_void, *mut c_void) -> Hr = std::mem::transmute(vt(smp, 42));
                add_buf(smp, mb);
                let set_time: extern "system" fn(*mut c_void, i64) -> Hr = std::mem::transmute(vt(smp, 36));
                let set_dur: extern "system" fn(*mut c_void, i64) -> Hr = std::mem::transmute(vt(smp, 38));
                set_time(smp, t100);
                set_dur(smp, frames as i64 * 10_000_000 / TARGET_RATE as i64);
                let write: extern "system" fn(*mut c_void, u32, *mut c_void) -> Hr = std::mem::transmute(vt(self.w, 6));
                write(self.w, 1, smp);
                com_release(smp);
            }
        }
        com_release(mb);
    }

    unsafe fn finalize(&mut self) {
        if !self.w.is_null() {
            let fin: extern "system" fn(*mut c_void) -> Hr = std::mem::transmute(vt(self.w, 11));
            fin(self.w);
            com_release(self.w);
            self.w = std::ptr::null_mut();
        }
    }
}

/// 视频媒体类型（输出/输入共用骨架：主类型=Video、子类型、帧尺寸、帧率、逐行扫描）
unsafe fn fmt_video(sub: &Guid, w: i32, h: i32) -> *mut c_void {
    let t = media_type(&MFMediaType_Video, sub);
    if t.is_null() {
        return t;
    }
    attr_u32(t, &MF_MT_INTERLACE_MODE, 2);   // MFVideoInterlace_Progressive
    attr_u64(t, &MF_MT_FRAME_SIZE, ((w as u64) << 32) | h as u64);
    attr_u64(t, &MF_MT_FRAME_RATE, ((FPS as u64) << 32) | 1);
    t
}

unsafe fn media_type(major: &Guid, sub: &Guid) -> *mut c_void {
    let mut t: *mut c_void = std::ptr::null_mut();
    if MFCreateMediaType(&mut t) < 0 || t.is_null() {
        return std::ptr::null_mut();
    }
    attr_guid(t, &MF_MT_MAJOR_TYPE, major);
    attr_guid(t, &MF_MT_SUBTYPE, sub);
    t
}

// ——————————————————————— WASAPI 抓音 ———————————————————————
// WAVEFORMATEX 在 mmreg.h 里是 1 字节对齐（pshpack1），这里跟着 packed
#[repr(C, packed)]
struct WaveFormatEx {
    tag: u16,
    channels: u16,
    rate: u32,
    avg_bytes: u32,
    block_align: u16,
    bits: u16,
    cb_size: u16,
}

struct AudioCap {
    client: *mut c_void,
    cap: *mut c_void,
    chans: u32,
    rate: u32,
    bits: u32,
    block: u32,
    is_float: bool,
    pos: f64,     // 重采样相位（源采样率 → 44.1k）
    started: bool,
}

impl AudioCap {
    /// loopback=true 抓系统声（默认渲染设备的回环），false 抓默认麦克风
    unsafe fn open(loopback: bool) -> Option<AudioCap> {
        let mut enumer: *mut c_void = std::ptr::null_mut();
        if CoCreateInstance(&CLSID_MMDEVICE_ENUMERATOR, std::ptr::null_mut(), CLSCTX_INPROC_SERVER,
                            &IID_IMMDEVICE_ENUMERATOR, &mut enumer) < 0 || enumer.is_null() {
            return None;
        }
        let mut dev: *mut c_void = std::ptr::null_mut();
        let flow: u32 = if loopback { 0 } else { 1 };   // eRender / eCapture
        let get_dev: extern "system" fn(*mut c_void, u32, u32, *mut *mut c_void) -> Hr =
            std::mem::transmute(vt(enumer, 4));
        let hr = get_dev(enumer, flow, 0, &mut dev);    // eConsole
        com_release(enumer);
        if hr < 0 || dev.is_null() {
            return None;
        }
        let mut client: *mut c_void = std::ptr::null_mut();
        let activate: extern "system" fn(*mut c_void, *const Guid, u32, *mut c_void, *mut *mut c_void) -> Hr =
            std::mem::transmute(vt(dev, 3));
        let hr = activate(dev, &IID_IAUDIO_CLIENT, CLSCTX_INPROC_SERVER, std::ptr::null_mut(), &mut client);
        com_release(dev);
        if hr < 0 || client.is_null() {
            return None;
        }
        let mut fmt: *mut u8 = std::ptr::null_mut();
        let get_fmt: extern "system" fn(*mut c_void, *mut *mut u8) -> Hr = std::mem::transmute(vt(client, 8));
        if get_fmt(client, &mut fmt) < 0 || fmt.is_null() {
            com_release(client);
            return None;
        }
        let wf = &*(fmt as *const WaveFormatEx);
        let chans = wf.channels as u32;
        let rate = wf.rate;
        let bits = wf.bits as u32;
        let block = wf.block_align as u32;
        let tag = wf.tag;
        // WAVE_FORMAT_EXTENSIBLE：真正格式在 SubFormat GUID 里（偏移 24）
        let mut is_float = tag == 3;
        if tag == 0xFFFE {
            let sub = std::ptr::read_unaligned((fmt.add(24)) as *const Guid);
            is_float = sub.d1 == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT.d1;
        }
        let init: extern "system" fn(*mut c_void, u32, u32, i64, i64, *const WaveFormatEx, *const Guid) -> Hr =
            std::mem::transmute(vt(client, 3));
        let flags = if loopback { AUDCLNT_STREAMFLAGS_LOOPBACK } else { 0 };
        let hr = init(client, 0 /*SHARED*/, flags, 0, 0, fmt as *const WaveFormatEx, std::ptr::null());
        CoTaskMemFree(fmt as *mut c_void);
        if hr < 0 || chans == 0 || rate == 0 || block == 0 {
            com_release(client);
            return None;
        }
        let mut cap: *mut c_void = std::ptr::null_mut();
        let get_srv: extern "system" fn(*mut c_void, *const Guid, *mut *mut c_void) -> Hr =
            std::mem::transmute(vt(client, 14));
        let hr = get_srv(client, &IID_IAUDIO_CAPTURE_CLIENT, &mut cap);
        if hr < 0 || cap.is_null() {
            com_release(client);
            return None;
        }
        let start: extern "system" fn(*mut c_void) -> Hr = std::mem::transmute(vt(client, 10));
        let started = start(client) >= 0;
        if !started {
            com_release(cap);
            com_release(client);
            return None;
        }
        Some(AudioCap { client, cap, chans, rate, bits, block, is_float, pos: 0.0, started: true })
    }

    /// 读一个采样值（按通道 c，源格式 float / 16 / 32 位整型）
    fn sample(&self, data: &[u8], frame: usize, c: usize) -> f32 {
        let cs = (self.bits / 8) as usize;
        let off = frame * self.block as usize + c.min(self.chans.saturating_sub(1) as usize) * cs;
        if off + cs > data.len() {
            return 0.0;
        }
        unsafe {
            let p = data.as_ptr().add(off);
            if self.is_float && cs == 4 {
                f32::from_le_bytes([*p, *p.add(1), *p.add(2), *p.add(3)])
            } else if self.bits == 16 {
                let v = i16::from_le_bytes([*p, *p.add(1)]);
                v as f32 / 32768.0
            } else if self.bits == 24 {
                // 24bit 打包 PCM：三字节小端有符号，符号扩展后归一化
                let v = ((*p as i32) | ((*p.add(1) as i32) << 8) | ((*p.add(2) as i32) << 16)) << 8 >> 8;
                v as f32 / 8388608.0
            } else if self.bits == 32 {
                let v = i32::from_le_bytes([*p, *p.add(1), *p.add(2), *p.add(3)]);
                v as f32 / 2147483648.0
            } else {
                0.0
            }
        }
    }

    /// 取一个包，转成 44.1k/双声道/i16 追加到 out。没有新包就直接返回。
    ///
    /// 这里**一次只取一个包**，绝不 `while (还有包) 一直抽`
    /// 每次进循环只 GetBuffer 一次就 ReleaseBuffer）。写成"抽干"循环会在回环采集上
    /// 会出现 GetNextPacketSize 永远返回 448、ReleaseBuffer 之后仍报同一个包的情况 ——
    /// 于是这个循环永不退出，整条录制线程卡死在里面：REC_STOP 读不到、编码器不 finalize、
    /// 临时文件一直占着。表现就是"录一次 → ESC → 再录，提示录制启动失败"（第二次打不开
    /// 还被子线程占着的 temp.mp4）。循环节拍是 5ms，一个包约 10ms，单包取足够跟上。
    unsafe fn drain(&mut self, out: &mut Vec<i16>) {
        let next: extern "system" fn(*mut c_void, *mut u32) -> Hr = std::mem::transmute(vt(self.cap, 5));
        let mut pkt: u32 = 0;
        if next(self.cap, &mut pkt) < 0 || pkt == 0 {
            return;
        }
        let get_buf: extern "system" fn(*mut c_void, *mut *mut u8, *mut u32, *mut u32,
                                         *mut u64, *mut u64) -> Hr =
            std::mem::transmute(vt(self.cap, 3));
        let mut data: *mut u8 = std::ptr::null_mut();
        let mut frames: u32 = 0;
        let mut flags: u32 = 0;
        let (mut devpos, mut qpc) = (0u64, 0u64);
        if get_buf(self.cap, &mut data, &mut frames, &mut flags, &mut devpos, &mut qpc) < 0 {
            return;
        }
        if frames > 0 {
            if flags & AUDCLNT_BUFFERFLAGS_SILENT != 0 || data.is_null() {
                // 静音包：照样按**源采样率 → 44.1k 的同一套相位推进**吐等量 0。
                // 不能直接 push frames*2 个 0（按源速率算长度）：设备是 44.1k 的话，
                // 每来一个静音包就少算约 7% 的样本，长时间录静音段会让音轨越来越短，
                // 最后声音比画面提前结束/整体错位。
                let ratio = self.rate as f64 / TARGET_RATE as f64;
                while self.pos < frames as f64 {
                    out.push(0);
                    out.push(0);
                    self.pos += ratio;
                }
                self.pos -= frames as f64;
            } else {
                let bytes = std::slice::from_raw_parts(data, frames as usize * self.block as usize);
                let ratio = self.rate as f64 / TARGET_RATE as f64;
                while self.pos < frames as f64 {
                    let i = self.pos as usize;
                    let l = self.sample(bytes, i, 0);
                    let r = if self.chans > 1 { self.sample(bytes, i, 1) } else { l };
                    // *32767 会把 +1.0 截成 32767，但 -1.0 会截到 -32767（差 1 LSB 的直流偏移）
                    out.push((l.clamp(-1.0, 1.0) * 32767.0) as i16);
                    out.push((r.clamp(-1.0, 1.0) * 32767.0) as i16);
                    self.pos += ratio;
                }
                self.pos -= frames as f64;
            }
        }
        // 无论有没有取到样本，包都必须还回去（不还的话下一轮 GetBuffer 会报 OUT_OF_ORDER）
        let release_buf: extern "system" fn(*mut c_void) -> Hr = std::mem::transmute(vt(self.cap, 4));
        release_buf(self.cap);
    }

    unsafe fn close(&mut self) {
        if self.started {
            let stop: extern "system" fn(*mut c_void) -> Hr = std::mem::transmute(vt(self.client, 11));
            stop(self.client);
            self.started = false;
        }
        if !self.cap.is_null() {
            com_release(self.cap);
            self.cap = std::ptr::null_mut();
        }
        if !self.client.is_null() {
            com_release(self.client);
            self.client = std::ptr::null_mut();
        }
    }
}

// ——————————————————————— 采集主循环 ———————————————————————
/// 抓选区、编码成 MP4 落到 path。阻塞到停止标志被置位（或出错），返回写出的帧数。
/// None = 起不来/写到一半就废：调用方要弹"录制启动失败"。
pub fn record_mp4(path: &str, _ox: i32, _oy: i32, sw: i32, sh: i32, spk: bool, mic: bool) -> Option<u32> {
    // H.264 编码器只吃偶数尺寸，宽再按 4 对齐是为了色度采样不半像素错位。
    let w = if sw >= 4 { sw & !3 } else { 4 };
    let h = if sh >= 2 { sh & !1 } else { 2 };
    unsafe {
        let _com = ComInit(CoInitializeEx(std::ptr::null_mut(), COINIT_MULTITHREADED));
        if MFStartup(MF_VERSION, 0) < 0 {
            return None;
        }
        let mut caps: Vec<AudioCap> = Vec::new();
        if spk {
            if let Some(c) = AudioCap::open(true) {
                caps.push(c);
            }
        }
        if mic {
            if let Some(c) = AudioCap::open(false) {
                caps.push(c);
            }
        }
        let want_audio = !caps.is_empty();
        // H.264 起不来（老系统缺编码器 / AddStream 或 SetInputMediaType 失败）才退 HEVC
        let mut writer = match Mp4Writer::open(path, w, h, want_audio, &MFVideoFormat_H264)
            .or_else(|| Mp4Writer::open(path, w, h, want_audio, &MFVideoFormat_HEVC))
        {
            Some(x) => x,
            None => {
                for c in caps.iter_mut() {
                    c.close();
                }
                MFShutdown();
                return None;
            }
        };
        // 音频流没建起来就把采集也关掉，别白转格式
        if !writer.has_audio {
            for c in caps.iter_mut() {
                c.close();
            }
            caps.clear();
        }

        let interval = Duration::from_millis(1000 / FPS as u64);
        let t_start = Instant::now();
        let mut next_frame = t_start;
        let mut frames: u32 = 0;
        let mut audio_pending: Vec<i16> = Vec::new();
        let mut audio_done: u64 = 0;           // 已写出的音频采样帧数
        let mut mix: Vec<i16> = Vec::new();
        let mut sum: Vec<i32> = Vec::new();
        let mut one: Vec<i16> = Vec::new();
        // 帧缓冲复用到整段录制：1080p 一帧 8MB，30fps 下每帧新申请再释放
        // 就是每秒 240MB 的分配/清零（4K 更夸张），全部换成一次分配反复写
        let mut frame: Vec<u8> = Vec::new();

        loop {
            if crate::REC_STOP.load(Ordering::Relaxed) {
                break;
            }
            let now = Instant::now();
            if now >= next_frame {
                // 原点每帧现取：录制中拖框移动录制范围时，下一帧就跟着换地方
                // （尺寸不变，所以编码器不用重建、已录的内容也不会丢）
                let (ox, oy) = crate::rec_origin();
                if crate::screenshot_bgra_into(ox, oy, w, h, &mut frame) {
                    // 时间戳按**真实经过的时间**给，不是"第几帧 / 30"。
                    // 抓屏慢的时候（大分辨率、机械硬盘、GPU 忙）会掉帧，按帧号算出来的
                    // 时间轴比真实时间短 —— 10 秒的录制会被压成 5 秒的视频，动作整体加速。
                    // 100ns 单位 = 纳秒 / 100。
                    let t100 = t_start.elapsed().as_nanos() as i64 / 100;
                    if !writer.write_video(&frame, t100) {
                        break;
                    }
                    frames += 1;
                }
                next_frame += interval;
                if next_frame < now {
                    next_frame = now + interval;   // 抓得慢就重同步，不补帧
                }
            }
            if writer.has_audio {
                // MIXBUFFER：整段都贴着 0 的音源不算一路（0.001 阈值，i16 约 33）
                // 两路都有效时**取平均**（Fin 除以路数），不是直接相加再削顶；
                // 全静音时仍然按采集到的帧数补 0，音轨长度才和画面对得上。
                mix.clear();
                sum.clear();
                let mut lanes = 0i32;
                for c in caps.iter_mut() {
                    one.clear();
                    c.drain(&mut one);
                    if one.len() > mix.len() {
                        mix.resize(one.len(), 0);     // 只占位，值下面按 sum 重填
                    }
                    if one.iter().all(|v| v.abs() <= 32) {
                        continue;
                    }
                    lanes += 1;
                    if one.len() > sum.len() {
                        sum.resize(one.len(), 0);
                    }
                    for (i, v) in one.iter().enumerate() {
                        sum[i] += *v as i32;
                    }
                }
                sum.resize(mix.len(), 0);     // 静音那路可能比有效那路长，补齐再抽样
                if lanes > 0 {
                    for (i, m) in mix.iter_mut().enumerate() {
                        *m = (sum[i] / lanes).clamp(-32768, 32767) as i16;
                    }
                }
                audio_pending.extend_from_slice(&mix);
                while audio_pending.len() >= TARGET_BLOCK * 2 {
                    let t = audio_done as i64 * 10_000_000 / TARGET_RATE as i64;
                    writer.write_audio(&audio_pending[..TARGET_BLOCK * 2], TARGET_BLOCK, t);
                    audio_pending.drain(..TARGET_BLOCK * 2);
                    audio_done += TARGET_BLOCK as u64;
                }
            }
            // 没有音频时只按帧间隔睡；有音频时 5ms 转一圈，包不积压
            let wait = if writer.has_audio { 5 } else {
                let left = next_frame.saturating_duration_since(Instant::now()).as_millis() as u64;
                left.max(1)
            };
            std::thread::sleep(Duration::from_millis(wait));
        }
        writer.finalize();
        for c in caps.iter_mut() {
            c.close();
        }
        MFShutdown();
        // 一帧都没写出来又没人叫停 = 启动/写入失败（返回 -15），要报错
        // 用户自己按停止的（哪怕不到一帧）不算失败，交给上层弹"没有录到画面"
        if frames == 0 && !crate::REC_STOP.load(Ordering::Relaxed) {
            return None;
        }
        Some(frames)
    }
}
