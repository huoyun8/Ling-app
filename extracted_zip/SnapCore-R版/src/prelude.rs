//! 全 crate 共用的外部导入：std / winapi / 第三方 crate 集中在此，
//! 各模块只需 `use crate::prelude::*;`，避免每个文件重复抄一长串 use。

pub use std::ffi::OsStr;
pub use std::os::windows::ffi::OsStrExt;
pub use std::sync::atomic::{AtomicBool, AtomicI32, AtomicI64, AtomicIsize, AtomicU32, AtomicUsize, Ordering};
pub use std::sync::{Arc, Mutex};
pub use std::time::Duration;

pub use qrcode::{QrCode, EcLevel};
pub use rqrr::PreparedImage;
pub use image::GenericImageView;
pub use winapi::shared::minwindef::{WPARAM, LPARAM, LRESULT, UINT, HKEY};
pub use winapi::shared::windef::{HWND, RECT, HBRUSH, HMENU, POINT, HFONT};
pub use winapi::um::winnt::{KEY_QUERY_VALUE, KEY_SET_VALUE, REG_DWORD, REG_SZ};
pub use winapi::um::errhandlingapi::{GetLastError, SetLastError};
pub use winapi::um::libloaderapi::{GetModuleFileNameW, GetModuleHandleW, GetProcAddress};
pub use winapi::shared::minwindef::FILETIME;
pub use winapi::shared::windef::SIZE;
pub use winapi::um::processthreadsapi::{GetCurrentThreadId, GetProcessTimes, OpenProcess, TerminateProcess};
pub use winapi::um::synchapi::CreateMutexW;
pub use winapi::um::winbase::{GlobalAlloc, GlobalFree, GlobalLock, GlobalUnlock, GMEM_MOVEABLE};
pub use winapi::um::sysinfoapi::GetWindowsDirectoryW;
pub use winapi::um::reason::SHTDN_REASON_MAJOR_OTHER;
pub use winapi::um::commdlg::{GetOpenFileNameW, OPENFILENAMEW};
pub use winapi::um::knownfolders::FOLDERID_Desktop;
pub use winapi::um::shlobj::SHGetKnownFolderPath;
pub use winapi::um::combaseapi::{CoInitializeEx, CoTaskMemFree, CoUninitialize};
pub use winapi::um::objbase::COINIT_APARTMENTTHREADED;
pub use winapi::um::winreg::{
    RegCloseKey, RegCreateKeyExW, RegDeleteValueW, RegOpenKeyExW, RegQueryValueExW,
    RegSetValueExW, HKEY_CURRENT_USER,
};
pub use winapi::um::tlhelp32::{
    CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W,
    TH32CS_SNAPPROCESS,
};
pub use winapi::um::shellapi::{Shell_NotifyIconW, NOTIFYICONDATAW, NIM_ADD, NIM_DELETE, NIF_ICON, NIF_MESSAGE, NIF_TIP,
    // 文件拖入：WM_DROPFILES
    DragAcceptFiles, DragQueryFileW, DragFinish, HDROP};
