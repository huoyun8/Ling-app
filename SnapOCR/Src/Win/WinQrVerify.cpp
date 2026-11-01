#include "pch.h"
#include <algorithm>
#include <commdlg.h>
#include <string>
#include <vector>
#include "../Hash/FileHash.h"
#include "../Lang.h"
#include "../Util.h"
#include "WinQr.h"

// 「文件校验」页：上方一整块结果框；下面一行操作按钮；右侧一列算法勾选；底部"文件 / 总计"两行。
//
// 结果框用自绘的 VfBox（见 Win/WinVfBox.cpp 的说明），不用多行只读 EDIT：
// EDIT 在"流式 SetWindowText + 用户滚动"这个组合下会把新行画在旧像素上，越滚越糊。
//
// 记录只留最新两条：新的顶掉旧的（12、23、34……），框里永远不会堆成一片。
// 算法与读盘都在 Hash/FileHash.cpp 里，这里只做三件事：摆控件、起工作线程、把结果贴上去。
// 大文件算哈希是秒级到分钟级的活，放在 UI 线程里转会让整个窗口假死（连"停止"都点不动）。

namespace {
	// ———————————————— 布局（96dpi 基准，与其它页同一套网格） ————————————————
	// 数字的唯一来源在 WinQr::VfGrid 里：WinQr.cpp 画进度条时要用同一套坐标
	constexpr int VF_LIST_Y = WinQr::VfListY;
	constexpr int VF_LIST_H = WinQr::VfListH;
	constexpr int VF_LIST_W = WinQr::VfListW;
	constexpr int VF_BTN_Y = WinQr::VfBtnY;
	constexpr int VF_BTN_H = WinQr::VfBtnH;
	constexpr int VF_CHK_X = WinQr::VfChkX;
	constexpr int VF_CHK_Y = WinQr::VfChkY;
	constexpr int VF_CHK_W = WinQr::VfChkW;
	constexpr int VF_CHK_H = WinQr::VfChkH;
	constexpr int VF_CHK_PITCH = WinQr::VfChkPitch;
	constexpr int VF_ROW_FILE = WinQr::VfRowFile;
	constexpr int VF_ROW_TOTAL = WinQr::VfRowTotal;

	struct TagRect { int id, x, w; };
	constexpr TagRect kVerifyBtns[] = {
		{ WinQr::VfBrowse, 14, 100 },
		{ WinQr::VfClear, 122, 70 },
		{ WinQr::VfCopy, 200, 70 },
		{ WinQr::VfSave, 278, 88 },
		{ WinQr::VfStop, 374, 74 },
	};
	// 五个勾选**竖着排在右下角**：按钮行右侧那块空地（x 470..571）从按钮行顶边
	// （321）往下排，每行 20、高 20，五行正好落到 421 —— 左边按钮行 321..351
	// 加两条进度条（最后一条 397..421）底边也是 421，两栏顶边、底边都齐。
	// 横排的写法宽度永远不够用：勾选框自己占 13px，18px 的中文标签一掐就被系统
	// 裁掉右半截，看着像字叠在一起。竖排没有宽度竞争，标签一律能完整显示。
	// 第三格原来是 SHA1，已原地换成 SHA256 —— 行数不变，位置也不用动
	struct ChkRect { int id, y; };
	constexpr ChkRect kVerifyChks[] = {
		{ WinQr::VfChkVer, 0 }, { WinQr::VfChkDate, 1 }, { WinQr::VfChkMd5, 2 },
		{ WinQr::VfChkSha256, 3 }, { WinQr::VfChkCrc, 4 },
	};

	constexpr int kVerifyIds[] = {
		WinQr::VfList, WinQr::VfBrowse, WinQr::VfClear, WinQr::VfCopy,
		WinQr::VfSave, WinQr::VfStop,
		WinQr::VfChkVer, WinQr::VfChkDate, WinQr::VfChkMd5, WinQr::VfChkSha256, WinQr::VfChkCrc,
	};

	// 版本 / 日期不参与哈希，借用掩码的高位当"要不要打印"的开关
	constexpr unsigned ShowVersion = 1u << 8;
	constexpr unsigned ShowDate = 1u << 9;

