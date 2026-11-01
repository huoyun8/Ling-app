#include "UiPch.h"
#include "SvgPath.h"
#include "D2D.h"
#include <unordered_map>

namespace ui {
    namespace {
        // SVG path 的数字语法：允许紧凑写法（m10.5 9.25-1.75v9 里的 -1.75）、
        // 省略整数位（.5）、科学计数法；分隔符可以省略
        struct Cursor
        {
            const wchar_t* p{ nullptr };

            void skip() {
                while (*p == L' ' || *p == L',' || *p == L'\t' || *p == L'\n' || *p == L'\r') ++p;
            }
            bool number(float& out) {
                skip();
                if (!*p) return false;
                wchar_t* end{ nullptr };
                const double v = wcstod(p, &end);
                if (end == p) return false;
                p = end;
                out = static_cast<float>(v);
                return true;
            }
            // 圆弧的两个标志位在压缩数据里可能是 "0 0 1" 也可能是 "001"，后者只取一位
            bool flag(bool& out) {
                skip();
                if (*p != L'0' && *p != L'1') return false;
                out = (*p == L'1');
                ++p;
                return true;
            }
            bool letter() const { return (*p >= L'a' && *p <= L'z') || (*p >= L'A' && *p <= L'Z'); }
        };

        using Cache = std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1PathGeometry>>;

        Cache& cache()
        {
            static Cache c;
            return c;
        }

        // 端头 / 拐角缓存：键是 cap*4+join，最多 9 项
        using StyleCache = std::unordered_map<unsigned, Microsoft::WRL::ComPtr<ID2D1StrokeStyle>>;

        StyleCache& styles()
        {
            static StyleCache s;
            return s;
        }

        // 我们的枚举顺序按 SVG 的语义排（butt/round/square），跟 D2D 的数值不同，
        // 所以必须显式映射 —— 靠 static_cast 会静默把圆头画成方头
        D2D1_CAP_STYLE toD2D(Cap c)
        {
            switch (c) {
            case Cap::Round:  return D2D1_CAP_STYLE_ROUND;
            case Cap::Square: return D2D1_CAP_STYLE_SQUARE;
            default:          return D2D1_CAP_STYLE_FLAT;
            }
        }

        D2D1_LINE_JOIN toD2D(Join j)
        {
            switch (j) {
            case Join::Round: return D2D1_LINE_JOIN_ROUND;
            case Join::Bevel: return D2D1_LINE_JOIN_BEVEL;
            default:          return D2D1_LINE_JOIN_MITER;
            }
        }

        // 缓存键：填充规则不同，同一份 d 要建两份几何
        std::wstring key(const std::wstring& d, bool evenOdd)
        {
            std::wstring k;
            k.reserve(d.size() + 2);
            k.push_back(evenOdd ? L'E' : L'W');
            k += d;
            return k;
        }

        // 圆弧：SVG 的端点式定义与 D2D 的 D2D1_ARC_SEGMENT 语义一致，
        // 半径不足时两边都会按比例放大，可以直接映射
        void addArc(ID2D1GeometrySink* sink, D2D1_POINT_2F to, float rx, float ry,
                    float rot, bool large, bool sweep)
        {
            D2D1_ARC_SEGMENT seg{};
            seg.point = to;
            seg.size = D2D1::SizeF(std::abs(rx), std::abs(ry));
            seg.rotationAngle = rot;
            seg.sweepDirection = sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                                       : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
            seg.arcSize = large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
            sink->AddArc(&seg);
        }

        bool build(const std::wstring& d, bool evenOdd, ID2D1PathGeometry** out)
        {
            auto d2d = D2D::get();
            if (!d2d || !d2d->d2dFactory) return false;

            Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
            if (FAILED(d2d->d2dFactory->CreatePathGeometry(geo.GetAddressOf()))) return false;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(geo->Open(sink.GetAddressOf()))) return false;
            sink->SetFillMode(evenOdd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);

