use crate::prelude::*;
use crate::*;

// ================= 图标：SVG 路径 =================
//
// 图标是设计稿的矢量路径（Res/Icons.generated.h
// 由 tools/icons/gen_icons.py 生成），运行期按 path 画几何、不经过字体光栅化：
// 字体字形受 hinting / ClearType / 小数 ppem 影响，19px 下笔画边缘不可控，
// 透明表面上还会带彩边；路径几何则完全由我们决定画多大、落不落在整像素上。
//
// 运行期自己解析同一份路径数据并光栅化（纯 Rust，不引第三方库）：
//   · 解析  M/L/H/V/C/S/Q/T/A/Z（含相对命令、一条命令多组参数、紧凑数字 -.5、001 这种压缩标志位）
//   · 填充  nonzero / evenodd 扫描线，4×4 超采样出覆盖率（灰度 AA，没有彩边）
//   · 描边  线段做成四边形 + 圆拐角 / 圆端头。这套图标里 stroke>0 的 op 全部声明
//           join=round，所以"每个顶点补一个半径 half 的圆"就是精确解，不是近似

#[derive(Clone, Copy)]
struct SvgOp {
    d: &'static str,
    stroke: f32,     // 0 = 填充；>0 = 描边宽度（与 vb 同一坐标系）
    even_odd: bool,  // 填充规则：奇偶 = true，非零 = false
    cap: u8,
    join: u8,
}

struct SvgIcon {
    name: &'static str,
    vb: f32,   // 设计视窗边长（24 或 1024）
    ops: &'static [SvgOp],
}

const CAP_BUTT: u8 = 0;
const CAP_ROUND: u8 = 1;
const CAP_SQUARE: u8 = 2;
const JOIN_MITER: u8 = 0;
const JOIN_ROUND: u8 = 1;

