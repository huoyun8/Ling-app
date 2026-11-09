//! 视觉常量单一来源。
//!
//! 界面代码只引用这里的 token，不再写死颜色 / 尺寸 / 字号 / 圆角 / 线宽 / 间距 / DPI。
//! 以后整体换肤或换 UI 框架，只改本文件（以及 `layout` 里的页面坐标），业务逻辑不动。
//! 所有取值均为现有值，原样搬入，不在这里做任何"优化配色"。

/// 颜色：统一 `0xRRGGBB`。需要 alpha 的场景单独用 `alpha::*`。
pub mod color {
    // —— 底面与层级 ——
    pub const PAGE: u32 = 0xF7F9FC;          // 主窗口页面底
    pub const SURFACE: u32 = 0xFFFFFF;       // 卡片 / 面板 / 工具条 / 输入框底
    pub const SURFACE_ALT: u32 = 0xE4EAF2;   // 灰底填充、悬停底
    pub const CANVAS: u32 = 0x343840;        // 标注画布外底

    // —— 边框与分隔 ——
    pub const BORDER: u32 = 0xD0D8E4;        // 控件 / 输入框 / 卡片描边
    pub const DIVIDER_SOFT: u32 = 0xE3E5E7;  // 设置页分组线

    // —— 文字 ——
    pub const TEXT: u32 = 0x2D394A;          // 主文字
    pub const TEXT_SECONDARY: u32 = 0x475569; // 次级标签
    pub const TEXT_TERTIARY: u32 = 0x788496; // 说明 / 占位
    pub const TEXT_INVERSE: u32 = 0xFFFFFF;  // 深底 / 彩色底上的文字

    // —— 强调 ——
    pub const ACCENT: u32 = 0x3867E0;        // 主操作、进度填充
    pub const ACCENT_HOVER: u32 = 0x1B71DB;
    pub const ACCENT_PRESSED: u32 = 0x2980B9;
    pub const ACCENT_DEEP: u32 = 0x0353B6;   // 主界面蓝色按钮默认（41 版复制同色）
    pub const TRACK: u32 = 0x94A8D2;         // 进度条轨道

    // —— 页签 ——
    pub const TAB_BG: u32 = 0xE2E8F0;        // 未选中页签底
    pub const TAB_ACTIVE: u32 = 0x2563EB;    // 选中页签底部指示条
    pub const GROUP_BAR: u32 = 0x2563EB;     // 设置页组标题左侧竖条

    // —— 危险 ——
    pub const DANGER: u32 = 0xE44848;
    pub const DANGER_HOVER: u32 = 0xF06E6E;
    pub const DANGER_PRESSED: u32 = 0xBE3232;

    // —— 中性按钮 ——
    pub const NEUTRAL: u32 = 0xCBD5E1;

    // —— 选择与聚焦 ——
    pub const SELECTION: u32 = 0x1677FF;     // 截图选框 / 聚焦描边
    pub const TEXT_SELECTION: u32 = 0x99C9EF; // 文本框选区

    // —— 工具条 ——
    pub const TOOL_SURFACE: u32 = 0xFFFFFF;
    pub const TOOL_HOVER: u32 = 0xEBEBEB;
    pub const TOOL_PRESSED: u32 = 0xD9D9D9;
    pub const TOOL_HOVER_ON: u32 = 0xC7C7C7;
    pub const ICON: u32 = 0x333333;
    pub const ICON_ACTIVE: u32 = 0x2E6BE6;
    pub const ICON_DANGER: u32 = 0xE63946;
    pub const ICON_DISABLED: u32 = 0xAAAAAA;
    pub const TOOL_SEP: u32 = 0xDDDDDD;
    pub const TOOL_BORDER: u32 = 0xA8A8A8; // 副条 / 录制条外框

    // —— 标注手柄 ——
    pub const DRAGGER: u32 = 0x479EFF;     // 选中图元的八柄描边

    // 滑块
    pub const SLIDER_TRACK: u32 = 0xDDDDDD;
    pub const SLIDER_FILL: u32 = 0x3B82F6;

    // —— 录制 / 贴屏 / 预览 ——
    pub const RECORD_ACCENT: u32 = 0x1677FF;
    pub const RECORD_BG: u32 = 0xE6F4FF;
    pub const RECORD_META: u32 = 0xC0C0C0;
    pub const PIN_OUTLINE: u32 = 0x1677FF;
    pub const PIN_FLASH_WHITE: u32 = 0xFFFFFF;
    pub const PIN_FLASH_CYAN: u32 = 0x00E5FF;
    pub const PIN_FLASH_BLUE: u32 = 0x1677FF;
    pub const PIN_FLASH_PURPLE: u32 = 0xB388FF;
    pub const PREVIEW_STROKE: u32 = 0xF00FF0;

