// 排版解析的离线自测：不启动引擎、不开界面，直接喂坐标进去看四种档位的输出。
// 用法（Git Bash，参数与 build.sh 一致）：
//   见文件末尾注释，或直接跑 tools/layout_test.sh
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

#include "../Src/Ocr/OcrLayout.h"

namespace
{
	OcrLine mk(const wchar_t* t, float x1, float y1, float x2, float y2)
	{
		OcrLine l;
		l.text = t;
		l.score = 0.99f;
		l.x1 = x1; l.y1 = y1; l.x2 = x2; l.y2 = y2;
		return l;
	}

	void show(const char* name, const std::vector<OcrLine>& ls)
	{
		wprintf(L"\n===== %hs =====\n", name);
		const Layout::Mode modes[] = { Layout::Mode::Auto, Layout::Mode::Line, Layout::Mode::Flat, Layout::Mode::Raw };
		for (auto m : modes)
		{
			wprintf(L"[%ls]\n", Layout::keyOf(m));
			const std::wstring t = Layout::format(ls, m);
			wprintf(L"%ls\n", t.c_str());
		}
	}
}

int wmain()
{
	_setmode(_fileno(stdout), _O_U8TEXT);   // 控制台默认 ANSI，中文会全变问号
	// 1) 双栏：左栏三行、右栏三行。Auto 应该读完左栏再读右栏，不是左右交错
	{
		std::vector<OcrLine> v{
			mk(L"左栏第一行正文", 10, 100, 400, 128),
			mk(L"左栏第二行正文", 10, 130, 395, 158),
			mk(L"左栏第三行正文", 10, 160, 402, 188),
			mk(L"右栏第一行正文", 430, 100, 820, 128),
			mk(L"右栏第二行正文", 430, 130, 815, 158),
			mk(L"右栏第三行正文", 430, 160, 822, 188),
		};
		show("two-column", v);
	}

	// 2) 自然段：第一段以句号收尾，第二段首行缩进。Auto 应该只出两段
	{
		std::vector<OcrLine> v{
			mk(L"这是第一段的第一行，写到边缘会换行继", 10, 100, 800, 128),
			mk(L"续到第二行，然后这一段结束。", 10, 130, 640, 158),
			mk(L"这是第二段，首行缩进了两个字。", 45, 160, 760, 188),
			mk(L"第二段也换了一行。", 10, 190, 520, 218),
		};
		show("paragraph", v);
	}

	// 3) 表格：两列三行。Line 档应该每框一行、按行优先读出六个格子
	{
		std::vector<OcrLine> v{
			mk(L"品名", 10, 100, 120, 128), mk(L"数量", 500, 100, 610, 128),
			mk(L"螺丝刀", 10, 140, 160, 168), mk(L"12", 500, 140, 545, 168),
			mk(L"扳手", 10, 180, 130, 208), mk(L"3", 500, 180, 530, 208),
		};
		show("table", v);
	}

	// 4) 竖排：三列，字框高大于宽。Auto/Line 都应该从右往左读，列内从上到下
	{
		std::vector<OcrLine> v{
			mk(L"春", 700, 100, 730, 150), mk(L"眠", 700, 155, 730, 205), mk(L"不", 700, 210, 730, 260),
			mk(L"处", 640, 100, 670, 150), mk(L"处", 640, 155, 670, 205), mk(L"闻", 640, 210, 670, 260),
			mk(L"啼", 580, 100, 610, 150), mk(L"鸟", 580, 155, 610, 205),
		};
		show("vertical", v);
	}

	// 5) 单行：不该被任何规则改写
	{
		std::vector<OcrLine> v{ mk(L"只有一行文字 123", 10, 100, 300, 128) };
		show("single", v);
	}
	return 0;
}
