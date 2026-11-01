#pragma once
#include <string>
#include <wrl.h>
#include <d2d1_1.h>
#include <dwrite_3.h>
#include "Node.h"
#include "Color.h"

namespace ui {
    class WinBase;
    class Canvas;

    // 多行文本输入控件。
    //
    // 布局：尺寸由外部 flex / setWidth/setHeight 决定，高度固定不随内容增长；
    //       内容超出可视高度时可滚轮滚动，光标移动时自动滚入可视区。
    //       文本按内容区宽度自动换行（DWrite 负责折行），所以没有横向滚动。
    //
    // 外观：背景 / 边框 / 圆角直接用 Node 现成的 setBg / setBorder / setBorderRadius；
    //       内边距用 Node::setPadding，文本区域即 padding 以内的部分。
    //
    // 焦点：控件自己维护焦点态 —— 点在自己身上聚焦，点在别处或宿主窗口失焦时失焦。
    //       只有聚焦时才画光标、才响应键盘。同一窗口内可以放多个 TextBox。
    class TextBox : public Node
    {
    public:
        TextBox(WinBase* win);
        ~TextBox();

        void setText(const std::wstring& val);
        std::wstring getText();
        void setFontSize(float val);
        void setFontFamily(const std::wstring& val);
        void setBold(bool val);
        void setItalic(bool val);
        // 自适应尺寸：文本不折行，控件宽高跟着文本内容长。
        // 开启后 setWidth/setHeight 设的尺寸会被覆盖，也不会再出现滚动条（内容永远装得下）。
        void setAutoSize(bool val);
        // 垂直居中：文本总高度小于内容区时，绘制位置下移使其视觉上居中。
        // 文本溢出时此设置无效（正常滚动）。
        void setVerticalCenter(bool val);
        // 文字颜色。未单独设置光标色时，光标也用这个颜色。
        void setColor(Color color);
        void setCaretColor(Color color);
        void setSelectionBgColor(Color color);
        // 占位符：仅在文本为空时显示，不参与编辑。
        void setPlaceholder(const std::wstring& val);
        void setPlaceholderColor(Color color);

        void selectAll();
        bool isFocused() const;
        // 主动聚焦 / 结束编辑。控件自己也会在点击、ESC、宿主窗口失焦时调它们。
        void focus();
        void blur();
        // 下一次按下归调用方（标注上的四角/旋转柄）：不抢焦点、不捕获鼠标、不动光标。
        // 那些手柄压在文本框的角上/框外，按下时若让 TextBox 自己接管，光标会被挪走、
        // 拖选也会跟着跑起来（见 ShapeText::mouseDown）
        void swallowNextDown();
    public:
        // 文本被编辑后触发（setText 也会触发）。
        winrt::event<winrt::delegate<TextBox*, const std::wstring&>> onTextChanged;
        // 焦点态变化后触发。宿主可以据此接管"编辑开始 / 结束"这件事，
        // 不必自己去判断点击落在哪、按的是不是 ESC。
        winrt::event<winrt::delegate<TextBox*, bool>> onFocusChanged;
    private:
        // 内容与滚动都由自身绘制，屏蔽掉子节点与内部布局相关的 API（同 Text / Slider 的做法）。
        // setWidth/setHeight/setPadding/setBg/setBorder* 保持可用。
        using Node::makeChild;
        using Node::setFlexWrap;
        using Node::setAlignItems;
        using Node::setJustifyContent;
        using Node::setFlexDirection;

        void layout() override;
        void onDpiChanged() override;

        // ---- 文本布局 ----
        // 按当前内容区宽度重建 textLayout。文字、字号、字体、控件宽度变化后都要调。
        void buildLayout();
        // 按指定折行宽度创建 textLayout / placeholderLayout。只由 buildLayout 调用。
        void createLayouts(float maxW);
        // 折行宽度：autoSize 时不折行，否则按内容区宽度。布局还没跑过（宽度为 0）时也不折行，
        // 等 layout() 拿到真实宽度再重建。判断"要不要重建 layout"用它而不是 contentW ——
        // autoSize 下 w 是由文本反推出来的，拿 contentW 比会每帧都判定成"宽度变了"。
        float wrapWidth() const;
        // 垂直居中偏移量（物理像素）。verticalCenter 开启且文本未溢出时返回居中偏移，否则 0。
        float vCenterOffset() const;
        // autoSize 下按文本度量反推控件宽高并写回布局引擎。只由 buildLayout 调用。
        void applyAutoSize();
        // 内容区（padding 以内）的宽高，物理像素。也就是 canvas 的宽高。
        float contentW() const;
        float contentH() const;
        // textLayout 的总高度，物理像素。
        float textH() const;

        // ---- 光标与选区 ----
        // 文本位置 -> 光标坐标（内容坐标系，未减 scrollY）。
        void resetCaretPos(int textIndex);
        // 鼠标坐标（窗口坐标系）-> 文本位置。
        int hitTextIndex(POINT pos) const;
        // 内容坐标系里的点 -> 文本位置（已夹到 [0, length]）。
        // 方向键上下、Home/End 都是"给定 x/y 问落在哪个字符上"，差别只在喂进来的 x/y
        int hitIndexAt(float lx, float ly) const;
        // 文本内容改过之后的统一收尾：重建排版 → 光标归位 → 滚进可视区 → 重绘 → 通知宿主。
        // 删选区 / 插入 / 退格 / Delete 四处原来各抄一遍
        void afterTextEdit();
        // 把光标移到 index。extendSel 为真则保持锚点扩选，否则清掉选区并把锚点跟到光标。
        void moveCaret(int index, bool extendSel);
        int selStart() const;
        int selEnd() const;
        bool hasSelection() const;
        // 删掉选中文本。没有选区时返回 false，调用方据此决定是否执行单字符删除。
        bool delSelection();
        void insertText(const std::wstring& val);