    // —— 提示气泡 ——
    pub const TOOLTIP_BG: u32 = 0xFFFFE1;
    pub const TOOLTIP_BORDER: u32 = 0x767676;

    // —— 文件校验结论 ——
    pub const SUCCESS: u32 = 0x15803D;       // 结论一致
    pub const WARNING: u32 = 0xDC2626;       // 结论不一致
    pub const SELECTION_SOFT: u32 = 0xCCE2FF; // 结果框选中底 / 列表选中行
    pub const DIVIDER_DEEP: u32 = 0xD6DEE8;  // 结果框记录分隔线 / 列表内细分隔
    pub const ROW_HOVER: u32 = 0xE2ECFD;

    /// 0xRRGGBB → GDI COLORREF（0x00BBGGRR）。
    pub const fn cref(c: u32) -> u32 {
        ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF)
    }

    /// 0xRRGGBB → (r, g, b)，供自绘像素缓冲使用。
    pub const fn rgb8(c: u32) -> (u8, u8, u8) {
        (((c >> 16) & 0xFF) as u8, ((c >> 8) & 0xFF) as u8, (c & 0xFF) as u8)
    }

    /// 按百分比压暗（keep = 100 保持原色，80 压掉 20%）。
    pub const fn dim(c: u32, keep: u32) -> u32 {
        let r = (((c >> 16) & 0xFF) * keep / 100) << 16;
        let g = (((c >> 8) & 0xFF) * keep / 100) << 8;
        let b = (c & 0xFF) * keep / 100;
        r | g | b
    }
}

/// 需要透明度时与 `color::*` 搭配使用的 alpha（0-255）。
pub mod alpha {
    pub const SCRIM: u8 = 82;          // 截图选区外压暗（0.32 × 255）
    pub const PIN_EDGE: u8 = 77;       // 贴屏隔离线（0.30 × 255）
    pub const PREVIEW_STROKE: u8 = 97; // #F00FF0 38%
    pub const TOAST: u8 = 200;         // 轻提示整体不透明度
    pub const DRAGGER: u8 = 242;       // 手柄蓝 alpha 0.95（ColorF(0.28,0.62,1.0,0.95)）
}

/// 尺寸、间距、线宽、圆角。逻辑像素，绘制时统一过 `dpi::sc`。
pub mod metric {
    // 间距刻度
    pub const SPACE_1: i32 = 2;
    pub const SPACE_3: i32 = 6;
    pub const SPACE_4: i32 = 8;
    pub const SPACE_5: i32 = 12;
    pub const SPACE_6: i32 = 14;

    // 线宽 / 圆角
    pub const STROKE: i32 = 1;
    /// 主界面按钮圆角区（FillRect 已铺满整块，视觉仍是直角，保留原参数）。
    pub const RADIUS_BTN: i32 = 12;

    // 主窗口
    pub const WIN_W: i32 = 585;
    pub const WIN_H: i32 = 445;

    // 主编辑器 / 副编辑器网格
    pub const CELL: i32 = 32;
    pub const PAD: i32 = 2;
    pub const BTN: i32 = 28;
    pub const SEP: i32 = 1;
    pub const BORDER: i32 = 1;
    pub const SLIDER_W: i32 = 80;
    pub const SLIDER_PAD: i32 = 3;
    pub const ARROW_ZONE: i32 = 2;
    pub const ARROW_HALF: i32 = 3;
    pub const BTN_N: i32 = 14;
    pub const TB_H: i32 = CELL + BORDER * 2;                     // 34
    pub const SUB_H: i32 = ARROW_ZONE + BORDER * 2 + CELL;       // 36
    pub const TB_W: i32 = BTN_N * CELL + 2 * SEP;                // 450

    // 选区
    pub const SELECT_STROKE: i32 = 2;

    // 贴屏
    pub const PIN_PAD: i32 = 1;             // 贴屏四周隔离带