pub use winapi::um::winuser::{
    AppendMenuW, BeginPaint, BS_OWNERDRAW, CreatePopupMenu, CreateWindowExW,
    CS_HREDRAW, CS_VREDRAW, DefWindowProcW, DestroyMenu, DispatchMessageW,
    DRAWITEMSTRUCT, DrawTextW, DT_CENTER, DT_SINGLELINE, DT_VCENTER, EmptyClipboard,
    EndPaint, EnumWindows, FindWindowW, GetAsyncKeyState, GetCapture, GetCursorPos, GetDlgItem, GetKeyState, GetMessageW,
    GetSystemMetrics, GetWindowTextW, GetWindowThreadProcessId, IsWindowVisible,
    SystemParametersInfoW,
    IDC_ARROW, LoadCursorW, LoadIconW, MF_STRING, MOD_ALT, MOD_CONTROL, MOD_SHIFT,
    MOD_WIN, ODS_SELECTED, OpenClipboard, GetClipboardData, PAINTSTRUCT, PostQuitMessage, RegisterClassExW,
    RegisterHotKey, SetForegroundWindow, SetWindowTextW, SM_CXSCREEN, SM_CYSCREEN, SW_HIDE,
    // 多显示器：默认只铺"光标所在的那一块屏"，设置页可切到整个虚拟桌面
    MonitorFromPoint, GetMonitorInfoW, MONITORINFO, MONITOR_DEFAULTTONEAREST,
    SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN, SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN,
    SW_SHOW, ShowWindow, SendMessageW, SetClipboardData, CloseClipboard, TrackPopupMenu, SetCursor,
    IsWindow, SW_SHOWNA,
    TranslateMessage, TPM_BOTTOMALIGN, TPM_RETURNCMD, TPM_RIGHTBUTTON, UnregisterHotKey,
    WM_HOTKEY, WM_CLOSE, WS_CHILD, WS_VISIBLE, WM_COMMAND, WM_CREATE,
    WM_DESTROY, WM_DRAWITEM, WM_ERASEBKGND, WM_KEYDOWN, WM_LBUTTONDOWN, WM_LBUTTONDBLCLK,
    WM_LBUTTONUP, WM_MOUSELEAVE, WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_PAINT, WM_RBUTTONUP, WM_USER,
    WM_GETMINMAXINFO,
    WM_SETFOCUS,
    WM_CTLCOLORSTATIC, GetDlgCtrlID,
    WNDCLASSEXW, DestroyWindow, FillRect, GetClientRect, GetDC, GetWindowRect,
    InvalidateRect, ReleaseCapture, ReleaseDC, SetCapture, TrackMouseEvent, TME_LEAVE,
    TRACKMOUSEEVENT, UpdateWindow, MSG, WS_BORDER, WS_POPUP, WS_EX_TOPMOST,
    WS_EX_TOOLWINDOW, WS_EX_NOACTIVATE, IDC_CROSS, VK_CONTROL, VK_ESCAPE, VK_LWIN, VK_MENU, VK_RWIN, VK_SHIFT,
    VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_HOME, VK_END, VK_DELETE, IDC_IBEAM,
    IDC_HAND, IDC_SIZENWSE, IDC_SIZENESW, IDC_SIZEWE, IDC_SIZEALL, SetCaretPos, CreateCaret,
    GetForegroundWindow, AttachThreadInput,
    WS_CLIPCHILDREN,
    AdjustWindowRect, SetProcessDPIAware, SetWindowLongPtrW, GetWindowLongPtrW, CallWindowProcW, GWLP_WNDPROC, GWLP_USERDATA,
    WM_SETFONT, VK_RETURN, PostMessageW, SetFocus, SetTimer, KillTimer, WM_TIMER, SetWindowPos,
    ES_MULTILINE, ES_AUTOVSCROLL, ES_WANTRETURN, SWP_NOMOVE, SWP_NOSIZE, SWP_NOZORDER, SWP_NOACTIVATE, SWP_NOREDRAW, SWP_FRAMECHANGED, GWL_STYLE,
    ExitWindowsEx, WM_SYSCOMMAND,
    EWX_POWEROFF, EWX_FORCE,
    MessageBoxW, MB_OK, MB_ICONINFORMATION, MB_ICONERROR, MB_TOPMOST, MB_SETFOREGROUND,
    ES_CENTER,
    PeekMessageW, PM_REMOVE, WM_QUIT,
    SS_CENTERIMAGE, FrameRect, DT_LEFT,
    // 录制中宿主遮罩：区域挖洞 + 分层透明度 + 不进采集
    SetWindowDisplayAffinity, SetLayeredWindowAttributes, SetWindowRgn, HWND_TOPMOST,
    WM_CHAR, UpdateLayeredWindow, ULW_ALPHA, WS_EX_LAYERED, CS_DBLCLKS, ScreenToClient,
    DT_CALCRECT, DT_NOPREFIX, WS_EX_TRANSPARENT,
    NONCLIENTMETRICSW, SPI_GETNONCLIENTMETRICS,
    // 智能选区：桌面图标层（FindWindowExW 找 DefView/SysListView32，跨进程 LVM 要
    // 带超时的 SendMessageTimeoutW）、系统 z 序（GetTopWindow/GetWindow）、并集矩形
    FindWindowExW, SendMessageTimeoutW,
    GetTopWindow, GetWindow,
    // 拖放：主窗口和每个子控件都要接 WM_DROPFILES（子控件自己不接就拒收），
    // 收到后统一转给主窗口的 handle_drop
    EnumChildWindows, GetParent, WS_EX_ACCEPTFILES,
    // UIPI 放行（管理员权限运行时普通窗口发来的 WM_DROPFILES 会被过滤掉）
    ChangeWindowMessageFilterEx,
};
pub use winapi::um::wingdi::{
    CreateRoundRectRgn, CreateSolidBrush, CreateFontW, CreateBitmap, DeleteObject, FillRgn,
    SelectObject, SetBkMode, SetBkColor, SetTextColor, StretchBlt, FF_SWISS, OUT_DEFAULT_PRECIS,
    CLIP_DEFAULT_PRECIS, DEFAULT_CHARSET, VARIABLE_PITCH, TRANSPARENT,
    BitBlt, CreateCompatibleBitmap, CreateCompatibleDC, DeleteDC, SetDIBitsToDevice, StretchDIBits, SRCCOPY,
    BI_RGB, DIB_RGB_COLORS, BITMAPINFO, BITMAPINFOHEADER, GetDeviceCaps, LOGPIXELSX,
    CLEARTYPE_QUALITY,
    CreateDIBSection, DEFAULT_QUALITY, TextOutW, GetTextExtentPoint32W,
    ExtTextOutW, GetCharABCWidthsFloatW,
    CreatePen, GetStockObject, NULL_BRUSH, Rectangle, PS_SOLID,
    GetTextMetricsW, TEXTMETRICW,
    Polygon, CreateRectRgn, CreatePolygonRgn, CombineRgn, WINDING, RGN_OR, RGN_DIFF,
    MoveToEx, LineTo, XFORM, SetGraphicsMode, SetWorldTransform, GM_ADVANCED,
    SetStretchBltMode, HALFTONE, GdiFlush,
    GetPixel, SetPixelV, CLR_INVALID,
    SetViewportOrgEx, SaveDC, RestoreDC, IntersectClipRect, GetClipRgn, SelectClipRgn,
    AlphaBlend, BLENDFUNCTION, AC_SRC_OVER, AC_SRC_ALPHA,
    LOGFONTW,
};
