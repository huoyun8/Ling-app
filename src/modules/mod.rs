//! 业务模块：二维码 / 文件校验 / 定时关机 / 设置 / 文字识别 / 配置，以及截图与标注的各个功能块。
//! main.rs 只留窗口类注册、消息循环、页面切换与命令分派。

pub mod capture;
pub mod config;
pub mod editor;
pub mod filehash;
pub mod finalize;
pub mod icons;
pub mod ocr;
pub mod pin;
pub mod qr;
pub mod record;
pub mod rename;
pub mod selection;
pub mod settings;
pub mod shutdown;
pub mod toolbar;
pub mod tooltip;
pub mod verify;