    // 放大镜
    pub const PIX_SRC_W: i32 = 50;
    pub const PIX_SRC_H: i32 = 30;
    pub const PIX_W: i32 = 188;
    pub const PIX_IMG_H: i32 = 113;
    // 卡高 = 图片高 + 信息条高，信息条按 DPI 放大：113 + 66×5 = 443 才够 Windows
    // 最大 500% 缩放；仍留 300 的话 288 DPI 起卡片就比常驻 DIB 高，绘制会越界
    pub const PIX_CARD_H_MAX: i32 = 443;

    // 文本图元
    pub const TXT_PAD: i32 = 6;
    pub const TX_HANDLE_HIT: i32 = 7; // 编辑中命中 = hr*0.8 + 2*dpi
    pub const TX_HIT_PAD: i32 = 6;    // 非编辑命中 = hr + 6*dpi
    pub const TX_ROT_R: i32 = 6;      // 柄半径 = 6*dpi（ShapeText.cpp handleRadius）
    pub const TX_ROT_GAP: f32 = 15.6; // 柄挂在框顶外 2.6*hr

    // 图标超采样
    pub const ICON_SS: i32 = 4;
}

/// 字体族、字号、字重。
pub mod font {
    pub const UI: &str = "Microsoft YaHei";
    pub const UI_ALT: &str = "Microsoft YaHei UI";

    pub const SIZE_10: i32 = 10;
    pub const SIZE_11: i32 = 11;
    pub const SIZE_12: i32 = 12;
    pub const SIZE_13: i32 = 13;
    pub const SIZE_15: i32 = 15;
    pub const SIZE_18: i32 = 18;
    pub const SIZE_19: i32 = 19;
    pub const SIZE_22: i32 = 22;

    pub const W_REGULAR: i32 = 400;
    pub const W_MEDIUM: i32 = 500;
    pub const W_SEMIBOLD: i32 = 600;
    pub const W_BOLD: i32 = 700;

    pub const MONO: &str = "Consolas";
}

/// DPI 换算。全工程只此一处。
pub mod dpi {
    /// 逻辑像素 → 物理像素（整数截断）。
    pub fn sc(v: i32) -> i32 {
        (v * crate::dpi()) / 96
    }

    /// 逻辑像素 → 物理像素（浮点，供自绘几何用）。
    pub fn scale_f(v: f32) -> f32 {
        v * crate::dpi() as f32 / 96.0
    }

    /// 物理像素 → 逻辑像素（`sc` 的逆运算，回写配置/滑块时用）。
    pub fn unsc(v: i32) -> i32 {
        (v as f32 * 96.0 / crate::dpi() as f32).round() as i32
    }

    /// 吸附到整物理像素后回逻辑像素。
    pub fn snap_phys(v: f32) -> f32 {
        let d = crate::dpi() as f32;
        let phys = (v * d).round();
        if phys < 1.0 { v } else { phys / d }
    }

    /// 逻辑字号 → 物理像素恰为整数的逻辑字号。
    pub fn font_px(logical: f32) -> f32 {
        snap_phys(logical)
    }
}

/// 页面布局坐标。每个页面一组，业务代码不再出现裸数字。
pub mod layout {
    /// 主窗口公共部分（页签、顶部输入框、轻提示、气泡）。
    pub mod main_page {
        // 页签
        pub const TAB_Y: i32 = 2;
        pub const TAB_H: i32 = 30;
        pub const TAB_W_INIT: i32 = 90;   // 首次创建时的占位宽度（随后由 place_tabs 实测改写）
        pub const TAB_PAD_X: i32 = 30;    // 文字两侧内边距
        pub const TAB_GAP: i32 = 7;
        pub const TAB_UNDERLINE: i32 = 4; // 选中页签底部指示条高度

        // 顶部大输入框
        pub const EDIT_Y: i32 = 42;
        pub const EDIT_W: i32 = 557;
        pub const EDIT_H: i32 = 101;
        pub const GAP_BELOW_EDIT: i32 = 10; // 输入框下沿到内容区

        // 轻提示
        pub const TOAST_PAD: i32 = 22;
        pub const TOAST_MIN_W: i32 = 140;
        pub const TOAST_H: i32 = 44;
        pub const TOAST_BOTTOM_GAP: i32 = 72;
        pub const TOAST_MAX_PCT: i32 = 70;  // 最宽占工作区宽度的百分比

        // 光标气泡
        pub const TIP_INSET: i32 = 4;
        pub const TIP_ANCHOR_DY: i32 = 2; // 滑块框上沿偏移（物理像素）
        pub const TIP_PAD_X: i32 = 5;
        pub const TIP_PAD_Y: i32 = 3;
        pub const TIP_RAD: f32 = 2.0;
        pub const TIP_STROKE: f32 = 1.0;
    }