	// 一个文件的输出块。字段名与 Hash 一致，便于和别的工具对照。
	// 末尾不再留空行：记录之间的分隔线由 VfBox 画，留空行会让两条记录差半行、装不下
	std::vector<std::wstring> digestLines(const FileDigest& d, unsigned mask)
	{
		std::vector<std::wstring> lines;
		lines.push_back(Lang::get(L"vf.nameLabel") + L": " + d.name);
		lines.push_back(Lang::get(L"vf.sizeLabel") + L": " + groupDigits(d.size)
			+ L" " + Lang::get(L"vf.byteUnit"));
		if ((mask & ShowVersion) && !d.version.empty()) {
			lines.push_back(Lang::get(L"vf.verLabel") + L": " + d.version);
		}
		if ((mask & ShowDate) && !d.modified.empty()) {
			lines.push_back(Lang::get(L"vf.dateLabel") + L": " + d.modified);
		}
		if (!d.md5.empty()) lines.push_back(L"MD5: " + d.md5);
		if (!d.sha256.empty()) lines.push_back(L"SHA256: " + d.sha256);
		if (!d.crc32.empty()) lines.push_back(L"CRC32: " + d.crc32);
		return lines;
	}

	// ———————————————— 粘贴对照用的两个小工具 ————————————————
	// 剪贴板里的纯文本；拿不到就给空串
	std::wstring clipboardText()
	{
		std::wstring text;
		if (!OpenClipboard(nullptr)) return text;
		if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
			if (auto p = static_cast<const wchar_t*>(GlobalLock(h))) {
				text.assign(p);
				GlobalUnlock(h);
			}
		}
		CloseClipboard();
		return text;
	}

	bool isHexChar(wchar_t c)
	{
		return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
	}

	// 一行里取"候选摘要值"：先剥掉行首的标签前缀（"MD5:" / "sha256="），
	// 再取剩下的第一个空白分隔 token，并要求这个 token 整段都是十六进制字符。
	//
	// 关键就在这最后一条：文件名、大小、版本号、时间都不是"纯十六进制的一段"，
	// 于是被挡在门外 ——
	//   "abcdef12.exe"           token 里带点，不是纯 hex  → 不认（旧逻辑会当成 CRC32）
	//   "2024-01-01 12:00:00"     token 里带横线            → 不认
	//   "1,234,567" / "1.0.0.0"   带逗号/点                 → 不认
	// 而 "SHA256: <64位>" 和 "<64位>  setup.exe"（sha256sum 风格）都照常认出来
	bool lineDigestValue(const std::wstring& line, std::wstring& val)
	{
		val.clear();
		size_t b = 0, e = line.size();
		while (b < e && iswspace(line[b])) b++;
		while (e > b && iswspace(line[e - 1])) e--;
		if (b >= e) return false;
		// 标签只认紧贴行首的一小段：中间有空白就说明是"文件名: xxx"这种整句，不当标签
		const size_t cut = line.find_first_of(L":=", b);
		if (cut != std::wstring::npos && cut - b <= 16) {
			bool hasSpace = false;
			for (size_t i = b; i < cut; i++) {
				if (iswspace(line[i])) { hasSpace = true; break; }
			}
			if (!hasSpace) {
				b = cut + 1;
				while (b < e && iswspace(line[b])) b++;
			}
		}
		if (b >= e) return false;
		size_t te = b;
		while (te < e && !iswspace(line[te])) te++;
		const std::wstring tok = line.substr(b, te - b);
		if (tok.empty()) return false;
		for (wchar_t c : tok) {
			if (!isHexChar(c)) return false;
		}
		val = tok;
		std::transform(val.begin(), val.end(), val.begin(), ::towlower);
		return true;
	}

	// 从粘贴的文字里认出三种摘要：下标 0/1/2 = MD5 / SHA256 / CRC32。
	// 只认这三种值本身，文件名/大小/版本/时间一律不参与判断：
	// 第一轮按行找标签（"MD5:" "SHA256:" "CRC32:"），第二轮对没有标签的纯值行按长度兜底
	// （32 位当 MD5、64 位当 SHA256、8 位当 CRC32，8 位还要求带字母，
	//  否则 "1331712" 这类文件大小会被当成 CRC32）。
	// 40 位的 SHA1 不再认：这一格已经换成 SHA256，旧值认出来也没东西可比
	void parseDigests(const std::wstring& text, std::wstring out[3])
	{
		out[0].clear(); out[1].clear(); out[2].clear();
		// 两轮（先认标签、再按长度兜底）用同一套切行：原来各写一遍 while，
		// 改了一处忘了另一处，两边对同一段文字就会给出不同的行边界
		auto eachLine = [&text](auto&& fn) {
			size_t pos = 0;
			while (pos < text.size()) {
				size_t e = text.find(L'\n', pos);
				if (e == std::wstring::npos) e = text.size();
				fn(text.substr(pos, e - pos));
				pos = e + 1;
			}
		};

		// 第一轮：认标签，标签和长度都要对上
		eachLine([&out](const std::wstring& line) {
			std::wstring low;
			low.reserve(line.size());
			for (wchar_t c : line) {
				low.push_back(c >= L'A' && c <= L'Z' ? (wchar_t)(c - L'A' + L'a') : c);
			}
			int want = -1;
			if (low.find(L"crc32") != std::wstring::npos) want = 2;
			else if (low.find(L"sha256") != std::wstring::npos) want = 1;
			else if (low.find(L"md5") != std::wstring::npos) want = 0;
			if (want < 0) return;

			std::wstring v;
			if (!lineDigestValue(line, v)) return;
			const size_t len = want == 0 ? 32u : (want == 1 ? 64u : 8u);
			if (v.size() == len) out[want] = v;
		});

		if (!out[0].empty() && !out[1].empty() && !out[2].empty()) return;
		// 第二轮：没有标签的纯值行，按长度兜底
		eachLine([&out](const std::wstring& line) {
			std::wstring v;
			if (!lineDigestValue(line, v)) return;
			if (v.size() == 64 && out[1].empty()) out[1] = v;
			else if (v.size() == 32 && out[0].empty()) out[0] = v;
			else if (v.size() == 8 && out[2].empty()
				&& v.find_first_of(L"abcdef") != std::wstring::npos) out[2] = v;
		});
	}
}