            Cursor c{ d.c_str() };
            D2D1_POINT_2F cur{ 0.f, 0.f }, start{ 0.f, 0.f }, ctrl{ 0.f, 0.f };
            bool open{ false }, hasCtrl{ false };      // S/T 需要知道上一段是不是同类曲线
            enum class Kind { None, Cubic, Quad } kind{ Kind::None };
            wchar_t cmd{ 0 };

            auto begin = [&]() {
                if (open) {
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    open = false;
                }
            };
            auto moveTo = [&](float x, float y) {
                begin();
                sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
                open = true;
                cur = start = { x, y };
            };

            while (true) {
                c.skip();
                if (!*c.p) break;
                if (c.letter()) cmd = *c.p++;
                else if (!cmd) return false;        // 没有命令却是数字：数据坏了

                const bool rel = (cmd >= L'a' && cmd <= L'z');
                wchar_t op = static_cast<wchar_t>(towupper(cmd));

                if (op == L'Z') {
                    if (open) {
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        open = false;
                    }
                    cur = start;
                    cmd = 0;                        // Z 无参数，后面必须是新命令
                    continue;
                }

                // 一条命令后面可以跟多组参数（"l 1 2 3 4" = 两次 lineto）
                while (true) {
                    const float ox = rel ? cur.x : 0.f, oy = rel ? cur.y : 0.f;
                    float a[6]{};
                    switch (op) {
                    case L'M':
                        if (!c.number(a[0]) || !c.number(a[1])) return false;
                        moveTo(a[0] + ox, a[1] + oy);
                        op = L'L';                  // 后续参数是隐式 lineto
                        kind = Kind::None;
                        break;
                    case L'L':
                        if (!c.number(a[0]) || !c.number(a[1])) return false;
                        cur = { a[0] + ox, a[1] + oy };
                        sink->AddLine(cur);
                        kind = Kind::None;
                        break;
                    case L'H':
                        if (!c.number(a[0])) return false;
                        cur = { a[0] + ox, cur.y };
                        sink->AddLine(cur);
                        kind = Kind::None;
                        break;
                    case L'V':
                        if (!c.number(a[0])) return false;
                        cur = { cur.x, a[0] + oy };
                        sink->AddLine(cur);
                        kind = Kind::None;
                        break;
                    case L'C':
                        if (!c.number(a[0]) || !c.number(a[1]) || !c.number(a[2]) ||
                            !c.number(a[3]) || !c.number(a[4]) || !c.number(a[5])) return false;
                        ctrl = { a[2] + ox, a[3] + oy };
                        cur = { a[4] + ox, a[5] + oy };
                        sink->AddBezier(D2D1::BezierSegment(
                            D2D1::Point2F(a[0] + ox, a[1] + oy), ctrl, cur));
                        kind = Kind::Cubic;
                        break;
                    case L'S':
                        if (!c.number(a[0]) || !c.number(a[1]) ||
                            !c.number(a[2]) || !c.number(a[3])) return false;
                        {
                            // 首控制点反射上一段的三次控制点；上一段不是三次曲线时取当前点
                            const D2D1_POINT_2F c1 = (kind == Kind::Cubic)
                                ? D2D1::Point2F(2 * cur.x - ctrl.x, 2 * cur.y - ctrl.y) : cur;
                            ctrl = { a[0] + ox, a[1] + oy };
                            cur = { a[2] + ox, a[3] + oy };
                            sink->AddBezier(D2D1::BezierSegment(c1, ctrl, cur));
                            kind = Kind::Cubic;
                        }
                        break;
                    case L'Q':
                        if (!c.number(a[0]) || !c.number(a[1]) ||
                            !c.number(a[2]) || !c.number(a[3])) return false;
                        {
                            // D2D 只有三次贝塞尔：二次升阶，两个控制点按 2/3 内插
                            const D2D1_POINT_2F q{ a[0] + ox, a[1] + oy };
                            const D2D1_POINT_2F to{ a[2] + ox, a[3] + oy };
                            const D2D1_POINT_2F c1{ cur.x + 2.f / 3.f * (q.x - cur.x),
                                                    cur.y + 2.f / 3.f * (q.y - cur.y) };
                            const D2D1_POINT_2F c2{ to.x + 2.f / 3.f * (q.x - to.x),
                                                    to.y + 2.f / 3.f * (q.y - to.y) };
                            ctrl = q;
                            cur = to;
                            sink->AddBezier(D2D1::BezierSegment(c1, c2, to));
                            kind = Kind::Quad;
                        }
                        break;
                    case L'T':
                        if (!c.number(a[0]) || !c.number(a[1])) return false;
                        {
                            const D2D1_POINT_2F q = (kind == Kind::Quad)
                                ? D2D1::Point2F(2 * cur.x - ctrl.x, 2 * cur.y - ctrl.y) : cur;
                            const D2D1_POINT_2F to{ a[0] + ox, a[1] + oy };
                            const D2D1_POINT_2F c1{ cur.x + 2.f / 3.f * (q.x - cur.x),
                                                    cur.y + 2.f / 3.f * (q.y - cur.y) };
                            const D2D1_POINT_2F c2{ to.x + 2.f / 3.f * (q.x - to.x),
                                                    to.y + 2.f / 3.f * (q.y - to.y) };
                            ctrl = q;
                            cur = to;
                            sink->AddBezier(D2D1::BezierSegment(c1, c2, to));
                            kind = Kind::Quad;
                        }
                        break;
                    case L'A':
                        {
                            float rx, ry, rot, x, y;
                            bool large, sweep;
                            if (!c.number(rx) || !c.number(ry) || !c.number(rot) ||
                                !c.flag(large) || !c.flag(sweep) ||
                                !c.number(x) || !c.number(y)) return false;
                            const D2D1_POINT_2F to{ x + ox, y + oy };
                            addArc(sink.Get(), to, rx, ry, rot, large, sweep);
                            cur = to;
                            kind = Kind::None;
                        }
                        break;
                    default:
                        return false;
                    }

                    c.skip();
                    if (!*c.p || c.letter()) break;      // 参数组结束
                }
            }

            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (FAILED(sink->Close())) return false;
            *out = geo.Detach();
            return true;
        }
    }

    ID2D1PathGeometry* SvgPath::get(const std::wstring& d, bool evenOdd)
    {
        if (d.empty()) return nullptr;
        auto& c = cache();
        auto k = key(d, evenOdd);
        if (auto it = c.find(k); it != c.end()) return it->second.Get();

        ID2D1PathGeometry* geo{ nullptr };
        if (build(d, evenOdd, &geo)) c.emplace(std::move(k), geo);
        return geo;
    }

    ID2D1StrokeStyle* SvgPath::stroke(Cap cap, Join join)
    {
        auto d2d = D2D::get();
        if (!d2d || !d2d->d2dFactory) return nullptr;
        const unsigned k = static_cast<unsigned>(cap) * 4u + static_cast<unsigned>(join);
        auto& c = styles();
        if (auto it = c.find(k); it != c.end()) return it->second.Get();

        D2D1_STROKE_STYLE_PROPERTIES p{};
        p.startCap = p.endCap = p.dashCap = toD2D(cap);
        p.lineJoin = toD2D(join);
        // 4 是 D2D 的默认上限。图标里没有需要长尖角的地方，用默认值即可
        p.miterLimit = 4.f;
        p.dashStyle = D2D1_DASH_STYLE_SOLID;
        p.dashOffset = 0.f;
        Microsoft::WRL::ComPtr<ID2D1StrokeStyle> st;
        if (FAILED(d2d->d2dFactory->CreateStrokeStyle(p, nullptr, 0, st.GetAddressOf())))
            return nullptr;
        return c.emplace(k, std::move(st)).first->second.Get();
    }

    void SvgPath::clear()
    {
        cache().clear();
        styles().clear();
    }
}