    /// 二维码页。
    pub mod qr {
        pub const BTN_X: i32 = 14;
        pub const BTN_Y0: i32 = 153;
        pub const BTN_W: i32 = 132;
        pub const BTN_H: i32 = 46;
        pub const BTN_PITCH: i32 = 58;
        pub const IMG_MIN: i32 = 24;      // 二维码最小边长
        pub const CARD_X: i32 = 293;
        pub const CARD_Y: i32 = 153;
        pub const CARD_SIDE: i32 = 278;
    }

    /// 文字识别页。
    pub mod ocr {
        pub const SEARCH_X: i32 = 14;
        pub const SEARCH_Y: i32 = 42;
        pub const SEARCH_W: i32 = 331;
        pub const FIND_X: i32 = 357;
        pub const FIND_W: i32 = 100;
        pub const LAYOUT_X: i32 = 469;
        pub const LAYOUT_W: i32 = 102;
        pub const ROW_H: i32 = 28;
        pub const EDIT_Y: i32 = 80;
        pub const EDIT_W: i32 = 557;
        pub const EDIT_H: i32 = 295;
        pub const BTN_Y: i32 = 385;
        pub const BTN_W: i32 = 132;
        pub const BTN_H: i32 = 46;
        pub const BTN_X: [i32; 3] = [14, 158, 302];
        pub const COUNT_X: i32 = 446;
        pub const COUNT_W: i32 = 125;
    }

    /// 设置页：页底一张白卡片，三组「竖条 + 标题 + 分隔线」。
    pub mod settings {
        pub const CARD_X: i32 = 42;
        pub const CARD_Y: i32 = 40;
        pub const CARD_W: i32 = 500;
        pub const CARD_H: i32 = 382;
        pub const PAD: i32 = 16;
        // 三条贯穿全页的竖线：标签 / 值 / 动作
        pub const LBL_X: i32 = CARD_X + PAD;        // 58
        pub const LBL_W: i32 = 132;
        pub const VAL_X: i32 = LBL_X + LBL_W + 4;   // 194
        pub const VAL_W: i32 = 246;
        pub const ACT_X: i32 = VAL_X + VAL_W + 8;   // 448
        pub const ACT_W: i32 = 78;
        pub const ROW_H: i32 = 28;
        // 常规组勾选：2 行 3 列，块宽 148、列距 160、行距 40
        pub const CHK_W: i32 = 148;
        pub const CHK_H: i32 = 24;
        pub const CHK_PITCH_X: i32 = 160;
        pub const CHK_PITCH: i32 = 40;
        // 组标题：高 24，标题→线 4，线→首行 8
        pub const TITLE_H: i32 = 24;
        pub const GAP_TITLE: i32 = 4;
        pub const GAP_LINE: i32 = 8;
        pub const G1_Y: i32 = 56;
        pub const G2_Y: i32 = 200;
        pub const G3_Y: i32 = 306;
        pub const G1_ROW_Y: i32 = G1_Y + TITLE_H + GAP_TITLE + GAP_LINE; // 92
        pub const G1_PITCH: i32 = 34;
        pub const G2_ROW_Y: i32 = G2_Y + TITLE_H + GAP_TITLE + GAP_LINE; // 236
        pub const G2_PITCH: i32 = 36;
        pub const G3_ROW_Y: i32 = G3_Y + TITLE_H + GAP_TITLE + GAP_LINE; // 342
        // 组标题左侧主蓝竖条 / 标题文字缩进 / 分隔线高
        pub const BAR_W: i32 = 3;
        pub const BAR_H: i32 = 14;
        pub const TITLE_DX: i32 = 10;
        pub const LINE_H: i32 = 1;
        // 自绘勾选框：14×14，离行左 1，勾后文字留 6
        pub const CHK_BOX: i32 = 14;
        pub const CHK_BOX_INSET: i32 = 1;
        pub const CHK_TEXT_GAP: i32 = 6;
        // 路径框内左右留白
        pub const EDIT_PAD_X: i32 = 6;
    }

