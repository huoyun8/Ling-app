#pragma once
#include <wrl.h>
#include <dwrite_3.h>
#include <winrt/Windows.UI.Composition.h>
#include "Node.h"

namespace ui {
    class WinBase;
    namespace icons { struct IconDef; }

    // 内部实现类：一段文本的最小渲染单元。
    //   - surface 尺寸恒等于 DWrite 的 metric（避免父容器 flexGrow 撑爆显存）；
    //   - 通过 measureFunc 把 metric 报告给父节点，父节点用原生 flex
    //     完成对齐（不再在 paint() 里手写偏移）。
    //   - 内容若是图标（单个字符且命中图标表），不建文本布局，直接画矢量路径：
    //     小尺寸下比字形光栅化清晰得多，参数也完全可控，见 paintIcon。
    // 只在 src 内部使用，不对外暴露。
    class Text : public Node
    {
    public:
        Text(WinBase* win);
        ~Text();
        void setText(const std::wstring& text);
        // 按语义名设图标（Res/Icons.generated.h 里的名字常量），不经过任何字符。
        // 名字取不到时保持原样，不会把节点清空。码位写法仍然有效，两者等价
        void setIcon(const wchar_t* name);
        std::wstring getText();
        void setFontSize(float val);
        void setFontFamily(const std::wstring& val);
        void setColor(Color color);
    private:
        // 尺寸/布局完全由内容决定，屏蔽掉外部改尺寸、改子节点的 API，避免误用。
        using Node::makeChild;
        using Node::setFlexGrow;
        using Node::setFlexShrink;
        using Node::setWidth;
        using Node::setHeight;
        using Node::setSize;
        using Node::setWidthPercent;
        using Node::setHeightPercent;
        using Node::setSizePercent;
        using Node::setPadding;
        using Node::setPaddingLeft;
        using Node::setPaddingTop;
        using Node::setPaddingRight;
        using Node::setPaddingBottom;
        using Node::setFlexWrap;
        using Node::setAlignItems;
        using Node::setJustifyContent;
        using Node::setFlexDirection;
        using Node::setBg;

        static FlSize measureCB(FlNodeConstRef node, float width, FlMeasureMode widthMode, float height, FlMeasureMode heightMode);
        void layout() override;
        void onDpiChanged() override;
        void paint();
        // 建 textLayout。字体族名决定用哪个 TextFormat（图标字体和系统字体不在同一个
        // 字体集合里），所以换族名不能只 SetFontFamilyName，得整个重建
        void makeLayout();
        // 判断当前内容是不是图标：单字符且命中图标表。命中则不建文本布局
        void resolveIcon();
        // 图标按矢量路径画：以设备像素为单位取整、居中、几何缓存复用
        void paintIcon(ID2D1DeviceContext* ctx, ID2D1SolidColorBrush* brush, int pxW, int pxH);

    private:
        winrt::Windows::UI::Composition::CompositionDrawingSurface surface{ nullptr };
        // 建 surface 时的设备代次。设备被拆过一轮之后（空闲释放）旧表面已经不能用，
        // 代次对不上就丢掉重建，否则 BeginDraw 静默失败、图标和文字都画不出来
        unsigned surfaceGen{ 0 };
        Microsoft::WRL::ComPtr<IDWriteTextLayout> textLayout;
        const icons::IconDef* icon{ nullptr };
        Color color{ 0x333333FF };
        std::wstring text;
        std::wstring fontFamily;
        float fontSize{ 12.f };
    };
}
