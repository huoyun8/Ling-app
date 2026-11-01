#include "UiPch.h"
#include "Text.h"
#include "WinBase.h"
#include "D2D.h"
#include "SvgPath.h"
#include "../Res/Icons.generated.h"

namespace ui {

    Text::Text(WinBase* win) : Node(win)
    {
        FlNodeSetContext(this->node, this);
        FlNodeSetMeasureFunc(this->node, &Text::measureCB);
    }

    Text::~Text()
    {
    }

    // 契约：measureFunc 必须是纯函数 —— 只读当前节点的度量信息并返回尺寸，
    // 不得修改 style、不得创建/销毁 Composition 资源。否则会在同一次
    // CalculateLayout 内触发 flex 的重测量循环上限，导致后续兄弟节点的布局
    // 输出为“未完成的中间状态”，视觉上表现为最后一条节点被截掉一半。
    FlSize Text::measureCB(FlNodeConstRef node, float, FlMeasureMode, float, FlMeasureMode)
    {
        auto self = static_cast<Text*>(FlNodeGetContext(node));
        // 图标：盒子就是边长 fontSize*dpi 的正方形。取整到整像素，
        // 让 surface 尺寸、绘制偏移、几何缩放全部落在整数上
        if (self->icon) {
            const float side = std::ceil(self->fontSize * self->win->dpi);
            return { side, side };
        }
        if (!self->textLayout) return { 0.f, 0.f };
        DWRITE_TEXT_METRICS m;
        self->textLayout->GetMetrics(&m);
        // 向上取整到整像素，返回给布局引擎用于布局（下游会得到整数 x/y）。
        return { std::ceil(m.width), std::ceil(m.height) };
    }

    void Text::resolveIcon()
    {
        // 图标 ID 仍是单个私有区字符（沿用原来的调用点），但它不再对应任何字体
        icon = (text.size() == 1) ? icons::find(text[0]) : nullptr;
    }

    void Text::setIcon(const wchar_t* name)
    {
        // 按语义名取图标，比写私有区码位可读得多（icons::ShapeArrowSolid 一眼知道是什么）。
        // 名字没命中就保持原状：宁可不换，也不能把按钮清成空白 ——
        // "图标丢失"比"图标不对"难查得多
        auto* def = icons::findName(name);
        if (!def) return;
        icon = def;
        text.clear();
        textLayout.Reset();
        FlNodeMarkDirty(node);
        win->refresh();
    }

    void Text::makeLayout()
    {
        resolveIcon();
        if (icon) {
            // 图标不走文本排版：没有字形、没有字体、没有度量可算
            textLayout.Reset();
            return;
        }
        auto d2d = D2D::get();
        if (!d2d || !d2d->dwriteFactory) return;
        auto format = d2d->getTextFormat(fontFamily);
        d2d->dwriteFactory->CreateTextLayout(text.data(), (UINT32)text.length(), format, FLT_MAX, FLT_MAX, textLayout.ReleaseAndGetAddressOf());
        if (!textLayout) return;
        textLayout->SetFontSize(fontSize * win->dpi, { 0, INT_MAX });
        if (!fontFamily.empty()) {
            // format 已经是对应集合的了，这句是为了在同一集合内指定具体族名
            // （比如系统集合里从默认的雅黑换成别的）
            textLayout->SetFontFamilyName(fontFamily.data(), { 0, INT_MAX });
        }
    }

    void Text::setText(const std::wstring& text)
    {
        this->text = text;
        makeLayout();
        FlNodeMarkDirty(node);
        win->refresh();
    }

    std::wstring Text::getText()
    {
        return text;
    }

    void Text::setFontSize(float val)
    {
        this->fontSize = val;
        if (textLayout.Get()) {
            textLayout->SetFontSize(fontSize * win->dpi, { 0, INT_MAX });
        }
        FlNodeMarkDirty(node);
        // 必须再要一次布局：本项目的 FlNodeMarkDirty 是空实现（每次 CalculateLayout
        // 都重新测量），光置脏不会让布局重跑 —— 尺寸还是旧的，而下面 paint()
        // 已经按新字号把字画进了旧尺寸的 surface，字会被裁掉一块
        win->refresh();
        if (surface) paint();
    }