    pub mod shutdown {
        pub const SD_MARGIN: i32 = 14;      // 页面左右边距
        pub const SD_RIGHT: i32 = 571;      // 内容区右边界
        pub const SD_Y_TITLE: i32 = 56;
        pub const SD_TITLE_H: i32 = 56;
        pub const SD_Y_AT: i32 = 140;       // 指定时刻行
        pub const SD_Y_ED: i32 = 212;       // 倒计时行
        pub const SD_ED_W: i32 = 72;        // 格子宽
        pub const SD_ED_H: i32 = 44;        // 格子高
        pub const SD_ED_PAD: i32 = 5;       // EDIT 离格子左侧缩进
        pub const SD_ED_AIR: i32 = 4;       // EDIT 框高 = 实测行高 + 4
        pub const SD_UNIT_W: i32 = 24;      // 单位字宽度
        pub const SD_UNIT_GAP: i32 = 4;     // 单位字离格子右边框
        pub const SD_UNIT_BOTTOM: i32 = 3;  // 单位字离格子底边框
        pub const SD_EDIT_W: i32 = SD_ED_W - SD_UNIT_W - SD_UNIT_GAP - SD_ED_PAD; // 39
        pub const SD_MARK_X: i32 = 14;      // 生效行行首竖标记
        pub const SD_MARK_W: i32 = 2;
        pub const SD_LBL_X: i32 = 20;       // 行首标签列
        pub const SD_LBL_W: i32 = 126;      // 右边界 146，与第一格 154 留 8
        pub const SD_GX: [i32; 3] = [154, 274, 394];
        pub const SD_CHK_X: i32 = 514;      // 「每天」
        pub const SD_CHK_W: i32 = 52;
        pub const SD_Y_BTN: i32 = 300;
        pub const SD_BTN_W: i32 = 178;
        pub const SD_BTN_H: i32 = 50;
        pub const SD_BTN_X: [i32; 3] = [14, 204, 394];
        pub const SD_Y_BAR: i32 = 374;
        pub const SD_BAR_H: i32 = 40;
        pub const SD_ARROW_W: i32 = 40;
        pub const SD_ARROW_H: i32 = 21;
        pub const SD_ARROW_DY: i32 = 23;
        pub const SD_TIME_GAP: i32 = 12;    // 状态词与剩余时间之间
    }

    /// 文件校验页。
    pub mod vf {
        pub const LIST_X: i32 = 14;
        pub const LIST_Y: i32 = 44;
        pub const LIST_W: i32 = 557;
        pub const LIST_H: i32 = 271;
        pub const BTN_Y: i32 = 321;
        pub const BTN_H: i32 = 30;
        pub const BTN_X: [i32; 5] = [14, 122, 200, 278, 374];
        pub const BTN_W: [i32; 5] = [100, 70, 70, 88, 74];
        pub const CHK_X: i32 = 470;
        pub const CHK_Y: i32 = 321;
        pub const CHK_W: i32 = 101;
        pub const CHK_H: i32 = 20;
        pub const CHK_PITCH: i32 = 25;   // 四项居中撑满 321..421：行高 20 + 行距 5
        pub const ROW_FILE: i32 = 359;   // 条子顶边 = 行号 + 3
        pub const ROW_TOTAL: i32 = 394;
        pub const BAR_TOP_OFFSET: i32 = 3;
        pub const BAR_X: i32 = 14;
        pub const BAR_W: i32 = 434;
        pub const BAR_H: i32 = 24;

        // 结果框内部
        pub const TEXT_LEFT: i32 = 10;
        pub const TEXT_TOP: i32 = 8;
        pub const TEXT_BOTTOM: i32 = 8;
        pub const CONTENT_PAD: i32 = 6;
        pub const VIEW_PAD: i32 = 12;
        pub const FRAME: i32 = 5;
        pub const SEP_RIGHT_INSET: i32 = 4;
    }

    /// 截图选区 / 尺寸标签 / 放大镜。
    pub mod cap {
        // 尺寸标签
        pub const SIZE_LABEL_PAD: i32 = 3;   // 标签四周内边距

        // 放大镜卡片
        pub const PIX_INFO_H: i32 = 66;      // 图下方信息条高度
        pub const PIX_GAP: i32 = 10;         // 卡片与光标的间距
        pub const PIX_CROSS_HALF: i32 = 4;   // 十字辅助线半宽
        pub const PIX_INFO_PAD: i32 = 5;     // 信息条内边距
        pub const PIX_TEXT_DY: i32 = 2;      // GDI 字形顶相对 DWrite 行盒顶的回退
        pub const PIX_BG_KEEP_ON: u32 = 44;  // 卡片压在选区/高亮内：原图保留 44%
        pub const PIX_BG_KEEP_OFF: u32 = 30; // 卡片压在压暗区：原图保留 30%
        pub const PIX_EDGE_KEEP: u32 = 44;   // 1px 边框与底色同口径
    }

