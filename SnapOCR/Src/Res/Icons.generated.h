// 由 tools/icons/gen_icons.py 生成，请勿手改。
//
// 图标 = 一组绘制操作（路径 + 填充/描边 + 端头拐角），运行期用 D2D 几何绘制，
// 不经过字体光栅化。name 是语义名（表真正的键），code 是老的私有区码位（仅兼容）。
// 命名规范见 tools/icons/README.md 与 Doc/图标命名规范.md。
#pragma once
#include <Ui/SvgPath.h>

namespace ui::icons {

    struct IconOp {
        const wchar_t* d;
        float stroke;      // 0 = 填充；>0 = 描边宽度（与 vb 同一坐标系）
        bool evenOdd;      // 填充规则：奇偶 = true，非零 = false
        Cap  cap;          // 描边端头（填充时无意义）
        Join join;         // 描边拐角（填充时无意义）
    };

    struct IconDef {
        wchar_t code;         // 老码位，仅作兼容别名（0 = 纯别名，无码位）
        const wchar_t* name;  // 语义名
        float vb;             // 设计视窗边长（24 或 1024）
        const IconOp* ops;
        int opCount;
    };

    // ---- 名字常量：调用点一律写 icons::Xxx ----
    // 传字符串字面量时,写错一个字母只会让按钮空着（setIcon 找不到就保持原状）；
    // 换成常量就是编译错误。这是从另一版（Snow Shot 的 icon pack）借来的做法。
    inline constexpr wchar_t ActionClose[] = L"action-close";
    inline constexpr wchar_t ActionCopy[] = L"action-copy";
    inline constexpr wchar_t ActionPin[] = L"action-pin";
    inline constexpr wchar_t ActionRedo[] = L"action-redo";
    inline constexpr wchar_t ActionSave[] = L"action-save";
    inline constexpr wchar_t ActionUndo[] = L"action-undo";
    inline constexpr wchar_t ActionVideo[] = L"action-video";
    inline constexpr wchar_t VideoMic[] = L"video-mic";
    inline constexpr wchar_t VideoRecord[] = L"video-record";
    inline constexpr wchar_t VideoSpeaker[] = L"video-speaker";
    // —— 录制相关（取自 Snow Shot 图标包，见 attachments 的 snow_shot_icons）——
    // 录制入口 / 开始录制 / 麦克风开与关 / 丢弃 / 快速保存。
    // 这里只补当前界面真正用到的那几个，不整包导入
    inline constexpr wchar_t RecordingStart[] = L"recording-start";
    inline constexpr wchar_t RecordingResume[] = L"recording-resume";
    inline constexpr wchar_t RecordingMic[] = L"recording-microphone";
    inline constexpr wchar_t RecordingMicOff[] = L"recording-microphone-off";
    inline constexpr wchar_t Trash[] = L"trash";
    inline constexpr wchar_t QuickSave[] = L"quick-save";
    inline constexpr wchar_t BlurBrush[] = L"blur-brush";
    inline constexpr wchar_t BlurRect[] = L"blur-rect";
    inline constexpr wchar_t EraserBrush[] = L"eraser-brush";
    inline constexpr wchar_t EraserRect[] = L"eraser-rect";
    inline constexpr wchar_t MosaicBrush[] = L"mosaic-brush";
    inline constexpr wchar_t MosaicRect[] = L"mosaic-rect";
    inline constexpr wchar_t ShapeArrowOutline[] = L"shape-arrow-outline";
    inline constexpr wchar_t ShapeArrowSolid[] = L"shape-arrow-solid";
    inline constexpr wchar_t ShapeEllipseOutline[] = L"shape-ellipse-outline";
    inline constexpr wchar_t ShapeEllipseSolid[] = L"shape-ellipse-solid";
    // 正圆的实心圆点：手绘那一档用它，不复用 shape-ellipse-solid（那是椭圆工具的形状图标）
    inline constexpr wchar_t ShapeCircleSolid[] = L"shape-circle-solid";
    inline constexpr wchar_t ShapeLine[] = L"shape-line";
    // 序号副条：圆、方两颗形状按钮（与矩形/椭圆那对同口径），各带实心/空心两态
    // 共 4 种样式；语义名与副条文案、ToolSub 的按钮变量一一对应
    inline constexpr wchar_t ShapeNumberCircleOutline[] = L"shape-number-circle-outline";
    inline constexpr wchar_t ShapeNumberCircleSolid[] = L"shape-number-circle-solid";
    inline constexpr wchar_t ShapeNumberSquareOutline[] = L"shape-number-square-outline";
    inline constexpr wchar_t ShapeNumberSquareSolid[] = L"shape-number-square-solid";
    inline constexpr wchar_t ShapeRectOutline[] = L"shape-rect-outline";
    inline constexpr wchar_t ShapeRectSolid[] = L"shape-rect-solid";
    inline constexpr wchar_t TextBold[] = L"text-bold";
    inline constexpr wchar_t TextItalic[] = L"text-italic";
    inline constexpr wchar_t ToolArrow[] = L"tool-arrow";
    // 主条箭头按钮：斜线 + 直角箭头的字形（取自图标包 arrow-type-straight）
    inline constexpr wchar_t ToolArrowStraight[] = L"tool-arrow-straight";
    inline constexpr wchar_t ToolEraser[] = L"tool-eraser";
    inline constexpr wchar_t ToolLine[] = L"tool-line";
    inline constexpr wchar_t ToolLineSolid[] = L"tool-line-solid";
    inline constexpr wchar_t ToolMosaic[] = L"tool-mosaic";
    // 马赛克副条里"模式"那颗按钮切到高斯模糊时的字形（图标包里那团模糊）
    inline constexpr wchar_t ToolBlur[] = L"blur-brush";
    inline constexpr wchar_t ToolNumber[] = L"tool-number";
    // 马赛克副条里"智能消除"那颗按钮：两颗四角星（消除 = 局部被"擦干净"）
    inline constexpr wchar_t ToolSmartErase[] = L"tool-smart-erase";
    inline constexpr wchar_t ToolRect[] = L"tool-rect";
    inline constexpr wchar_t ToolText[] = L"tool-text";
    inline constexpr wchar_t UiCheck[] = L"ui-check";