void WinQr::createVerifyPage()
{
	// 结果框：自绘控件（VfBox）。id 沿用 VfList，切页显隐 / GetDlgItem 都还按这个 id 走
	vfb.create(hwnd, VfList, dpi);
	// 框内 Ctrl+V / 粘贴：跟"再拖一个文件进来"走同一条对比路径
	vfb.setOnPaste([this]() { vfPasteCompare(); });

	// 按钮带 & 助记符，和原工具一样按 Alt+B/L/C/S/T 就能点
	mkButton(VfBrowse, Lang::get(L"vf.browse").c_str());
	mkButton(VfClear, Lang::get(L"vf.clear").c_str());
	mkButton(VfCopy, Lang::get(L"vf.copy").c_str());
	mkButton(VfSave, Lang::get(L"vf.save").c_str());
	mkButton(VfStop, Lang::get(L"vf.stop").c_str());
	// 初始态就是禁用灰：没有进度条在走，停止按了也没有意义
	vfSyncStopBtn();

	const struct { int id; const wchar_t* text; bool fromLang; } chks[] = {
		{ VfChkVer, L"vf.ver", true },
		{ VfChkDate, L"vf.date", true },
		// 算法名不进语言包：翻成"消息摘要第五号"反而没人认得
		{ VfChkMd5, L"MD5", false },
		{ VfChkSha256, L"SHA256", false },
		{ VfChkCrc, L"CRC32", false },
	};
	for (const auto& c : chks) {
		const std::wstring text = c.fromLang ? Lang::get(c.text) : std::wstring(c.text);
		// 自绘（BS_OWNERDRAW）而不是 BS_AUTOCHECKBOX：
		// 主题复选框是"透明背景画字"，自己不擦底，而主窗口是 WS_CLIPCHILDREN +
		// 整幅双缓冲，父窗口也画不到子控件底下去。结果就是被别的窗口盖过之后，
		// 控件矩形里留着别人的旧像素，看起来像标签重叠/串字。
		// 改成自绘后每一帧先整块填底色，物理上不可能残留。
		// 勾选状态仍然走 BM_SETCHECK / BM_GETCHECK，vfAlgoMask 不用改
		auto box = CreateWindowExW(0, L"BUTTON", text.c_str(),
			WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd,
			reinterpret_cast<HMENU>(static_cast<INT_PTR>(c.id)), GetModuleHandleW(nullptr), nullptr);
		// 勾选状态不在按钮上（自绘按钮存不住），这里只保证控件存在
		(void)box;
	}

	// 「文件」「总计」两行不再建 STATIC 控件：标签文字和百分比都画进进度条内部，
	// 左边贴着标签、正中是百分比，由 drawProgressBar 一次画完。
	// 这样一整条就是一块，不会有"标签控件 + 进度条"两个矩形拼在一起的割裂感。
	// 结论"相同 / 不同"不写字，只把结果框整圈外框染成绿 / 红，见 VfBox::paint。
}

