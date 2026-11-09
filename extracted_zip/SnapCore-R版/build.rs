// 构建期资源处理
//   1) OCR 资产压缩：把 assets/ 下的 ocr-process.exe + PP-OCRv6 模型 + 字典
//      用 flate2(deflate) 压成 SOCP 风格容器，写到 OUT_DIR，由 main.rs include_bytes! 内嵌。
// 45.9MB 原始资产 -> 约 31.5MB 压缩载荷，exe 体积由 47MB 降到约 33MB。
//   2) 图标资源：直接调用 Windows SDK 的 rc.exe 把 assets/logo.ico 编成资源（资源 ID 1），
//      再通过 rustc-link-arg 交给链接器。不再依赖 winres —— 它内部要调 reg.exe 查注册表，
//      在受限沙箱里会被拦截；直接调 rc.exe 在任何带 Windows SDK 的机器上都可用。
use std::env;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");

    // 品牌名按 feature 注入：不带 OCR 的构建 = SnapCore，带 OCR 的构建 = SnapOCR。
    // main.rs 侧用 env!/concat! 消费，一份源码出两个产物（窗口类名、互斥体、
    // 注册表项、配置目录都由它派生），两个版本可同时安装、同时运行。
    let app_name = if env::var("CARGO_FEATURE_OCR").is_ok() { "SnapOCR" } else { "SnapCore" };
    println!("cargo:rustc-env=APP_NAME={app_name}");
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR 未设置"));
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR 未设置"));
    let assets = manifest.join("assets");

    // 仅当 ocr feature 开启时才压缩（main.rs 的 include_bytes! 也只在 ocr 下编译）
    if env::var("CARGO_FEATURE_OCR").is_ok() {
        compress_assets(&out_dir, &assets);
    }


    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        embed_icon(&out_dir, &assets);
    }
}

/// 把 4 个 OCR 资产压缩到 OUT_DIR（带 mtime 缓存：源比产物新才重压）
///
/// 资产缺失时直接 `error:` 中止构建：否则 include_bytes! 会报
/// "couldn't read .../xxx.sz"，看错误的人根本不知道是自己少放了模型文件。
fn compress_assets(out_dir: &Path, assets: &Path) {
    let items = [
        ("ocr-process.exe", "ocr-process.exe.sz"),
        ("PP-OCRv6_det_small.onnx", "PP-OCRv6_det_small.onnx.sz"),
        ("PP-OCRv6_rec_small.onnx", "PP-OCRv6_rec_small.onnx.sz"),
        ("ppocrv6_dict.txt", "ppocrv6_dict.txt.sz"),
    ];
    // 先整批检查一遍，免得压到一半才发现少东西
    let missing: Vec<&str> = items
        .iter()
        .filter(|(src, _)| !assets.join(src).exists())
        .map(|(src, _)| *src)
        .collect();
    if !missing.is_empty() {
        // 用非零退出码把构建卡在这里：光 eprintln 一句 "error: ..." 并不会让 cargo 停下，
        // 它会继续编到 include_bytes! 才炸，那时报的是"找不到 xxx.sz"，
        // 看错误的人根本想不到是自己少放了模型文件。
        eprintln!(
            "OCR 资产缺失：assets/ 下缺少 {}。\n\
             请补齐这些文件后再构建，或用 `cargo build --release`（默认不带引擎）出包。",
            missing.join("、")
        );
        std::process::exit(1);
    }
    for (src, dst) in items {
        let sp = assets.join(src);
        println!("cargo:rerun-if-changed={}", sp.display());
        let dp = out_dir.join(dst);
        if !needs_rebuild(&sp, &dp) {
            continue;
        }
        let Ok(data) = fs::read(&sp) else { continue };
        let mut enc = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::new(6));
        if enc.write_all(&data).is_err() {
            continue;
        }
        if let Ok(buf) = enc.finish() {
            let _ = fs::write(&dp, &buf);
        }
    }
}

/// 源文件存在且比产物新（或产物不存在）时需要重建
fn needs_rebuild(src: &Path, dst: &Path) -> bool {
    let Ok(sm) = fs::metadata(src).and_then(|m| m.modified()) else { return true };
    let Ok(dm) = fs::metadata(dst).and_then(|m| m.modified()) else { return true };
    sm > dm
}

