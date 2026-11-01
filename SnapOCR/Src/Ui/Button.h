#pragma once
#include "Node.h"

namespace ui {
	class WinBase;
	class Text;   // 内部类型的前向声明
	class Button :public Node
	{
	public:
		Button(WinBase* win);
		~Button();
		void setText(const std::wstring& text);
		// 按语义名设图标，内部转给 Text；调用点一律传 icons:: 下的名字常量，写错就是编译错误
		void setIcon(const wchar_t* name);
		std::wstring getText();
		void setFontSize(float val);
		void setFontFamily(const std::wstring& val);
		void setColor(Color color);
		void setBg(const Color& color) override;
		void setBorderColor(const Color& color) override;
		void setHoverColor(Color color);
		void setHoverBg(Color color);
		void setHoverBorderColor(Color color);
	public:
		winrt::event<winrt::delegate<Button*>> onClick;
		winrt::event<winrt::delegate<Button*>> onEnter;
		winrt::event<winrt::delegate<Button*>> onLeave;
	private:
		void onMove(POINT pos);
		void onDown(POINT pos, bool isRight);
	private:
		Text* text{ nullptr };
		winrt::event_token moveTok{}, downTok{};
		Color hoverColor{ 0x333333FF }, hoverBg{ 0 }, hoverBorderColor{ 0 }, color{ 0x333333FF }, borderColorNormal{ 0 };
		// 两个背景刷缓存下来，hover 切换时只做引用替换，不再每次 new。
		winrt::Windows::UI::Composition::CompositionColorBrush normalBrush{ nullptr };
		winrt::Windows::UI::Composition::CompositionColorBrush hoverBrush{ nullptr };
		bool hasHoverBorderColor{ false };
		bool isHover{ false };
	};
}