void WinQr::relayoutVerify()
{
	auto place = [this](int id, int x, int y, int w, int h) {
		if (auto c = GetDlgItem(hwnd, id)) {
			SetWindowPos(c, nullptr, sc(x), sc(y), sc(w), sc(h), SWP_NOZORDER | SWP_NOACTIVATE);
		}
	};

	place(VfList, 14, VF_LIST_Y, VF_LIST_W, VF_LIST_H);
	vfb.setDpi(dpi);
	for (const auto& b : kVerifyBtns) place(b.id, b.x, VF_BTN_Y, b.w, VF_BTN_H);
	// 勾选行：五个一横排，紧跟按钮行下面
	for (const auto& c : kVerifyChks) {
		place(c.id, VF_CHK_X, VF_CHK_Y + VF_CHK_PITCH * c.y, VF_CHK_W, VF_CHK_H);
	}
	// 底部两条进度条是纯绘制的，没有控件要摆、也没有字要挂字体

	auto mountFont = [this](int id, HFONT f) {
		if (f) if (auto c = GetDlgItem(hwnd, id)) {
			SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
		}
	};
	// 结果框的等宽字 15 + 一档备用的小字 13：一行（尤其 SHA256 那 72 个字符）
	// 放不下时由 VfBox 自己降档，不会把末尾几位省略掉。
	// 结论不再画字，改成整圈外框染色：相同 = 绿框，不同 = 红框，见 VfBox::paint
	vfb.setFonts(fontOf(FsMono, 400, false, FaceMono), fontOf(FsMonoSmall, 400, false, FaceMono));
	for (const auto& c : kVerifyChks) mountFont(c.id, fontOf(FsBody, 400));
	for (const auto& b : kVerifyBtns) mountFont(b.id, fontOf(FsBody, 500));
}

void WinQr::showVerifyPage(bool visible)
{
	for (int id : kVerifyIds) {
		if (auto c = GetDlgItem(hwnd, id)) ShowWindow(c, visible ? SW_SHOW : SW_HIDE);
	}
	// 切回来时按当前忙闲把停止按钮的两态对齐（在别的页时校验可能已经跑完）
	if (visible) vfSyncStopBtn();
}

unsigned WinQr::vfAlgoMask() const
{
	unsigned mask = 0;
	// 勾选状态在窗口自己手里（vfChk），不从按钮的 BM_GETCHECK 取：
	// 自绘按钮存不住 check 位，见 WinQr.h 里 vfChk 的注释
	if (vfChk[2]) mask |= HashMd5;
	if (vfChk[3]) mask |= HashSha256;
	if (vfChk[4]) mask |= HashCrc32;
	if (vfChk[0]) mask |= ShowVersion;
	if (vfChk[1]) mask |= ShowDate;
	return mask;
}

void WinQr::vfBrowse()
{
	if (vfBusy) {
		showToast(Lang::get(L"vf.busy"));
		return;
	}
	// 多选：一次校验一批文件是这页最常见的用法
	std::vector<wchar_t> buf(64 * 1024, L'\0');
	const std::wstring title = Lang::get(L"vf.dlgTitle");   // 临时对象的 c_str 会悬空，先落一个变量
	// 过滤串必须是 "描述\0通配\0描述\0通配\0\0" 这种带嵌入 0 的形式。
	// 注意不能用 L"..." + 字面量拼接：operator+ 按 wcslen 走，第一个 \0 就被截断了，
	// 结果对话框拿到的是"只有描述、没有通配符"的半个串（原来就是这么错的）
	std::wstring filter = Lang::get(L"vf.filter");
	filter.push_back(L'\0');
	filter += L"*.*";
	filter.push_back(L'\0');
	filter.push_back(L'\0');

	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hwnd;
	ofn.lpstrFile = buf.data();
	ofn.nMaxFile = (DWORD)buf.size();
	ofn.lpstrFilter = filter.c_str();
	ofn.lpstrTitle = title.c_str();
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT |
		OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&ofn)) return;

	// 多选时 lpstrFile 是 "目录\0文件1\0文件2\0\0"；单选时整串就是完整路径
	std::vector<std::wstring> files;
	const wchar_t* dir = buf.data();
	const wchar_t* p = dir + wcslen(dir) + 1;
	if (*p == L'\0') {
		files.emplace_back(dir);
	}
	else {
		const std::wstring base = std::wstring(dir) + L"\\";
		for (; *p; p += wcslen(p) + 1) files.push_back(base + p);
	}
	vfStart(std::move(files));
}