    /// 标注工具条 / 副条 / 手柄 / 文本编辑框。
    pub mod anno {
        // 图标
        pub const ICON_BOX_RATIO: f32 = 0.58; // 图标盒边长 = cell × 0.58

        // 副条
        pub const SUB_BTN_Y: i32 = 5;         // 副条按钮上沿（箭头区 + 描边 + 居中余量）
        // 副条/录制条顶部"指向按钮"的小三角
        pub const SUB_ARROW_ZONE: i32 = crate::ui::metric::ARROW_ZONE; // 2：三角高
        pub const SUB_ARROW_HALF: i32 = crate::ui::metric::ARROW_HALF; // 3：三角半宽
        pub const SUB_TIP_Y: f32 = 5.0;       // 副条按钮气泡的锚点上沿
        pub const MAIN_TIP_Y: f32 = 3.0;      // 主条按钮气泡的锚点上沿
        pub const TB_FALLBACK_INSET: i32 = 3; // 主条上下都放不下时压进选区内的偏移

        // 副条滑块
        pub const SLIDER_CY: f32 = 17.0;      // 内容区 34 行的中心
        pub const SLIDER_DOT_R: f32 = 3.0;    // 轨道端点半径 / 圆点半径
        pub const SLIDER_TRACK_HALF: f32 = 0.5; // 轨道半厚

        // 色板
        pub const SWATCH: f32 = 13.0;         // 色块边长
        pub const SWATCH_RAD: f32 = 2.0;      // 色块圆角
        pub const SWATCH_X: f32 = 10.05;      // 色块相对格子的 x 偏移
        pub const SWATCH_Y: f32 = 8.05;       // 色块相对格子的 y 偏移
        pub const SWATCH_BORDER: f32 = 1.0;   // 白块描边宽度
        pub const SWATCH_CHECK: i32 = 8;      // 对勾图标边长
        pub const SWATCH_CHECK_OFF: i32 = 2;  // 对勾在色块内的偏移

        // 图元手柄
        pub const DRAGGER_HALF: i32 = 3;      // 八柄半边长（序号按 dpi 放大）
        pub const NUM_MIN_R: i32 = 8;         // 序号拖半径下限 8*dpi

        // 文本编辑框
        pub const TX_FRAME_HOVER: f32 = 12.0; // 框外悬停环带
        pub const TXT_DASH: f32 = 2.0;        // 虚线：实线段长
        pub const TXT_DASH_PERIOD: f32 = 4.0; // 虚线：周期（2 实 2 虚）
        pub const TX_HND_DIA: f32 = 10.0;     // 四角柄圆底直径
        pub const TX_HND_CLOSE: f32 = 0.18;   // × 的留边（相对直径）
        pub const TX_HND_SLOPE: f32 = 0.26;   // 斜杆半长（相对直径）
        pub const TX_HND_ARROW: f32 = 0.22;   // 箭头长度（相对直径）
        pub const TX_ROT_HOT_K: f32 = 1.05;   // 弧半径 = hr*1.05
        pub const TX_ROT_HOT_HS: f32 = 0.62;  // 箭头长 = hr*0.62
        pub const TX_ROT_DOT_K: f32 = 0.85;   // 旋转柄静息白点半径（相对柄半径）
        // 线宽换算：lw 都先取 max(1, dpi) 再乘系数
        pub const TX_HND_LW_K: f32 = 1.1;     // 四角柄里图形 = lw*1.1
        pub const TX_ROT_LW_K: f32 = 1.3;     // 旋转蓝弧 = lw*1.3
        pub const TX_ROT_DOT_LW_K: f32 = 1.4; // 静息蓝环 = lw*1.4
        pub const TX_ROT_ARC_GAP_DEG: f32 = 30.0; // 两段弧中间的缺口（箭头插在缺口正中）
    }