/// 编译图标资源：rc.exe 在 OUT_DIR 内生成 resource.res，链接器以输入形式并入
fn embed_icon(out_dir: &Path, assets: &Path) {
    // 品牌图标按 feature 分开：SnapCore 用红色 snap 标（logo_core.ico），
    // SnapOCR 用紫底 OCR 标（logo.ico）——两张图各带各的功能语义，
    // 任务栏/开始菜单里一眼能分出是哪个产物。
    let name = if env::var("CARGO_FEATURE_OCR").is_ok() { "logo.ico" } else { "logo_core.ico" };
    let ico = assets.join(name);
    // 缺核心标时退回老图标，别让 exe 裸奔没图标
    let ico = if ico.exists() { ico } else { assets.join("logo.ico") };
    if !ico.exists() {
        return;
    }
    // 把 ico 拷到 OUT_DIR，.rc 里只写相对名，避开 Windows 路径反斜杠在 .rc 字符串里的转义问题
    let dst_ico = out_dir.join("logo.ico");
    if fs::copy(&ico, &dst_ico).is_err() {
        return;
    }
    let rcfile = out_dir.join("resource.rc");
    if fs::write(&rcfile, "1 ICON \"logo.ico\"\r\n").is_err() {
        return;
    }
    // MSVC：rc.exe 产出 .res，直接交给链接器。
    if let Some(rc) = find_rc() {
        let res = out_dir.join("resource.res");
        let status = Command::new(&rc)
            .arg("/nologo")
            .arg(format!("/I{}", out_dir.display()))
            .arg(format!("/fo{}", res.display()))
            .arg(&rcfile)
            .status();
        match status {
            Ok(s) if s.success() && res.exists() => {
                println!("cargo:rustc-link-arg={}", res.display());
                println!("cargo:rerun-if-changed={}", ico.display());
                return;
            }
            _ => println!("cargo:warning=rc.exe 编译图标失败，尝试 windres"),
        }
    }
    // GNU 工具链（含 Linux/macOS 交叉编译）：windres 产出 COFF 对象，再交给链接器。
    if let Some(windres) = find_action("windres") {
        let obj = out_dir.join("resource.o");
        let status = Command::new(&windres)
            .arg("--input")
            .arg(&rcfile)
            .arg("--output")
            .arg(&obj)
            .arg("--output-format=coff")
            .arg(format!("--include-dir={}", out_dir.display()))
            .status();
        match status {
            Ok(s) if s.success() && obj.exists() => {
                println!("cargo:rustc-link-arg={}", obj.display());
                println!("cargo:rerun-if-changed={}", ico.display());
                return;
            }
            _ => println!("cargo:warning=windres 编译图标失败，exe 将不带图标"),
        }
    } else {
        println!("cargo:warning=未找到 rc.exe / windres，exe 将不带图标");
    }
}

/// 查找 GNU 工具链里的工具：先查带三元组前缀的名字，再查裸名
fn find_action(name: &str) -> Option<PathBuf> {
    for candidate in [format!("x86_64-w64-mingw32-{name}"), format!("i686-w64-mingw32-{name}"), name.to_string()] {
        if let Some(p) = find_in_path(&candidate) {
            return Some(p);
        }
    }
    None
}

/// 查找 rc.exe：先查 PATH，再扫 Windows Kits\bin\<ver>\x64\，取版本号最大的一档。
///
/// 不能只靠 `ProgramFiles(x86)` 这个环境变量：受限环境（CI 容器、精简的构建沙箱）
/// 里它可能是空的，那样就直接 return None，exe 悄悄丢掉图标还不报错。因此要
/// "环境变量 → 已装 SDK 的常规位置"逐层兜底，都没找到才放弃。
fn find_rc() -> Option<PathBuf> {
    if let Some(p) = find_in_path("rc.exe") {
        return Some(p);
    }
    let mut roots: Vec<PathBuf> = Vec::new();
    for var in ["WindowsSdkDir", "ProgramFiles(x86)", "ProgramFiles"] {
        if let Ok(v) = env::var(var) {
            if !v.trim().is_empty() {
                let root = PathBuf::from(&v);
                roots.push(root.join("Windows Kits").join("10").join("bin"));
                // WindowsSdkDir 直接指向 SDK 根，其余两个要拼 Windows Kits\10
                roots.push(root.join("bin"));
            }
        }
    }
    roots.push(PathBuf::from(r"C:\Program Files (x86)\Windows Kits\10\bin"));
    roots.push(PathBuf::from(r"C:\Program Files\Windows Kits\10\bin"));

    for bin in roots {
        let Ok(rd) = fs::read_dir(&bin) else { continue };
        // 版本按数值比（10.0.9000.0 要排在 10.0.19041.0 之后），字符串比会排错
        let mut best: Option<(Vec<u32>, PathBuf)> = None;
        for e in rd.flatten() {
            let p = e.path().join("x64").join("rc.exe");
            if !p.exists() {
                continue;
            }
            let key = version_key(&e.file_name().to_string_lossy());
            if best.as_ref().is_none_or(|(bv, _)| key > *bv) {
                best = Some((key, p));
            }
        }
        if let Some((_, p)) = best {
            return Some(p);
        }
    }
    None
}

/// "10.0.22621.0" -> [10, 0, 22621, 0]；非数字段记 0，保证长度不齐也能比大小
fn version_key(name: &str) -> Vec<u32> {
    name.split('.').map(|s| s.parse::<u32>().unwrap_or(0)).collect()
}

fn find_in_path(name: &str) -> Option<PathBuf> {
    let path = env::var_os("PATH")?;
    for dir in env::split_paths(&path) {
        let p = dir.join(name);
        if p.exists() {
            return Some(p);
        }
    }
    None
}