void WinQr::vfClear()
{
	// 只置 vfCancel 是不够的：线程还在读下一块，之后仍会往 vfPending 里塞结果、
	// 往 vfProg 里写进度。清空框之后马上又冒出一行结果、进度条回跳，就是这个原因。
	// 先让线程收手并等它退出（最多一块 1MB），再动界面
	if (vfBusy || vfThread.joinable()) {
		vfStop();
		vfJoinWorker();
		vfBusy = false;
		// 线程退出前 Post 的 MsgVfDone / MsgVfProgress 还排在队列里：留到下一次消息循环，
		// vfOnDone 会看到 vfCancel=true 而弹一句"用户取消"，并把刚清掉的指纹重新写成基线，
		// 于是"清空 = 从头再来"当场失效。清空就一起丢掉这两条
		if (hwnd) {
			MSG stale{};
			while (PeekMessageW(&stale, hwnd, MsgVfProgress, MsgVfDone, PM_REMOVE)) {}
		}
		{
			std::lock_guard<std::mutex> lk(vfMutex);
			vfPending.clear();
		}
		vfSyncStopBtn();
	}
	vfb.clear();
	// 清空就是从头再来：记录、对比基线、结论一起丢掉，下一次校验回到"首次"。
	// 结论（一致 / 不一致）也算结果的一部分，不能留在框里
	vfHasPrev = false;
	vfPrevSig.clear();
	vfHasPrevDig = false;
	vfPrevDig[0].clear(); vfPrevDig[1].clear(); vfPrevDig[2].clear();
	vfVerdict = VfNone;
	vfSyncVerdict();
	vfProg.fileDone = 0; vfProg.fileTotal = 0;
	vfProg.allDone = 0;  vfProg.allTotal = 0;
	if (page == PageVerify) {
		RECT r{ sc(VfBarX), sc(VfRowFile), sc(VfBarX + VfBarW), sc(VfRowTotal + 3 + VfBarH) };
		InvalidateRect(hwnd, &r, FALSE);
	}
}

void WinQr::vfCopyText()
{
	const auto text = vfb.allText();
	if (text.empty()) {
		showToast(Lang::get(L"vf.empty"));
		return;
	}
	showToast(Lang::get(Util::copyText(text) ? L"vf.copied" : L"vf.copyFail"));
}

void WinQr::vfSave()
{
	const auto text = vfb.allText();
	if (text.empty()) {
		showToast(Lang::get(L"vf.empty"));
		return;
	}
	const auto path = Util::getSaveFilePath(hwnd, L"txt");
	if (path.empty()) return;
	showToast(Lang::get(Util::saveTextFile(path, text) ? L"vf.saved" : L"vf.saveFail"));
}

void WinQr::vfStop()
{
	// 只打标记：工作线程每读一块（1MB）检查一次，所以最多一秒内就停
	vfCancel = true;
}

// 停止按钮的两态：进度条在走（vfBusy）= 红 + 可点；闲着 = 灰 + 禁用。
// 用户的口径："只有进度条在走的时候，停止按钮都为红，可点击状态"。
// 自绘（BS_OWNERDRAW）按钮不跟着状态自己重画 —— WM_DRAWITEM 只在被 invalidate 时来，
// 所以这里必须显式重画一次，否则开始校验后按钮还是上个状态的灰，
// 看着"不起作用"（只有鼠标蹭上去才偶然变红）
void WinQr::vfSyncStopBtn()
{
	if (!hwnd) return;
	auto btn = GetDlgItem(hwnd, VfStop);
	if (!btn) return;
	EnableWindow(btn, vfBusy ? TRUE : FALSE);
	// 立刻重画：UpdateWindow 同步走完 WM_PAINT → WM_DRAWITEM，颜色当场就换过来
	InvalidateRect(btn, nullptr, FALSE);
	UpdateWindow(btn);
}

void WinQr::vfJoinWorker()
{
	if (vfThread.joinable()) vfThread.join();
}

void WinQr::vfSyncVerdict()
{
	const wchar_t* key = nullptr;
	// 结论只在框上显颜色，不写字：相同 = 整圈绿框，不同 = 整圈红框，
	// 中性态（首次 / 校验中）回到原来那条浅灰边框
	COLORREF frame = RGB(208, 216, 228);
	switch (vfVerdict) {
	case VfWaiting: key = L"vf.cmpWait"; break;
	case VfFirst: key = L"vf.cmpFirst"; break;
	case VfSame: key = L"vf.cmpSame"; frame = RGB(21, 128, 61); break;
	case VfDiff: key = L"vf.cmpDiff"; frame = RGB(220, 38, 38); break;
	default: break;   // VfNone：清空之后连"未对比"都不留
	}
	vfb.setVerdict(key ? Lang::get(key) : std::wstring{}, frame);
}

void WinQr::refreshVerdict()
{
	if (!hwnd) return;
	// 结论现在画在结果框内部，重画由 VfBox 自己触发；这里只负责把状态推过去
	vfSyncVerdict();
}

void WinQr::vfAppendRecord(const std::wstring& text)
{
	// 工作线程交上来的是"一个文件的完整文本"，按行拆开交给结果框。
	// 框里只留最新两条，拆行这一步在 UI 线程做，工作线程不碰控件
	std::vector<std::wstring> lines;
	size_t pos = 0;
	while (pos < text.size()) {
		const size_t e = text.find(L"\r\n", pos);
		if (e == std::wstring::npos) {
			lines.push_back(text.substr(pos));
			break;
		}
		lines.push_back(text.substr(pos, e - pos));
		pos = e + 2;
	}
	while (!lines.empty() && lines.back().empty()) lines.pop_back();
	if (!lines.empty()) vfb.pushRecord(std::move(lines));
}