    /// 录制条与计时区。
    pub mod record {
        // 与副条同高：顶部 2px 是透明箭头区，可见框仍是 34
        pub const H: i32 = crate::ui::metric::SUB_H;             // 条高 36
        pub const TIM_W: i32 = 112;                          // 计时 Label 宽
        pub const SET_W: i32 = crate::ui::metric::CELL * 5
            + crate::ui::metric::SEP * 2;                        // 162
        pub const SET_SEP: [i32; 2] = [
            crate::ui::metric::CELL * 2,
            crate::ui::metric::CELL * 4 + crate::ui::metric::SEP,
        ];                                                   // 64、129
        pub const RUN_W: i32 = TIM_W + crate::ui::metric::SEP
            + crate::ui::metric::CELL * 3;                       // 209
        pub const BTN_Y: i32 = crate::ui::layout::anno::SUB_BTN_Y; // 按钮上沿（跳过箭头区）
    }

    /// 贴屏倍数提示。
    pub mod pin {
        pub const TIP_PAD: i32 = 3;       // 提示内边距
        pub const TIP_MARGIN: i32 = 5;    // 提示外边距
        pub const TIP_KEEP: u32 = 54;     // 提示底：底图保留 54%
    }

    /// 动态列（「改为」框、查找/替换、前后缀、序号三格、删除文本）的 x 由
    /// `modules::rename::relayout` 按实测字宽现算，不在这里登记。
    pub mod rename {
        // 页内标题与面板
        pub const TITLE_Y: i32 = 73;
        pub const TITLE_H: i32 = 18;
        pub const PANEL_Y: i32 = 82;
        pub const PANEL_H: i32 = 80;
        pub const PANEL_W: i32 = 436;
        pub const PANEL_IN_R: i32 = 440;
        pub const ROW1_Y: i32 = 96;
        pub const ROW2_Y: i32 = 126;

        // 「转换」面板
        pub const RULE1_X: i32 = 24;
        pub const RULE1_W: i32 = 100;
        // 是 `RULE1_X + RULE1_W + kSpaceLg(10)` = 134
        pub const RULE2_X: i32 = 134;
        pub const RULE2_W: i32 = 100;
        pub const CONV_LBL_X: i32 = 240;

        // 「扩展名」面板
        pub const EXT_UPPER_X: i32 = 24;
        pub const EXT_UPPER_W: i32 = 98;
        pub const EXT_LOWER_X: i32 = 24;
        pub const EXT_LOWER_W: i32 = 98;
        pub const EXT_KEEP_X: i32 = 132;
        pub const EXT_KEEP_W: i32 = 56;
        pub const EXT_DROP_X: i32 = 132;
        pub const EXT_DROP_W: i32 = 56;
        pub const EXT_LBL_X: i32 = 192;
        pub const EXT_SET_W: i32 = 140;

        // 「替换」面板
        pub const REP_CASE_X: i32 = 24;
        pub const REP_CASE_W: i32 = 112;
        // 同上：24 + 112 + 10 = 146
        pub const REP_ALL_X: i32 = 146;
        pub const REP_ALL_W: i32 = 94;
        pub const FIND_LBL_X: i32 = 24;

        // 「序号 / 前后缀」面板
        pub const SEQ_LBL_X: i32 = 24;
        pub const SEQ_ON_W: i32 = 56;
        pub const AFFIX_ON_X: i32 = 24;
        pub const AFFIX_ON_W: i32 = 56;

        // 「删除」面板
        pub const DEL_UPPER_X: i32 = 24;
        pub const DEL_UPPER_W: i32 = 98;
        pub const DEL_LOWER_X: i32 = 24;
        pub const DEL_LOWER_W: i32 = 98;
        pub const DEL_SPACE_X: i32 = 132;
        pub const DEL_SPACE_W: i32 = 56;
        pub const DEL_DIGIT_X: i32 = 132;
        pub const DEL_DIGIT_W: i32 = 56;
        pub const DEL_CJK_X: i32 = 198;
        pub const DEL_CJK_W: i32 = 56;
        pub const DEL_SPEC_X: i32 = 264;
        pub const DEL_SPEC_W: i32 = 94;
        pub const DEL_LBL_X: i32 = 198;

        // 右上「过滤」小组
        pub const G3_X: i32 = 460;
        pub const G3_Y: i32 = 117;
        pub const G3_W: i32 = 111;
        pub const G3_H: i32 = 45;
        pub const G3_ROW_Y: i32 = 126;
        pub const FILTER_X: i32 = 470;
        pub const FILTER_W: i32 = 91;

        // 文件列表
        pub const LIST_X: i32 = 14;
        pub const LIST_Y: i32 = 170;
        pub const LIST_W: i32 = 557;
        pub const LIST_H: i32 = 246;