    // action-close —— 来自 本地设计 action-close
    inline constexpr IconOp kIcon0[] = {
        { L"m4.5 4.5 15 15m0-15-15 15", 1.89f, false, Cap::Round, Join::Round },
    };
    // action-copy —— 来自 本地设计 action-copy
    inline constexpr IconOp kIcon1[] = {
        { L"M11.5 9.5H19.5A2 2 0 0 1 21.5 11.5V19.5A2 2 0 0 1 19.5 21.5H11.5A2 2 0 0 1 9.5 19.5V11.5A2 2 0 0 1 11.5 9.5Z", 1.89f, false, Cap::Round, Join::Round },
        { L"M5.5 15.5h-1a2 2 0 0 1-2-2v-9a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1", 1.89f, false, Cap::Round, Join::Round },
    };
    // action-pin —— 来自 本地设计 action-pin
    inline constexpr IconOp kIcon2[] = {
        { L"m16.1 2.42 5.48 5.48-5.48 5.47-.91 6.38L4.25 8.81l6.38-.91Z", 1.89f, false, Cap::Round, Join::Round },
        { L"m9.72 14.28-7.3 7.3", 1.89f, false, Cap::Round, Join::Round },
    };
    // action-redo —— 来自 本地设计 action-redo
    inline constexpr IconOp kIcon3[] = {
        { L"M21 12a9 9 0 1 1-3-6.7L21 8", 1.89f, false, Cap::Round, Join::Round },
        { L"M21 3v5h-5", 1.89f, false, Cap::Round, Join::Round },
    };
    // action-save —— 来自 本地设计 action-save
    inline constexpr IconOp kIcon4[] = {
        { L"M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2", 1.89f, false, Cap::Round, Join::Round },
        { L"M17 21v-8H7v8", 1.89f, false, Cap::Round, Join::Round },
        { L"M7 3v5h8", 1.89f, false, Cap::Round, Join::Round },
    };
    // action-undo —— 来自 本地设计 action-undo
    inline constexpr IconOp kIcon5[] = {
        { L"M3 12a9 9 0 1 0 3-6.7L3 8", 1.89f, false, Cap::Round, Join::Round },
        { L"M3 3v5h5", 1.89f, false, Cap::Round, Join::Round },
    };
    // blur-brush —— 来自 本地设计 blur-brush
    inline constexpr IconOp kIcon6[] = {
        { L"m2.69 13.12 8.19 8.19.15.13.18.12.18.09.2.07.2.04.21.01.21-.01.2-.04.2-.07.18-.09.18-.12.15-.13 8.19-8.19.13-.15.12-.18.09-.18.07-.2.04-.2.01-.21-.01-.21-.04-.2-.07-.2-.09-.18-.12-.18-.13-.15-8.19-8.19-.15-.13-.18-.12-.18-.09-.2-.07-.2-.04-.21-.01-.21.01-.2.04-.2.07-.18.09-.18.12-.15.13-8.19 8.19-.12.14-.11.16-.09.17-.07.18-.04.18-.03.19v.2l.03.19.04.18.07.18.09.17.11.16z", 1.46f, false, Cap::Butt, Join::Round },
        { L"m5.3 12 1.67 1.68L8.65 12l-1.68-1.68Zm3.35 3.35 1.67 1.68L12 15.35l-1.68-1.67Zm0-3.35 1.67 1.68L12 12l-1.68-1.68ZM12 15.35l1.68 1.68 1.67-1.68-1.67-1.67Zm-3.35-6.7 1.67 1.67L12 8.65l-1.68-1.68ZM12 12l1.68 1.68L15.35 12l-1.67-1.68Zm0-3.35 1.68 1.67 1.67-1.67-1.67-1.68ZM15.35 12l1.68 1.68L18.7 12l-1.67-1.68Z", 0.0f, false, Cap::Butt, Join::Miter },
    };
    // blur-rect —— 来自 本地设计 blur-rect
    inline constexpr IconOp kIcon7[] = {
        { L"m2.17 13.64 8.19 8.19.21.18.23.16.25.13.26.1.27.07.28.03h.28l.28-.03.27-.07.26-.1.25-.13.23-.16.21-.18 8.19-8.19.18-.21.16-.23.13-.25.1-.26.07-.27.03-.28v-.28l-.03-.28-.07-.27-.1-.26-.13-.25-.16-.23-.18-.21-8.19-8.19-.21-.18-.23-.16-.25-.13-.26-.1-.27-.07-.28-.03h-.28l-.28.03-.27.07-.26.1-.25.13-.23.16-.21.18-8.19 8.19-.18.21-.16.23-.13.25-.1.26-.07.27-.03.28v.28l.03.28.07.27.1.26.13.25.16.23zM5.3 12l1.67 1.68L8.65 12l-1.68-1.68Zm3.35 3.35 1.67 1.68L12 15.35l-1.68-1.67Zm0-3.35 1.67 1.68L12 12l-1.68-1.68ZM12 15.35l1.68 1.68 1.67-1.68-1.67-1.67Zm-3.35-6.7 1.67 1.67L12 8.65l-1.68-1.68ZM12 12l1.68 1.68L15.35 12l-1.67-1.68Zm0-3.35 1.68 1.67 1.67-1.67-1.67-1.68ZM15.35 12l1.68 1.68L18.7 12l-1.67-1.68Z", 0.0f, true, Cap::Butt, Join::Miter },
    };
    // eraser-brush —— 来自 本地设计 eraser-brush
    inline constexpr IconOp kIcon8[] = {
        { L"m7.23 20.59-4.1-4.1a2.3 2.3 0 0 1 0-3.24l9.16-9.16a2.3 2.3 0 0 1 3.24 0l5.34 5.34a2.3 2.3 0 0 1 0 3.24l-7.92 7.92z", 1.89f, false, Cap::Round, Join::Round },
        { L"M21.54 20.59H7.23", 1.89f, false, Cap::Round, Join::Round },
        { L"m5.32 11.05 8.59 8.59", 1.89f, false, Cap::Round, Join::Round },
    };
    // eraser-rect —— 来自 本地设计 eraser-rect
    inline constexpr IconOp kIcon9[] = {
        { L"m7.23 20.59-4.1-4.1a2.3 2.3 0 0 1 0-3.24l9.16-9.16a2.3 2.3 0 0 1 3.24 0l5.34 5.34a2.3 2.3 0 0 1 0 3.24l-7.92 7.92z", 1.89f, false, Cap::Round, Join::Round },
        { L"M21.54 20.59H7.23", 1.89f, false, Cap::Round, Join::Round },
        { L"m5.32 11.05 8.59 8.59", 1.89f, false, Cap::Round, Join::Round },
        { L"m5.32 11.05 6.97-6.97a2.3 2.3 0 0 1 3.24 0l5.34 5.34a2.3 2.3 0 0 1 0 3.24l-6.96 6.97z", 0.0f, false, Cap::Butt, Join::Miter },
    };
    // mosaic-brush —— 来自 本地设计 mosaic-outline
    inline constexpr IconOp kIcon10[] = {
        { L"M192 104.32H832A87.68 87.68 0 0 1 919.68 192V832A87.68 87.68 0 0 1 832 919.68H192A87.68 87.68 0 0 1 104.32 832V192A87.68 87.68 0 0 1 192 104.32Z", 80.64f, false, Cap::Butt, Join::Round },
        { L"M250 250h131v131H250z", 0.0f, false, Cap::Butt, Join::Miter },
        { L"M512 250h131v131H512z", 0.0f, false, Cap::Butt, Join::Miter },
        { L"M381 381h131v131H381z", 0.0f, false, Cap::Butt, Join::Miter },
        { L"M643 381h131v131H643z", 0.0f, false, Cap::Butt, Join::Miter },
        { L"M250 512h131v131H250z", 0.0f, false, Cap::Butt, Join::Miter },
        { L"M512 512h131v131H512z", 0.0f, false, Cap::Butt, Join::Miter },
        { L"M381 643h131v131H381z", 0.0f, false, Cap::Butt, Join::Miter },
        { L"M643 643h131v131H643z", 0.0f, false, Cap::Butt, Join::Miter },
    };
    // mosaic-rect —— 来自 本地设计 mosaic-solid
    inline constexpr IconOp kIcon11[] = {
        { L"M192 64h640a128 128 0 0 1 128 128v640a128 128 0 0 1-128 128H192A128 128 0 0 1 64 832V192A128 128 0 0 1 192 64m58 186h131v131H250Zm262 0h131v131H512ZM381 381h131v131H381Zm262 0h131v131H643ZM250 512h131v131H250Zm262 0h131v131H512ZM381 643h131v131H381Zm262 0h131v131H643Z", 0.0f, true, Cap::Butt, Join::Miter },
    };
    // shape-arrow-outline —— 来自 shape-arrow-solid的描边
    inline constexpr IconOp kIcon12[] = {
        { L"M5.1 22.5L16.3 11.3L20.1 15L22.5 1.5L9 3.9L12.7 7.7L1.5 18.9ZM14 7.7L10.9 4.5L21.3 2.7L19.5 13.1L16.3 10L5.1 21.2L2.8 18.9Z", 0.0f, true, Cap::Butt, Join::Miter },
    };
    // shape-arrow-solid —— 来自 本地设计 shape-arrow-solid
    inline constexpr IconOp kIcon13[] = {
        { L"M1.48 18.89 12.69 7.68 8.96 3.94l13.56-2.46-2.46 13.56-3.74-3.73L5.11 22.52Z", 0.0f, false, Cap::Butt, Join::Miter },
    };
    // shape-ellipse-outline —— 来自 本地设计 shape-ellipse-outline
    inline constexpr IconOp kIcon14[] = {
        { L"M3 12A9 7 0 1 0 21 12A9 7 0 1 0 3 12Z", 1.89f, false, Cap::Round, Join::Round },
    };
    // shape-ellipse-solid —— 来自 shape-ellipse-outline的外轮廓填充
    inline constexpr IconOp kIcon15[] = {
        { L"M2 11.9L2 12L2 12.1L2.1 13L2.1 13.1L2.3 14L2.4 14L2.4 14.1L2.7 14.9L2.8 15L2.8 15.1L3.3 15.9L3.4 16L4 16.7L4.1 16.8L4.8 17.5L4.9 17.6L5.7 18.2L5.8 18.2L6.7 18.8L6.8 18.8L7.8 19.2L7.9 19.3L8.9 19.6L9 19.6L10.1 19.8L10.2 19.8L11.3 19.9L11.4 20L12.6 20L12.7 19.9L13.8 19.8L13.9 19.8L15 19.6L15.1 19.6L16.1 19.3L16.2 19.2L17.2 18.8L17.3 18.8L18.2 18.2L18.3 18.2L19.1 17.6L19.2 17.5L19.9 16.8L20 16.7L20.6 16L20.7 15.9L21.2 15.1L21.2 15L21.3 14.9L21.6 14.1L21.6 14L21.7 14L21.9 13.1L21.9 13L22 12.1L22 12L22 11.9L21.9 11L21.9 10.9L21.7 10L21.6 10L21.6 9.9L21.3 9.1L21.2 9L21.2 8.9L20.7 8.1L20.6 8L20 7.3L19.9 7.2L19.2 6.5L19.1 6.4L18.3 5.8L18.2 5.8L17.3 5.2L17.2 5.2L16.2 4.8L16.1 4.7L15.1 4.4L15 4.4L13.9 4.2L13.8 4.2L12.7 4.1L12.6 4L11.4 4L11.3 4.1L10.2 4.2L10.1 4.2L9 4.4L8.9 4.4L7.9 4.7L7.8 4.8L6.8 5.2L6.7 5.2L5.8 5.8L5.7 5.8L4.9 6.4L4.8 6.5L4.1 7.2L4 7.3L3.4 8L3.3 8.1L2.8 8.9L2.8 9L2.7 9.1L2.4 9.9L2.4 10L2.3 10L2.1 10.9L2.1 11Z", 0.0f, false, Cap::Butt, Join::Miter },
    };
    // 空心圆序号（副条圆序号按钮的空心态）与主条 tool-number 别名共用这一份几何：
    // 空心圆环 + 数字 1
    inline constexpr IconOp kIcon16[] = {
        { L"M3 12A9 9 0 1 0 21 12A9 9 0 1 0 3 12Z", 1.89f, false, Cap::Round, Join::Round },
        { L"M9.3 8.3 12.5 6v12", 1.89f, false, Cap::Round, Join::Round },
    };
    // 实心圆序号 —— 空心圆序号（kIcon16）的反白填充
    inline constexpr IconOp kIcon17[] = {
        { L"M2 12.1L2 13.2L2 13.3L2.3 14.4L2.3 14.6L2.6 15.6L2.7 15.8L3.2 16.8L3.2 16.9L3.8 17.9L3.9 18L4.6 18.8L4.7 18.9L5.5 19.7L5.6 19.8L6.6 20.4L6.7 20.5L7.7 21.1L7.8 21.1L8.8 21.5L9 21.6L10.1 21.9L10.2 21.9L11.3 22L11.4 22L12.6 22L12.7 22L13.8 21.9L13.9 21.9L15 21.6L15.2 21.5L16.2 21.1L16.3 21.1L17.3 20.5L17.4 20.4L18.4 19.8L18.5 19.7L19.3 18.9L19.4 18.8L20.1 18L20.2 17.8L20.8 16.9L20.8 16.8L21.3 15.8L21.4 15.6L21.7 14.6L21.7 14.4L22 13.3L22 13.2L22 12.1L22 11.9L22 10.8L22 10.7L21.7 9.6L21.7 9.4L21.4 8.4L21.3 8.2L20.8 7.2L20.8 7.1L20.2 6.2L20.1 6L19.4 5.2L19.3 5.1L18.5 4.3L18.4 4.2L17.4 3.6L17.3 3.5L16.3 2.9L16.2 2.9L15.2 2.5L15 2.4L13.9 2.1L13.8 2.1L12.7 2L12.6 2L11.4 2L11.3 2L10.2 2.1L10.1 2.1L9 2.4L8.8 2.5L7.8 2.9L7.7 2.9L6.7 3.5L6.6 3.6L5.6 4.2L5.5 4.3L4.7 5.1L4.6 5.2L3.9 6L3.8 6.1L3.2 7.1L3.2 7.2L2.7 8.2L2.6 8.4L2.3 9.4L2.3 9.6L2 10.7L2 10.8L2 11.9L2 12ZM9.9 9.1L9.8 9.1L9.7 9.2L9.6 9.2L9.5 9.2L9.4 9.2L9.3 9.2L9.2 9.2L9.1 9.2L9 9.2L8.9 9.1L8.8 9.1L8.7 9.1L8.7 9L8.6 8.9L8.5 8.9L8.5 8.8L8.4 8.7L8.4 8.6L8.4 8.5L8.4 8.4L8.4 8.3L8.4 8.2L8.4 8.1L8.4 8L8.5 7.9L8.5 7.8L8.5 7.7L8.6 7.7L8.7 7.6L8.7 7.5L11.9 5.2L12 5.2L12.1 5.1L12.2 5.1L12.3 5.1L12.4 5.1L12.5 5.1L12.6 5.1L12.7 5.1L12.8 5.1L12.9 5.2L13 5.2L13.1 5.3L13.2 5.3L13.2 5.4L13.3 5.5L13.4 5.6L13.4 5.7L13.4 5.8L13.4 5.9L13.4 6L13.4 18L13.4 18.1L13.4 18.2L13.4 18.3L13.4 18.4L13.3 18.4L13.3 18.5L13.2 18.6L13.2 18.7L13.1 18.7L13 18.8L12.9 18.8L12.9 18.9L12.8 18.9L12.7 18.9L12.6 18.9L12.5 18.9L12.4 18.9L12.3 18.9L12.2 18.9L12.1 18.9L12.1 18.8L12 18.8L11.9 18.7L11.8 18.7L11.8 18.6L11.7 18.5L11.7 18.4L11.6 18.4L11.6 18.3L11.6 18.2L11.6 18.1L11.6 18L11.6 7.8Z", 0.0f, true, Cap::Butt, Join::Miter },
    };
    // shape-rect-outline —— 来自 本地设计 shape-rect-outline
    inline constexpr IconOp kIcon18[] = {
        { L"M5 5H19A2 2 0 0 1 21 7V17A2 2 0 0 1 19 19H5A2 2 0 0 1 3 17V7A2 2 0 0 1 5 5Z", 1.89f, false, Cap::Round, Join::Round },
    };
    // shape-rect-solid —— 来自 shape-rect-outline的外轮廓填充
    inline constexpr IconOp kIcon19[] = {
        { L"M5 4L4.9 4L4.7 4.1L4.6 4.1L4.3 4.1L4.2 4.1L4 4.2L3.9 4.2L3.7 4.3L3.6 4.4L3.4 4.5L3.3 4.6L3.1 4.7L3 4.8L2.8 5L2.7 5.1L2.6 5.3L2.5 5.4L2.4 5.6L2.3 5.7L2.2 5.9L2.2 6L2.1 6.2L2.1 6.3L2.1 6.6L2.1 6.7L2 6.9L2 7L2 17L2 17.1L2.1 17.3L2.1 17.4L2.1 17.7L2.1 17.8L2.2 18L2.2 18.1L2.3 18.3L2.4 18.4L2.5 18.6L2.6 18.7L2.7 18.9L2.8 19L3 19.2L3.1 19.3L3.3 19.4L3.4 19.5L3.6 19.6L3.7 19.7L3.9 19.8L4 19.8L4.2 19.9L4.3 19.9L4.6 19.9L4.7 19.9L4.9 20L5 20L19 20L19.1 20L19.3 19.9L19.4 19.9L19.7 19.9L19.8 19.9L20 19.8L20.1 19.8L20.3 19.7L20.4 19.6L20.6 19.5L20.7 19.4L20.9 19.3L21 19.2L21.2 19L21.3 18.9L21.4 18.7L21.5 18.6L21.6 18.4L21.7 18.3L21.8 18.1L21.8 18L21.9 17.8L21.9 17.7L21.9 17.4L21.9 17.3L22 17.1L22 17L22 7L22 6.9L21.9 6.7L21.9 6.6L21.9 6.3L21.9 6.2L21.8 6L21.8 5.9L21.7 5.7L21.6 5.6L21.5 5.4L21.4 5.3L21.3 5.1L21.2 5L21 4.8L20.9 4.7L20.7 4.6L20.6 4.5L20.4 4.4L20.3 4.3L20.1 4.2L20 4.2L19.8 4.1L19.7 4.1L19.4 4.1L19.3 4.1L19.1 4L19 4Z", 0.0f, false, Cap::Butt, Join::Miter },
    };
    // text-bold —— 来自 字形轮廓
    inline constexpr IconOp kIcon20[] = {
        { L"M188.58 102.4 L554.63 102.4 L576.95 102.95 L598.31 104.61 L618.72 107.36 L638.18 111.22 L656.68 116.18 L674.23 122.25 L690.82 129.42 L706.46 137.69 L721.14 147.06 L734.87 157.54 L747.46 168.98 L758.72 181.24 L768.66 194.33 L777.28 208.25 L784.56 222.99 L790.53 238.55 L795.17 254.94 L798.48 272.15 L800.47 290.19 L801.13 309.05 L800.76 322.59 L799.65 335.78 L797.79 348.61 L795.2 361.1 L791.86 373.23 L787.78 385.01 L782.97 396.43 L777.41 407.51 L771.1 418.23 L764.06 428.6 L756.23 437.25 L747.94 445.43 L739.18 453.12 L729.96 460.33 L720.28 467.06 L710.13 473.3 L699.52 479.07 L688.44 484.35 L676.91 489.15 L664.9 493.47 L680.33 496.64 L695.1 500.58 L709.21 505.31 L722.66 510.81 L735.45 517.1 L747.58 524.16 L759.06 532 L769.88 540.62 L780.04 550.01 L789.55 560.19 L798.26 571.03 L806.06 582.43 L812.94 594.38 L818.9 606.89 L823.95 619.96 L828.08 633.58 L831.29 647.76 L833.58 662.5 L834.96 677.79 L835.42 693.63 L834.56 716.94 L832.01 739.04 L827.74 759.94 L821.78 779.63 L814.1 798.12 L804.72 815.4 L793.64 831.48 L780.85 846.35 L766.36 860.02 L750.16 872.49 L735 881.82 L718.8 890.17 L701.57 897.53 L683.29 903.92 L663.98 909.32 L643.63 913.74 L622.24 917.18 L599.81 919.64 L576.35 921.11 L551.85 921.6 L188.58 921.6 Z M283.11 182.1 L283.11 461.03 L531.46 461.03 L547.95 460.67 L563.67 459.57 L578.64 457.74 L592.85 455.17 L606.29 451.88 L618.98 447.85 L630.9 443.1 L642.07 437.6 L652.48 431.38 L662.12 424.43 L670.93 416.78 L678.81 408.49 L685.76 399.55 L691.78 389.95 L696.88 379.71 L701.05 368.83 L704.29 357.29 L706.61 345.1 L708 332.27 L708.46 318.78 L708.02 305.33 L706.7 292.58 L704.5 280.53 L701.42 269.19 L697.46 258.55 L692.61 248.61 L686.89 239.38 L680.29 230.86 L672.81 223.04 L664.44 215.92 L655.2 209.49 L645.07 203.74 L634.07 198.67 L622.18 194.27 L609.42 190.55 L595.77 187.51 L581.25 185.14 L565.84 183.45 L549.55 182.43 L532.39 182.1 Z M283.11 538.87 L283.11 841.9 L544.43 841.9 L560.64 841.59 L576.28 840.64 L591.33 839.07 L605.82 836.86 L619.73 834.03 L633.06 830.56 L645.82 826.47 L658.01 821.74 L669.62 816.38 L680.66 810.4 L692.28 802.39 L702.68 793.55 L711.85 783.89 L719.8 773.4 L726.53 762.09 L732.03 749.96 L736.32 737 L739.37 723.21 L741.21 708.6 L741.82 693.17 L741.29 677.73 L739.71 663.09 L737.07 649.26 L733.37 636.23 L728.62 624.01 L722.8 612.6 L715.94 602 L708.01 592.2 L699.04 583.2 L689 575.02 L678.71 568.15 L667.5 562 L655.36 556.58 L642.29 551.89 L628.3 547.91 L613.38 544.66 L597.53 542.13 L580.76 540.32 L563.06 539.24 L544.43 538.87 Z", 0.0f, false, Cap::Butt, Join::Miter },
    };
    // text-italic —— 来自 字形轮廓
    inline constexpr IconOp kIcon21[] = {
        { L"M731.07 102.4 L838.76 102.4 L877.42 921.6 L775.25 921.6 L768.81 693.33 L425.48 693.33 L260.72 921.6 L146.58 921.6 Z M477.02 610.49 L768.81 610.49 L755.92 203.65 L749.48 203.65 Z", 0.0f, false, Cap::Butt, Join::Miter },
    };
    // tool-line —— 来自 本地设计 tool-line
    inline constexpr IconOp kIcon22[] = {
        { L"M16.81 3.33a2.72 2.72 0 1 1 3.85 3.85l-13 13.01-5.29 1.44 1.45-5.3z", 1.89f, false, Cap::Round, Join::Round },
        { L"m14.89 5.26 3.85 3.85", 1.89f, false, Cap::Round, Join::Round },
    };
    // tool-line-solid —— 来自 本地设计 tool-line-solid
    inline constexpr IconOp kIcon23[] = {
        { L"M16.81 3.33a2.72 2.72 0 1 1 3.85 3.85l-13 13.01-5.29 1.44 1.45-5.3z", 1.89f, false, Cap::Round, Join::Round },
        { L"m14.89 5.26 3.85 3.85", 1.89f, false, Cap::Round, Join::Round },
        { L"m14.89 5.26 3.85 3.85L7.66 20.19l-5.29 1.44 1.44-5.29Z", 0.0f, false, Cap::Butt, Join::Miter },
    };
    // tool-rect —— 来自 本地设计 tool-rect
    inline constexpr IconOp kIcon24[] = {
        { L"M5 5H19A2 2 0 0 1 21 7V17A2 2 0 0 1 19 19H5A2 2 0 0 1 3 17V7A2 2 0 0 1 5 5Z", 1.89f, false, Cap::Round, Join::Round },
    };
    // tool-text —— 来自 本地设计 tool-text
    inline constexpr IconOp kIcon25[] = {
        { L"M4 7V5h16v2", 1.89f, false, Cap::Round, Join::Round },
        { L"M12 5v14", 1.89f, false, Cap::Round, Join::Round },
        { L"M9 19h6", 1.89f, false, Cap::Round, Join::Round },
    };
    // ui-check —— 来自 本地设计 ui-check
    inline constexpr IconOp kIcon26[] = {
        { L"m20 6.5-11 11-5-5", 1.89f, false, Cap::Round, Join::Round },
    };