int WinQr::vfJudge(const std::wstring cur[3]) const
{
	// 只比双方都有的算法：只算了 MD5 的一轮去比带了 SHA256 的对照值，
	// 不该因为"缺一项"就判不一致
	//
	// 但共同项必须**全部**一致才算一致。原来是"任意一项撞上就 same=true"，
	// 于是对照值里 MD5 与 SHA256 并存、而内容只改了后半段（MD5 相同、SHA256 不同）
	// 时会亮绿框 —— 校验工具给出错误的"一致"，比不给结论严重得多
	bool cmp{ false }, diff{ false };
	for (int i = 0; i < 3; i++) {
		if (cur[i].empty() || vfPrevDig[i].empty()) continue;
		cmp = true;
		if (_wcsicmp(cur[i].c_str(), vfPrevDig[i].c_str()) != 0) diff = true;
	}
	if (!cmp) return VfNone;
	return diff ? VfDiff : VfSame;
}

void WinQr::vfPasteCompare()
{
	// 没有输入框：整块结果框就是粘贴的地方。剪贴板空着就什么都不做
	const std::wstring text = clipboardText();
	if (text.empty()) return;

	// 粘贴的内容原样当一条记录（换行拆开、去掉行尾 \r、末尾空行丢掉），
	// 跟拖一个文件进来时长得一模一样，不额外加框、不加提示文字
	std::vector<std::wstring> lines;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t e = text.find(L'\n', pos);
		if (e == std::wstring::npos) e = text.size();
		std::wstring line = text.substr(pos, e - pos);
		if (!line.empty() && line.back() == L'\r') line.pop_back();
		lines.push_back(std::move(line));
		pos = e + 1;
	}
	while (!lines.empty() && lines.back().empty()) lines.pop_back();
	if (lines.empty()) return;

	std::wstring dig[3];
	parseDigests(text, dig);
	vfb.pushRecord(std::move(lines));

	bool gotDig{ false };
	for (const auto& v : dig) if (!v.empty()) gotDig = true;
	if (!gotDig) return;   // 认不出摘要：只显示记录，结论栏不动

	int v = vfHasPrevDig ? vfJudge(dig) : VfNone;
	if (v == VfNone) v = VfFirst;

	// 这一份成为下一次的基线：只覆盖认出来的算法，
	// 只粘 MD5 也不会把已有的 SHA256 基线抹掉
	for (int i = 0; i < 3; i++) if (!dig[i].empty()) vfPrevDig[i] = dig[i];
	vfHasPrevDig = true;
	// 上一次文件校验的整批指纹到这儿为止。用户刚贴了对照值，接下来校验文件就该跟它比；
	// 留着 vfHasPrev 会让 vfOnDone 优先走"整批指纹"那条，贴进来的值被彻底忽略
	// —— "先校验一次、再粘贴、再校验"就是这么错的
	vfHasPrev = false;
	vfPrevSig.clear();
	vfVerdict = v;
	vfSyncVerdict();
}