        // ---- 滚动 ----
        float maxScrollY() const;
        void setScrollY(float val);
        void scrollCaretIntoView();
        // 内容高度超出可视高度 —— 需要显示滚动条。
        bool needScrollBar() const;
        // 画布（内容区）在窗口坐标里的矩形。滚动条是画在画布上的，
        // 所以绘制原点、命中判定、拖拽换算必须共用这一个矩形；
        // 以前命中/拖拽用 x/w/h（边框盒），绘制用画布，两者差着一整圈内边距
        D2D1_RECT_F canvasRect() const;
        // 鼠标（窗口坐标）是否落在滚动条那一竖条上。
        bool isPosInScrollBar(POINT pos) const;
        // 鼠标（窗口坐标）是否落在滑块上（而不是滑轨的空白处）。
        bool isPosInThumb(POINT pos) const;
        // 滑块的高度与顶端位置（canvas 坐标，也就是相对控件原点）。
        float thumbH() const;
        float thumbY() const;

        // ---- 绘制 ----
        void paint();
        void paintSelectionBg(ID2D1DeviceContext* ctx, ID2D1SolidColorBrush* brush);

        // ---- 事件 ----
        void onDown(POINT pos, bool isRight);
        void onMove(POINT pos);
        void onUp(POINT pos, bool isRight);
        void onWheel(POINT pos, float space);
        void onKey(UINT key);
        void onCharInput(UINT code);
        // 宿主转来的 IME 消息：msg 是消息号、wp 是 wParam，用来分辨"组字开始"与
        // "候选框打开 / 翻页"这两种该摆位的时机
        void onImeStart(UINT msg, UINT wp);
        // 把隐形系统光标挪到自绘光标的位置：输入法摆候选框时拿 GetCaretPos 兜底
        void syncImeCaret();
        // 摆组字窗 / 候选框。comp/cand 分别控制两件事要不要做。
        // 所有 IME 摆位都必须走这一个函数，不许各处再自己算坐标
        void placeImeUi(bool comp, bool cand);
        void onCursorEvent(bool* handled);
        void onTick(UINT id);
    private:
        // 文本画在这块画布上。用 swap chain 后端：光标每 500ms 闪一次，属于"每帧重画"，
        // 单缓冲 surface 会被合成器采样到"擦干净→重画"的中间态而闪烁。
        Canvas* canvas{ nullptr };
        Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> placeholderLayout;
        std::wstring text;
        std::wstring placeholder;
        std::wstring fontFamily;
        float fontSize{ 14.f };
        bool isBold{ false }, isItalic{ false };
        // 尺寸跟着文本走（不折行）。见 setAutoSize。
        bool autoSize{ false };
        bool verticalCenter{ false };
        Color color{ 0x333333FF };
        Color caretColor{ 0x333333FF };
        Color selectionBgColor{ 0x99C9EF99 };
        Color placeholderColor{ 0xAAAAAAFF };
        bool hasCaretColor{ false };

        // 光标在文本中的位置，以及选区锚点。选区恒为 [min(anchor,caret), max(anchor,caret)]。
        int caretIndex{ 0 };
        int selAnchor{ 0 };
        // 光标坐标（内容坐标系）与高度。
        D2D1_POINT_2F caretPos{ 0.f, 0.f };
        float caretHeight{ 0.f };
        bool caretVisible{ false };

        float scrollY{ 0.f };
        // 重建 layout 的判断依据：内容区宽度没变就不必重建。
        float lastLayoutW{ -1.f };

        bool focused{ false }, isHover{ false }, dragging{ false };
        // 下一次按下要被自己吞掉（见 swallowNextDown）。按完就复位，免得吞掉正常的那一下。
        bool swallowDown{ false };
        // 鼠标是否悬在滚动条上：onCursor 事件不带坐标，只能在 onMouseMove 里缓存。
        bool isHoverScrollBar{ false };
        // 正在拖拽滑块。拖拽起点的鼠标 y 与当时的 scrollY —— 增量式换算，
        // 免得用"鼠标当前位置直接映射"导致按住滑块中段时滑块瞬间跳到鼠标下。
        bool scrollBarDragging{ false };
        float dragStartMouseY{ 0.f }, dragStartScrollY{ 0.f };
        // 光标闪烁定时器 id。同窗口内多个 TextBox 必须用不同 id，否则失焦那个的
        // killTimer 会顺手把刚聚焦那个的定时器杀掉，光标就不闪了。
        UINT timerId{ 0 };
        // 组字开始后再补摆一次的定时器 id（避开 timerId 与宿主自己的 100~107）
        static constexpr UINT kImeNudge{ 211 };
        winrt::event_token downTok{}, moveTok{}, upTok{}, wheelTok{},
            keyTok{}, charTok{}, imeTok{}, blurTok{}, cursorTok{}, tickTok{}, captureTok{};
    };
}