    // action-video —— 录屏（机身 + 镜头）
    inline constexpr IconOp kIcon27[] = {
        { L"M5 5H13A2 2 0 0 1 15 7V17A2 2 0 0 1 13 19H5A2 2 0 0 1 3 17V7A2 2 0 0 1 5 5Z", 1.89f, false, Cap::Round, Join::Round },
        { L"M15 10L21 7V17L15 14Z", 1.89f, false, Cap::Round, Join::Round },
    };
    // video-mic —— 麦克风
    inline constexpr IconOp kIcon28[] = {
        { L"M12 4A2.5 2.5 0 0 1 14.5 6.5V11.5A2.5 2.5 0 0 1 9.5 11.5V6.5A2.5 2.5 0 0 1 12 4Z", 1.89f, false, Cap::Round, Join::Round },
        { L"M6.5 11A5.5 5.5 0 0 0 17.5 11", 1.89f, false, Cap::Round, Join::Round },
        { L"M12 16.5V20", 1.89f, false, Cap::Round, Join::Round },
        { L"M9 20H15", 1.89f, false, Cap::Round, Join::Round },
    };
    // video-record —— 圆环 + 实心圆点（开始录制），字形取自 r21 的 record-start
    // 圆用两段半圆弧拼：起点与终点重合的"整圆弧"（A r r 0 1 0 起点 x）在算圆心时
    // 有两个候选、且弦长趋近 0，解析器一旦挑中另一个圆心就画成贴边的一条小弧，
    // 看上去就是"图标偏到了图标盒顶上"。两段半圆没有这种歧义，圆必定落在盒心
    inline constexpr IconOp kIcon29[] = {
        { L"M117.76 512A394.24 394.24 0 0 1 906.24 512A394.24 394.24 0 0 1 117.76 512Z", 80.64f, false, Cap::Round, Join::Round },
        { L"M364.16 512A147.84 147.84 0 0 1 659.84 512A147.84 147.84 0 0 1 364.16 512Z", 0.0f, false, Cap::Butt, Join::Miter },
    };
    // video-speaker —— 扬声器（系统声音）
    inline constexpr IconOp kIcon30[] = {
        { L"M4 9H7L11 6V18L7 15H4Z", 1.89f, false, Cap::Round, Join::Round },
        { L"M15 8.5A5 5 0 0 1 15 15.5", 1.89f, false, Cap::Round, Join::Round },
        { L"M17.5 6A8.5 8.5 0 0 1 17.5 18", 1.89f, false, Cap::Round, Join::Round },
    };
    // recording-start —— 圆环 + 实心圆点。与 video-record 同一份几何，
    // 只是名字用图标包里的语义名；主条"录制"入口用它
    // recording-resume —— 圆环 + 实心三角（开始录制）；副条那颗开始按钮用它
    inline constexpr IconOp kIcon31[] = {
        { L"M117.76 512A394.24 394.24 0 0 1 906.24 512A394.24 394.24 0 0 1 117.76 512Z", 80.64f, false, Cap::Round, Join::Round },
        { L"M413.44 330.56L691.2 512L413.44 693.44V330.56Z", 0.0f, false, Cap::Butt, Join::Miter },
    };
    // recording-microphone —— 麦克风（录音开）
    // 机身是上下两个半圆拼成的胶囊：整圆的 A 弧有两个圆心候选，解析器可能挑错，
    // 与 video-record 里同样的处理 —— 拆成半圆，圆心唯一
    inline constexpr IconOp kIcon32[] = {
        { L"M512 81.92A125.44 125.44 0 0 1 637.44 207.36V494.08A125.44 125.44 0 0 1 512 619.52A125.44 125.44 0 0 1 386.56 494.08V207.36A125.44 125.44 0 0 1 512 81.92Z", 80.64f, false, Cap::Round, Join::Round },
        { L"M243.2 467.2v26.88a268.8 268.8 0 0 0 537.6 0v-26.88M512 762.88v152.32M332.8 915.2h358.4", 80.64f, false, Cap::Round, Join::Round },
    };
    // recording-microphone-off —— 麦克风（录音关）：机身 + 一条斜杠。
    // 设计稿用 clip-path 在斜杠两侧给机身留了缺口；这里直接用整条斜杠盖上去，
    // 少了那圈缺口，但一眼仍是"麦克风被划掉"，实现只用三条路径
    inline constexpr IconOp kIcon33[] = {
        { L"M512 81.92A125.44 125.44 0 0 1 637.44 207.36V494.08A125.44 125.44 0 0 1 512 619.52A125.44 125.44 0 0 1 386.56 494.08V207.36A125.44 125.44 0 0 1 512 81.92Z", 80.64f, false, Cap::Round, Join::Round },
        { L"M243.2 467.2v26.88a268.8 268.8 0 0 0 537.6 0v-26.88M512 762.88v152.32M332.8 915.2h358.4", 80.64f, false, Cap::Round, Join::Round },
        { L"M243.2 243.2l537.6 537.6", 80.64f, false, Cap::Round, Join::Round },
    };
    // trash —— 垃圾桶（录制条的"丢弃"用它，原来用的是关闭的叉）
    inline constexpr IconOp kIcon34[] = {
        { L"M126.72 252.16h770.56", 80.64f, false, Cap::Round, Join::Round },
        { L"M341.76 252.16v-107.52h340.48v107.52", 80.64f, false, Cap::Round, Join::Round },
        { L"M216.32 252.16l38.08 636.16a53.76 53.76 0 0 0 53.76 49.28h407.68a53.76 53.76 0 0 0 53.76-49.28l38.08-636.16", 80.64f, false, Cap::Round, Join::Round },
        { L"M413.44 404.48v367.36M610.56 404.48v367.36", 80.64f, false, Cap::Round, Join::Round },
    };
    // quick-save —— 闪电 + 底座（保存，比 action-save 的软盘更贴近"立刻落盘"）
    inline constexpr IconOp kIcon35[] = {
        { L"M428.28 72.41Q431.36 64 440.32 64L718.08 64Q727.04 64 723.96 72.41L631.56 324.39Q628.48 332.8 637.44 332.8L790.4 332.8Q799.36 332.8 792.57 339.32L365.83 792.2Q359.68 799.36 361.72 790L420.36 538.64Q422.4 530.24 413.44 530.24L270.08 530.24Q261.12 530.24 264.2 521.51Z", 0.0f, true, Cap::Butt, Join::Miter },
        { L"M482.07 144.1Q485.12 135.68 494.08 135.68L615.04 135.68Q623.36 135.68 620.92 144.09L528.52 396.07Q525.44 404.48 534.4 404.48L623.36 404.48Q632.32 404.48 626.81 411L491.27 554.76Q485.12 561.28 487.38 552.61L509.74 466.91Q512 458.24 502.88 458.24L377.6 458.24Q368.64 458.24 371.69 449.82Z", 0.0f, true, Cap::Butt, Join::Miter },
        { L"M99.84 673.28h62.72v206.08h698.88V673.28h62.72q8.96 0 8.96 8.96v232.96q0 44.8-44.8 44.8H135.68q-44.8 0-44.8-44.8V682.24q0-8.96 8.96-8.96Z", 0.0f, false, Cap::Butt, Join::Miter },
    };

