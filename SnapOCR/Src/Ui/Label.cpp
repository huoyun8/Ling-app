#include "UiPch.h"
#include "Label.h"
#include "WinBase.h"
#include "Text.h"

namespace ui {

    Label::Label(WinBase* win) : Node(win)
    {
        // 默认让 Text 在容器内居中；使用者可通过 Node::setJustify/setAlign 覆盖。
        text = makeChild<Text>();
    }

    Label::~Label()
    {
    }

    std::wstring Label::getText()
    {
        return text->getText();
    }

    void Label::setText(const std::wstring& s)       { text->setText(s); }
    void Label::setIcon(const wchar_t* name)         { text->setIcon(name); }
    void Label::setFontSize(float v)                 { text->setFontSize(v); }
    void Label::setFontFamily(const std::wstring& f) { text->setFontFamily(f); }
    void Label::setColor(Color c)                    { text->setColor(c); }
}