void WinQr::vfStart(std::vector<std::wstring> files)
{
	if (!hwnd || files.empty()) return;
	if (vfBusy) {
		showToast(Lang::get(L"vf.busy"));
		return;
	}
	const unsigned mask = vfAlgoMask();
	if (!(mask & (HashMd5 | HashSha256 | HashCrc32))) {
		showToast(Lang::get(L"vf.noAlgo"));
		return;
	}
	// 勾了算法不等于这台机器跑得起：FIPS 策略下 bcrypt 常把 MD5 直接禁掉。
	// 不在这儿拦下来，每个文件都会落成一行"读取失败"，用户根本看不出是算法不可用
	if (!HashCalc(mask).usable()) {
		showToast(Lang::get(L"vf.algoUnavail"));
		return;
	}

	vfJoinWorker();
	vfCancel = false;
	vfBusy = true;
	vfSyncStopBtn();   // 进度条开始走：停止按钮转红、转为可点
	{
		std::lock_guard<std::mutex> lk(vfMutex);
		vfPending.clear();
	}
	// 上一轮的结果留在框里，新一轮的记录进来后把最旧的顶掉（框内上限 2 条）
	vfSig.clear();
	vfRunDig[0].clear(); vfRunDig[1].clear(); vfRunDig[2].clear();
	vfRunDigValid = false;
	vfRunDigFiles = 0;
	vfProg.fileDone = 0; vfProg.fileTotal = 0;
	vfProg.allDone = 0;  vfProg.allTotal = 0;
	// 第一次校验根本没有"上一次"可比，连"对比中…"也是假的，干脆整栏不显示。
	// 从第二次起才提示"对比中…"；已经粘贴过对照值的也算"有上一次"
	vfVerdict = (vfHasPrev || vfHasPrevDig) ? VfWaiting : VfNone;
	vfSyncVerdict();

	const HWND target = hwnd;
	try {
		vfThread = std::thread([this, files, mask, target]() {
		// 本轮指纹：拿它跟上一轮比，就知道两次结果一不一致。
		// 判据只看"内容"—— 大小 + 这次真算出来的哈希，具体是：
		//   · 不带文件名、不带路径。同一份内容改名、换目录、换盘，仍然是同一份内容，
		//     判一致才符合直觉；哈希工具的惯例也一样（md5sum 比的从来是摘要，不是名字）。
		//     以前把完整路径拼进指纹，于是"同一张图另存为 copy.png"就被判成不一致。
		//   · 不带修改时间：拷贝、解压、从网盘拉回来都会改时间，跟内容无关。
		//   · 只放真正算了的那几种算法：没勾的留空，不参与比较。
		//   · 多个文件不看拖入顺序：拼之前按内容排序，否则先拖 A 后拖 B 与反过来会被
		//     当成"不一致"，可内容明明是同一批。
		std::vector<std::wstring> parts;

		// 总计进度得先有分母。这里只读文件元数据，不碰内容，几十个文件也是毫秒级
		unsigned long long allBytes = 0;
		for (const auto& f : files) {
			WIN32_FILE_ATTRIBUTE_DATA fa{};
			if (GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &fa)) {
				allBytes += ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
			}
		}
		vfProg.allTotal = allBytes;

		// 每读一块就 Post 一次会把消息队列灌爆，按时间限流到 ~12fps 足够顺
		ULONGLONG lastPost = 0;
		auto tick = [&lastPost, target]() {
			const ULONGLONG now = GetTickCount64();
			if (now - lastPost < 80) return;
			lastPost = now;
			PostMessageW(target, MsgVfProgress, 0, 0);
		};

		for (const auto& f : files) {
			if (vfCancel) break;
			FileDigest d;
			const bool ok = hashFile(f, mask, d, vfCancel, &vfProg, tick);
			if (ok) {
				// 一个文件的"内容身份证"：大小兜底，勾了的算法逐个附上
				std::wstring part = std::to_wstring(d.size);
				if (!d.md5.empty()) part += L"|" + d.md5;
				if (!d.sha256.empty()) part += L"|" + d.sha256;
				if (!d.crc32.empty()) part += L"|" + d.crc32;
				parts.push_back(std::move(part));
			}
			// 交上去的是"行 + \r\n"，UI 侧拆行后整块替换进结果框
			std::wstring chunk;
			if (ok) {
				for (const auto& line : digestLines(d, mask)) {
					chunk += line;
					chunk += L"\r\n";
				}
			}
			else if (!vfCancel) {
				// 取消不是失败：点了停止就只在该停的地方停，别往结果里写一行"读取失败" ——
				// 用户的口径"失败就是失败，取消就是取消"，停止的提示由 vfOnDone 的
				// toast（"用户取消"）去说。真失败也只写"读取失败"四个字：
				// 不跟路径、不跟原因，结果框里不堆一长串看不懂的文字
				chunk = Lang::get(L"vf.readFail") + L"\r\n";
			}
			{
				std::lock_guard<std::mutex> lk(vfMutex);
				// 一个文件一条：UI 那侧按元素逐个 pushRecord，记录之间才有分隔线
				vfPending.push_back(std::move(chunk));
				// 本轮算出来的摘要：多文件时最后一个算完的赢。
				// 跟 vfPending 同一把锁，UI 在 vfOnDone 里才读得到
				if (ok) {
					vfRunDig[0] = d.md5;
					vfRunDig[1] = d.sha256;
					vfRunDig[2] = d.crc32;
					vfRunDigValid = true;
					vfRunDigFiles++;
				}
			}
			PostMessageW(target, MsgVfProgress, 0, 0);
		}
		{
			// 排序后再拼：同一批文件换个拖入顺序，内容还是那一批
			std::sort(parts.begin(), parts.end());
			std::wstring sig;
			for (const auto& p : parts) {
				sig += p;
				sig += L'\n';
			}
			std::lock_guard<std::mutex> lk(vfMutex);
			vfSig = sig;
			// 跑完就把两条进度条落到 100%：中途读失败的文件会让 allDone 差一点，
			// 停在 97% 会让人以为还没跑完
			if (!vfCancel) {
				vfProg.allDone = vfProg.allTotal.load();
				vfProg.fileDone = vfProg.fileTotal.load();
			}
		}
		PostMessageW(target, MsgVfDone, 0, 0);
	});
	}
	catch (...) {
		// 线程创建失败：复位 busy，否则 MsgVfDone 永远不来，校验页从此拒绝一切操作。
		// 停止按钮也得跟着回灰 —— 上面刚把它转成"红 + 可点"，不复位就留着一个点不动的红按钮。
		// 结论栏同理：上面刚摆成"对比中…"，不撤掉就永远停在那句话上
		vfBusy = false;
		vfVerdict = VfNone;
		vfSyncVerdict();
		vfSyncStopBtn();
		showToast(Lang::get(L"vf.fail"));
	}
}