    // shape-line —— 图标包里的 tool-line：一条斜的实心长条（1024 视窗、纯填充），
    // 和画笔那颗铅笔字形区分开，不会再看成同一个图标
    inline constexpr IconOp kIcon36[] = {
        { L"M807.68 132.32L132.32 807.68A8.96 8.96 0 0 0 132.32 820L177.12 864.8A8.96 8.96 0 0 0 189.44 864.8L864.8 189.44A8.96 8.96 0 0 0 864.8 177.12L820 132.32A8.96 8.96 0 0 0 807.68 132.32Z", 0.0f, false, Cap::Butt, Join::Miter },
    };
    // shape-circle-solid —— 正圆实心点（手绘档）。直径取 2/3 显得更小一圈，与其它形状图标留白一致
    inline constexpr IconOp kIcon37[] = {
        { L"M213.33 512A298.67 298.67 0 0 1 810.67 512A298.67 298.67 0 0 1 213.33 512Z", 0.0f, false, Cap::Butt, Join::Miter },
    };
    // tool-arrow-straight —— 斜线 + 直角箭头，取自 arrow-type-straight（1024 视窗）
    inline constexpr IconOp kIcon38[] = {
        { L"M207.36 816.64L816.64 207.36", 80.64f, false, Cap::Round, Join::Round },
        { L"M512 207.36H816.64V512", 80.64f, false, Cap::Round, Join::Round },
    };
    // shape-number-square-outline —— 空心圆角方框 + 数字 1，序号"空心方序号"按钮用它（24 视窗）
    inline constexpr IconOp kIcon39[] = {
        { L"M5.9 3.5H18.1A2.4 2.4 0 0 1 20.5 5.9V18.1A2.4 2.4 0 0 1 18.1 20.5H5.9A2.4 2.4 0 0 1 3.5 18.1V5.9A2.4 2.4 0 0 1 5.9 3.5Z", 1.5f, false, Cap::Round, Join::Round },
        { L"M10.5 9.25L12.75 7.5V16.5", 1.75f, false, Cap::Round, Join::Round },
    };
    // shape-number-square-solid —— 实心圆角方框，数字 1 挖空，序号"实心方序号"按钮用它（外框 + 数字洞）
    inline constexpr IconOp kIcon40[] = {
        { L"M5.55 2.75H18.45A2.8 2.8 0 0 1 21.25 5.55V18.45A2.8 2.8 0 0 1 18.45 21.25H5.55A2.8 2.8 0 0 1 2.75 18.45V5.55A2.8 2.8 0 0 1 5.55 2.75ZM9.55 9.4L12.55 7.2H13.55V16.55H11.95V8.55L9.55 10.75Z", 0.0f, true, Cap::Butt, Join::Miter },
    };
    // tool-smart-erase —— 两颗四角星：大星表示"被抹掉的那一块"，小星表示自动补出来的内容
    inline constexpr IconOp kIcon41[] = {
        { L"M9 2.5L11.05 6.95L15.5 9L11.05 11.05L9 15.5L6.95 11.05L2.5 9L6.95 6.95Z M18 13L19.05 15.95L22 17L19.05 18.05L18 21L16.95 18.05L14 17L16.95 15.95Z", 1.9f, false, Cap::Round, Join::Round },
    };