// ---- 图标路径表：与 SnapCore/Src/Res/Icons.generated.h 逐条同源 ----
// 每个 op = 一条 SVG path + 描边宽度（0 = 填充）+ 填充规则 + 端头/拐角。
// vb 是设计视窗边长（24 或 1024），绘制时按 side/vb 缩放。
static SIO_0: [SvgOp; 1] = [
    SvgOp { d: "m4.5 4.5 15 15m0-15-15 15", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_1: [SvgOp; 2] = [
    SvgOp { d: "M11.5 9.5H19.5A2 2 0 0 1 21.5 11.5V19.5A2 2 0 0 1 19.5 21.5H11.5A2 2 0 0 1 9.5 19.5V11.5A2 2 0 0 1 11.5 9.5Z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M5.5 15.5h-1a2 2 0 0 1-2-2v-9a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_2: [SvgOp; 2] = [
    SvgOp { d: "m16.1 2.42 5.48 5.48-5.48 5.47-.91 6.38L4.25 8.81l6.38-.91Z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "m9.72 14.28-7.3 7.3", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_3: [SvgOp; 2] = [
    SvgOp { d: "M21 12a9 9 0 1 1-3-6.7L21 8", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M21 3v5h-5", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_4: [SvgOp; 3] = [
    SvgOp { d: "M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M17 21v-8H7v8", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M7 3v5h8", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_5: [SvgOp; 2] = [
    SvgOp { d: "M3 12a9 9 0 1 0 3-6.7L3 8", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M3 3v5h5", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_6: [SvgOp; 2] = [
    SvgOp { d: "m2.69 13.12 8.19 8.19.15.13.18.12.18.09.2.07.2.04.21.01.21-.01.2-.04.2-.07.18-.09.18-.12.15-.13 8.19-8.19.13-.15.12-.18.09-.18.07-.2.04-.2.01-.21-.01-.21-.04-.2-.07-.2-.09-.18-.12-.18-.13-.15-8.19-8.19-.15-.13-.18-.12-.18-.09-.2-.07-.2-.04-.21-.01-.21.01-.2.04-.2.07-.18.09-.18.12-.15.13-8.19 8.19-.12.14-.11.16-.09.17-.07.18-.04.18-.03.19v.2l.03.19.04.18.07.18.09.17.11.16z", stroke: 1.46f32, even_odd: false, cap: CAP_BUTT, join: JOIN_ROUND },
    SvgOp { d: "m5.3 12 1.67 1.68L8.65 12l-1.68-1.68Zm3.35 3.35 1.67 1.68L12 15.35l-1.68-1.67Zm0-3.35 1.67 1.68L12 12l-1.68-1.68ZM12 15.35l1.68 1.68 1.67-1.68-1.67-1.67Zm-3.35-6.7 1.67 1.67L12 8.65l-1.68-1.68ZM12 12l1.68 1.68L15.35 12l-1.67-1.68Zm0-3.35 1.68 1.67 1.67-1.67-1.67-1.68ZM15.35 12l1.68 1.68L18.7 12l-1.67-1.68Z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_7: [SvgOp; 1] = [
    SvgOp { d: "m2.17 13.64 8.19 8.19.21.18.23.16.25.13.26.1.27.07.28.03h.28l.28-.03.27-.07.26-.1.25-.13.23-.16.21-.18 8.19-8.19.18-.21.16-.23.13-.25.1-.26.07-.27.03-.28v-.28l-.03-.28-.07-.27-.1-.26-.13-.25-.16-.23-.18-.21-8.19-8.19-.21-.18-.23-.16-.25-.13-.26-.1-.27-.07-.28-.03h-.28l-.28.03-.27.07-.26.1-.25.13-.23.16-.21.18-8.19 8.19-.18.21-.16.23-.13.25-.1.26-.07.27-.03.28v.28l.03.28.07.27.1.26.13.25.16.23zM5.3 12l1.67 1.68L8.65 12l-1.68-1.68Zm3.35 3.35 1.67 1.68L12 15.35l-1.68-1.67Zm0-3.35 1.67 1.68L12 12l-1.68-1.68ZM12 15.35l1.68 1.68 1.67-1.68-1.67-1.67Zm-3.35-6.7 1.67 1.67L12 8.65l-1.68-1.68ZM12 12l1.68 1.68L15.35 12l-1.67-1.68Zm0-3.35 1.68 1.67 1.67-1.67-1.67-1.68ZM15.35 12l1.68 1.68L18.7 12l-1.67-1.68Z", stroke: 0.0f32, even_odd: true, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_8: [SvgOp; 3] = [
    SvgOp { d: "m7.23 20.59-4.1-4.1a2.3 2.3 0 0 1 0-3.24l9.16-9.16a2.3 2.3 0 0 1 3.24 0l5.34 5.34a2.3 2.3 0 0 1 0 3.24l-7.92 7.92z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M21.54 20.59H7.23", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "m5.32 11.05 8.59 8.59", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_9: [SvgOp; 4] = [
    SvgOp { d: "m7.23 20.59-4.1-4.1a2.3 2.3 0 0 1 0-3.24l9.16-9.16a2.3 2.3 0 0 1 3.24 0l5.34 5.34a2.3 2.3 0 0 1 0 3.24l-7.92 7.92z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M21.54 20.59H7.23", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "m5.32 11.05 8.59 8.59", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "m5.32 11.05 6.97-6.97a2.3 2.3 0 0 1 3.24 0l5.34 5.34a2.3 2.3 0 0 1 0 3.24l-6.96 6.97z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_10: [SvgOp; 9] = [
    SvgOp { d: "M192 104.32H832A87.68 87.68 0 0 1 919.68 192V832A87.68 87.68 0 0 1 832 919.68H192A87.68 87.68 0 0 1 104.32 832V192A87.68 87.68 0 0 1 192 104.32Z", stroke: 80.64f32, even_odd: false, cap: CAP_BUTT, join: JOIN_ROUND },
    SvgOp { d: "M250 250h131v131H250z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
    SvgOp { d: "M512 250h131v131H512z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
    SvgOp { d: "M381 381h131v131H381z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
    SvgOp { d: "M643 381h131v131H643z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
    SvgOp { d: "M250 512h131v131H250z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
    SvgOp { d: "M512 512h131v131H512z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
    SvgOp { d: "M381 643h131v131H381z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
    SvgOp { d: "M643 643h131v131H643z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_11: [SvgOp; 1] = [
    SvgOp { d: "M192 64h640a128 128 0 0 1 128 128v640a128 128 0 0 1-128 128H192A128 128 0 0 1 64 832V192A128 128 0 0 1 192 64m58 186h131v131H250Zm262 0h131v131H512ZM381 381h131v131H381Zm262 0h131v131H643ZM250 512h131v131H250Zm262 0h131v131H512ZM381 643h131v131H381Zm262 0h131v131H643Z", stroke: 0.0f32, even_odd: true, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_12: [SvgOp; 1] = [
    SvgOp { d: "M22.5 1.5L10.74 5.22L14.36 6.62L1.5 22.5L17.38 9.64L18.78 13.26ZM20.66 3.34L14.36 5.33L16.33 6.09L9.62 14.38L17.91 7.67L18.67 9.64Z", stroke: 0.0f32, even_odd: true, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_13: [SvgOp; 1] = [
    SvgOp { d: "M22.5 1.5L10.74 5.22L14.36 6.62L1.5 22.5L17.38 9.64L18.78 13.26Z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_14: [SvgOp; 1] = [
    SvgOp { d: "M3 12A9 7 0 1 0 21 12A9 7 0 1 0 3 12Z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_15: [SvgOp; 1] = [
    SvgOp { d: "M2 11.9L2 12L2 12.1L2.1 13L2.1 13.1L2.3 14L2.4 14L2.4 14.1L2.7 14.9L2.8 15L2.8 15.1L3.3 15.9L3.4 16L4 16.7L4.1 16.8L4.8 17.5L4.9 17.6L5.7 18.2L5.8 18.2L6.7 18.8L6.8 18.8L7.8 19.2L7.9 19.3L8.9 19.6L9 19.6L10.1 19.8L10.2 19.8L11.3 19.9L11.4 20L12.6 20L12.7 19.9L13.8 19.8L13.9 19.8L15 19.6L15.1 19.6L16.1 19.3L16.2 19.2L17.2 18.8L17.3 18.8L18.2 18.2L18.3 18.2L19.1 17.6L19.2 17.5L19.9 16.8L20 16.7L20.6 16L20.7 15.9L21.2 15.1L21.2 15L21.3 14.9L21.6 14.1L21.6 14L21.7 14L21.9 13.1L21.9 13L22 12.1L22 12L22 11.9L21.9 11L21.9 10.9L21.7 10L21.6 10L21.6 9.9L21.3 9.1L21.2 9L21.2 8.9L20.7 8.1L20.6 8L20 7.3L19.9 7.2L19.2 6.5L19.1 6.4L18.3 5.8L18.2 5.8L17.3 5.2L17.2 5.2L16.2 4.8L16.1 4.7L15.1 4.4L15 4.4L13.9 4.2L13.8 4.2L12.7 4.1L12.6 4L11.4 4L11.3 4.1L10.2 4.2L10.1 4.2L9 4.4L8.9 4.4L7.9 4.7L7.8 4.8L6.8 5.2L6.7 5.2L5.8 5.8L5.7 5.8L4.9 6.4L4.8 6.5L4.1 7.2L4 7.3L3.4 8L3.3 8.1L2.8 8.9L2.8 9L2.7 9.1L2.4 9.9L2.4 10L2.3 10L2.1 10.9L2.1 11Z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_16: [SvgOp; 2] = [
    SvgOp { d: "M3 12A9 9 0 1 0 21 12A9 9 0 1 0 3 12Z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M9.3 8.3 12.5 6v12", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_17: [SvgOp; 1] = [
    SvgOp { d: "M2 12.1L2 13.2L2 13.3L2.3 14.4L2.3 14.6L2.6 15.6L2.7 15.8L3.2 16.8L3.2 16.9L3.8 17.9L3.9 18L4.6 18.8L4.7 18.9L5.5 19.7L5.6 19.8L6.6 20.4L6.7 20.5L7.7 21.1L7.8 21.1L8.8 21.5L9 21.6L10.1 21.9L10.2 21.9L11.3 22L11.4 22L12.6 22L12.7 22L13.8 21.9L13.9 21.9L15 21.6L15.2 21.5L16.2 21.1L16.3 21.1L17.3 20.5L17.4 20.4L18.4 19.8L18.5 19.7L19.3 18.9L19.4 18.8L20.1 18L20.2 17.8L20.8 16.9L20.8 16.8L21.3 15.8L21.4 15.6L21.7 14.6L21.7 14.4L22 13.3L22 13.2L22 12.1L22 11.9L22 10.8L22 10.7L21.7 9.6L21.7 9.4L21.4 8.4L21.3 8.2L20.8 7.2L20.8 7.1L20.2 6.2L20.1 6L19.4 5.2L19.3 5.1L18.5 4.3L18.4 4.2L17.4 3.6L17.3 3.5L16.3 2.9L16.2 2.9L15.2 2.5L15 2.4L13.9 2.1L13.8 2.1L12.7 2L12.6 2L11.4 2L11.3 2L10.2 2.1L10.1 2.1L9 2.4L8.8 2.5L7.8 2.9L7.7 2.9L6.7 3.5L6.6 3.6L5.6 4.2L5.5 4.3L4.7 5.1L4.6 5.2L3.9 6L3.8 6.1L3.2 7.1L3.2 7.2L2.7 8.2L2.6 8.4L2.3 9.4L2.3 9.6L2 10.7L2 10.8L2 11.9L2 12ZM9.9 9.1L9.8 9.1L9.7 9.2L9.6 9.2L9.5 9.2L9.4 9.2L9.3 9.2L9.2 9.2L9.1 9.2L9 9.2L8.9 9.1L8.8 9.1L8.7 9.1L8.7 9L8.6 8.9L8.5 8.9L8.5 8.8L8.4 8.7L8.4 8.6L8.4 8.5L8.4 8.4L8.4 8.3L8.4 8.2L8.4 8.1L8.4 8L8.5 7.9L8.5 7.8L8.5 7.7L8.6 7.7L8.7 7.6L8.7 7.5L11.9 5.2L12 5.2L12.1 5.1L12.2 5.1L12.3 5.1L12.4 5.1L12.5 5.1L12.6 5.1L12.7 5.1L12.8 5.1L12.9 5.2L13 5.2L13.1 5.3L13.2 5.3L13.2 5.4L13.3 5.5L13.4 5.6L13.4 5.7L13.4 5.8L13.4 5.9L13.4 6L13.4 18L13.4 18.1L13.4 18.2L13.4 18.3L13.4 18.4L13.3 18.4L13.3 18.5L13.2 18.6L13.2 18.7L13.1 18.7L13 18.8L12.9 18.8L12.9 18.9L12.8 18.9L12.7 18.9L12.6 18.9L12.5 18.9L12.4 18.9L12.3 18.9L12.2 18.9L12.1 18.9L12.1 18.8L12 18.8L11.9 18.7L11.8 18.7L11.8 18.6L11.7 18.5L11.7 18.4L11.6 18.4L11.6 18.3L11.6 18.2L11.6 18.1L11.6 18L11.6 7.8Z", stroke: 0.0f32, even_odd: true, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_18: [SvgOp; 1] = [
    SvgOp { d: "M5 5H19A2 2 0 0 1 21 7V17A2 2 0 0 1 19 19H5A2 2 0 0 1 3 17V7A2 2 0 0 1 5 5Z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_19: [SvgOp; 1] = [
    SvgOp { d: "M5 4L4.9 4L4.7 4.1L4.6 4.1L4.3 4.1L4.2 4.1L4 4.2L3.9 4.2L3.7 4.3L3.6 4.4L3.4 4.5L3.3 4.6L3.1 4.7L3 4.8L2.8 5L2.7 5.1L2.6 5.3L2.5 5.4L2.4 5.6L2.3 5.7L2.2 5.9L2.2 6L2.1 6.2L2.1 6.3L2.1 6.6L2.1 6.7L2 6.9L2 7L2 17L2 17.1L2.1 17.3L2.1 17.4L2.1 17.7L2.1 17.8L2.2 18L2.2 18.1L2.3 18.3L2.4 18.4L2.5 18.6L2.6 18.7L2.7 18.9L2.8 19L3 19.2L3.1 19.3L3.3 19.4L3.4 19.5L3.6 19.6L3.7 19.7L3.9 19.8L4 19.8L4.2 19.9L4.3 19.9L4.6 19.9L4.7 19.9L4.9 20L5 20L19 20L19.1 20L19.3 19.9L19.4 19.9L19.7 19.9L19.8 19.9L20 19.8L20.1 19.8L20.3 19.7L20.4 19.6L20.6 19.5L20.7 19.4L20.9 19.3L21 19.2L21.2 19L21.3 18.9L21.4 18.7L21.5 18.6L21.6 18.4L21.7 18.3L21.8 18.1L21.8 18L21.9 17.8L21.9 17.7L21.9 17.4L21.9 17.3L22 17.1L22 17L22 7L22 6.9L21.9 6.7L21.9 6.6L21.9 6.3L21.9 6.2L21.8 6L21.8 5.9L21.7 5.7L21.6 5.6L21.5 5.4L21.4 5.3L21.3 5.1L21.2 5L21 4.8L20.9 4.7L20.7 4.6L20.6 4.5L20.4 4.4L20.3 4.3L20.1 4.2L20 4.2L19.8 4.1L19.7 4.1L19.4 4.1L19.3 4.1L19.1 4L19 4Z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_20: [SvgOp; 1] = [
    SvgOp { d: "M188.58 102.4 L554.63 102.4 L576.95 102.95 L598.31 104.61 L618.72 107.36 L638.18 111.22 L656.68 116.18 L674.23 122.25 L690.82 129.42 L706.46 137.69 L721.14 147.06 L734.87 157.54 L747.46 168.98 L758.72 181.24 L768.66 194.33 L777.28 208.25 L784.56 222.99 L790.53 238.55 L795.17 254.94 L798.48 272.15 L800.47 290.19 L801.13 309.05 L800.76 322.59 L799.65 335.78 L797.79 348.61 L795.2 361.1 L791.86 373.23 L787.78 385.01 L782.97 396.43 L777.41 407.51 L771.1 418.23 L764.06 428.6 L756.23 437.25 L747.94 445.43 L739.18 453.12 L729.96 460.33 L720.28 467.06 L710.13 473.3 L699.52 479.07 L688.44 484.35 L676.91 489.15 L664.9 493.47 L680.33 496.64 L695.1 500.58 L709.21 505.31 L722.66 510.81 L735.45 517.1 L747.58 524.16 L759.06 532 L769.88 540.62 L780.04 550.01 L789.55 560.19 L798.26 571.03 L806.06 582.43 L812.94 594.38 L818.9 606.89 L823.95 619.96 L828.08 633.58 L831.29 647.76 L833.58 662.5 L834.96 677.79 L835.42 693.63 L834.56 716.94 L832.01 739.04 L827.74 759.94 L821.78 779.63 L814.1 798.12 L804.72 815.4 L793.64 831.48 L780.85 846.35 L766.36 860.02 L750.16 872.49 L735 881.82 L718.8 890.17 L701.57 897.53 L683.29 903.92 L663.98 909.32 L643.63 913.74 L622.24 917.18 L599.81 919.64 L576.35 921.11 L551.85 921.6 L188.58 921.6 Z M283.11 182.1 L283.11 461.03 L531.46 461.03 L547.95 460.67 L563.67 459.57 L578.64 457.74 L592.85 455.17 L606.29 451.88 L618.98 447.85 L630.9 443.1 L642.07 437.6 L652.48 431.38 L662.12 424.43 L670.93 416.78 L678.81 408.49 L685.76 399.55 L691.78 389.95 L696.88 379.71 L701.05 368.83 L704.29 357.29 L706.61 345.1 L708 332.27 L708.46 318.78 L708.02 305.33 L706.7 292.58 L704.5 280.53 L701.42 269.19 L697.46 258.55 L692.61 248.61 L686.89 239.38 L680.29 230.86 L672.81 223.04 L664.44 215.92 L655.2 209.49 L645.07 203.74 L634.07 198.67 L622.18 194.27 L609.42 190.55 L595.77 187.51 L581.25 185.14 L565.84 183.45 L549.55 182.43 L532.39 182.1 Z M283.11 538.87 L283.11 841.9 L544.43 841.9 L560.64 841.59 L576.28 840.64 L591.33 839.07 L605.82 836.86 L619.73 834.03 L633.06 830.56 L645.82 826.47 L658.01 821.74 L669.62 816.38 L680.66 810.4 L692.28 802.39 L702.68 793.55 L711.85 783.89 L719.8 773.4 L726.53 762.09 L732.03 749.96 L736.32 737 L739.37 723.21 L741.21 708.6 L741.82 693.17 L741.29 677.73 L739.71 663.09 L737.07 649.26 L733.37 636.23 L728.62 624.01 L722.8 612.6 L715.94 602 L708.01 592.2 L699.04 583.2 L689 575.02 L678.71 568.15 L667.5 562 L655.36 556.58 L642.29 551.89 L628.3 547.91 L613.38 544.66 L597.53 542.13 L580.76 540.32 L563.06 539.24 L544.43 538.87 Z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_21: [SvgOp; 1] = [
    SvgOp { d: "M731.07 102.4 L838.76 102.4 L877.42 921.6 L775.25 921.6 L768.81 693.33 L425.48 693.33 L260.72 921.6 L146.58 921.6 Z M477.02 610.49 L768.81 610.49 L755.92 203.65 L749.48 203.65 Z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_22: [SvgOp; 2] = [
    SvgOp { d: "M16.81 3.33a2.72 2.72 0 1 1 3.85 3.85l-13 13.01-5.29 1.44 1.45-5.3z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "m14.89 5.26 3.85 3.85", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_23: [SvgOp; 3] = [
    SvgOp { d: "M16.81 3.33a2.72 2.72 0 1 1 3.85 3.85l-13 13.01-5.29 1.44 1.45-5.3z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "m14.89 5.26 3.85 3.85", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "m14.89 5.26 3.85 3.85L7.66 20.19l-5.29 1.44 1.44-5.29Z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_24: [SvgOp; 1] = [
    SvgOp { d: "M5 5H19A2 2 0 0 1 21 7V17A2 2 0 0 1 19 19H5A2 2 0 0 1 3 17V7A2 2 0 0 1 5 5Z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_25: [SvgOp; 3] = [
    SvgOp { d: "M4 7V5h16v2", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M12 5v14", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M9 19h6", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_26: [SvgOp; 1] = [
    SvgOp { d: "m20 6.5-11 11-5-5", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
// video-mic / video-record / video-speaker —— 录制条那三颗
static SIO_28: [SvgOp; 4] = [
    SvgOp { d: "M12 4A2.5 2.5 0 0 1 14.5 6.5V11.5A2.5 2.5 0 0 1 9.5 11.5V6.5A2.5 2.5 0 0 1 12 4Z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M6.5 11A5.5 5.5 0 0 0 17.5 11", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M12 16.5V20", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M9 20H15", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
// 圆环 + 实心圆点：整圆拆成两段半圆弧，避免解析器在弦长 0 时挑错圆心
static SIO_29: [SvgOp; 2] = [
    SvgOp { d: "M117.76 512A394.24 394.24 0 0 1 906.24 512A394.24 394.24 0 0 1 117.76 512Z", stroke: 80.64f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M364.16 512A147.84 147.84 0 0 1 659.84 512A147.84 147.84 0 0 1 364.16 512Z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];
static SIO_30: [SvgOp; 3] = [
    SvgOp { d: "M4 9H7L11 6V18L7 15H4Z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M15 8.5A5 5 0 0 1 15 15.5", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M17.5 6A8.5 8.5 0 0 1 17.5 18", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
// action-video —— 录屏（机身 + 镜头）
static SIO_27: [SvgOp; 2] = [
    SvgOp { d: "M5 5H13A2 2 0 0 1 15 7V17A2 2 0 0 1 13 19H5A2 2 0 0 1 3 17V7A2 2 0 0 1 5 5Z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M15 10L21 7V17L15 14Z", stroke: 1.89f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];

// 副条/主条用到的几个形状图标（取自 kIcon36..kIcon41）
// shape-line —— 一条 45° 粗斜线（箭头工具副条的"直线"那颗，1024 视窗）
static SIO_31: [SvgOp; 1] = [
    SvgOp { d: "M807.68 132.32L132.32 807.68A8.96 8.96 0 0 0 132.32 820L177.12 864.8A8.96 8.96 0 0 0 189.44 864.8L864.8 189.44A8.96 8.96 0 0 0 864.8 177.12L820 132.32A8.96 8.96 0 0 0 807.68 132.32Z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];
// shape-circle-solid —— 正圆实心点（手绘档）
static SIO_32: [SvgOp; 1] = [
    SvgOp { d: "M213.33 512A298.67 298.67 0 0 1 810.67 512A298.67 298.67 0 0 1 213.33 512Z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];
// tool-arrow-straight —— 斜线 + 直角箭头（主条"箭头"那颗用它）
static SIO_33: [SvgOp; 2] = [
    SvgOp { d: "M207.36 816.64L816.64 207.36", stroke: 80.64f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M512 207.36H816.64V512", stroke: 80.64f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
// shape-number-square-outline / solid —— 序号"方"档的两颗
static SIO_34: [SvgOp; 2] = [
    SvgOp { d: "M5.9 3.5H18.1A2.4 2.4 0 0 1 20.5 5.9V18.1A2.4 2.4 0 0 1 18.1 20.5H5.9A2.4 2.4 0 0 1 3.5 18.1V5.9A2.4 2.4 0 0 1 5.9 3.5Z", stroke: 1.5f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M10.5 9.25L12.75 7.5V16.5", stroke: 1.75f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_35: [SvgOp; 1] = [
    SvgOp { d: "M5.55 2.75H18.45A2.8 2.8 0 0 1 21.25 5.55V18.45A2.8 2.8 0 0 1 18.45 21.25H5.55A2.8 2.8 0 0 1 2.75 18.45V5.55A2.8 2.8 0 0 1 5.55 2.75ZM9.55 9.4L12.55 7.2H13.55V16.55H11.95V8.55L9.55 10.75Z", stroke: 0.0f32, even_odd: true, cap: CAP_BUTT, join: JOIN_MITER },
];
// tool-smart-erase —— 两颗四角星（大星=被抹掉的区域，小星=补出来的内容）
static SIO_36: [SvgOp; 1] = [
    SvgOp { d: "M9 2.5L11.05 6.95L15.5 9L11.05 11.05L9 15.5L6.95 11.05L2.5 9L6.95 6.95Z M18 13L19.05 15.95L22 17L19.05 18.05L18 21L16.95 18.05L14 17L16.95 15.95Z", stroke: 1.9f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];

// quick-save —— 闪电 + 底座（主条"保存"那颗用它）
static SIO_37: [SvgOp; 3] = [
    SvgOp { d: "M428.28 72.41Q431.36 64 440.32 64L718.08 64Q727.04 64 723.96 72.41L631.56 324.39Q628.48 332.8 637.44 332.8L790.4 332.8Q799.36 332.8 792.57 339.32L365.83 792.2Q359.68 799.36 361.72 790L420.36 538.64Q422.4 530.24 413.44 530.24L270.08 530.24Q261.12 530.24 264.2 521.51Z", stroke: 0.0f32, even_odd: true, cap: CAP_BUTT, join: JOIN_MITER },
    SvgOp { d: "M482.07 144.1Q485.12 135.68 494.08 135.68L615.04 135.68Q623.36 135.68 620.92 144.09L528.52 396.07Q525.44 404.48 534.4 404.48L623.36 404.48Q632.32 404.48 626.81 411L491.27 554.76Q485.12 561.28 487.38 552.61L509.74 466.91Q512 458.24 502.88 458.24L377.6 458.24Q368.64 458.24 371.69 449.82Z", stroke: 0.0f32, even_odd: true, cap: CAP_BUTT, join: JOIN_MITER },
    SvgOp { d: "M99.84 673.28h62.72v206.08h698.88V673.28h62.72q8.96 0 8.96 8.96v232.96q0 44.8-44.8 44.8H135.68q-44.8 0-44.8-44.8V682.24q0-8.96 8.96-8.96Z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];

// trash —— 录制态"丢弃"那颗
static SIO_38: [SvgOp; 4] = [
    SvgOp { d: "M126.72 252.16h770.56", stroke: 80.64f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M341.76 252.16v-107.52h340.48v107.52", stroke: 80.64f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M216.32 252.16l38.08 636.16a53.76 53.76 0 0 0 53.76 49.28h407.68a53.76 53.76 0 0 0 53.76-49.28l38.08-636.16", stroke: 80.64f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M413.44 404.48v367.36M610.56 404.48v367.36", stroke: 80.64f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
// recording-resume —— 圆环 + 实心三角（录制态"开始"那颗）
static SIO_39: [SvgOp; 2] = [
    SvgOp { d: "M117.76 512A394.24 394.24 0 0 1 906.24 512A394.24 394.24 0 0 1 117.76 512Z", stroke: 80.64f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M413.44 330.56L691.2 512L413.44 693.44V330.56Z", stroke: 0.0f32, even_odd: false, cap: CAP_BUTT, join: JOIN_MITER },
];
// recording-microphone —— 实心麦（开）；-off 多一条斜杠（关）
static SIO_40: [SvgOp; 2] = [
    SvgOp { d: "M512 81.92A125.44 125.44 0 0 1 637.44 207.36V494.08A125.44 125.44 0 0 1 512 619.52A125.44 125.44 0 0 1 386.56 494.08V207.36A125.44 125.44 0 0 1 512 81.92Z", stroke: 80.64f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M243.2 467.2v26.88a268.8 268.8 0 0 0 537.6 0v-26.88M512 762.88v152.32M332.8 915.2h358.4", stroke: 80.64f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];
static SIO_41: [SvgOp; 3] = [
    SvgOp { d: "M512 81.92A125.44 125.44 0 0 1 637.44 207.36V494.08A125.44 125.44 0 0 1 512 619.52A125.44 125.44 0 0 1 386.56 494.08V207.36A125.44 125.44 0 0 1 512 81.92Z", stroke: 80.64f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M243.2 467.2v26.88a268.8 268.8 0 0 0 537.6 0v-26.88M512 762.88v152.32M332.8 915.2h358.4", stroke: 80.64f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
    SvgOp { d: "M243.2 243.2l537.6 537.6", stroke: 80.64f32, even_odd: false, cap: CAP_ROUND, join: JOIN_ROUND },
];

static SVG_ICONS: [SvgIcon; 47] = [
    SvgIcon { name: "action-close", vb: 24.0f32, ops: &SIO_0 },
    SvgIcon { name: "action-copy", vb: 24.0f32, ops: &SIO_1 },
    SvgIcon { name: "action-pin", vb: 24.0f32, ops: &SIO_2 },
    SvgIcon { name: "action-redo", vb: 24.0f32, ops: &SIO_3 },
    SvgIcon { name: "action-save", vb: 24.0f32, ops: &SIO_4 },
    SvgIcon { name: "action-undo", vb: 24.0f32, ops: &SIO_5 },
    SvgIcon { name: "action-video", vb: 24.0f32, ops: &SIO_27 },
    SvgIcon { name: "blur-brush", vb: 24.0f32, ops: &SIO_6 },
    SvgIcon { name: "blur-rect", vb: 24.0f32, ops: &SIO_7 },
    SvgIcon { name: "eraser-brush", vb: 24.0f32, ops: &SIO_8 },
    SvgIcon { name: "eraser-rect", vb: 24.0f32, ops: &SIO_9 },
    SvgIcon { name: "mosaic-brush", vb: 1024.0f32, ops: &SIO_10 },
    SvgIcon { name: "mosaic-rect", vb: 1024.0f32, ops: &SIO_11 },
    SvgIcon { name: "quick-save", vb: 1024.0f32, ops: &SIO_37 },
    SvgIcon { name: "recording-microphone", vb: 1024.0f32, ops: &SIO_40 },
    SvgIcon { name: "recording-microphone-off", vb: 1024.0f32, ops: &SIO_41 },
    SvgIcon { name: "recording-resume", vb: 1024.0f32, ops: &SIO_39 },
    // IconRecordingStart = kIcon29，与 video-record 同一份几何（圆环 + 实心圆点）
    // 主条第 8 颗用的就是它
    SvgIcon { name: "recording-start", vb: 1024.0f32, ops: &SIO_29 },
    SvgIcon { name: "shape-arrow-outline", vb: 24.0f32, ops: &SIO_12 },
    SvgIcon { name: "shape-arrow-solid", vb: 24.0f32, ops: &SIO_13 },
    SvgIcon { name: "shape-circle-solid", vb: 1024.0f32, ops: &SIO_32 },
    SvgIcon { name: "shape-ellipse-outline", vb: 24.0f32, ops: &SIO_14 },
    SvgIcon { name: "shape-ellipse-solid", vb: 24.0f32, ops: &SIO_15 },
    SvgIcon { name: "shape-line", vb: 1024.0f32, ops: &SIO_31 },
    SvgIcon { name: "shape-number-circle-outline", vb: 24.0f32, ops: &SIO_16 },
    SvgIcon { name: "shape-number-circle-solid", vb: 24.0f32, ops: &SIO_17 },
    SvgIcon { name: "shape-number-square-outline", vb: 24.0f32, ops: &SIO_34 },
    SvgIcon { name: "shape-number-square-solid", vb: 24.0f32, ops: &SIO_35 },
    SvgIcon { name: "shape-rect-outline", vb: 24.0f32, ops: &SIO_18 },
    SvgIcon { name: "shape-rect-solid", vb: 24.0f32, ops: &SIO_19 },
    SvgIcon { name: "text-bold", vb: 1024.0f32, ops: &SIO_20 },
    SvgIcon { name: "text-italic", vb: 1024.0f32, ops: &SIO_21 },
    SvgIcon { name: "tool-line", vb: 24.0f32, ops: &SIO_22 },
    SvgIcon { name: "tool-line-solid", vb: 24.0f32, ops: &SIO_23 },
    SvgIcon { name: "tool-rect", vb: 24.0f32, ops: &SIO_24 },
    SvgIcon { name: "tool-text", vb: 24.0f32, ops: &SIO_25 },
    SvgIcon { name: "ui-check", vb: 24.0f32, ops: &SIO_26 },
    SvgIcon { name: "tool-arrow", vb: 24.0f32, ops: &SIO_13 },
    SvgIcon { name: "tool-arrow-straight", vb: 1024.0f32, ops: &SIO_33 },
    SvgIcon { name: "tool-eraser", vb: 24.0f32, ops: &SIO_9 },
    SvgIcon { name: "tool-mosaic", vb: 1024.0f32, ops: &SIO_10 },
    SvgIcon { name: "tool-number", vb: 24.0f32, ops: &SIO_16 },
    SvgIcon { name: "tool-smart-erase", vb: 24.0f32, ops: &SIO_36 },
    SvgIcon { name: "trash", vb: 1024.0f32, ops: &SIO_38 },
    SvgIcon { name: "video-mic", vb: 24.0f32, ops: &SIO_28 },
    SvgIcon { name: "video-record", vb: 1024.0f32, ops: &SIO_29 },
    SvgIcon { name: "video-speaker", vb: 24.0f32, ops: &SIO_30 },
];

/// 按语义名取图标（线性查表，几十项）
fn svg_icon(name: &str) -> Option<&'static SvgIcon> {
    SVG_ICONS.iter().find(|i| i.name == name)
}

// ---- 路径词法：允许 "m10.5 9.25-1.75v9" 这种把负号当分隔符的紧凑写法 ----
struct PathLex<'a> {
    b: &'a [u8],
    i: usize,
}

impl<'a> PathLex<'a> {
    fn skip(&mut self) {
        while self.i < self.b.len()
            && matches!(self.b[self.i], b' ' | b',' | b'\t' | b'\n' | b'\r')
        {
            self.i += 1;
        }
    }
    fn num(&mut self) -> Option<f32> {
        self.skip();
        let st = self.i;
        if self.i < self.b.len() && (self.b[self.i] == b'-' || self.b[self.i] == b'+') {
            self.i += 1;
        }
        let mut any = false;
        while self.i < self.b.len() && self.b[self.i].is_ascii_digit() {
            self.i += 1;
            any = true;
        }
        if self.i < self.b.len() && self.b[self.i] == b'.' {
            self.i += 1;
            while self.i < self.b.len() && self.b[self.i].is_ascii_digit() {
                self.i += 1;
                any = true;
            }
        }
        if !any {
            self.i = st;
            return None;
        }
        if self.i < self.b.len() && (self.b[self.i] | 0x20) == b'e' {
            let sv = self.i;
            self.i += 1;
            if self.i < self.b.len() && (self.b[self.i] == b'-' || self.b[self.i] == b'+') {
                self.i += 1;
            }
            let mut d2 = false;
            while self.i < self.b.len() && self.b[self.i].is_ascii_digit() {
                self.i += 1;
                d2 = true;
            }
            if !d2 {
                self.i = sv;
            }
        }
        std::str::from_utf8(&self.b[st..self.i])
            .ok()
            .and_then(|s| s.parse::<f32>().ok())
    }
    /// 圆弧的两个标志位可能是 "0 0 1"，也可能是 "001"（只吃一位）
    fn flag(&mut self) -> Option<bool> {
        self.skip();
        if self.i < self.b.len() && (self.b[self.i] == b'0' || self.b[self.i] == b'1') {
            let v = self.b[self.i] == b'1';
            self.i += 1;
            Some(v)
        } else {
            None
        }
    }
}

fn pt_seg_dist(p: (f32, f32), a: (f32, f32), b: (f32, f32)) -> f32 {
    let (dx, dy) = (b.0 - a.0, b.1 - a.1);
    let l = (dx * dx + dy * dy).sqrt();
    if l < 1e-6 {
        return ((p.0 - a.0) * (p.0 - a.0) + (p.1 - a.1) * (p.1 - a.1)).sqrt();
    }
    ((p.0 - a.0) * dy - (p.1 - a.1) * dx).abs() / l
}

fn mid(a: (f32, f32), b: (f32, f32)) -> (f32, f32) {
    ((a.0 + b.0) * 0.5, (a.1 + b.1) * 0.5)
}

/// 三次贝塞尔按平坦度递归细分。tol 是"设计单位下的允许偏差"，
/// 调用方按 side/vb 换算，于是 24 视窗和 1024 视窗得到同样细腻的折线
fn flat_cubic(
    p0: (f32, f32), p1: (f32, f32), p2: (f32, f32), p3: (f32, f32),
    tol: f32, depth: i32, out: &mut Vec<(f32, f32)>,
) {
    let d1 = pt_seg_dist(p1, p0, p3);
    let d2 = pt_seg_dist(p2, p0, p3);
    if depth <= 0 || d1 + d2 <= tol {
        out.push(p3);
        return;
    }
    let p01 = mid(p0, p1);
    let p12 = mid(p1, p2);
    let p23 = mid(p2, p3);
    let p012 = mid(p01, p12);
    let p123 = mid(p12, p23);
    let m = mid(p012, p123);
    flat_cubic(p0, p01, p012, m, tol, depth - 1, out);
    flat_cubic(m, p123, p23, p3, tol, depth - 1, out);
}

/// 二次贝塞尔升阶成三次，复用上面的细分
fn flat_quad(
    p0: (f32, f32), p1: (f32, f32), p2: (f32, f32),
    tol: f32, depth: i32, out: &mut Vec<(f32, f32)>,
) {
    let c1 = (p0.0 + (p1.0 - p0.0) * 2.0 / 3.0, p0.1 + (p1.1 - p0.1) * 2.0 / 3.0);
    let c2 = (p2.0 + (p1.0 - p2.0) * 2.0 / 3.0, p2.1 + (p1.1 - p2.1) * 2.0 / 3.0);
    flat_cubic(p0, c1, c2, p2, tol, depth, out);
}

/// SVG 端点式圆弧 → 圆心式 → 按弧长采样成折线（规范 F.6.5 / F.6.6）
#[allow(clippy::too_many_arguments)]
fn flat_arc(
    p0: (f32, f32), rx0: f32, ry0: f32, rot_deg: f32, large: bool, sweep: bool,
    p1: (f32, f32), tol: f32, out: &mut Vec<(f32, f32)>,
) {
    let (mut rx, mut ry) = (rx0.abs(), ry0.abs());
    if rx < 1e-6 || ry < 1e-6 {
        out.push(p1);
        return;
    }
    let phi = rot_deg.to_radians();
    let (sinp, cosp) = (phi.sin(), phi.cos());
    let dx2 = (p0.0 - p1.0) * 0.5;
    let dy2 = (p0.1 - p1.1) * 0.5;
    let x1 = cosp * dx2 + sinp * dy2;
    let y1 = -sinp * dx2 + cosp * dy2;
    // 半径不够就按比例放大（这才是 SVG 规范的行为，不是画不出来）
    let lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if lam > 1.0 {
        let s = lam.sqrt();
        rx *= s;
        ry *= s;
    }
    let num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    let den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    let mut co = if den <= 0.0 { 0.0 } else { (num / den).max(0.0).sqrt() };
    if large == sweep {
        co = -co;
    }
    let cxp = co * rx * y1 / ry;
    let cyp = -co * ry * x1 / rx;
    let cx = cosp * cxp - sinp * cyp + (p0.0 + p1.0) * 0.5;
    let cy = sinp * cxp + cosp * cyp + (p0.1 + p1.1) * 0.5;
    let ang = |ux: f32, uy: f32, vx: f32, vy: f32| -> f32 {
        let cross = ux * vy - uy * vx;
        let dot = ux * vx + uy * vy;
        let l = ((ux * ux + uy * uy) * (vx * vx + vy * vy)).sqrt();
        let mut a = if l <= 0.0 { 0.0 } else { (cross / l).clamp(-1.0, 1.0).asin() };
        if dot < 0.0 {
            a = if a >= 0.0 { std::f32::consts::PI - a } else { -std::f32::consts::PI - a };
        }
        a
    };
    let ux = (x1 - cxp) / rx;
    let uy = (y1 - cyp) / ry;
    let vx = (-x1 - cxp) / rx;
    let vy = (-y1 - cyp) / ry;
    let th1 = ang(1.0, 0.0, ux, uy);
    let mut dth = ang(ux, uy, vx, vy);
    if !sweep && dth > 0.0 {
        dth -= std::f32::consts::PI * 2.0;
    }
    if sweep && dth < 0.0 {
        dth += std::f32::consts::PI * 2.0;
    }
    let rmax = rx.max(ry);
    let n = ((dth.abs() * rmax / tol.max(0.02)).sqrt() as i32).clamp(6, 160);
    for k in 1..=n {
        let t = th1 + dth * (k as f32 / n as f32);
        let (c, s) = (t.cos(), t.sin());
        out.push((cx + cosp * rx * c - sinp * ry * s, cy + sinp * rx * c + cosp * ry * s));
    }
    // 采样点最后落到精确端点上，避免累积误差在闭合处留一条缝
    *out.last_mut().unwrap() = p1;
}

/// d 串 → 子路径列表（点集 + 是否闭合）。闭合的由 Z 决定，填充时一律按闭合处理
fn svg_path_polys(d: &str, tol: f32) -> Vec<(Vec<(f32, f32)>, bool)> {
    let mut lx = PathLex { b: d.as_bytes(), i: 0 };
    let mut out: Vec<(Vec<(f32, f32)>, bool)> = Vec::new();
    let mut cur: Vec<(f32, f32)> = Vec::new();
    let mut pen = (0f32, 0f32);
    let mut startpt = (0f32, 0f32);
    let mut ctrl = (0f32, 0f32); // 上一控制点（S/T 用）
    let mut pk = 0u8; // 上一段曲线：0 无 1 三次 2 二次
    let mut cmd = 0u8;
    'parse: loop {
        lx.skip();
        if lx.i >= lx.b.len() {
            break;
        }
        let ch = lx.b[lx.i];
        if ch.is_ascii_alphabetic() {
            cmd = ch;
            lx.i += 1;
        } else if cmd == 0 {
            break;
        }
        let rel = cmd.is_ascii_lowercase();
        let op = cmd.to_ascii_uppercase();
        let ox = if rel { pen.0 } else { 0.0 };
        let oy = if rel { pen.1 } else { 0.0 };
        macro_rules! num {
            () => {
                match lx.num() {
                    Some(v) => v,
                    None => break 'parse,
                }
            };
        }
        match op {
            b'M' => {
                let x = num!();
                let y = num!();
                if cur.len() > 1 {
                    out.push((std::mem::take(&mut cur), false));
                } else {
                    cur.clear();
                }
                pen = (x + ox, y + oy);
                startpt = pen;
                cur.push(pen);
                pk = 0;
                // 后续参数是隐式 lineto
                cmd = if rel { b'l' } else { b'L' };
            }
            b'L' => {
                let x = num!();
                let y = num!();
                pen = (x + ox, y + oy);
                cur.push(pen);
                pk = 0;
            }
            b'H' => {
                let x = num!();
                pen = (x + ox, pen.1);
                cur.push(pen);
                pk = 0;
            }
            b'V' => {
                let y = num!();
                pen = (pen.0, y + oy);
                cur.push(pen);
                pk = 0;
            }
            b'C' | b'S' => {
                // S：第一控制点是上一控制点关于当前点的镜像；上一段不是三次曲线就退化成当前点
                let (ax, ay) = if op == b'C' {
                    let c1x = num!();
                    let c1y = num!();
                    (c1x + ox, c1y + oy)
                } else if pk == 1 {
                    (pen.0 * 2.0 - ctrl.0, pen.1 * 2.0 - ctrl.1)
                } else {
                    pen
                };
                let c2x = num!();
                let c2y = num!();
                let x = num!();
                let y = num!();
                let c2 = (c2x + ox, c2y + oy);
                let e = (x + ox, y + oy);
                flat_cubic(pen, (ax, ay), c2, e, tol, 12, &mut cur);
                pen = e;
                ctrl = c2;
                pk = 1;
            }
            b'Q' | b'T' => {
                // T 同理：上一控制点关于当前点的镜像
                let q = if op == b'Q' {
                    let qx = num!();
                    let qy = num!();
                    (qx + ox, qy + oy)
                } else if pk == 2 {
                    (pen.0 * 2.0 - ctrl.0, pen.1 * 2.0 - ctrl.1)
                } else {
                    pen
                };
                let x = num!();
                let y = num!();
                let e = (x + ox, y + oy);
                flat_quad(pen, q, e, tol, 12, &mut cur);
                pen = e;
                ctrl = q;
                pk = 2;
            }
            b'A' => {
                let rx = num!();
                let ry = num!();
                let rot = num!();
                let large = match lx.flag() {
                    Some(v) => v,
                    None => break 'parse,
                };
                let sweep = match lx.flag() {
                    Some(v) => v,
                    None => break 'parse,
                };
                let x = num!();
                let y = num!();
                let e = (x + ox, y + oy);
                flat_arc(pen, rx, ry, rot, large, sweep, e, tol, &mut cur);
                pen = e;
                pk = 0;
            }
            b'Z' => {
                if cur.len() > 1 {
                    out.push((std::mem::take(&mut cur), true));
                } else {
                    cur.clear();
                }
                pen = startpt;
                pk = 0;
                cmd = 0; // Z 无参数，后面必须跟新命令
            }
            _ => break,
        }
    }
    // 收尾：没有 Z 收尾的子路径是**开口**的 —— 描边时不能自动接回首点，
    // 否则一条勾 / 一条折线会被补成闭合轮廓（ui-check 就成了一个三角形）
    if cur.len() > 1 {
        out.push((cur, false));
    }
    out
}

type Edge = (f32, f32, f32, f32);

fn poly_area(p: &[(f32, f32)]) -> f32 {
    let mut a = 0.0;
    for i in 0..p.len() {
        let j = (i + 1) % p.len();
        a += p[i].0 * p[j].1 - p[j].0 * p[i].1;
    }
    a * 0.5
}

/// 闭合多边形 → 边表。flip 时统一朝向，让"多个子多边形取并集"不会互相抵消：
/// 描边是一条条独立的四边形 + 圆，方向反了重叠处绕数会归零，笔画上就被啃出洞
fn push_poly_edges(p: &[(f32, f32)], out: &mut Vec<Edge>, flip: bool) {
    let n = p.len();
    if n < 3 {
        return;
    }
    let rev = flip && poly_area(p) > 0.0;
    for i in 0..n {
        let j = (i + 1) % n;
        let (a, b) = if rev { (p[j], p[i]) } else { (p[i], p[j]) };
        out.push((a.0, a.1, b.0, b.1));
    }
}

/// 圆（拐角/端头用）。段数按半径给足：段数不够时，相邻顶点的圆会各露一角，
/// 描边内侧就出现一圈"花瓣"，看着像梳齿状的横纹
fn add_disc(cx: f32, cy: f32, r: f32, out: &mut Vec<Edge>) {
    let n = ((r * 6.0) as i32).clamp(20, 128);
    let pts: Vec<(f32, f32)> = (0..n)
        .map(|k| {
            let t = -std::f32::consts::PI * 2.0 * k as f32 / n as f32;
            (cx + r * t.cos(), cy + r * t.sin())
        })
        .collect();
    push_poly_edges(&pts, out, true);
}

/// 折线 → 描边轮廓边表（线段四边形 + 圆拐角 + 端头）
fn push_stroke_edges(
    pts: &[(f32, f32)], closed: bool, width: f32, cap: u8, _join: u8, out: &mut Vec<Edge>,
) {
    let n = pts.len();
    let h = width * 0.5;
    if n < 2 || h <= 0.0 {
        return;
    }
    let segs = if closed { n } else { n - 1 };
    for i in 0..segs {
        let a = pts[i];
        let b = pts[(i + 1) % n];
        let (dx, dy) = (b.0 - a.0, b.1 - a.1);
        let l = (dx * dx + dy * dy).sqrt();
        if l < 1e-6 {
            continue;
        }
        let (ux, uy) = (dx / l, dy / l);
        let (nx, ny) = (-uy * h, ux * h);
        // 方头：两端各沿走向伸出半个线宽
        let mut a2 = a;
        let mut b2 = b;
        if cap == CAP_SQUARE && !closed && (i == 0 || i == segs - 1) {
            if i == 0 {
                a2 = (a.0 - ux * h, a.1 - uy * h);
            }
            if i == segs - 1 {
                b2 = (b.0 + ux * h, b.1 + uy * h);
            }
        }
        let q = [
            (a2.0 + nx, a2.1 + ny),
            (b2.0 + nx, b2.1 + ny),
            (b2.0 - nx, b2.1 - ny),
            (a2.0 - nx, a2.1 - ny),
        ];
        push_poly_edges(&q, out, true);
    }
    if closed {
        for p in pts.iter() {
            add_disc(p.0, p.1, h, out);
        }
    } else {
        // 内部顶点补圆 = 圆拐角（这套图标 stroke 的 op 全是 join=round）
        for p in pts.iter().take(n - 1).skip(1) {
            add_disc(p.0, p.1, h, out);
        }
        if cap == CAP_ROUND {
            add_disc(pts[0].0, pts[0].1, h, out);
            add_disc(pts[n - 1].0, pts[n - 1].1, h, out);
        }
    }
}

/// 扫描线填充：把**覆盖率**（0..1）写进 acc，取 max 做并集。
///
/// 注意这里必须算覆盖面积，不能只判"像素中心在不在里面"：
/// 描边是由一连串首尾相接的四边形拼出来的，接缝处的窄缝在中心采样下会整列丢掉，
/// 于是椭圆环上出现一圈放射状梳齿（看着像虚线）。按面积累加，接缝两侧加起来正好 1
fn fill_edges_into(acc: &mut [f32], w: i32, h: i32, edges: &[Edge], even_odd: bool) {
    if edges.is_empty() {
        return;
    }
    let mut ymin = f32::MAX;
    let mut ymax = f32::MIN;
    for e in edges {
        ymin = ymin.min(e.1.min(e.3));
        ymax = ymax.max(e.1.max(e.3));
    }
    let y0 = (ymin.floor() as i32).clamp(0, h);
    let y1 = ((ymax.ceil() as i32) + 1).clamp(0, h);
    let mut xs: Vec<(f32, i32)> = Vec::new();
    for sy in y0..y1 {
        let yy = sy as f32 + 0.5;
        xs.clear();
        for e in edges {
            let (ex0, ey0, ex1, ey1) = *e;
            if ey0 == ey1 {
                continue;
            }
            let (lo, hi) = if ey0 < ey1 { (ey0, ey1) } else { (ey1, ey0) };
            if yy < lo || yy >= hi {
                continue;
            }
            let t = (yy - ey0) / (ey1 - ey0);
            xs.push((ex0 + (ex1 - ex0) * t, if ey1 > ey0 { 1 } else { -1 }));
        }
        if xs.len() < 2 {
            continue;
        }
        xs.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(std::cmp::Ordering::Equal));
        let row = (sy as usize) * (w as usize);
        let mut wind = 0i32;
        for k in 0..xs.len() - 1 {
            wind += xs[k].1;
            let inside = if even_odd { (k & 1) == 0 } else { wind != 0 };
            if !inside {
                continue;
            }
            let (xa, xb) = (xs[k].0, xs[k + 1].0);
            if xb <= xa {
                continue;
            }
            let ia = (xa.floor() as i32).max(0);
            let ib = (xb.ceil() as i32).min(w);
            for ix in ia..ib {
                let l = xa.max(ix as f32);
                let r = xb.min(ix as f32 + 1.0);
                let c = r - l;
                if c <= 0.0 {
                    continue;
                }
                // 饱和相加，不是取 max：描边是一串首尾相接的四边形，接缝两侧各覆盖
                // 半个像素，取 max 会漏掉一半（笔画整体偏淡、面积小于设计值）；
                // 相加再夹到 1 才是并集的真实覆盖率
                let i = row + ix as usize;
                let nv = acc[i] + c;
                acc[i] = if nv > 1.0 { 1.0 } else { nv };
            }
        }
    }
}

/// 把图标光栅化成 side×side 的覆盖率（0..255）。返回 None = 名字没命中
fn svg_mask(name: &str, side: i32) -> Option<Vec<u8>> {
    let def = svg_icon(name)?;
    let side = side.max(4);
    let sub = side * ui::metric::ICON_SS;
    let mut acc = vec![0f32; (sub * sub) as usize];
    let scale = side as f32 / def.vb;
    let ss = ui::metric::ICON_SS as f32;
    // 允许 0.06 物理像素偏差 → 换回设计单位。取这么小是为了让折线步长明显小于
    // 半个线宽：步长一大，描边就成了"一串首尾不相接的圆"，边缘起伏能看出来
    let tol = 0.06 / scale;
    for op in def.ops.iter() {
        let polys = svg_path_polys(op.d, tol);
        if polys.is_empty() {
            continue;
        }
        let mut edges: Vec<Edge> = Vec::new();
        for (pts, closed) in polys.iter() {
            let tp: Vec<(f32, f32)> = pts
                .iter()
                .map(|&(x, y)| (x * scale * ss, y * scale * ss))
                .collect();
            if op.stroke > 0.0 {
                push_stroke_edges(&tp, *closed, op.stroke * scale * ss, op.cap, op.join, &mut edges);
            } else {
                // 填充一律按闭合处理（SVG 语义），最后一段自动接回首点
                push_poly_edges(&tp, &mut edges, false);
            }
        }
        // 描边永远按非零绕数取并集；只有填充才看 op 声明的 evenodd
        let use_eo = op.stroke <= 0.0 && op.even_odd;
        fill_edges_into(&mut acc, sub, sub, &edges, use_eo);
    }
    let mut cov = vec![0u8; (side * side) as usize];
    let total = (ui::metric::ICON_SS * ui::metric::ICON_SS) as f32;
    for y in 0..side {
        for x in 0..side {
            let mut c = 0f32;
            for sy in 0..ui::metric::ICON_SS {
                for sx in 0..ui::metric::ICON_SS {
                    let ix = x * ui::metric::ICON_SS + sx;
                    let iy = y * ui::metric::ICON_SS + sy;
                    c += acc[(iy * sub + ix) as usize];
                }
            }
            cov[(y * side + x) as usize] = (c * 255.0 / total).round().clamp(0.0, 255.0) as u8;
        }
    }
    Some(cov)
}

/// 蒙版缓存：同一 (名字, 边长) 只光栅化一次。按钮每次重绘都取，命中就是一次查表 + 拷贝
static ICON_MASK_CACHE: Mutex<Vec<(&'static str, i32, Vec<u8>)>> = Mutex::new(Vec::new());

pub(crate) fn svg_mask_cached(name: &str, side: i32) -> Option<Vec<u8>> {
    if let Some(def) = svg_icon(name) {
        let key = def.name;
        {
            let c = ICON_MASK_CACHE.lock_ok();
            for (n, s, v) in c.iter() {
                if *n == key && *s == side {
                    return Some(v.clone());
                }
            }
        }
        let m = svg_mask(key, side)?;
        ICON_MASK_CACHE.lock_ok().push((key, side, m.clone()));
        Some(m)
    } else {
        None
    }
}

// 小图标合成用的常驻 DIB：边长上限 64（按钮图标 19、复选框对勾 14 都在里面）。
// 源位图必须按 64 宽声明、再用 SrcX/SrcY 挑左上角那一块 —— 声明成 side 宽会按
// side 的行距去读，图整个错行（一片横条）。
const ICON_DIB: i32 = 64;
static ICON_DIB_BITS: AtomicIsize = AtomicIsize::new(0);

/// 把 side×side 的蒙版按覆盖率合成（前景 fg 压在底色 bg 上）后贴到 dst 的 (ox,oy)。
/// 边缘用覆盖率做混色，所以勾、箭头这类斜线不会出现硬锯齿。
unsafe fn mask_blit(
    dst: winapi::shared::windef::HDC, ox: i32, oy: i32, side: i32, mask: &[u8], bg: u32, fg: u32,
) {
    if side <= 0 || side > ICON_DIB || mask.len() < (side * side) as usize { return; }
    let mut bits = ICON_DIB_BITS.load(Ordering::Relaxed) as *mut u8;
    if bits.is_null() {
        let hdc = GetDC(std::ptr::null_mut());
        let mut bmi: BITMAPINFO = std::mem::zeroed();
        bmi.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        bmi.bmiHeader.biWidth = ICON_DIB;
        bmi.bmiHeader.biHeight = -ICON_DIB;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        let mut p: *mut winapi::ctypes::c_void = std::ptr::null_mut();
        let bmp = CreateDIBSection(hdc, &mut bmi, DIB_RGB_COLORS, &mut p, std::ptr::null_mut(), 0);
        ReleaseDC(std::ptr::null_mut(), hdc);
        // 建不出来（GDI 资源耗尽）就直接不画：拿空指针当像素写会立刻访问违例
        if bmp.is_null() || p.is_null() { return; }
        bits = p as *mut u8;
        ICON_DIB_BITS.store(bits as isize, Ordering::Relaxed);
    }
    let buf = std::slice::from_raw_parts_mut(bits, (ICON_DIB * ICON_DIB * 4) as usize);
    let (br, bgc, bb) = ((bg >> 16) & 0xFF, (bg >> 8) & 0xFF, bg & 0xFF);
    let (fr, fgc, fb) = ((fg >> 16) & 0xFF, (fg >> 8) & 0xFF, fg & 0xFF);
    for py in 0..side {
        for px in 0..side {
            let a = mask[(py * side + px) as usize] as u32;
            let inv = 255 - a;
            let i = ((py * ICON_DIB + px) * 4) as usize;
            buf[i] = ((fb * a + bb * inv) / 255) as u8;
            buf[i + 1] = ((fgc * a + bgc * inv) / 255) as u8;
            buf[i + 2] = ((fr * a + br * inv) / 255) as u8;
            buf[i + 3] = 255;
        }
    }
    let mut bmi2: BITMAPINFO = std::mem::zeroed();
    bmi2.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
    bmi2.bmiHeader.biWidth = ICON_DIB;
    bmi2.bmiHeader.biHeight = -ICON_DIB;
    bmi2.bmiHeader.biPlanes = 1;
    bmi2.bmiHeader.biBitCount = 32;
    bmi2.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(
        dst, ox, oy, side as u32, side as u32, 0, 0, 0, side as u32,
        bits as *const winapi::ctypes::c_void, &bmi2, DIB_RGB_COLORS,
    );
}

/// 图标贴图：蒙版覆盖率 × 前景色，直接合成到底色上再一次性贴出。
///
/// 图标盒边长 = cell*0.58 ≈ 18.6 → 取整 19
/// 居中放进 29 的按钮里（上下各 5px 留白）。不能把 34×34 的字形位图整块铺到
/// 按钮上，那样图形占了 87%，四周的留白全丢。
pub(crate) unsafe fn anno_blit_icon(
    dst: winapi::shared::windef::HDC, x: i32, y: i32, size: i32, name: &str, bg: u32, fg: u32,
) {
    // 整格先铺底色（box 之外的留白也要干净，否则会残留上一帧的悬停色）
    let mut cell_rc: RECT = RECT { left: x, top: y, right: x + size, bottom: y + size };
    let brush = CreateSolidBrush(((bg & 0xFF) << 16) | (bg >> 8 & 0xFF) << 8 | (bg >> 16 & 0xFF));
    FillRect(dst, &mut cell_rc, brush);
    DeleteObject(brush as *mut winapi::ctypes::c_void);
    let side = ((ui::metric::CELL as f32) * ui::layout::anno::ICON_BOX_RATIO).round() as i32;
    let mask = match svg_mask_cached(name, side) {
        Some(m) => m,
        None => return, // 名字没命中：宁可空着也不留半张图
    };
    // 图标在按钮里居中
    // 半像素（28-19)/2 = 4.5）进上去 → 5。这里用 +1 再整除等价于"四舍五入"，
    // 29 宽的旧值和 28 宽的新值落点一致
    let ox = x + (size - side + 1) / 2;
    let oy = y + (size - side + 1) / 2;
    mask_blit(dst, ox, oy, side, &mask, bg, fg);
}

/// 在任意矩形里画一枚 SVG 图标（与按钮图标同一套蒙版缓存）。
/// 复选框的对勾用它：底色传方框的填充色，边缘才和背景融得干净。
pub(crate) unsafe fn svg_icon_blit(
    dst: winapi::shared::windef::HDC, x: i32, y: i32, side: i32, name: &str, bg: u32, fg: u32,
) {
    if let Some(mask) = svg_mask_cached(name, side) {
        mask_blit(dst, x, y, side, &mask, bg, fg);
    }
}