        // 页底「全选 / 反选 / 格式勾选 / 计数」
        pub const BOT_Y: i32 = 414;
        pub const BOT_H: i32 = 24;
        pub const BOT_BTN_W: i32 = 44;
        pub const BOT_BTN2_X: i32 = 66;
        pub const FMT_X: i32 = 122;
        pub const FMT_PITCH: i32 = 78;
        pub const FMT_W: i32 = 68;
        pub const FMT_END: i32 = 508;

        // 五个压线面板标签：(x, w)
        pub const TABS: [(i32, i32); 5] = [(26, 44), (70, 44), (114, 33), (147, 33), (180, 46)];

        // 页底内容下沿
        pub const PAGE_BOT: i32 = 445;
    }
}

// ───────────────────────── 页面级扁平 token ─────────────────────────
// 「文件改名」页按 `UITheme.h` 的键名取色/取度量，这里平铺到 `ui::` 根上
// 页面模块写 `ui::C_TEXT` / `ui::CHECK_BOX` 即可。值全部指回本文件的唯一来源；
// 只有列表的两档底色是 独有、此前本文件没有的（`ROW_HOVER` / `SELECTION`）
pub const C_TEXT: u32 = color::TEXT;
pub const C_TEXT_MUTED: u32 = color::TEXT_TERTIARY;
pub const C_BORDER: u32 = color::BORDER;
pub const C_SURFACE: u32 = color::SURFACE;
pub const C_SURFACE_PAGE: u32 = color::PAGE;
pub const C_ACCENT_DEEP: u32 = color::ACCENT_DEEP;
pub const C_SUCCESS: u32 = color::SUCCESS;
pub const C_DANGER: u32 = color::DANGER;
pub const C_ROW_HOVER: u32 = color::ROW_HOVER;
pub const C_TEXT_SEC: u32 = color::TEXT_SECONDARY;
pub const C_WHITE: u32 = color::SURFACE;
pub const C_SURFACE_SUBTLE: u32 = color::SURFACE_ALT;
pub const C_PRIMARY: u32 = color::ACCENT;
pub const C_BTN_BLUE: u32 = color::ACCENT_DEEP;
pub const C_BTN_GR: u32 = color::NEUTRAL;
pub const C_BG_TOP: u32 = color::PAGE;
pub const C_TRACK: u32 = color::TRACK;

/// 0xRRGGBB → GDI COLORREF（`color::cref` 的页面级别名）。
pub fn rgb(c: u32) -> u32 {
    color::cref(c)
}

pub const FONT_BODY: i32 = 18;
pub const FONT_LABEL_LARGE: i32 = 19;
pub const HEADER_H: i32 = 26;
pub const ROW_H: i32 = 22;
pub const CHECK_BOX: i32 = 14;
pub const CHECK_INSET: i32 = 1;
pub const CHECK_GAP: i32 = 6;
pub const LINE_THICK: i32 = 2;
pub const WINDOW_W: i32 = metric::WIN_W;
pub const PAGE_MARGIN: i32 = 14;
pub const TOOLBAR_TOP: i32 = 40;
pub const CONTROL_HEIGHT: i32 = 26;
pub const SPACE_MD: i32 = 8;
pub const SPACE_LG: i32 = 10;   // theme::kSpaceLg
pub const TAB_BAR_BOTTOM: i32 = 32;
pub const HOVER_DIM_PCT: u32 = 8;
pub const PRESS_DIM_PCT: u32 = 20;
pub const HOVER_DIM_SOFT: u32 = 4;    // theme::kHoverDimSoft（浅底只降一档）
pub const CHECK_TICK_P1X: f32 = 0.30;
pub const CHECK_TICK_P1Y: f32 = 0.52;
pub const CHECK_TICK_P2X: f32 = 0.44;
pub const CHECK_TICK_P2Y: f32 = 0.74;
pub const CHECK_TICK_P3X: f32 = 0.76;
pub const CHECK_TICK_P3Y: f32 = 0.28;

/// 逻辑像素 → 物理像素（`dpi::sc` 的页面级别名）。
pub use dpi::sc;

/// 按百分比压暗（`color::dim` 的页面级别名）。
pub use color::dim;

/// 界面字体：逻辑字号 + 字重 → HFONT（归 `crate::ui_font` 缓存所有，调用方不要 DeleteObject）。
pub fn font(size: i32, weight: u32) -> usize {
    crate::ui_font(dpi::sc(size), weight as i32)
}