    inline constexpr IconDef kIcons[] = {
        { 0xE712, L"action-close", 24.0f, kIcon0, 1 },
        { 0xE72E, L"action-copy", 24.0f, kIcon1, 2 },
        { 0xE714, L"action-pin", 24.0f, kIcon2, 2 },
        { 0xED8A, L"action-redo", 24.0f, kIcon3, 2 },
        { 0xE608, L"action-save", 24.0f, kIcon4, 3 },
        { 0xED85, L"action-undo", 24.0f, kIcon5, 2 },
        { 0xE9F0, L"action-video", 24.0f, kIcon27, 2 },
        { 0xE71E, L"blur-brush", 24.0f, kIcon6, 2 },
        { 0xE71F, L"blur-rect", 24.0f, kIcon7, 1 },
        { 0xE70C, L"eraser-brush", 24.0f, kIcon8, 3 },
        { 0xE70D, L"eraser-rect", 24.0f, kIcon9, 4 },
        { 0xE70A, L"mosaic-brush", 1024.0f, kIcon10, 9 },
        { 0xE70B, L"mosaic-rect", 1024.0f, kIcon11, 1 },
        { 0xE702, L"shape-arrow-outline", 24.0f, kIcon12, 1 },
        { 0xE703, L"shape-arrow-solid", 24.0f, kIcon13, 1 },
        { 0xE71C, L"shape-ellipse-outline", 24.0f, kIcon14, 1 },
        { 0xE71D, L"shape-ellipse-solid", 24.0f, kIcon15, 1 },
        { 0, L"shape-line", 1024.0f, kIcon36, 1 },
        { 0, L"shape-circle-solid", 1024.0f, kIcon37, 1 },
        { 0, L"tool-arrow-straight", 1024.0f, kIcon38, 2 },
        { 0xE776, L"shape-number-circle-outline", 24.0f, kIcon16, 2 },
        { 0xE605, L"shape-number-circle-solid", 24.0f, kIcon17, 1 },
        { 0, L"shape-number-square-outline", 24.0f, kIcon39, 2 },
        { 0, L"shape-number-square-solid", 24.0f, kIcon40, 1 },
        { 0xE71A, L"shape-rect-outline", 24.0f, kIcon18, 1 },
        { 0xE71B, L"shape-rect-solid", 24.0f, kIcon19, 1 },
        { 0xE634, L"text-bold", 1024.0f, kIcon20, 1 },
        { 0xE682, L"text-italic", 1024.0f, kIcon21, 1 },
        { 0xE706, L"tool-line", 24.0f, kIcon22, 2 },
        { 0xE70F, L"tool-line-solid", 24.0f, kIcon23, 3 },
        { 0xE72F, L"tool-rect", 24.0f, kIcon24, 1 },
        { 0xE709, L"tool-text", 24.0f, kIcon25, 3 },
        { 0xE6AD, L"ui-check", 24.0f, kIcon26, 1 },
        { 0xE9F1, L"video-mic", 24.0f, kIcon28, 4 },
        { 0xE9F2, L"video-record", 1024.0f, kIcon29, 2 },
        { 0xE9F3, L"video-speaker", 24.0f, kIcon30, 3 },
        // —— 录制相关：码位一律 0（纯别名，仅按名字查）——
        { 0, L"recording-start", 1024.0f, kIcon29, 2 },   // 与 video-record 同一份几何
        { 0, L"recording-resume", 1024.0f, kIcon31, 2 },
        { 0, L"recording-microphone", 1024.0f, kIcon32, 2 },
        { 0, L"recording-microphone-off", 1024.0f, kIcon33, 3 },
        { 0, L"trash", 1024.0f, kIcon34, 4 },
        { 0, L"quick-save", 1024.0f, kIcon35, 3 },
        { 0, L"tool-arrow", 24.0f, kIcon13, 1 },   // 别名 → shape-arrow-solid
        { 0, L"tool-eraser", 24.0f, kIcon9, 4 },   // 别名 → eraser-rect
        { 0, L"tool-mosaic", 1024.0f, kIcon10, 9 },   // 别名 → mosaic-brush
        { 0, L"tool-number", 24.0f, kIcon16, 2 },   // 序号主按钮（空心圆 + 数字1），与副条"空心圆序号"共用几何
        { 0, L"tool-smart-erase", 24.0f, kIcon41, 1 },
    };

    // 线性查表：一共几十项，比哈希更省事，也无需静态初始化。
    inline const IconDef* find(wchar_t code) {
        if (!code) return nullptr;            // 0 是纯别名的占位码位
        for (const auto& ic : kIcons) if (ic.code == code) return &ic;
        return nullptr;
    }

    inline const IconDef* findName(const wchar_t* name) {
        if (!name) return nullptr;
        for (const auto& ic : kIcons) {
            const wchar_t* a = ic.name;
            const wchar_t* b = name;
            while (*a && *a == *b) { ++a; ++b; }
            if (*a == *b) return &ic;
        }
        return nullptr;
    }

}  // namespace ui::icons