void WinQr::vfOnProgress()
{
	std::vector<std::wstring> delta;
	{
		std::lock_guard<std::mutex> lk(vfMutex);
		delta.swap(vfPending);
	}
	// 一个文件一条记录（框里只留最新两条，来回滚着看）。
	// 一次刷新可能攒了几个文件的文本，必须逐个 push —— 整块塞进去就成了一条，
	// 这批文件之间连分隔线都没有
	for (const auto& one : delta) vfAppendRecord(one);

	// 进度条画在父窗口上（见 drawVerifyPage），刷这一块就够了，不必整窗重绘
	if (page == PageVerify) {
		RECT r{ sc(VfBarX), sc(VfRowFile), sc(VfBarX + VfBarW), sc(VfRowTotal + 3 + VfBarH) };
		InvalidateRect(hwnd, &r, FALSE);
	}
}

void WinQr::vfOnDone()
{
	std::wstring sig;
	std::wstring runDig[3];
	bool runValid{ false };
	int runFiles{ 0 };
	{
		std::lock_guard<std::mutex> lk(vfMutex);
		sig = vfSig;
		runDig[0] = vfRunDig[0];
		runDig[1] = vfRunDig[1];
		runDig[2] = vfRunDig[2];
		runValid = vfRunDigValid;
		runFiles = vfRunDigFiles;
	}
	const bool canceled = vfCancel.load();
	vfCancel = false;
	vfBusy = false;
	vfSyncStopBtn();   // 进度条停了：停止按钮回灰、转为禁用
	vfJoinWorker();

	// 对比：跟"上一次跑完的指纹"比。被取消的这一轮不算数，不参与对比，
	// 否则点一下停止就把历史顶掉了。
	// 第一次校验显示"首次校验"：这是 ReadMe 里写明的行为（"没有历史时显示「首次校验」"），
	// 也是用户点名要修的问题 —— 右侧结论栏空着反而像漏了结果。VfFirst 走中性浅灰，
	// 和"对比中…"同一套配色，不会跟绿/红结论混淆
	if (!canceled && (!sig.empty() || runValid)) {
		// 上一次也是文件校验：按整批指纹比（多文件、拖入顺序都不影响）
		if (vfHasPrev) {
			vfVerdict = (sig == vfPrevSig) ? VfSame : VfDiff;
		}
		else if (vfHasPrevDig) {
			// 上一次是粘贴进来的对照值：没有整批指纹，只能按摘要比 ——
			// "先输入、后文件"就是走这一条。
			// 但粘贴的对照值是"一个文件的一份摘要"，一批文件跟它没有对应关系：
			// 拿最后一个文件的摘要去比，10 个里 9 个没比也照样亮绿框。
			// 这种场合宁可不给结论（中性外框），也不给一个以偏概全的"一致"
			if (runFiles > 1) {
				vfVerdict = VfNone;
			}
			else {
				const int v = vfJudge(runDig);
				vfVerdict = (v == VfNone) ? VfFirst : v;
			}
		}
		else {
			vfVerdict = VfFirst;
		}
		// 本轮结果成为下一次的基线：摘要与整批指纹各留一份。
		// 只覆盖这一轮真算出来的算法 —— 本轮只勾了 MD5 时，runDig[1]/[2] 是空串，
		// 无脑覆盖会把上一轮粘贴进来的 SHA256 基线抹掉（vfPasteCompare 那边就是按
		// "只覆盖认出来的算法"写的，两处口径必须一致）
		if (runValid) {
			for (int i = 0; i < 3; i++) {
				if (!runDig[i].empty()) vfPrevDig[i] = runDig[i];
			}
			vfHasPrevDig = true;
		}
		vfPrevSig = sig;
		vfHasPrev = true;
	}
	// 取消或一个文件都没读成功：别把"校验中…"留在那儿冒充结果
	else if (vfVerdict == VfWaiting) {
		vfVerdict = VfNone;
	}
	refreshVerdict();
	// 完成不弹提示：两条进度条落到 100%、右边结论已写清楚，浮层只会压住进度条。
	// 只有"被停止"这种非正常结束才需要说一声
	if (canceled) showToast(Lang::get(L"vf.stopped"));
}