    void Text::setFontFamily(const std::wstring& val)
    {
        this->fontFamily = val;
        // 换族名可能意味着换字体集合（图标字体自成一个集合），只改 layout 上的族名不够，
        // 那样在系统集合里找不到图标字体，会退回默认字体显示成方块
        if (textLayout.Get()) makeLayout();
        FlNodeMarkDirty(node);
        win->refresh();   // 同上：族名变了度量也变，得重新测量
        if (surface) paint();
    }

    void Text::setColor(Color color)
    {
        if (this->color == color) return;
        this->color = color;
        if (surface) paint();
    }

    void Text::onDpiChanged()
    {
        if (icon) {
            // 图标盒边长由 fontSize*dpi 决定，dpi 变了要重新测量、重画
            FlNodeMarkDirty(node);
            return;
        }
        if (textLayout.Get()) {
            textLayout->SetFontSize(fontSize * win->dpi, { 0, INT_MAX });
            FlNodeMarkDirty(node);
        }
    }

    void Text::layout()
    {
        // 贴整已在 Node::layout() 对整棵树做过（含祖先链），这里不再单独 round：
        // 只圆自己那一层是无效的 —— 父节点的小数偏移照样让绝对位置落在半个像素上。
        Node::layout();
        paint();
    }

    void Text::paint()
    {
        // 此时 w/h 已经是 measureCB 返回的整数像素，一次到位创建/resize。
        const int pxW = static_cast<int>(w);
        const int pxH = static_cast<int>(h);
        if (pxW <= 0 || pxH <= 0) return;
        // 设备换代了（空闲时被释放、这次又重建）：旧表面绑的是上一代 graphics device，
        // 直接丢。不丢的话这条分支会拿它去 BeginDraw —— 失败是静默的，画面上就是
        // "按钮空着、图标没了"，而且下一帧也不会自己好
        if (surface && surfaceGen != D2D::generation()) {
            surface = nullptr;
        }
        if (!surface) {
            auto d2d = D2D::get();
            surface = d2d->createDrawingSurface(win->compositor, (float)pxW, (float)pxH);
            // 建表面失败（设备不可用）时 surface 是空的：下面 as<> / CreateSurfaceBrush
            // 都会抛异常，而这条路径在 WM_PAINT 里，没人接
            if (!surface) return;
            surfaceGen = D2D::generation();
            auto brush = win->compositor.CreateSurfaceBrush(surface);
            // 必须显式 None：CompositionSurfaceBrush 默认按 Fill 拉伸，
            // 只要 visual 尺寸和 surface 尺寸差一点点（节点 w/h 是浮点、surface 是整数），
            // 整张字形图就会被缩放一遍 —— 小图标看着"始终发虚"就是这么来的。
            // Canvas 那边早就设了 None，这里漏了
            brush.Stretch(winrt::Windows::UI::Composition::CompositionStretch::None);
            visual.Brush(brush);
        }
        else {
            auto sz = surface.SizeInt32();
            if (sz.Width != pxW || sz.Height != pxH) {
                surface.Resize({ pxW, pxH });
            }
        }

        auto s = surface.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
        ComPtr<ID2D1DeviceContext> ctx;
        POINT offset{};
        s->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(ctx.GetAddressOf()), &offset);
        // BeginDraw 失败 / 没拿到 ctx 时不能往下用：ctx 为空就是一次访问冲突
        if (!ctx) return;
        auto trans = D2D1::Matrix3x2F::Translation((float)offset.x, (float)offset.y);
        ctx->SetTransform(trans);
        // 这里的 surface 是 Premultiplied alpha 的透明表面（见 D2D::createDrawingSurface），
        // 而 D2D 设备默认的文字抗锯齿是 ClearType —— 它要求不透明底色，会把次像素
        // 覆盖率按 RGB 三个通道分别写进透明像素，合成到按钮底色后就带着彩边，
        // 小图标看着"永远发虚"。图形几何只受灰度 AA 影响，所以只有字形糊、线条不糊。
        // 图标与文字统一走灰度 AA，边缘立刻干净。
        ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        ctx->Clear(0);    // surface 只画图标/文字，背景由外层容器负责
        ComPtr<ID2D1SolidColorBrush> brush;
        ctx->CreateSolidColorBrush(color.getD2DColor(), brush.GetAddressOf());
        if (icon) {
            paintIcon(ctx.Get(), brush.Get(), pxW, pxH);
            s->EndDraw();
            return;
        }
        // ink snapping：DWrite 的字形墨盒起点常带小数（如 left=0.38、top=1.72），
        // 而合成器把 surface 贴到 visual 时按 visual 的位置做双线性重采样，
        // 墨盒只要不在整数像素上，15px 的小图标边缘就会被抹糊。
        // GetMetrics 的 left/top 是墨盒相对绘制原点的偏移，把绘制原点平移
        // (round(left)-left, round(top)-top) 即可让墨盒落整数。
        // 若平移后右/下边会溢出 surface，则退回 floor（向左/上移，一定在界内）。
        DWRITE_TEXT_METRICS tm{};
        textLayout->GetMetrics(&tm);
        float snapX = std::round(tm.left) - tm.left;
        float snapY = std::round(tm.top) - tm.top;
        if (snapX > 0.f && std::round(tm.left) + tm.width > (float)pxW)
            snapX = std::floor(tm.left) - tm.left;
        if (snapY > 0.f && std::round(tm.top) + tm.height > (float)pxH)
            snapY = std::floor(tm.top) - tm.top;
        ctx->DrawTextLayout({ snapX, snapY }, textLayout.Get(), brush.Get());
        s->EndDraw();
    }

    void Text::paintIcon(ID2D1DeviceContext* ctx, ID2D1SolidColorBrush* brush, int pxW, int pxH)
    {
        // 图标清晰的三个前提，全在这里：
        //   1) 边长取整 —— 图标盒是整数像素，几何按此缩放，不会出现"半个像素"的模糊边；
        //   2) 偏移取整 —— 盒心落在整像素上，合成阶段不再有双线性重采样；
        //   3) 灰度 AA —— 透明表面上的几何填充，边缘是干净的覆盖率，没有彩边。
        const float side = std::round(fontSize * win->dpi);
        const float scale = side / icon->vb;
        const float ox = std::round((pxW - side) * 0.5f);
        const float oy = std::round((pxH - side) * 0.5f);
        // 必须叠在 paint() 已经设好的变换之上，不能直接覆盖。
        // 原因：CompositionDrawingSurface 会被合成器放进一张共享图集（atlas），
        // BeginDraw 回填的 offset 就是这张 surface 在图集里的位置 —— 绘制得先平移到
        // 自己的那格，再在格内画。文字那条路走的是 DrawTextLayout 的相对坐标，天然带着
        // 这个平移；图标这边一旦自己 SetTransform(Scale*Translation)，就把它抹掉了：
        // 图集位置不是 (0,0) 的那张 surface 上，图标会被画到格子外面（落在别的图集项上，
        // 或者干脆画到 surface 边界之外），表现就是"这颗按钮空着、图标丢了"。
        // 同一个窗口里，先建好的 surface 常见 offset=(0,0)~(1,2)，所以大部分图标是好的；
        // 窗口又新建了几个 surface 之后图集会重排，offset 变成 (22,2) 这种值，图标立刻消失
        // —— 这就是"图标时有时无"的根子。
        D2D1_MATRIX_3X2_F base{};
        ctx->GetTransform(&base);
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                          D2D1::Matrix3x2F::Translation(ox, oy) * base);
        // 几何抗锯齿只有两种：PER_PRIMITIVE 是灰度覆盖率（我们想要的），ALIASED 是硬边。
        // 显式写出来，避免以后有人改了设备默认值导致边缘变硬
        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (int i = 0; i < icon->opCount; ++i) {
            const auto& op = icon->ops[i];
            auto* geo = SvgPath::get(op.d, op.evenOdd);
            if (!geo) continue;
            // 描边宽度用设计坐标给出，随上面的缩放一起缩小，与 SVG 里的语义一致。
            // 端头 / 拐角也照设计稿的声明走 —— 圆头还是方头决定了线条"软不软"，
            // 统一凑一个方头会让整套图标比设计稿硬一档
            if (op.stroke > 0.f) ctx->DrawGeometry(geo, brush, op.stroke, SvgPath::stroke(op.cap, op.join));
            else ctx->FillGeometry(geo, brush);
        }
    }
}
