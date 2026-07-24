// Phase 1 viewer: main frame window with a toolbar, status bar, a
// collapsible thumbnail sidebar, and a scrolling page canvas. Rendering is
// done by PdfDocument (statically-linked MuPDF) into cached DIB sections.
#include "viewer_window.h"
#include "pdf_document.h"
#include "print_document.h"
#include "resource.h"
#include "updater.h"
#include "version.h"
#include "webview_convert.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>   // SHChangeNotify
#include <shobjidl.h> // IApplicationAssociationRegistrationUI (offer-set-as-default)
#include <uxtheme.h>  // SetWindowTheme (flat "Explorer" edit-box border)
#include <dwmapi.h>   // DwmSetWindowAttribute (Win11 rounded corners, Mica, dark title bar)
#include <windowsx.h>
#include <richedit.h> // MSFTEDIT_CLASS / CHARFORMAT2W -- the rich-text-editor tool's edit control

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <regex>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Vector toolbar icons only -- no external image assets, keeps the exe
// self-contained. gdiplus.dll is a system component (ships with Windows),
// not a redistributable dependency.
#include <gdiplus.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

// ---------------------------------------------------------------------------
// Windows 11 native chrome (DwmSetWindowAttribute) -- rounded corners, Mica
// backdrop, dark title bar. These attribute/enum values landed in newer
// Windows SDK headers; define them defensively so the build works regardless
// of the installed SDK vintage. On pre-Win11 the DwmSetWindowAttribute calls
// simply return a failure HRESULT we ignore -- no version gating needed.
// ---------------------------------------------------------------------------
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2   // Mica
#endif

namespace {

// True if the user's Windows app theme is Dark (HKCU AppsUseLightTheme == 0).
// Defaults to light on any read failure. Reused by the DWM dark-title-bar call
// and (later) the custom-drawn chrome palette.
bool SystemUsesDarkTheme()
{
	DWORD value = 1, size = sizeof(value);
	if (RegGetValueW(HKEY_CURRENT_USER,
			L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
			L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
		return false;
	return value == 0;
}

// Apply Win11 native window chrome: rounded corners + Mica backdrop + a title
// bar tinted `dark`. Each call fails harmlessly on pre-Win11 builds. Safe to
// call again -- ApplyTitleBarDarkMode() re-invokes just the title-bar part
// whenever the app theme toggles (rounded corners/Mica don't need reapplying).
void ApplyTitleBarDarkMode(HWND hwnd, bool dark)
{
	BOOL v = dark ? TRUE : FALSE;
	DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &v, sizeof(v));
}

void ApplyModernWindowChrome(HWND hwnd, bool dark)
{
	ApplyTitleBarDarkMode(hwnd, dark);

	int corner = DWMWCP_ROUND;
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

	int backdrop = DWMSBT_MAINWINDOW;
	DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
}

// ---------------------------------------------------------------------------
// App-wide light/dark palette for all the custom-drawn chrome (toolbar, tab
// strip, canvas surround, thumbnail panel, empty-state grid) plus the handful
// of native controls (status bar, secondary-bar STATIC/EDIT children) we
// explicitly recolor. Every kLightTheme value matches what was previously a
// hardcoded literal at its use site, so switching this in is a no-op for the
// existing light appearance; kDarkTheme targets Edge's dark PDF viewer look.
// ---------------------------------------------------------------------------
struct ThemeColors {
	COLORREF toolbarBg, toolbarText, toolbarHoverPill, toolbarPressedPill, toolbarActivePill;
	Gdiplus::ARGB iconInk;
	COLORREF tabActiveBg, tabInactiveBg, tabBorder, tabText, tabAccent;
	COLORREF canvasSurround;
	COLORREF emptyHeading, emptySubtitle, tileBg, tileBorder, tileText;
	COLORREF thumbBg, thumbText, thumbCurrentText, thumbSelFill, thumbSelBorder;
	COLORREF ctrlBg, ctrlText, frameBg;
	COLORREF statusBg, statusText;
};

constexpr ThemeColors kLightTheme = {
	RGB(255, 255, 255), RGB(0x30, 0x30, 0x30), RGB(0xF2, 0xF2, 0xF2), RGB(0xDE, 0xDE, 0xDE), RGB(0xE8, 0xE8, 0xE8),
	0xFF424242,
	RGB(255, 255, 255), RGB(238, 238, 238), RGB(210, 210, 210), RGB(40, 40, 40), RGB(0, 120, 215),
	RGB(236, 236, 236),
	RGB(60, 60, 60), RGB(150, 150, 150), RGB(249, 249, 249), RGB(226, 226, 226), RGB(70, 70, 70),
	RGB(243, 243, 243), RGB(90, 90, 90), RGB(0, 90, 180), RGB(204, 228, 247), RGB(0, 120, 215),
	RGB(255, 255, 255), RGB(0, 0, 0), RGB(255, 255, 255),
	RGB(255, 255, 255), RGB(0, 0, 0),
};

constexpr ThemeColors kDarkTheme = {
	RGB(50, 50, 50), RGB(225, 225, 225), RGB(70, 70, 70), RGB(90, 90, 90), RGB(80, 80, 80),
	0xFFE0E0E0,
	RGB(50, 50, 50), RGB(32, 32, 32), RGB(70, 70, 70), RGB(225, 225, 225), RGB(28, 130, 225),
	RGB(32, 32, 32),
	RGB(225, 225, 225), RGB(160, 160, 160), RGB(45, 45, 45), RGB(75, 75, 75), RGB(210, 210, 210),
	RGB(30, 30, 30), RGB(170, 170, 170), RGB(90, 165, 250), RGB(35, 65, 100), RGB(28, 130, 225),
	RGB(45, 45, 45), RGB(220, 220, 220), RGB(32, 32, 32),
	RGB(50, 50, 50), RGB(225, 225, 225),
};

inline const ThemeColors& Theme(bool dark) { return dark ? kDarkTheme : kLightTheme; }

// Persisted user theme override: HKCU\Software\PDFast\ThemeMode = "light" |
// "dark" (any other value, or absent, means "follow the system theme").
constexpr wchar_t kThemeRegKey[] = L"Software\\PDFast";
constexpr wchar_t kThemeRegValue[] = L"ThemeMode";

// Returns the mode to start in: the user's saved explicit choice if any,
// otherwise whatever the OS is currently set to.
bool InitialDarkMode()
{
	wchar_t buf[16] = {};
	DWORD size = sizeof(buf);
	if (RegGetValueW(HKEY_CURRENT_USER, kThemeRegKey, kThemeRegValue, RRF_RT_REG_SZ,
			nullptr, buf, &size) == ERROR_SUCCESS) {
		if (_wcsicmp(buf, L"dark") == 0) return true;
		if (_wcsicmp(buf, L"light") == 0) return false;
	}
	return SystemUsesDarkTheme();
}

void SaveThemeOverride(bool dark)
{
	HKEY key;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kThemeRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
		const wchar_t* v = dark ? L"dark" : L"light";
		RegSetValueExW(key, kThemeRegValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(v),
			static_cast<DWORD>((wcslen(v) + 1) * sizeof(wchar_t)));
		RegCloseKey(key);
	}
}

} // anonymous namespace for DWM chrome + theme helpers

namespace {

constexpr wchar_t kFrameClass[] = L"PdfViewerFrame";
constexpr wchar_t kCanvasClass[] = L"PdfViewerCanvas";
constexpr wchar_t kThumbClass[] = L"PdfViewerThumbs";

// Count of live top-level FrameWindow instances (normally 1, but dragging a
// tab out or "Open in New Window" can create more) -- PostQuitMessage only
// fires once the LAST one is destroyed, not just the one closed first. Every
// FrameWindow is heap-allocated and self-deletes on its own WM_NCDESTROY
// (see FrameWindow::Proc), so nothing else needs to own or free it.
int g_liveFrameCount = 0;

// Single-instance plumbing: a second launch (e.g. double-clicking another PDF
// while PDFast is the default handler) hands its path to the already-running
// instance via WM_COPYDATA and exits, so everything lives in one window as
// tabs. This magic tags our WM_COPYDATA so we don't act on stray ones.
constexpr ULONG_PTR kCopyDataOpenFile = 0x50444601; // 'PDF\1'
constexpr wchar_t kColorPopupClass[] = L"PdfColorPopup";
constexpr wchar_t kResizeHandleClass[] = L"PdfResizeHandle";

// Posted from the background update-check thread back to the frame; LPARAM is
// a heap-allocated updater::UpdateInfo (frame takes ownership), WPARAM is the
// `manual` flag. See startUpdateCheck()/onUpdateResult().
constexpr UINT WM_APP_UPDATE_RESULT = WM_APP + 1;

// Resize-handle direction indices, matching the 8-entry layout used by
// positionResizeHandles()/updateResize(): NW,N,NE,W,E,SW,S,SE. kResizeMove is
// a sentinel for activeResizeDir_ meaning "dragging the move frame", not one
// of the 8 corner/edge squares.
enum ResizeDir { kResizeNW, kResizeN, kResizeNE, kResizeW, kResizeE, kResizeSW, kResizeS, kResizeSE, kResizeMove };

// Fixed color choices for the inline free-text swatch popup (matches the
// small always-visible palette other viewers show instead of a color dialog).
constexpr COLORREF kTextSwatches[4] = { RGB(210, 40, 40), RGB(30, 140, 60), RGB(40, 110, 220), RGB(20, 20, 20) };

constexpr int kBaseThumbWidth = 160; // logical px at 96 DPI
constexpr int kBasePageGap = 12;
constexpr float kZoomStep = 1.25f;
constexpr float kMinZoom = 0.08f;
constexpr float kMaxZoom = 12.0f;

int Scale(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

// Width of the page-number edit box within the toolbar's reserved
// IDM_VIEW_PAGELABEL rect (the rest shows "of N") -- shared by
// beginPageNumberEdit(), layout(), and the CDDS_ITEMPREPAINT "of N" draw so
// all three always agree on where the boundary is.
int PageEditBoxWidth(int totalW, UINT dpi) { return std::max(Scale(30, dpi), totalW * 4 / 10); }

// Ctrl+Backspace in a stock Win32 EDIT control does NOT delete the previous
// word -- the keyboard layout translates it to the ASCII "DEL" control char
// (0x7F), which the default edit proc just inserts as literal text, showing
// up as a missing-glyph box. Call this from a WM_KEYDOWN handler (returns
// true if it did the delete, in which case also swallow the WM_CHAR(0x7F)
// that follows) to make Ctrl+Backspace behave like every other text field.
bool ConsumeCtrlBackspaceWordDelete(HWND hwnd, WPARAM wp)
{
	if (wp != VK_BACK || !(GetKeyState(VK_CONTROL) & 0x8000)) return false;
	DWORD selStart = 0, selEnd = 0;
	SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));
	if (selStart != selEnd) {
		SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
		return true;
	}
	if (selStart == 0) return true; // nothing before the caret; just swallow
	int len = GetWindowTextLengthW(hwnd);
	std::wstring text(static_cast<size_t>(len) + 1, L'\0');
	GetWindowTextW(hwnd, text.data(), len + 1);
	text.resize(static_cast<size_t>(len));
	int i = static_cast<int>(selStart);
	while (i > 0 && std::iswspace(text[i - 1])) i--;
	while (i > 0 && !std::iswspace(text[i - 1])) i--;
	SendMessageW(hwnd, EM_SETSEL, i, selStart);
	SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
	return true;
}

// A stock Win32 EDIT control has no built-in Ctrl+A -- unlike Ctrl+C/X/V/Z,
// "select all" was never wired into the base control (that only came with
// RichEdit). Call from a WM_KEYDOWN handler; returns true if it handled it.
bool ConsumeCtrlSelectAll(HWND hwnd, WPARAM wp)
{
	if (wp != 'A' || !(GetKeyState(VK_CONTROL) & 0x8000)) return false;
	SendMessageW(hwnd, EM_SETSEL, 0, -1);
	return true;
}

std::string WideToUtf8(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (n <= 0) return {};
	std::string s(static_cast<size_t>(n - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring Utf8ToWide(const std::string& s)
{
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	if (n <= 0) return {};
	std::wstring w(static_cast<size_t>(n - 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	return w;
}

// Alpha-blend a solid color over a rect (for translucent search highlights).
void FillAlpha(HDC dc, const RECT& r, COLORREF color, BYTE alpha)
{
	int w = r.right - r.left, h = r.bottom - r.top;
	if (w <= 0 || h <= 0) return;
	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = 1; bmi.bmiHeader.biHeight = 1;
	bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HDC mem = CreateCompatibleDC(dc);
	HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (bmp && bits) {
		// premultiplied BGRA for AC_SRC_ALPHA
		auto* px = static_cast<BYTE*>(bits);
		px[0] = static_cast<BYTE>(GetBValue(color) * alpha / 255);
		px[1] = static_cast<BYTE>(GetGValue(color) * alpha / 255);
		px[2] = static_cast<BYTE>(GetRValue(color) * alpha / 255);
		px[3] = alpha;
		HBITMAP old = static_cast<HBITMAP>(SelectObject(mem, bmp));
		BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
		AlphaBlend(dc, r.left, r.top, w, h, mem, 0, 0, 1, 1, bf);
		SelectObject(mem, old);
	}
	if (bmp) DeleteObject(bmp);
	DeleteDC(mem);
}

// A minimal modal text-input dialog (Win32 has no stock InputBox). Returns
// true if the user pressed OK; `text` is in/out (seed value -> entered value).
struct InputBoxState { HWND edit = nullptr; std::wstring text; bool ok = false; bool done = false; };

LRESULT CALLBACK InputBoxProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	auto* st = reinterpret_cast<InputBoxState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
	if (m == WM_NCCREATE) {
		auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
		SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
	}
	if (!st) return DefWindowProcW(h, m, w, l);
	switch (m) {
	case WM_COMMAND:
		if (LOWORD(w) == IDOK) {
			int n = GetWindowTextLengthW(st->edit);
			std::wstring buf(static_cast<size_t>(n) + 1, L'\0');
			GetWindowTextW(st->edit, buf.data(), n + 1);
			buf.resize(n);
			st->text = buf; st->ok = true; DestroyWindow(h);
			return 0;
		}
		if (LOWORD(w) == IDCANCEL) { st->ok = false; DestroyWindow(h); return 0; }
		break;
	case WM_CLOSE: st->ok = false; DestroyWindow(h); return 0;
	case WM_DESTROY: st->done = true; return 0;
	}
	return DefWindowProcW(h, m, w, l);
}

bool InputBox(HWND owner, HINSTANCE hInst, const wchar_t* title,
	const wchar_t* prompt, std::wstring& text)
{
	static bool reg = false;
	if (!reg) {
		WNDCLASSW wc = {};
		wc.lpfnWndProc = InputBoxProc;
		wc.hInstance = hInst;
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
		wc.lpszClassName = L"PdfInputBox";
		RegisterClassW(&wc);
		reg = true;
	}
	UINT dpi = GetDpiForWindow(owner);
	int W = Scale(360, dpi), H = Scale(150, dpi);
	RECT o; GetWindowRect(owner, &o);
	int x = o.left + ((o.right - o.left) - W) / 2;
	int y = o.top + ((o.bottom - o.top) - H) / 2;

	InputBoxState st; st.text = text;
	HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"PdfInputBox", title,
		WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, owner, nullptr, hInst, &st);
	if (!h) return false;

	HFONT f = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	NONCLIENTMETRICSW ncm = { sizeof(ncm) };
	if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi))
		f = CreateFontIndirectW(&ncm.lfMessageFont);

	int pad = Scale(12, dpi);
	HWND lbl = CreateWindowExW(0, L"STATIC", prompt, WS_CHILD | WS_VISIBLE,
		pad, pad, W - 2 * pad, Scale(18, dpi), h, nullptr, hInst, nullptr);
	st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
		WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, pad, Scale(34, dpi),
		W - 2 * pad - Scale(16, dpi), Scale(24, dpi), h, nullptr, hInst, nullptr);
	HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
		W - Scale(180, dpi), Scale(72, dpi), Scale(78, dpi), Scale(26, dpi),
		h, reinterpret_cast<HMENU>(IDOK), hInst, nullptr);
	HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
		W - Scale(94, dpi), Scale(72, dpi), Scale(78, dpi), Scale(26, dpi),
		h, reinterpret_cast<HMENU>(IDCANCEL), hInst, nullptr);
	for (HWND c : { lbl, st.edit, ok, cancel })
		SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);

	ShowWindow(h, SW_SHOW);
	SetFocus(st.edit);
	SendMessageW(st.edit, EM_SETSEL, 0, -1);
	EnableWindow(owner, FALSE);
	MSG msg;
	while (!st.done && GetMessageW(&msg, nullptr, 0, 0)) {
		if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
	}
	EnableWindow(owner, TRUE);
	SetActiveWindow(owner);
	if (f != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(f);
	if (st.ok) text = st.text;
	return st.ok;
}

// Native font picker mapped to a PDF base-14 font name (Helv/TiRo/Cour) plus
// size (points) and color. Free-text annotations need a base font name.
bool ChooseFontMapped(HWND owner, std::string& fontOut, float& sizeOut, COLORREF& colorInOut)
{
	LOGFONTW lf = {};
	HDC dc = GetDC(owner);
	lf.lfHeight = -MulDiv(static_cast<int>(sizeOut > 0 ? sizeOut : 12),
		GetDeviceCaps(dc, LOGPIXELSY), 72);
	ReleaseDC(owner, dc);
	CHOOSEFONTW cf = { sizeof(cf) };
	cf.hwndOwner = owner;
	cf.lpLogFont = &lf;
	cf.rgbColors = colorInOut;
	cf.Flags = CF_SCREENFONTS | CF_EFFECTS | CF_INITTOLOGFONTSTRUCT;
	if (!ChooseFontW(&cf)) return false;
	sizeOut = cf.iPointSize / 10.0f;
	colorInOut = cf.rgbColors;
	std::wstring face = lf.lfFaceName;
	auto has = [&](const wchar_t* s) { return face.find(s) != std::wstring::npos; };
	bool fixed = (lf.lfPitchAndFamily & 0x03) == FIXED_PITCH ||
		(lf.lfPitchAndFamily & 0xF0) == FF_MODERN;
	if (fixed || has(L"Courier") || has(L"Consol") || has(L"Mono")) fontOut = "Cour";
	else if ((lf.lfPitchAndFamily & 0xF0) == FF_ROMAN ||
		has(L"Times") || has(L"Serif") || has(L"Georgia") || has(L"Roman")) fontOut = "TiRo";
	else fontOut = "Helv";
	return true;
}

std::wstring OpenFileDialog(HWND owner)
{
	wchar_t file[MAX_PATH] = L"";
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = owner;
	ofn.lpstrFilter = L"PDF Documents (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0";
	ofn.lpstrFile = file;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
	ofn.lpstrTitle = L"Open PDF";
	if (GetOpenFileNameW(&ofn)) return file;
	return L"";
}

// Multi-select variant used by Merge/"Insert Pages..." (default filter/title)
// and Convert to PDF (its own filter/title). Uses the modern IFileOpenDialog
// (COM) rather than the classic GetOpenFileNameW -- the classic dialog's
// multi-select buffer convention (one shared directory + a list of names)
// can only represent files from a single folder, and outright refuses a
// selection spanning more than one ("You can choose multiple items only if
// they are all located in the same folder"), including within an aggregating
// view like Explorer's own Search Results. IFileOpenDialog's GetResults()
// returns an IShellItemArray of independent shell items, each carrying its
// own full path, so a multi-select spanning several real folders (via search
// results, "Recent", a library, etc.) works with no extra looping needed.
std::vector<std::wstring> OpenFileDialogMulti(HWND owner,
	const wchar_t* filter = L"PDF Documents (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0",
	const wchar_t* title = L"Select PDF Files")
{
	std::vector<std::wstring> out;

	HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	bool comOwned = SUCCEEDED(hrInit);
	if (!(comOwned || hrInit == RPC_E_CHANGED_MODE)) return out;

	// Parse the legacy double-null-terminated "Name\0*.ext\0..." filter
	// string (kept as-is so every call site stays unchanged) into the
	// COMDLG_FILTERSPEC pairs IFileOpenDialog wants.
	std::vector<std::wstring> specStorage;
	const wchar_t* fp = filter;
	while (*fp) {
		std::wstring name = fp; fp += name.size() + 1;
		if (!*fp) break;
		std::wstring pattern = fp; fp += pattern.size() + 1;
		specStorage.push_back(std::move(name));
		specStorage.push_back(std::move(pattern));
	}
	std::vector<COMDLG_FILTERSPEC> specs;
	for (size_t i = 0; i + 1 < specStorage.size(); i += 2)
		specs.push_back({ specStorage[i].c_str(), specStorage[i + 1].c_str() });

	IFileOpenDialog* dlg = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg) {
		DWORD opts = 0;
		dlg->GetOptions(&opts);
		dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
		if (!specs.empty()) dlg->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
		dlg->SetTitle(title);
		if (SUCCEEDED(dlg->Show(owner))) {
			IShellItemArray* items = nullptr;
			if (SUCCEEDED(dlg->GetResults(&items)) && items) {
				DWORD count = 0;
				items->GetCount(&count);
				for (DWORD i = 0; i < count; ++i) {
					IShellItem* item = nullptr;
					if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
						PWSTR path = nullptr;
						if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
							out.push_back(path);
							CoTaskMemFree(path);
						}
						item->Release();
					}
				}
				items->Release();
			}
		}
		dlg->Release();
	}
	if (comOwned) CoUninitialize();
	return out;
}

// Even with IFileOpenDialog removing the same-folder restriction, users may
// still want to build a set across separate picker sessions (e.g. picking
// from two different drives' search results). Loop the picker with a plain
// Yes/No "add more?" prompt (same category as this app's other merge/convert
// workflow confirms) after each non-empty batch. Cancelling the very first
// round returns empty, same as a single OpenFileDialogMulti call always did.
std::vector<std::wstring> PickFilesAcrossFolders(HWND owner, const wchar_t* filter, const wchar_t* title)
{
	std::vector<std::wstring> all;
	for (;;) {
		auto batch = OpenFileDialogMulti(owner, filter, title);
		if (batch.empty()) break;
		all.insert(all.end(), batch.begin(), batch.end());
		wchar_t msg[192];
		swprintf(msg, 192, L"%d file(s) added (%d total).\n\nAdd more files from another folder?",
			static_cast<int>(batch.size()), static_cast<int>(all.size()));
		if (MessageBoxW(owner, msg, title, MB_YESNO | MB_ICONQUESTION) != IDYES) break;
	}
	return all;
}

} // namespace

// ---------------------------------------------------------------------------
// CanvasView: the scrolling page area.
// ---------------------------------------------------------------------------
class ThumbPanel;

class CanvasView {
public:
	enum class Mode { Continuous, SinglePage };
	enum class Fit { None, Width, Page };

	static void Register(HINSTANCE hInst);
	CanvasView(HWND parent, HINSTANCE hInst);
	HWND hwnd() const { return hwnd_; }

	void setDocument(PdfDocument* doc);
	void setThumbPanel(ThumbPanel* tp) { thumbs_ = tp; }
	void setStatusBar(HWND sb) { status_ = sb; }
	// Light/dark theme, driven by FrameWindow::applyTheme(). Only affects the
	// page surround/empty-state chrome -- rendered page content (always white
	// PDF pixels) is untouched, so a plain repaint is enough, no re-render.
	void setDarkMode(bool dark) { dark_ = dark; updateStatus(); invalidate(); }
	// The text updateStatus() last sent to the status bar -- see statusText_.
	const std::wstring& currentStatusText() const { return statusText_; }
	void setOnChanged(std::function<void()> f) { onChanged_ = std::move(f); }
	// Fired whenever the visible page/zoom changes (scroll, wheel-zoom,
	// fit mode, ...) -- lets the frame keep a toolbar zoom-% readout in sync
	// without polling.
	void setOnViewChanged(std::function<void()> f) { onViewChanged_ = std::move(f); }
	float zoom() const { return zoom_; }
	// Fired when a free-text box finishes editing (committed or cancelled),
	// so the frame can drop back to the Select tool/cursor -- Add Text is a
	// one-shot action per activation, matching how other viewers behave.
	void setOnExitTextTool(std::function<void()> f) { onExitTextTool_ = std::move(f); }
	// Fired when a tile in the empty-state tools grid (shown when no document
	// is open) is clicked; the int is the IDM_* command id to run, so the
	// frame just forwards it straight into its existing onCommand().
	void setOnEmptyStateAction(std::function<void(int)> f) { onEmptyStateAction_ = std::move(f); }
	// Fired when the user picks "Edit as Rich Text" from the right-click menu
	// over an active text selection -- the string is the selection's plain
	// text (see selectedText()); the frame owns opening the actual editor
	// window (CanvasView has no business owning a top-level window).
	void setOnOpenTextEditor(std::function<void(const std::wstring&)> f) { onOpenTextEditor_ = std::move(f); }
	// After Save, PdfDocument reopens its underlying fz_document internally
	// (same PdfDocument object, new handle) -- drop caches keyed to the old
	// handle without resetting scroll/zoom/page position like setDocument().
	void refreshAfterSave() { widgetCache_.clear(); charsCache_.clear(); annotCache_.clear(); linksCache_.clear(); invalidateCache(); relayout(); invalidate(); }

	void zoomIn() { setZoom(zoom_ * kZoomStep); fit_ = Fit::None; }
	void zoomOut() { setZoom(zoom_ / kZoomStep); fit_ = Fit::None; }
	void actualSize() { fit_ = Fit::None; setZoom(1.0f); }
	void setFit(Fit f) { fit_ = f; applyFit(); relayout(); invalidate(); updateStatus(); }
	void setMode(Mode m);
	void goToPage(int i);
	int currentPage() const { return currentPage_; }
	Mode mode() const { return mode_; }

	// Annotation tools
	enum class Tool { Select, Highlight, Draw, Erase, Text, Redact };
	void setTool(Tool t) {
		if (inlineEdit_) commitInlineEdit(true);
		tool_ = t;
		SetCursor(LoadCursor(nullptr, t == Tool::Select ? IDC_ARROW : IDC_CROSS));
		if (t != Tool::Select && !hoveredLinkText_.empty()) { hoveredLinkText_.clear(); updateStatus(); }
	}
	Tool tool() const { return tool_; }
	// Commits whatever the user is currently typing (form field or new
	// text-box annotation) so Save never silently drops in-progress edits.
	void flushPendingEdit() { if (inlineEdit_) commitInlineEdit(true); }
	// Each tool remembers its own last-picked color (a highlighter defaults
	// to yellow; pen/text annotations default to black, matching normal ink).
	// While the free-text popup is up (creating OR re-editing a box), color
	// always targets textColor_ regardless of the active tool_: re-editing an
	// existing box deliberately stays on the Select tool (see
	// beginExistingAnnotEdit), so routing through activeColor() there would
	// silently write to drawColor_ instead and never visibly apply.
	void setColor(COLORREF c) {
		if (inlineEdit_ && inlineShowPopup_) {
			textColor_ = c;
			InvalidateRect(inlineEdit_, nullptr, TRUE);
		} else {
			activeColor() = c;
		}
	}
	COLORREF color() { return (inlineEdit_ && inlineShowPopup_) ? textColor_ : activeColor(); }
	void setPenWidth(float w) { penWidth_ = w; }
	void setOpacity(float o) { opacity_ = o; }

	// Search. Returns number of matches found.
	int search(const std::wstring& text);
	void findNext() { stepHit(+1); }
	void findPrev() { stepHit(-1); }
	void clearSearch();
	int hitCount() const { return static_cast<int>(hits_.size()); }
	int currentHitOrdinal() const { return currentHit_; } // 0-based, -1 if none

	// Plain-text selection (drag over rendered text with the Select tool).
	void copySelectionToClipboard();
	// The current selection's text (same extraction copySelectionToClipboard
	// uses), for callers that want the string itself instead of the
	// clipboard -- e.g. handing it to the rich-text-editor tool.
	std::wstring selectedText();
	void selectAll();
	bool hasTextSelection() const { return selAnchorIdx_ >= 0 && selFocusIdx_ >= 0; }

	// Print preview: while active, onPaint() shows one sheet from `pages`
	// (rendered via RenderPrintPreviewPage, reflecting grayscale/landscape)
	// instead of the normal scrollable document -- the print panel's live
	// preview. endPrintPreview() restores normal viewing exactly as it was
	// (no state elsewhere is touched by entering/leaving preview).
	void beginPrintPreview(std::vector<int> pages, bool grayscale, bool landscape);
	void setPrintPreviewOptions(std::vector<int> pages, bool grayscale, bool landscape);
	void printPreviewGoTo(int index);
	void endPrintPreview();
	bool printPreviewActive() const { return printPreviewActive_; }
	int printPreviewPageCount() const { return static_cast<int>(printPreviewPages_.size()); }
	int printPreviewCursor() const { return printPreviewCursor_; }

	// Used by the floating popup (a free-standing window, not a CanvasView
	// member) to adjust the in-progress text-box/annotation session.
	void bumpFontSize(float deltaPt) { adjustInlineFontSize(deltaPt); }
	void deleteCurrentAnnotation() { deleteInlineAnnotAndClose(); }
	bool inlineHasDeleteOption() const { return inlineExistingAnnotIndex_ >= 0; }
	// Used by the resize-handle / move-frame child windows (also
	// free-standing, not CanvasView members) to start a drag when clicked.
	void beginResize(int dir);
	void beginMove(int mx, int my);

	static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);

private:
	float effScale() const { return zoom_ * dpi_ / 72.0f; }
	void pagePixelSize(int i, int& w, int& h) const;
	void relayout();
	void applyFit();
	void setZoom(float z);
	void setZoomAnchored(float z, int anchorClientX, int anchorClientY);
	void clampScroll();
	void updateScrollbars();
	int clampedScrollY(int y) const;
	int clampedScrollX(int x) const;
	void beginSmoothScrollY(int targetY);
	void beginSmoothScrollX(int targetX);
	void stepScrollAnim();
	void invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
	void invalidateCache() { cache_.clear(); cacheKey_ = 0; }
	HBITMAP ensureRendered(int i, int& w, int& h);
	void onPaint();
	void onSize();
	void onVScroll(WPARAM);
	void onHScroll(WPARAM);
	void onWheel(short delta, bool ctrl, bool horiz, POINT screenPt);
	void updateStatus();
	int firstVisiblePage() const;
	void stepHit(int dir);
	void scrollToHit(int hitIdx);
	void drawHighlights(HDC dc, int pageIndex, int pageX, int pageY);

	// Tool interaction
	bool pageScreenOrigin(int page, int& sx, int& sy) const;
	bool hitTestPage(int mx, int my, int& page, float& px, float& py) const;
	void onLButtonDown(int mx, int my);
	void onRButtonDown(int mx, int my);
	void onMouseMove(int mx, int my);
	void onLButtonUp(int mx, int my);
	void eraseAt(int mx, int my);
	void doFormFill(int page, float px, float py);
	void invalidatePage(int page) { cache_.erase(page); widgetCache_.erase(page); charsCache_.erase(page); annotCache_.erase(page); linksCache_.erase(page); }
	void drawFieldHighlights(HDC dc, int pageIndex, int pageX, int pageY);
	// Tools-grid empty state (shown instead of the plain "drag a file here"
	// hint when no document is open). Defined after the icons:: namespace so
	// it can draw the same vector glyphs the toolbar uses. hitTestEmptyState
	// recomputes the same tile layout purely for hit-testing -- the tile list
	// is tiny (~8), so there's no need to cache it between paint and click.
	void drawEmptyState(HDC dc, const RECT& rc);
	bool hitTestEmptyState(int mx, int my, int& cmdId) const;
	const std::vector<WidgetInfo>& widgetsForPage(int page);

	// Inline (no-dialog) text editing for form fields and free-text annots.
	struct InlineEditOptions {
		bool multiline = false;
		bool isWidget = false;         // editing an existing form field
		bool showPopup = false;        // show floating color/size/delete popup
		bool autoExitSelect = false;   // switch tool back to Select on commit
		int existingAnnotIndex = -1;   // >=0 => editing an existing FreeText annot
		int widgetIndex = -1;          // >=0 => editing this form-field widget (for Tab navigation)
		float fontSize = 12.0f;
	};
	void beginInlineEdit(int page, PageRectPt rectPt, const std::string& utf8,
		const InlineEditOptions& opts, std::function<void(const std::string&, bool committed)> onDone);
	void commitInlineEdit(bool commit);
	// Shared by doFormFill's click-to-edit and Tab/Shift+Tab navigation.
	void beginTextWidgetEdit(int page, int widgetIndex, PageRectPt rect, const std::string& value, bool multiline);
	// Commits the field currently being edited and opens the next (or, if
	// !forward, previous) text field on the same page, wrapping around.
	// No-op unless a form-field text widget is currently being edited.
	void tabToAdjacentTextWidget(bool forward);
	static LRESULT CALLBACK InlineEditSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
	void positionInlineEdit();
	void updateInlineEditFont();
	void adjustInlineFontSize(float deltaPt);
	void deleteInlineAnnotAndClose();
	void beginExistingAnnotEdit(int page, const AnnotInfo& ai);
	// Natural (shrink-to-fit) box size for `text` at `fontSizePt`, anchored
	// at (x0,y0) -- used on commit so the saved box always fits its content
	// with a small margin, regardless of any manual resize while editing.
	PageRectPt autoFitTextRect(int page, const std::wstring& text, float x0, float y0, float fontSizePt) const;

	// Resize/move handles for free-text annotation boxes (not shown for form widgets).
	void createResizeHandles();
	void destroyResizeHandles();
	void positionResizeHandles();
	void updateResize(int mx, int my);
	void endResize();
	void makeInlineBgBrush();

	// Plain-text selection. Anchor and focus each carry their own page, so a
	// drag that crosses a page boundary (continuous mode) extends the
	// selection onto the new page instead of freezing -- see
	// selectionRangeForPage() for how a multi-page span is resolved back to
	// a per-page character range.
	void beginTextSelection(int page, float px, float py);
	void updateTextSelection(int page, float px, float py);
	void clearTextSelection();
	void drawTextSelection(HDC dc, int pageIndex, int pageX, int pageY);
	// If any part of the current selection falls on `pageIndex`, fills
	// lo/hi (a char-index range into charsForPage(pageIndex), hi may exceed
	// the page's char count -- callers already clamp with their own loop's
	// `i < chars.size()` guard) and returns true.
	bool selectionRangeForPage(int pageIndex, int& lo, int& hi) const;
	static int nearestCharIndex(const std::vector<PageChar>& chars, float px, float py);
	void selectWordAt(int page, float px, float py);
	void selectLineAt(int page, float px, float py);
	const std::vector<PageChar>& charsForPage(int page);
	const std::vector<AnnotInfo>& annotsForPage(int page);
	const std::vector<LinkInfo>& linksForPage(int page);
	// Detect plain-text URLs/emails in a page's text and turn them into links
	// (Chrome/Edge do this; these PDFs carry no real link annotations).
	std::vector<LinkInfo> detectTextLinks(int page);
	// If (px,py) on `page` is inside a hyperlink, returns it; else nullptr.
	const LinkInfo* linkAt(int page, float px, float py);
	// Opens an external link (browser/mail) or jumps to an internal page.
	void activateLink(const LinkInfo& link);
	HCURSOR cursorForSelectAt(int mx, int my);
	// Browser-style link preview: shows the target in the status bar while
	// hovering (like the bottom-left URL preview in Chrome/Edge), restoring
	// the normal page/zoom text once the cursor leaves the link.
	void updateLinkHover(int mx, int my);

	HWND hwnd_ = nullptr;
	PdfDocument* doc_ = nullptr;
	ThumbPanel* thumbs_ = nullptr;
	HWND status_ = nullptr;
	bool dark_ = false;

	float zoom_ = 1.0f;
	UINT dpi_ = 96;
	Mode mode_ = Mode::Continuous;
	Fit fit_ = Fit::None; // default zoom is 100%, not fit-to-width
	int currentPage_ = 0;
	int scrollX_ = 0, scrollY_ = 0;

	// Smooth-scroll animation (keyboard arrows / Page Up-Down only; wheel
	// and scrollbar-drag stay instant to track the input 1:1).
	static constexpr UINT_PTR kScrollAnimTimerId = 1;
	int scrollAnimFromY_ = 0, scrollAnimToY_ = 0;
	int scrollAnimFromX_ = 0, scrollAnimToX_ = 0;
	DWORD scrollAnimStartY_ = 0, scrollAnimStartX_ = 0;
	bool scrollAnimY_ = false, scrollAnimX_ = false;

	struct PageLayout { int x, y, w, h; };
	std::vector<PageLayout> layout_; // continuous: one per page
	int canvasW_ = 0, canvasH_ = 0;

	long long cacheKey_ = 0; // effScale rounded; invalidates cache on change
	std::unordered_map<int, PageBitmap> cache_;
	std::unordered_map<int, std::vector<WidgetInfo>> widgetCache_;

	// Inline edit state (form text fields, new free-text annotations, and
	// editing of existing free-text annotations).
	HWND inlineEdit_ = nullptr;
	HWND colorPopup_ = nullptr;
	HFONT inlineEditFont_ = nullptr;
	HBRUSH inlineBgBrush_ = nullptr; // page pixels behind the box, for a "transparent" look while typing
	int inlineEditPage_ = -1;
	PageRectPt inlineEditRectPt_;
	std::function<void(const std::string&, bool)> inlineEditDone_;
	bool inlineIsWidget_ = false;
	bool inlineShowPopup_ = false;
	bool inlineAutoExitSelect_ = false;
	int inlineExistingAnnotIndex_ = -1;
	int inlineWidgetIndex_ = -1; // >=0 while inlineIsWidget_ -- which widget, for Tab navigation
	float inlineFontSize_ = 12.0f;

	// Resize-handle state for the free-text box currently being edited.
	// Moving the box is triggered from InlineEditSubclass (a click near the
	// EDIT control's own border), not a separate overlapping window -- see
	// its WM_LBUTTONDOWN handling.
	HWND resizeHandles_[8] = {};
	int activeResizeDir_ = -1;
	PageRectPt resizeBound_;
	float resizeScale_ = 1.0f;
	int resizeOrgX_ = 0, resizeOrgY_ = 0;
	float moveGrabDX_ = 0.0f, moveGrabDY_ = 0.0f; // page-space offset of the grab point from the box origin

	// Search state
	std::string needleUtf8_;
	struct Hit { int page; PageRectPt rc; };
	std::vector<Hit> hits_;
	int currentHit_ = -1;

	// Plain-text selection (Select tool, drag over rendered text). Chars are
	// fetched on demand via charsForPage() (already cached) rather than
	// snapshotted here, since a selection can now span more than one page.
	int selAnchorPage_ = -1;
	int selFocusPage_ = -1;
	int selAnchorIdx_ = -1;
	int selFocusIdx_ = -1;
	bool selDragging_ = false;

	// Print preview (see beginPrintPreview()'s comment).
	bool printPreviewActive_ = false;
	std::vector<int> printPreviewPages_;
	int printPreviewCursor_ = 0;
	bool printPreviewGrayscale_ = false;
	bool printPreviewLandscape_ = false;
	void onPaintPrintPreview(HDC mem, int cw, int ch);
	DWORD lastClickTime_ = 0;
	POINT lastClickPos_ = {};
	int clickCount_ = 0;
	std::unordered_map<int, std::vector<PageChar>> charsCache_;
	std::unordered_map<int, std::vector<AnnotInfo>> annotCache_;
	std::unordered_map<int, std::vector<LinkInfo>> linksCache_;
	std::string hoveredLinkText_; // non-empty while the cursor sits over a link
	// Mirrors whatever updateStatus() last sent to the status bar. Needed
	// because SB_SETTEXTW's SBT_OWNERDRAW flag (dark mode) repurposes the
	// part's text slot as opaque app data -- SB_GETTEXTW can't read the
	// string back afterward, so FrameWindow::drawStatusBarItem reads this
	// member (via currentStatusText()) instead.
	std::wstring statusText_;
	bool trackingMouseLeave_ = false; // TrackMouseEvent armed, so WM_MOUSELEAVE fires

	// Tool state -- each tool keeps its own last-used color.
	Tool tool_ = Tool::Select;
	COLORREF highlightColor_ = RGB(255, 235, 0); // yellow
	COLORREF drawColor_ = RGB(0, 0, 0);          // black
	COLORREF textColor_ = RGB(0, 0, 0);          // black
	COLORREF& activeColor() {
		switch (tool_) {
		case Tool::Highlight: return highlightColor_;
		case Tool::Text: return textColor_;
		default: return drawColor_;
		}
	}
	float penWidth_ = 2.0f;             // points
	float opacity_ = 0.4f;              // highlight opacity
	bool dragging_ = false;
	int dragPage_ = -1;
	int dragStartX_ = 0, dragStartY_ = 0; // screen
	int dragCurX_ = 0, dragCurY_ = 0;     // screen
	int pageOrgX_ = 0, pageOrgY_ = 0;     // screen coords of page top-left at drag start
	float dragScale_ = 1.0f;
	PageRectPt dragBound_;
	std::vector<PagePointF> curStroke_;   // page-space (draw tool)
	std::function<void()> onChanged_;
	std::function<void()> onExitTextTool_;
	std::function<void()> onViewChanged_;
	std::function<void(int)> onEmptyStateAction_;
	std::function<void(const std::wstring&)> onOpenTextEditor_;
};

// ---------------------------------------------------------------------------
// ThumbPanel: left sidebar with clickable page thumbnails.
// ---------------------------------------------------------------------------
class ThumbPanel {
public:
	static void Register(HINSTANCE hInst);
	ThumbPanel(HWND parent, HINSTANCE hInst);
	HWND hwnd() const { return hwnd_; }
	void setDocument(PdfDocument* doc);
	void setCanvas(CanvasView* c) { canvas_ = c; }
	void setCurrent(int page);
	void refreshAfterSave() { cache_.clear(); relayout(); InvalidateRect(hwnd_, nullptr, FALSE); }
	void setDarkMode(bool dark) { dark_ = dark; InvalidateRect(hwnd_, nullptr, FALSE); }
	static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);

	// --- Organize mode: an interactive reorder/rotate/delete/insert session
	// over an ordered page plan (see PdfDocument::PagePlanEntry), used by
	// Tools > Organize Pages and Merge. Nothing is written to `doc_` while
	// this is active -- only on FrameWindow's "Done" (rebuildFromPages) or
	// discarded outright on "Cancel". Reuses this panel's own thumbnail
	// rendering/scroll machinery rather than a whole new grid widget.
	void enterOrganizeMode(std::vector<PdfDocument::PagePlanEntry> seed);
	void exitOrganizeMode();
	bool organizeMode() const { return organizeMode_; }
	void organizeInsertFile(int fileIndex);
	const std::vector<PdfDocument::PagePlanEntry>& organizeOrder() const { return order_; }

private:
	void relayout();
	void clampScroll();
	void updateScrollbar();
	HBITMAP ensureThumb(int i, int& w, int& h);
	void onPaint();
	void onSize();
	void onVScroll(WPARAM);
	void onWheel(short delta);
	void onClick(int y);

	// Organize-mode internals.
	PageSizePt orderedPageSize(int i) const;
	struct OrganizeIconRects { RECT ccw, cw, del; };
	OrganizeIconRects organizeIconRects(int i) const;
	int organizeSlotAt(int y) const;
	void onOrganizeLButtonDown(int x, int y);
	void onOrganizeMouseMove(int x, int y);
	void onOrganizeLButtonUp();

	HWND hwnd_ = nullptr;
	PdfDocument* doc_ = nullptr;
	CanvasView* canvas_ = nullptr;
	UINT dpi_ = 96;
	int scrollY_ = 0;
	int current_ = 0;

	int thumbW_ = kBaseThumbWidth;
	struct ThumbSlot { int y, h; }; // vertical placement of each thumbnail
	std::vector<ThumbSlot> slots_;
	int contentH_ = 0;
	std::unordered_map<int, PageBitmap> cache_;

	bool organizeMode_ = false;
	std::vector<PdfDocument::PagePlanEntry> order_;
	bool organizeDragArmed_ = false;
	int organizeDragIndex_ = -1;
	bool dark_ = false;
};

class FrameWindow;
// Creates a brand-new top-level window (its own FrameWindow, own tab strip)
// and opens each of `paths` as its own tab (first one lands in the window's
// initial empty tab, same as FrameWindow::openDocument()'s usual reuse
// logic). Used by "drag a tab out" and the tab context menu's "Open in New
// Window" -- for a multi-tab-selection detach, every selected tab's file
// lands together in the SAME new window. Defined near RunViewer() since it
// mirrors that function's own window-creation dance. The returned pointer is
// never owned by the caller: every FrameWindow self-deletes on its own
// WM_NCDESTROY (see g_liveFrameCount's comment).
FrameWindow* SpawnNewWindow(HINSTANCE hInst, const std::vector<std::wstring>& paths);

// One open document = one PdfDocument + its own CanvasView/ThumbPanel child
// windows (so each tab keeps its own scroll/zoom/tool/search/selection state
// completely independently -- only the toolbar/status bar/search bar are
// shared chrome). Only the active tab's canvas/thumbs are shown; the rest
// sit hidden until switched to.
struct DocTab {
	std::unique_ptr<PdfDocument> doc;
	std::unique_ptr<CanvasView> canvas;
	std::unique_ptr<ThumbPanel> thumbs;
	std::wstring path; // full path of the open file ("" if none)
	std::wstring name; // file name, for the title bar and tab label
	bool protDismissed = false; // user closed the protection info bar for this tab
};

// ---------------------------------------------------------------------------
// Frame window: owns everything.
// ---------------------------------------------------------------------------
class FrameWindow {
public:
	FrameWindow(HINSTANCE hInst) : hInst_(hInst) {}
	HWND create(int nCmdShow, bool checkForUpdates = true);
	HWND hwnd() const { return hwnd_; }
	void openDocument(const wchar_t* path);
	static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);

private:
	void createChildren();
	void rebuildToolbarIcons(); // re-rasterizes every toolbar icon at the current ink color
	void layout();
	void onCommand(int id);
	void syncMenuChecks();
	void toggleThumbs();
	void showSearchBar(bool show);
	void runSearch();
	void liveSearch();
	void updateSearchLabel();
	static LRESULT CALLBACK EditSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
	static LRESULT CALLBACK PageEditSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
	static LRESULT CALLBACK SplitEditSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
	static LRESULT CALLBACK SetPwdEditSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
	static LRESULT CALLBACK WebPdfEditSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
	void updatePageEditBox();
	void beginPageNumberEdit();          // creates the transient page-number EDIT control and focuses it
	void endPageNumberEdit(bool commit); // destroys it, optionally jumping to the typed page first
	void selectTool(int id);
	void chooseColor();
	int popupUnderButton(int buttonId, HMENU menu);
	void chooseWidth();
	void chooseOpacity();
	void saveDocument(bool saveAs);
	void updateTitle();
	void updateStatusPath();          // right-hand status bar part: current tab's full file path
	void layoutStatusParts();         // recomputes the two status bar part boundaries for the current width
	bool promptSaveIfDirty(); // returns false if user cancelled
	void updateZoomLabel();

	// Theme (light/dark). isDark_ starts from InitialDarkMode() (saved user
	// override, else the system theme) and only changes via the toolbar
	// toggle button -- the app never re-polls the system after launch.
	// applyTheme() re-skins everything already on screen: DWM title bar,
	// toolbar icon ImageList (colors are pre-rasterized, so this rebuilds
	// it), and every open tab's canvas/thumbnail panel.
	void applyTheme();
	void toggleTheme();

	// Finishes wiring up a just-opened (and, if needed, authenticated)
	// document into the active tab -- shared by the direct-open and
	// password-unlock paths in openDocument().
	void finishOpenDocument(const wchar_t* path);

	// Password prompt bar (see pwdVisible_'s comment).
	void showPasswordBar(bool show);
	void submitPassword();
	void cancelPasswordPrompt();
	static LRESULT CALLBACK PwdEditSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

	// Protection info bar (see protVisible_'s comment).
	void updateProtectionBar();
	void removeProtection();

	// Set-password bar (see setPwdVisible_'s comment).
	void showSetPasswordBar(bool show);
	void runSetPassword();

	// Web-to-PDF bar (see webPdfVisible_'s comment).
	void showWebPdfBar(bool show);
	void runWebToPdf();

	// Auto-update (see updater.h). startUpdateCheck spawns a background thread
	// that posts WM_APP_UPDATE_RESULT back here; onUpdateResult handles the
	// prompt/download/relaunch on the UI thread. `manual` distinguishes the
	// Tools-menu "Check for Updates" (which reports "up to date"/errors) from
	// the silent on-launch check.
	void startUpdateCheck(bool manual);
	void onUpdateResult(const updater::UpdateInfo& info, bool manual);
	bool promptSaveAllIfDirty(); // save-guard across every tab; false = user cancelled

	// Shared operation-result bar (see opResultVisible_'s comment).
	void showOpResultBar(bool show);

	// Tools popup menu + one-shot document operations.
	void showToolsMenu();
	void doResizeToA4();
	void doFlatten();
	void doFlattenEdits();
	void doCompress();
	void doMerge();
	void doConvertToPdf();

	// Split bar (see splitVisible_'s comment).
	void showSplitBar(bool show);
	void runSplit();

	// Redact bar (see redactBarVisible_'s comment).
	void updateRedactBar();
	void applyRedactionsCmd();
	void clearRedactions();

	// Organize side panel (extends ThumbPanel::organizeMode_).
	void enterOrganizeMode(std::vector<PdfDocument::PagePlanEntry> seed);
	void organizeInsertPages();
	void organizeDone();
	void organizeCancel();

	// Tabs: one PdfDocument+CanvasView+ThumbPanel per open file.
	int newTab();                            // creates + appends an empty tab, returns its index
	void switchToTab(int idx);                // makes idx active (repoints doc_/canvas_/thumbs_, shows/hides views)
	void closeTab(int idx);                   // prompts save-if-dirty, then removes the tab
	void detachTabToNewWindow(int idx);       // closes the tab here, reopens its file in a brand-new window
	void detachTabsToNewWindow(std::vector<int> indices); // same, for a whole multi-selected group at once
	void cycleTab(int dir);                   // +1 = Ctrl+Tab, -1 = Ctrl+Shift+Tab
	void updateTabLabel(int idx);
	void reorderTab(int from, int to);        // drag-to-reorder: moves the DocTab and mirrors it in tabStrip_
	void reorderTabGroup(std::vector<int> indices, int to); // same, moving a multi-selected block together
	void updateTabWidths(int barW);           // shrinks tab item width so all tabs fit barW (down to a floor)
	// Multi-tab selection (see multiSelectedTabs_'s comment).
	bool isTabMultiSelected(int idx) const;
	void toggleTabMultiSelect(int idx);
	void selectTabRange(int fromIdx, int toIdx);
	void clearTabMultiSelect();
	static LRESULT CALLBACK TabStripSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
	RECT tabCloseRect(int idx) const; // hit-test rect for a tab's close "x", also used to draw it
	void drawTabItem(const DRAWITEMSTRUCT* dis);
	void drawStatusBarItem(const DRAWITEMSTRUCT* dis);

	HINSTANCE hInst_;
	HWND hwnd_ = nullptr;
	HWND toolbar_ = nullptr;
	HIMAGELIST toolbarImages_ = nullptr;
	HWND status_ = nullptr;
	HWND tabStrip_ = nullptr;
	// The tab strip's own auto-managed tooltip control (TCS_TOOLTIPS) --
	// captured once so WM_NOTIFY can tell its TTN_GETDISPINFOW requests
	// apart from the toolbar's (see the handler for why that distinction
	// matters: idFrom means something different for each).
	HWND tabTooltip_ = nullptr;
	std::vector<std::unique_ptr<DocTab>> tabs_;
	int activeTab_ = -1;
	// Drag-to-reorder state for the tab strip (see TabStripSubclass).
	int tabDragIndex_ = -1;
	bool tabDragArmed_ = false;
	// Multi-tab selection (Ctrl+click toggles, Shift+click ranges) -- lets
	// several tabs be dragged/reordered or torn off into a new window
	// together. Always either empty (no multi-selection active -- normal
	// single-tab behavior) or holds >=2 sorted indices; `activeTab_` is
	// still the one tab whose document is actually shown, independent of
	// this. `tabSelectAnchor_` is the last plain-clicked tab, for Shift+click
	// range selection.
	std::vector<int> multiSelectedTabs_;
	int tabSelectAnchor_ = -1;
	// Non-owning: always point at tabs_[activeTab_]'s members (or null when
	// there are no tabs, which in practice is only ever transiently true).
	CanvasView* canvas_ = nullptr;
	ThumbPanel* thumbs_ = nullptr;
	PdfDocument* doc_ = nullptr;
	bool showThumbs_ = false; // page-preview pane hidden by default
	bool isDark_ = false;     // set from InitialDarkMode() in create(); see applyTheme()
	// Cached background brush for WM_CTLCOLORSTATIC/EDIT in dark mode -- these
	// fire on every keystroke/caret blink, so the brush is created once and
	// reused (recreated only on an actual theme change), not per-message.
	HBRUSH ctrlBgBrush_ = nullptr;

	// Organize side panel's bottom action strip (Insert Pages/Done/Cancel).
	// The interactive reorder/rotate/delete state itself lives in the
	// active tab's ThumbPanel (see its organizeMode_ comment); these are
	// just the FrameWindow-owned buttons pinned under that column.
	HWND organizeInsert_ = nullptr;
	HWND organizeDone_ = nullptr;
	HWND organizeCancel_ = nullptr;
	bool organizeShownThumbsBefore_ = false; // showThumbs_ value to restore on exit
	int organizeBarH_ = 0;

	// Search bar
	HWND searchEdit_ = nullptr;
	HWND searchPrev_ = nullptr;
	HWND searchNext_ = nullptr;
	HWND searchClose_ = nullptr;
	HWND searchLabel_ = nullptr;
	bool searchVisible_ = false;
	int searchBarH_ = 0;
	std::wstring lastSearch_;

	// Password prompt bar -- shown in place of the canvas top edge when a
	// just-opened PDF needs a password (see openDocument()/PdfDocument::open).
	HWND pwdLabel_ = nullptr;
	HWND pwdEdit_ = nullptr;
	HWND pwdUnlock_ = nullptr;
	HWND pwdCancel_ = nullptr;
	bool pwdVisible_ = false;
	int pwdBarH_ = 0;
	std::wstring pendingPwdPath_; // path being unlocked; committed once authenticate() succeeds
	// Set when the empty-state "Set Password" tile triggers the Open dialog
	// (see IDM_EMPTY_SET_PASSWORD) -- finishOpenDocument() checks this to
	// chain straight into the set-password bar for a freshly opened,
	// not-yet-encrypted PDF, so picking a file is the only extra click.
	bool pendingSetPasswordAfterOpen_ = false;
	// Same chaining pattern for the empty-state "Flatten to Image"/"Flatten
	// Edits Only" tiles (see IDM_EMPTY_FLATTEN_IMAGE/IDM_EMPTY_FLATTEN_EDITS):
	// 0 = none pending, 1 = flatten to image, 2 = flatten edits only.
	int pendingFlattenAfterOpen_ = 0;

	// Protection info bar -- offers to strip password/restrictions from an
	// encrypted PDF that's currently open. Both buttons call the same
	// PdfDocument::removeProtection() (see its comment for why); shown
	// whenever the active tab's document is encrypted and not dismissed.
	HWND protLabel_ = nullptr;
	HWND protRemovePwd_ = nullptr;
	HWND protRemoveRestrictions_ = nullptr;
	HWND protClose_ = nullptr;
	bool protVisible_ = false;
	int protBarH_ = 0;

	// Shared operation-result bar -- shown after Organize/Merge apply,
	// Resize to A4, Flatten, Compress, and Apply Redactions to ask whether
	// to overwrite the original file or save a copy. Wires directly to the
	// existing saveDocument(false)/saveDocument(true).
	HWND opResultLabel_ = nullptr;
	HWND opResultSave_ = nullptr;
	HWND opResultSaveAs_ = nullptr;
	HWND opResultClose_ = nullptr;
	bool opResultVisible_ = false;
	int opResultBarH_ = 0;

	// Split bar (Tools > Split PDF...).
	HWND splitLabel_ = nullptr;
	HWND splitEdit_ = nullptr;
	HWND splitButton_ = nullptr;
	HWND splitClose_ = nullptr;
	HWND splitResult_ = nullptr;
	bool splitVisible_ = false;
	int splitBarH_ = 0;

	// Set-password bar (Tools > Set Password...). One password field is used
	// as both the open (user) password and the owner password -- see
	// PdfDocument::setPassword's comment for why (mirrors removeProtection()
	// clearing both together). Directly encrypts and writes to disk on
	// submit, same as removeProtection() -- no separate save step.
	HWND setPwdLabel_ = nullptr;
	HWND setPwdEdit_ = nullptr;
	HWND setPwdButton_ = nullptr;
	HWND setPwdClose_ = nullptr;
	bool setPwdVisible_ = false;
	int setPwdBarH_ = 0;

	// Web-to-PDF bar (Tools > Web Page to PDF...). Runs ConvertWebPageToPdf()
	// synchronously on submit (see its header comment on why there's no
	// progress UI) and opens the result in a fresh tab on success.
	HWND webPdfLabel_ = nullptr;
	HWND webPdfEdit_ = nullptr;
	HWND webPdfButton_ = nullptr;
	HWND webPdfClose_ = nullptr;
	bool webPdfVisible_ = false;
	int webPdfBarH_ = 0;

	// Redact bar -- shown whenever the active tool is Redact.
	HWND redactLabel_ = nullptr;
	HWND redactApply_ = nullptr;
	HWND redactClear_ = nullptr;
	HWND redactDone_ = nullptr;
	bool redactBarVisible_ = false;
	int redactBarH_ = 0;

	// Print side panel -- replaces the native print dialog. Right-docked
	// (canvas shrinks to make room, see layout()); settings changes drive
	// CanvasView's print-preview mode live (see updatePrintPreviewFromPanel).
	HWND printTitle_ = nullptr;
	HWND printPrinterLabel_ = nullptr, printPrinterCombo_ = nullptr;
	HWND printCopiesLabel_ = nullptr, printCopiesEdit_ = nullptr;
	HWND printRangeLabel_ = nullptr, printRangeAll_ = nullptr, printRangeCurrent_ = nullptr,
		printRangeCustom_ = nullptr, printRangeEdit_ = nullptr;
	HWND printOrientLabel_ = nullptr, printOrientPortrait_ = nullptr, printOrientLandscape_ = nullptr;
	HWND printColorLabel_ = nullptr, printColorColor_ = nullptr, printColorGray_ = nullptr;
	HWND printPageNavLabel_ = nullptr, printPageNavPrev_ = nullptr, printPageNavNext_ = nullptr;
	HWND printGo_ = nullptr, printCancel_ = nullptr;
	bool printPanelVisible_ = false;
	void showPrintPanel(bool show);
	void updatePrintPreviewFromPanel();
	void doExecutePrint();
	PrintSettings readPrintSettingsFromPanel() const;

	// Rich-text-editor side panel -- right-docked like the print panel (and
	// mutually exclusive with it: opening either closes the other, since
	// both claim the same right-hand column). "Edit as Rich Text..." on a
	// text selection's right-click menu opens this pre-filled with the
	// selection; Bold/Italic/Underline and the case-converter apply to
	// whatever's selected inside the rich edit (or the whole box if
	// nothing's selected there). Deliberately never writes back into the
	// PDF -- it's a formatting scratchpad, not another edit path.
	HWND textPanelTitle_ = nullptr;
	HWND textBoldBtn_ = nullptr, textItalicBtn_ = nullptr, textUnderlineBtn_ = nullptr;
	HWND textCaseLabel_ = nullptr, textCaseCombo_ = nullptr;
	HWND textRichEdit_ = nullptr;
	HWND textCopyBtn_ = nullptr, textCloseBtn_ = nullptr;
	bool textPanelVisible_ = false;
	void showTextEditorPanel(bool show, const std::wstring& initialText = std::wstring());
	void textPanelToggleFormat(DWORD mask, DWORD effect);
	// Syncs the B/I/U buttons' checked (pressed) look to whatever formatting
	// is actually active at the current caret/selection -- called after every
	// toggle and on caret movement (EN_SELCHANGE), since clicking a button is
	// not the only way the effective format changes.
	void updateTextFormatButtons();
	void textPanelApplyCase(int mode); // 0=UPPER, 1=lower, 2=Title Case, 3=Sentence case
	void textPanelCopy();

	// Toolbar tooltip text, keyed by button command id. Deliberately NOT
	// using TBBUTTON::iString for icon-only buttons: a toolbar containing
	// any BTNS_SHOWTEXT button (our zoom-% label) appears to force ALL
	// buttons with an assigned iString to also render it as a caption under
	// the icon, regardless of BTNS_SHOWTEXT on that specific button -- an
	// undocumented comctl32 interaction, confirmed by testing (removing
	// TBSTYLE_EX_MIXEDBUTTONS did not fix it). Serving tooltip text on
	// demand via TTN_GETDISPINFOW sidesteps it entirely.
	std::unordered_map<int, std::wstring> toolbarTips_;

	// Zoom-%/page-number readouts: plain text the toolbar's own NM_CUSTOMDRAW
	// draws directly onto the IDM_VIEW_ZOOMLABEL/IDM_VIEW_PAGELABEL
	// placeholder buttons' reserved rects (see the CDDS_ITEMPREPAINT case) --
	// NOT a STATIC control overlaid on top of them like an earlier version of
	// this. That overlay-window approach had a real, confirmed-in-practice
	// bug: the toolbar's own hover/hot-tracking repaint of the button
	// underneath would win a race against the overlay window's own repaint,
	// leaving it visibly blank for as long as it stayed hovered (multiple
	// attempts at clipping/re-asserting the overlay never fully closed the
	// race). Plain owner-drawn text has no second competing window, so
	// there's no race at all -- the same technique the status bar already
	// uses successfully for this exact same content.
	std::wstring zoomText_ = L"100%";
	std::wstring pageLabelText_;
	// Page-number jump box: normally just the plain text above, like the
	// zoom readout. Clicking it creates this real EDIT control on demand,
	// positioned over the reserved rect and focused for typing (same
	// create-when-needed philosophy as this app's inline annotation/form
	// editing elsewhere) -- destroyed again on Enter/Escape/focus-loss, so
	// it only exists, and only competes for those pixels, for the few
	// seconds the user is actually typing a page number.
	HWND pageEditActive_ = nullptr;
	HFONT uiFont_ = nullptr;
};

// ===========================================================================
// CanvasView implementation
// ===========================================================================
namespace {

// Tiny floating toolbar shown while typing a text-box annotation (new or
// existing), so color/size/delete never need a modal dialog. Zones left to
// right: [A-][A+][4 color swatches][Delete, only when editing an existing
// annotation]. Fixed-width zones, hand hit-tested (no child controls).
constexpr int kPopupZoneW = 30;

LRESULT CALLBACK ColorPopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_NCCREATE) {
		auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
		return TRUE;
	}
	auto* cv = reinterpret_cast<CanvasView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	constexpr int nSwatch = 4;
	bool showDelete = cv && cv->inlineHasDeleteOption();
	switch (msg) {
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
		RECT rc; GetClientRect(hwnd, &rc);
		HBRUSH bg = CreateSolidBrush(RGB(250, 250, 250));
		FillRect(dc, &rc, bg); DeleteObject(bg);
		FrameRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
		int ch = rc.bottom - rc.top;
		SetBkMode(dc, TRANSPARENT);

		UINT popupDpi = GetDpiForWindow(hwnd);
		HFONT fSmall = CreateFontW(-MulDiv(11, popupDpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
			DEFAULT_PITCH, L"Segoe UI");
		HFONT fBig = CreateFontW(-MulDiv(16, popupDpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
			DEFAULT_PITCH, L"Segoe UI");
		HGDIOBJ oldFont = SelectObject(dc, fSmall);
		RECT z1 = { 0, 0, kPopupZoneW, ch };
		DrawTextW(dc, L"A-", -1, &z1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(dc, fBig);
		RECT z2 = { kPopupZoneW, 0, kPopupZoneW * 2, ch };
		DrawTextW(dc, L"A+", -1, &z2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(dc, oldFont);
		DeleteObject(fSmall);
		DeleteObject(fBig);

		int swatchStart = kPopupZoneW * 2;
		int cy = ch / 2;
		int rad = kPopupZoneW / 3;
		for (int i = 0; i < nSwatch; ++i) {
			int cx = swatchStart + i * kPopupZoneW + kPopupZoneW / 2;
			HBRUSH b = CreateSolidBrush(kTextSwatches[i]);
			HGDIOBJ old = SelectObject(dc, b);
			Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
			SelectObject(dc, old);
			DeleteObject(b);
		}

		if (showDelete) {
			int delStart = swatchStart + nSwatch * kPopupZoneW;
			RECT zd = { delStart, 0, delStart + kPopupZoneW, ch };
			HPEN pen = CreatePen(PS_SOLID, 1, RGB(180, 60, 60));
			HGDIOBJ oldPen = SelectObject(dc, pen);
			SetTextColor(dc, RGB(180, 60, 60));
			HFONT fDel = CreateFontW(-MulDiv(14, popupDpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
				DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
				DEFAULT_PITCH, L"Segoe UI");
			HGDIOBJ oldF2 = SelectObject(dc, fDel);
			DrawTextW(dc, L"✕", -1, &zd, DT_CENTER | DT_VCENTER | DT_SINGLELINE); // X mark
			SelectObject(dc, oldF2);
			DeleteObject(fDel);
			SelectObject(dc, oldPen);
			DeleteObject(pen);
		}
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_LBUTTONDOWN: {
		if (!cv) return 0;
		// This popup is WS_EX_NOACTIVATE (clicking it must not steal focus
		// from the live edit box), but that can also leave the owning frame
		// no longer the "active" top-level window -- and SetFocus() is a
		// silent no-op for any window whose top-level ancestor isn't active.
		// Reassert activation so every focus-dependent thing downstream
		// (committing the edit on a later outside click, keyboard input)
		// keeps working after this click.
		SetActiveWindow(GetAncestor(cv->hwnd(), GA_ROOT));
		int x = GET_X_LPARAM(lp);
		int swatchStart = kPopupZoneW * 2;
		int delStart = swatchStart + nSwatch * kPopupZoneW;
		if (x < kPopupZoneW) {
			cv->bumpFontSize(-1.0f);
		} else if (x < swatchStart) {
			cv->bumpFontSize(1.0f);
		} else if (x < delStart) {
			int idx = std::clamp((x - swatchStart) / kPopupZoneW, 0, nSwatch - 1);
			cv->setColor(kTextSwatches[idx]);
		} else if (showDelete) {
			cv->deleteCurrentAnnotation();
			return 0; // popup and edit control are now destroyed
		}
		InvalidateRect(hwnd, nullptr, FALSE);
		InvalidateRect(cv->hwnd(), nullptr, FALSE);
		return 0;
	}
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// Tiny square drag handle shown at the corners/edge-midpoints of a free-text
// box while it's being edited (Resize a la Edge/Acrobat's annotation boxes).
// Slot 0 of the extra window bytes holds the owning CanvasView*, slot 1 holds
// this handle's ResizeDir index -- mouse-down forwards into CanvasView, which
// does the actual drag via its own SetCapture(canvas hwnd_) so onMouseMove /
// onLButtonUp (already routed through CanvasView::Proc) see every subsequent
// message regardless of which window the cursor is physically over.
LRESULT CALLBACK ResizeHandleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
		RECT rc; GetClientRect(hwnd, &rc);
		HBRUSH b = CreateSolidBrush(RGB(0, 120, 215));
		FillRect(dc, &rc, b);
		DeleteObject(b);
		FrameRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_LBUTTONDOWN: {
		auto* cv = reinterpret_cast<CanvasView*>(GetWindowLongPtrW(hwnd, 0));
		int dir = static_cast<int>(GetWindowLongPtrW(hwnd, sizeof(LONG_PTR)));
		if (cv) cv->beginResize(dir);
		return 0;
	}
	case WM_SETCURSOR: {
		int dir = static_cast<int>(GetWindowLongPtrW(hwnd, sizeof(LONG_PTR)));
		static const LPCWSTR kCursors[8] = {
			IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE,
			IDC_SIZEWE, IDC_SIZENESW, IDC_SIZENS, IDC_SIZENWSE
		};
		SetCursor(LoadCursor(nullptr, kCursors[dir]));
		return TRUE;
	}
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void CanvasView::Register(HINSTANCE hInst)
{
	WNDCLASSW wc = {};
	wc.lpfnWndProc = CanvasView::Proc;
	wc.hInstance = hInst;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = nullptr; // painted ourselves
	wc.lpszClassName = kCanvasClass;
	wc.style = CS_HREDRAW | CS_VREDRAW;
	RegisterClassW(&wc);

	WNDCLASSW popup = {};
	popup.lpfnWndProc = ColorPopupProc;
	popup.hInstance = hInst;
	popup.hCursor = LoadCursor(nullptr, IDC_ARROW);
	popup.hbrBackground = nullptr;
	popup.lpszClassName = kColorPopupClass;
	RegisterClassW(&popup);

	WNDCLASSW handle = {};
	handle.lpfnWndProc = ResizeHandleProc;
	handle.hInstance = hInst;
	handle.hbrBackground = nullptr;
	handle.lpszClassName = kResizeHandleClass;
	handle.cbWndExtra = sizeof(LONG_PTR) * 2;
	RegisterClassW(&handle);

}

CanvasView::CanvasView(HWND parent, HINSTANCE hInst)
{
	hwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, kCanvasClass, L"",
		WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL,
		0, 0, 100, 100, parent, nullptr, hInst, this);
	dpi_ = GetDpiForWindow(hwnd_);
}

void CanvasView::setDocument(PdfDocument* doc)
{
	if (inlineEdit_) commitInlineEdit(false); // old doc's widget indices won't apply
	doc_ = doc;
	currentPage_ = 0;
	scrollX_ = scrollY_ = 0;
	hits_.clear();
	currentHit_ = -1;
	needleUtf8_.clear();
	widgetCache_.clear();
	charsCache_.clear();
	annotCache_.clear();
	linksCache_.clear();
	hoveredLinkText_.clear();
	invalidateCache();
	applyFit();
	relayout();
	invalidate();
	updateStatus();
}

void CanvasView::setMode(Mode m)
{
	if (mode_ == m) return;
	mode_ = m;
	scrollX_ = scrollY_ = 0;
	relayout();
	invalidate();
	updateStatus();
}

void CanvasView::goToPage(int i)
{
	if (!doc_ || !doc_->isOpen()) return;
	i = std::clamp(i, 0, doc_->pageCount() - 1);
	currentPage_ = i;
	scrollAnimY_ = false; // a direct jump overrides any in-flight smooth-scroll
	if (mode_ == Mode::SinglePage) {
		scrollY_ = 0;
		relayout();
	} else if (i < static_cast<int>(layout_.size())) {
		scrollY_ = layout_[i].y - Scale(kBasePageGap, dpi_);
		clampScroll();
		updateScrollbars();
	}
	invalidate();
	updateStatus();
	if (thumbs_) thumbs_->setCurrent(currentPage_);
}

int CanvasView::search(const std::wstring& text)
{
	hits_.clear();
	currentHit_ = -1;
	needleUtf8_ = WideToUtf8(text);
	if (!doc_ || !doc_->isOpen() || needleUtf8_.empty()) { invalidate(); return 0; }

	int n = doc_->pageCount();
	for (int p = 0; p < n; ++p) {
		auto rects = doc_->searchPage(p, needleUtf8_.c_str());
		for (const auto& r : rects) hits_.push_back({ p, r });
	}
	if (!hits_.empty()) {
		// Jump to the first hit on or after the current page.
		int start = (mode_ == Mode::Continuous) ? firstVisiblePage() : currentPage_;
		int idx = 0;
		for (size_t i = 0; i < hits_.size(); ++i) {
			if (hits_[i].page >= start) { idx = static_cast<int>(i); break; }
		}
		currentHit_ = idx;
		scrollToHit(currentHit_);
	}
	invalidate();
	return static_cast<int>(hits_.size());
}

void CanvasView::clearSearch()
{
	hits_.clear();
	currentHit_ = -1;
	needleUtf8_.clear();
	invalidate();
}

void CanvasView::stepHit(int dir)
{
	if (hits_.empty()) return;
	if (currentHit_ < 0) currentHit_ = 0;
	else currentHit_ = (currentHit_ + dir + static_cast<int>(hits_.size())) % static_cast<int>(hits_.size());
	scrollToHit(currentHit_);
	invalidate();
}

void CanvasView::scrollToHit(int hitIdx)
{
	if (hitIdx < 0 || hitIdx >= static_cast<int>(hits_.size())) return;
	// A direct jump overrides any in-flight smooth-scroll (see goToPage()) --
	// without this, a scroll animation left running from a prior arrow-key/
	// wheel scroll can tick again after this jump (or a zoom right after it)
	// and overwrite scrollY_ with stale pre-jump coordinates, snapping the
	// view to an unrelated page.
	if (scrollAnimY_ || scrollAnimX_) {
		scrollAnimY_ = scrollAnimX_ = false;
		KillTimer(hwnd_, kScrollAnimTimerId);
	}
	const Hit& h = hits_[hitIdx];
	if (mode_ == Mode::SinglePage) {
		if (h.page != currentPage_) { currentPage_ = h.page; relayout(); }
	}
	// Position the hit roughly one third down the viewport.
	PageRectPt b = doc_->pageBound(h.page);
	float sc = effScale();
	int hitTopPx = static_cast<int>((h.rc.y0 - b.y0) * sc);
	RECT rc; GetClientRect(hwnd_, &rc);
	int ch = rc.bottom - rc.top;
	int pageTop = (mode_ == Mode::Continuous && h.page < static_cast<int>(layout_.size()))
		? layout_[h.page].y : 0;
	scrollY_ = pageTop + hitTopPx - ch / 3;
	clampScroll();
	updateScrollbars();
	updateStatus();
	if (thumbs_) thumbs_->setCurrent(h.page);
	currentPage_ = h.page;
}

void CanvasView::drawHighlights(HDC dc, int pageIndex, int pageX, int pageY)
{
	if (hits_.empty()) return;
	PageRectPt b = doc_->pageBound(pageIndex);
	float sc = effScale();
	for (size_t i = 0; i < hits_.size(); ++i) {
		if (hits_[i].page != pageIndex) continue;
		const PageRectPt& r = hits_[i].rc;
		RECT dr;
		dr.left = pageX + static_cast<int>((r.x0 - b.x0) * sc);
		dr.top = pageY + static_cast<int>((r.y0 - b.y0) * sc);
		dr.right = pageX + static_cast<int>((r.x1 - b.x0) * sc);
		dr.bottom = pageY + static_cast<int>((r.y1 - b.y0) * sc);
		bool cur = (static_cast<int>(i) == currentHit_);
		FillAlpha(dc, dr, cur ? RGB(255, 150, 0) : RGB(255, 230, 0), cur ? 140 : 90);
	}
}

bool CanvasView::pageScreenOrigin(int page, int& sx, int& sy) const
{
	if (!doc_ || !doc_->isOpen()) return false;
	RECT rc; GetClientRect(hwnd_, &rc);
	int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
	int offX = (canvasW_ <= cw) ? (cw - canvasW_) / 2 : -scrollX_;
	int offY = (canvasH_ <= ch) ? (ch - canvasH_) / 2 : -scrollY_;
	if (mode_ == Mode::Continuous) {
		if (page < 0 || page >= static_cast<int>(layout_.size())) return false;
		sx = layout_[page].x + offX; sy = layout_[page].y + offY;
	} else {
		if (page != currentPage_) return false;
		sx = offX; sy = offY;
	}
	return true;
}

bool CanvasView::hitTestPage(int mx, int my, int& page, float& px, float& py) const
{
	if (!doc_ || !doc_->isOpen()) return false;
	float sc = effScale();
	auto test = [&](int i) -> bool {
		int sx, sy; if (!pageScreenOrigin(i, sx, sy)) return false;
		int w, h; pagePixelSize(i, w, h);
		if (mx < sx || mx >= sx + w || my < sy || my >= sy + h) return false;
		PageRectPt b = doc_->pageBound(i);
		page = i;
		px = b.x0 + (mx - sx) / sc;
		py = b.y0 + (my - sy) / sc;
		return true;
	};
	if (mode_ == Mode::Continuous) {
		for (int i = 0; i < static_cast<int>(layout_.size()); ++i)
			if (test(i)) return true;
		return false;
	}
	return test(currentPage_);
}

void CanvasView::onLButtonDown(int mx, int my)
{
	if (printPreviewActive_) return; // preview is read-only -- no selecting/drawing/form-filling
	// A real mouse click can only reach the canvas's own WM_LBUTTONDOWN when
	// it lands outside the inline edit control (clicks inside go straight to
	// that child window). So if a free-text box is being edited, this click
	// is necessarily "outside" it: finish that box (keeping what was typed).
	// New boxes (Text tool) drop back to Select; editing an existing one
	// (Select tool) just closes the editor and stays in Select.
	// Commit directly rather than relying only on SetFocus's WM_KILLFOCUS
	// side effect: SetFocus is a no-op if the top-level frame isn't the
	// active window, which can happen after interacting with the
	// WS_EX_NOACTIVATE color popup -- that must never leave the box stuck.
	bool wasEditingNewTextBox = (inlineEdit_ && inlineAutoExitSelect_);
	if (inlineEdit_) commitInlineEdit(true);
	SetFocus(hwnd_);
	if (wasEditingNewTextBox) {
		if (onExitTextTool_) onExitTextTool_();
		return;
	}
	if (!doc_ || !doc_->isOpen()) {
		int cmdId;
		if (hitTestEmptyState(mx, my, cmdId) && onEmptyStateAction_) onEmptyStateAction_(cmdId);
		return;
	}

	// Manual multi-click tracking (not WM_LBUTTONDBLCLK / CS_DBLCLKS): Windows
	// has no native triple-click message, so double- and triple-click are
	// both detected here from raw down events, using the same timing/distance
	// thresholds the OS uses for its own double-click recognition.
	DWORD now = GetMessageTime();
	if (clickCount_ > 0 && static_cast<DWORD>(now - lastClickTime_) <= GetDoubleClickTime() &&
		std::abs(mx - lastClickPos_.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
		std::abs(my - lastClickPos_.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2)
		++clickCount_;
	else
		clickCount_ = 1;
	lastClickTime_ = now;
	lastClickPos_ = { mx, my };

	int page; float px, py;
	if (!hitTestPage(mx, my, page, px, py)) return;

	if (tool_ == Tool::Select) {
		clearTextSelection();
		AnnotInfo ai = doc_->annotAt(page, { px, py });
		if (ai.kind == AnnotKind::FreeText) { beginExistingAnnotEdit(page, ai); return; }
		WidgetInfo wi = doc_->widgetAt(page, { px, py });
		if (wi.index >= 0) { doFormFill(page, px, py); return; }
		if (const LinkInfo* lk = linkAt(page, px, py)) { activateLink(*lk); return; }
		if (clickCount_ >= 3) { selectLineAt(page, px, py); return; }
		if (clickCount_ == 2) { selectWordAt(page, px, py); return; }
		beginTextSelection(page, px, py);
		return;
	}
	if (tool_ == Tool::Erase) {
		// Whole-stroke eraser: touching an ink stroke deletes that entire
		// annotation (no partial/pixel erasing -- ink annotations aren't
		// easily split). Erase immediately on click, then continuously as
		// the drag passes over more strokes (see onMouseMove).
		dragging_ = true;
		SetCapture(hwnd_);
		eraseAt(mx, my);
		return;
	}
	// Begin a drag for Highlight / Draw / Text.
	dragging_ = true;
	dragPage_ = page;
	dragStartX_ = dragCurX_ = mx;
	dragStartY_ = dragCurY_ = my;
	dragScale_ = effScale();
	dragBound_ = doc_->pageBound(page);
	pageScreenOrigin(page, pageOrgX_, pageOrgY_);
	curStroke_.clear();
	if (tool_ == Tool::Draw) curStroke_.push_back({ px, py });
	SetCapture(hwnd_);
}

void CanvasView::onMouseMove(int mx, int my)
{
	// Arm WM_MOUSELEAVE so hover state (link preview) clears when the cursor
	// exits the canvas; TrackMouseEvent must be re-armed after each leave.
	if (!trackingMouseLeave_) {
		TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd_, 0 };
		if (TrackMouseEvent(&tme)) trackingMouseLeave_ = true;
	}
	if (activeResizeDir_ >= 0) { updateResize(mx, my); return; }
	if (selDragging_) {
		int page; float px, py;
		if (hitTestPage(mx, my, page, px, py))
			updateTextSelection(page, px, py);
		return;
	}
	if (!dragging_) {
		if (tool_ == Tool::Select) updateLinkHover(mx, my);
		return;
	}
	if (tool_ == Tool::Erase) { eraseAt(mx, my); return; }
	dragCurX_ = mx; dragCurY_ = my;
	if (tool_ == Tool::Draw) {
		float px = dragBound_.x0 + (mx - pageOrgX_) / dragScale_;
		float py = dragBound_.y0 + (my - pageOrgY_) / dragScale_;
		curStroke_.push_back({ px, py });
	}
	invalidate();
}

void CanvasView::onLButtonUp(int mx, int my)
{
	if (activeResizeDir_ >= 0) { endResize(); return; }
	if (selDragging_) {
		selDragging_ = false;
		ReleaseCapture();
		// A click with no real drag (or a drag that collapsed to one spot)
		// clears the selection instead of leaving a stray single-char one.
		if (selAnchorIdx_ == selFocusIdx_) clearTextSelection();
		invalidate();
		return;
	}
	if (!dragging_) return;
	dragging_ = false;
	ReleaseCapture();
	dragCurX_ = mx; dragCurY_ = my;
	std::string err;
	bool changed = false;

	auto toPage = [&](int sx, int sy, float& px, float& py) {
		px = dragBound_.x0 + (sx - pageOrgX_) / dragScale_;
		py = dragBound_.y0 + (sy - pageOrgY_) / dragScale_;
	};

	if (tool_ == Tool::Highlight) {
		float x0, y0, x1, y1;
		toPage(dragStartX_, dragStartY_, x0, y0);
		toPage(mx, my, x1, y1);
		auto quads = doc_->textQuadsInRect(dragPage_, { x0, y0, x1, y1 });
		if (doc_->addHighlight(dragPage_, quads, highlightColor_, opacity_, err)) changed = true;
		else if (!quads.empty()) MessageBoxW(hwnd_, L"Could not add highlight.", L"Highlight", MB_OK | MB_ICONWARNING);
	} else if (tool_ == Tool::Redact) {
		float x0, y0, x1, y1;
		toPage(dragStartX_, dragStartY_, x0, y0);
		toPage(mx, my, x1, y1);
		PageRectPt rectPt{ std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1) };
		if (rectPt.x1 - rectPt.x0 > 2 && rectPt.y1 - rectPt.y0 > 2) {
			if (doc_->addRedaction(dragPage_, rectPt, err)) changed = true;
		}
	} else if (tool_ == Tool::Draw) {
		std::vector<std::vector<PagePointF>> strokes{ curStroke_ };
		if (doc_->addInk(dragPage_, strokes, drawColor_, penWidth_, err)) changed = true;
	} else if (tool_ == Tool::Text) {
		// No dialogs: type directly into an inline box, pick color from the
		// floating swatch popup, and it commits when you click away or hit Esc.
		float x0, y0, x1, y1;
		toPage(dragStartX_, dragStartY_, x0, y0);
		toPage(mx, my, x1, y1);
		if (std::abs(x1 - x0) < 8) x1 = x0 + 160; // default box if just a click
		if (std::abs(y1 - y0) < 8) y1 = y0 + 40;
		PageRectPt rectPt{ std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1) };
		int page = dragPage_;
		curStroke_.clear();
		InlineEditOptions opts;
		opts.multiline = true;
		opts.showPopup = true;
		opts.autoExitSelect = true;
		opts.fontSize = inlineFontSize_ > 0 ? inlineFontSize_ : 12.0f;
		beginInlineEdit(page, rectPt, "", opts,
			[this, page](const std::string& text, bool committed) {
				if (!committed || text.empty()) return;
				// Auto-fit to the final text, anchored at wherever the box's
				// top-left ended up, ignoring whatever size was dragged
				// during editing -- the saved box always shrink-wraps.
				PageRectPt finalRect = autoFitTextRect(page, Utf8ToWide(text),
					inlineEditRectPt_.x0, inlineEditRectPt_.y0, inlineFontSize_);
				std::string err2;
				if (doc_->addTextBox(page, finalRect, text, "Helv", inlineFontSize_, textColor_, err2)) {
					invalidatePage(page);
					invalidate();
					if (onChanged_) onChanged_();
				}
			});
		return; // inline editor owns the rest of this interaction
	}

	curStroke_.clear();
	if (changed) {
		invalidatePage(dragPage_);
		invalidate();
		if (onChanged_) onChanged_();
	} else {
		invalidate(); // clear any rubber-band overlay
	}
}

void CanvasView::eraseAt(int mx, int my)
{
	if (!doc_ || !doc_->isPdf()) return;
	int page; float px, py;
	if (!hitTestPage(mx, my, page, px, py)) return;
	// annotAt() picks whichever non-Other annotation is topmost at this
	// point (bbox hit test) -- only delete if that's actually an ink
	// stroke, so the eraser doesn't also eat highlights/text boxes it
	// happens to pass over.
	AnnotInfo ai = doc_->annotAt(page, { px, py });
	if (ai.kind != AnnotKind::Ink) return;
	std::string err;
	if (doc_->deleteAnnot(page, ai.index, err)) {
		invalidatePage(page);
		invalidate();
		if (onChanged_) onChanged_();
	}
}

void CanvasView::doFormFill(int page, float px, float py)
{
	if (!doc_ || !doc_->isPdf()) return;
	WidgetInfo wi = doc_->widgetAt(page, { px, py });
	if (wi.index < 0) return;
	std::string err;
	bool changed = false;

	switch (wi.kind) {
	case WidgetKind::Text: {
		// Type straight into the field on the page -- no popup dialog.
		beginTextWidgetEdit(page, wi.index, wi.rect, wi.value, wi.multiline);
		return; // inline editor owns the rest of this interaction
	}
	case WidgetKind::Checkbox:
	case WidgetKind::Radio:
		if (doc_->toggleWidget(page, wi.index, err)) changed = true;
		break;
	case WidgetKind::Combo:
	case WidgetKind::ListBox: {
		if (wi.options.empty()) break;
		HMENU menu = CreatePopupMenu();
		for (size_t i = 0; i < wi.options.size(); ++i)
			AppendMenuW(menu, MF_STRING, IDM_FORMOPT_BASE + i, Utf8ToWide(wi.options[i]).c_str());
		POINT pt; GetCursorPos(&pt);
		int sel = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
		DestroyMenu(menu);
		if (sel >= IDM_FORMOPT_BASE) {
			int oi = sel - IDM_FORMOPT_BASE;
			if (oi >= 0 && oi < static_cast<int>(wi.options.size()))
				if (doc_->setChoiceWidget(page, wi.index, wi.options[oi], err)) changed = true;
		}
		break;
	}
	default: break;
	}

	if (changed) {
		invalidatePage(page);
		invalidate();
		if (onChanged_) onChanged_();
	}
}

const std::vector<WidgetInfo>& CanvasView::widgetsForPage(int page)
{
	auto it = widgetCache_.find(page);
	if (it != widgetCache_.end()) return it->second;
	std::vector<WidgetInfo> w;
	if (doc_ && doc_->isPdf()) w = doc_->pageWidgets(page);
	return widgetCache_.emplace(page, std::move(w)).first->second;
}

void CanvasView::drawFieldHighlights(HDC dc, int pageIndex, int pageX, int pageY)
{
	if (!doc_ || !doc_->isPdf()) return;
	const auto& widgets = widgetsForPage(pageIndex);
	if (widgets.empty()) return;
	float sc = effScale();
	for (const auto& w : widgets) {
		if (w.kind == WidgetKind::Button || w.kind == WidgetKind::Signature) continue;
		// Skip the field currently being edited inline -- the live edit
		// control sits on top of it already.
		if (inlineEdit_ && inlineIsWidget_ && inlineEditPage_ == pageIndex &&
			std::abs(w.rect.x0 - inlineEditRectPt_.x0) < 0.5f &&
			std::abs(w.rect.y0 - inlineEditRectPt_.y0) < 0.5f)
			continue;
		RECT r;
		r.left = pageX + static_cast<int>(w.rect.x0 * sc);
		r.top = pageY + static_cast<int>(w.rect.y0 * sc);
		r.right = pageX + static_cast<int>(w.rect.x1 * sc);
		r.bottom = pageY + static_cast<int>(w.rect.y1 * sc);
		// Subtle pale-blue fill with no border, matching Chrome's PDF viewer
		// (the old saturated fill + solid blue outline read as "too highlighted").
		FillAlpha(dc, r, RGB(140, 165, 235), 34);
	}
}

// --- Plain-text selection (Select tool, drag over rendered text) ----------

int CanvasView::nearestCharIndex(const std::vector<PageChar>& chars, float px, float py)
{
	if (chars.empty()) return -1;
	int best = 0;
	float bestDist = std::numeric_limits<float>::max();
	for (size_t i = 0; i < chars.size(); ++i) {
		const auto& q = chars[i].quad;
		float cx = (q.x0 + q.x1) * 0.5f, cy = (q.y0 + q.y1) * 0.5f;
		// Prefer vertical proximity first (line), then horizontal, so a
		// point below the text still resolves to the nearest line's char.
		float dy = py - cy;
		float dx = px - cx;
		float dist = dy * dy * 4.0f + dx * dx; // weight line distance higher
		if (dist < bestDist) { bestDist = dist; best = static_cast<int>(i); }
	}
	return best;
}

const std::vector<PageChar>& CanvasView::charsForPage(int page)
{
	auto it = charsCache_.find(page);
	if (it != charsCache_.end()) return it->second;
	std::vector<PageChar> c;
	if (doc_) c = doc_->pageChars(page);
	return charsCache_.emplace(page, std::move(c)).first->second;
}

const std::vector<AnnotInfo>& CanvasView::annotsForPage(int page)
{
	auto it = annotCache_.find(page);
	if (it != annotCache_.end()) return it->second;
	std::vector<AnnotInfo> a;
	if (doc_ && doc_->isPdf()) a = doc_->pageAnnots(page);
	return annotCache_.emplace(page, std::move(a)).first->second;
}

const std::vector<LinkInfo>& CanvasView::linksForPage(int page)
{
	auto it = linksCache_.find(page);
	if (it != linksCache_.end()) return it->second;
	std::vector<LinkInfo> l;
	if (doc_ && doc_->isOpen()) {
		l = doc_->pageLinks(page);                 // real PDF link annotations
		auto autos = detectTextLinks(page);        // plain-text URLs / emails
		l.insert(l.end(), std::make_move_iterator(autos.begin()),
			std::make_move_iterator(autos.end()));
	}
	return linksCache_.emplace(page, std::move(l)).first->second;
}

// Scans a page's extracted text (line by line) for URL and email patterns and
// synthesizes a LinkInfo for each, with a rect spanning the matched glyphs.
// Matches how Chrome/Edge auto-linkify plain-text URLs/emails that the PDF
// itself never marked up as link annotations.
// Common TLDs, checked against a bare domain's final label (e.g. "au" in
// "abc.com.au") before linkifying it. A protocol-less, www-less domain like
// "abc.com.au" is otherwise indistinguishable from a file-name-shaped false
// positive ("readme.pdf", "report.docx" -- extension "pdf"/"docx" isn't a
// TLD, so those are correctly rejected). This list can't be exhaustive
// (Chrome/Edge use the same kind of heuristic for the same reason); it
// covers the generic gTLDs and the common country-code TLDs.
const std::unordered_set<std::string>& knownTlds()
{
	static const std::unordered_set<std::string> tlds = {
		"com", "net", "org", "edu", "gov", "mil", "int", "info", "biz", "name",
		"pro", "mobi", "io", "co", "me", "tv", "cc", "app", "dev", "xyz",
		"online", "site", "tech", "store", "shop", "cloud", "blog",
		"au", "uk", "us", "ca", "de", "fr", "jp", "cn", "in", "nz", "ie", "za",
		"br", "mx", "es", "it", "nl", "se", "no", "dk", "fi", "ru", "ch", "at",
		"be", "pl", "pt", "gr", "hk", "sg", "kr", "tw", "ae", "sa", "il", "tr",
		"id", "my", "ph", "th", "vn", "pk", "ng", "eg", "ar", "cl", "pe", "eu",
	};
	return tlds;
}

std::vector<LinkInfo> CanvasView::detectTextLinks(int page)
{
	std::vector<LinkInfo> out;
	const auto& chars = charsForPage(page);
	if (chars.empty()) return out;

	// Group 1: http(s):// URL. Group 2: www.-prefixed URL. Group 3: email.
	// Group 4: a protocol-less bare domain (e.g. "abc.com.au") -- accepted
	// only if its final label is a known TLD (see knownTlds()), since unlike
	// the other three this shape isn't otherwise distinguishable from a
	// filename. Applied per line so a match can't span a line break.
	static const std::regex re(
		R"((https?://[^\s]+)|(www\.[^\s]+)|([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})|(\b(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,63}(?:/[^\s]*)?\b))",
		std::regex::icase | std::regex::optimize);

	size_t i = 0;
	while (i < chars.size()) {
		std::string line;                 // ASCII-folded (non-ASCII -> space delimiter)
		std::vector<size_t> map;          // line offset -> index into `chars`
		while (i < chars.size()) {
			int u = chars[i].unicode;
			line.push_back((u > 32 && u < 127) ? static_cast<char>(u) : ' ');
			map.push_back(i);
			bool brk = chars[i].lineBreakAfter;
			++i;
			if (brk) break;
		}

		for (auto m = std::sregex_iterator(line.begin(), line.end(), re);
			m != std::sregex_iterator(); ++m) {
			size_t s = static_cast<size_t>(m->position());
			size_t e = s + static_cast<size_t>(m->length());
			// Trim trailing sentence punctuation that regex greedily grabbed.
			while (e > s && std::strchr(".,;:)]}>\"'", line[e - 1])) --e;
			if (e <= s) continue;

			std::string matched = line.substr(s, e - s);

			bool isBareDomain = (*m)[4].matched && !(*m)[1].matched && !(*m)[2].matched && !(*m)[3].matched;
			if (isBareDomain) {
				size_t dot = matched.find_last_of('.');
				std::string tld = (dot == std::string::npos) ? std::string() : matched.substr(dot + 1);
				std::transform(tld.begin(), tld.end(), tld.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (!knownTlds().count(tld)) continue; // e.g. "report.docx" -- not a TLD, skip
			}

			LinkInfo link;
			link.external = true;
			if (matched.find('@') != std::string::npos && matched.find("://") == std::string::npos) {
				link.uri = "mailto:" + matched;
			} else if (_strnicmp(matched.c_str(), "http://", 7) != 0 &&
			           _strnicmp(matched.c_str(), "https://", 8) != 0) {
				link.uri = "http://" + matched; // www.-prefixed or bare domain
			} else {
				link.uri = matched;
			}

			// Union the matched glyphs' quads into the link's clickable rect.
			PageRectPt r{ 1e9f, 1e9f, -1e9f, -1e9f };
			for (size_t k = s; k < e; ++k) {
				const auto& q = chars[map[k]].quad;
				r.x0 = std::min(r.x0, q.x0); r.y0 = std::min(r.y0, q.y0);
				r.x1 = std::max(r.x1, q.x1); r.y1 = std::max(r.y1, q.y1);
			}
			link.rect = r;
			out.push_back(std::move(link));
		}
	}
	return out;
}

const LinkInfo* CanvasView::linkAt(int page, float px, float py)
{
	for (const auto& l : linksForPage(page))
		if (px >= l.rect.x0 && px <= l.rect.x1 && py >= l.rect.y0 && py <= l.rect.y1)
			return &l;
	return nullptr;
}

void CanvasView::activateLink(const LinkInfo& link)
{
	if (!link.external) {
		// Internal jump to another page in the same document.
		if (link.targetPage >= 0) goToPage(link.targetPage);
		return;
	}
	// External URL / mailto: hand off to the OS default handler. Only allow a
	// safe scheme so a crafted PDF can't launch arbitrary local programs.
	int n = MultiByteToWideChar(CP_UTF8, 0, link.uri.c_str(), -1, nullptr, 0);
	if (n <= 0) return;
	std::wstring uri(static_cast<size_t>(n - 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, link.uri.c_str(), -1, uri.data(), n);
	auto starts = [&](const wchar_t* s) { return _wcsnicmp(uri.c_str(), s, wcslen(s)) == 0; };
	if (!(starts(L"http://") || starts(L"https://") || starts(L"mailto:"))) return;
	ShellExecuteW(hwnd_, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void CanvasView::onRButtonDown(int mx, int my)
{
	if (tool_ != Tool::Select) return;
	int page; float px, py;
	if (!hitTestPage(mx, my, page, px, py)) return;
	const LinkInfo* hit = linkAt(page, px, py);
	if (!hit) {
		// No link under the cursor: offer the active selection's menu
		// instead, if there is one (matches any text editor's right-click
		// behavior -- Copy plus the new "send to rich text tool" action).
		if (!hasTextSelection()) return;
		HMENU selMenu = CreatePopupMenu();
		AppendMenuW(selMenu, MF_STRING, IDM_SEL_COPY, L"Copy");
		AppendMenuW(selMenu, MF_STRING, IDM_SEL_EDIT_RICHTEXT, L"Edit as Rich Text...");
		POINT spt{ mx, my };
		ClientToScreen(hwnd_, &spt);
		int selCmd = TrackPopupMenu(selMenu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, spt.x, spt.y, 0, hwnd_, nullptr);
		DestroyMenu(selMenu);
		if (selCmd == IDM_SEL_COPY) {
			copySelectionToClipboard();
		} else if (selCmd == IDM_SEL_EDIT_RICHTEXT) {
			std::wstring text = selectedText();
			if (!text.empty() && onOpenTextEditor_) onOpenTextEditor_(text);
		}
		return;
	}
	LinkInfo link = *hit; // TrackPopupMenu pumps messages; don't hold a pointer into the cache across that.

	bool isMail = link.external && _strnicmp(link.uri.c_str(), "mailto:", 7) == 0;
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, IDM_LINK_OPEN, !link.external ? L"Open Link" : isMail ? L"Send Email" : L"Open Link");
	// Internal (page-jump) links carry no user-meaningful address to copy --
	// their `uri` is MuPDF's internal "#page=N" form, not something a link
	// annotation's target is ever expected to be shared/pasted.
	if (link.external)
		AppendMenuW(menu, MF_STRING, IDM_LINK_COPY, isMail ? L"Copy Email Address" : L"Copy Link Address");

	POINT pt{ mx, my };
	ClientToScreen(hwnd_, &pt);
	int sel = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
	DestroyMenu(menu);

	if (sel == IDM_LINK_OPEN) {
		activateLink(link);
	} else if (sel == IDM_LINK_COPY) {
		// mailto: links copy just the address (Outlook/Edge convention), not
		// the scheme prefix; internal (non-external) links have no real URI
		// worth copying, so the menu never offers Copy for those.
		std::string addr = link.uri;
		if (isMail) addr = addr.substr(7);
		int n = MultiByteToWideChar(CP_UTF8, 0, addr.c_str(), -1, nullptr, 0);
		if (n > 0) {
			std::wstring wtext(static_cast<size_t>(n - 1), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, addr.c_str(), -1, wtext.data(), n);
			if (OpenClipboard(hwnd_)) {
				EmptyClipboard();
				HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (wtext.size() + 1) * sizeof(wchar_t));
				if (mem) {
					void* p = GlobalLock(mem);
					memcpy(p, wtext.c_str(), (wtext.size() + 1) * sizeof(wchar_t));
					GlobalUnlock(mem);
					SetClipboardData(CF_UNICODETEXT, mem);
				}
				CloseClipboard();
			}
		}
	}
}

void CanvasView::beginTextSelection(int page, float px, float py)
{
	const auto& chars = charsForPage(page);
	selAnchorPage_ = selFocusPage_ = page;
	selAnchorIdx_ = selFocusIdx_ = nearestCharIndex(chars, px, py);
	selDragging_ = true;
	SetCapture(hwnd_);
	invalidate();
}

void CanvasView::updateTextSelection(int page, float px, float py)
{
	const auto& chars = charsForPage(page);
	if (chars.empty()) return;
	selFocusPage_ = page;
	selFocusIdx_ = nearestCharIndex(chars, px, py);
	invalidate();
}

void CanvasView::selectWordAt(int page, float px, float py)
{
	const auto& chars = charsForPage(page);
	selAnchorPage_ = selFocusPage_ = page;
	int idx = nearestCharIndex(chars, px, py);
	if (idx < 0) { clearTextSelection(); return; }
	auto isWordChar = [](int u) { return u != ' ' && u != '\t' && u != 0; };
	bool wantWord = isWordChar(chars[idx].unicode);
	int lo = idx, hi = idx;
	while (lo > 0 && !chars[lo - 1].lineBreakAfter && !chars[lo - 1].paragraphBreakAfter &&
		isWordChar(chars[lo - 1].unicode) == wantWord)
		--lo;
	while (hi + 1 < static_cast<int>(chars.size()) &&
		!chars[hi].lineBreakAfter && !chars[hi].paragraphBreakAfter &&
		isWordChar(chars[hi + 1].unicode) == wantWord)
		++hi;
	selAnchorIdx_ = lo;
	selFocusIdx_ = hi;
	invalidate();
}

void CanvasView::selectLineAt(int page, float px, float py)
{
	const auto& chars = charsForPage(page);
	selAnchorPage_ = selFocusPage_ = page;
	int idx = nearestCharIndex(chars, px, py);
	if (idx < 0) { clearTextSelection(); return; }
	int lo = idx, hi = idx;
	while (lo > 0 && !chars[lo - 1].lineBreakAfter && !chars[lo - 1].paragraphBreakAfter)
		--lo;
	while (hi + 1 < static_cast<int>(chars.size()) && !chars[hi].lineBreakAfter && !chars[hi].paragraphBreakAfter)
		++hi;
	selAnchorIdx_ = lo;
	selFocusIdx_ = hi;
	invalidate();
}

void CanvasView::selectAll()
{
	if (!doc_ || !doc_->isOpen()) return;
	int page = (mode_ == Mode::Continuous) ? firstVisiblePage() : currentPage_;
	const auto& chars = charsForPage(page);
	selAnchorPage_ = selFocusPage_ = page;
	if (chars.empty()) { clearTextSelection(); return; }
	selAnchorIdx_ = 0;
	selFocusIdx_ = static_cast<int>(chars.size()) - 1;
	invalidate();
}

void CanvasView::clearTextSelection()
{
	if (selAnchorIdx_ < 0 && selFocusIdx_ < 0) return;
	selAnchorPage_ = selFocusPage_ = -1;
	selAnchorIdx_ = selFocusIdx_ = -1;
	invalidate();
}

bool CanvasView::selectionRangeForPage(int pageIndex, int& lo, int& hi) const
{
	if (selAnchorIdx_ < 0 || selFocusIdx_ < 0) return false;
	int startPage = std::min(selAnchorPage_, selFocusPage_);
	int endPage = std::max(selAnchorPage_, selFocusPage_);
	if (pageIndex < startPage || pageIndex > endPage) return false;
	if (startPage == endPage) {
		// Single-page selection (the common case): order is whichever of
		// anchor/focus is smaller -- a drag can go either direction.
		lo = std::min(selAnchorIdx_, selFocusIdx_);
		hi = std::max(selAnchorIdx_, selFocusIdx_);
		return true;
	}
	// Multi-page: whichever of anchor/focus sits on the earlier page is the
	// span's start index; the other is the end index (independent of which
	// one is literally "anchor" -- the user can drag either up or down).
	bool anchorIsStart = selAnchorPage_ <= selFocusPage_;
	int startIdx = anchorIsStart ? selAnchorIdx_ : selFocusIdx_;
	int endIdx = anchorIsStart ? selFocusIdx_ : selAnchorIdx_;
	if (pageIndex == startPage) { lo = startIdx; hi = std::numeric_limits<int>::max(); return true; } // to end of page
	if (pageIndex == endPage) { lo = 0; hi = endIdx; return true; } // from start of page
	lo = 0; hi = std::numeric_limits<int>::max(); // a page fully between start and end is fully selected
	return true;
}

void CanvasView::drawTextSelection(HDC dc, int pageIndex, int pageX, int pageY)
{
	int lo, hi;
	if (!selectionRangeForPage(pageIndex, lo, hi)) return;
	const auto& chars = charsForPage(pageIndex);
	float sc = effScale();
	// Merge consecutive characters on the same line into one filled rect,
	// instead of alpha-blending each glyph's own quad separately -- doing it
	// per-glyph double-blends the shared edges between adjacent quads
	// (rounding in the float->pixel cast rarely lines them up exactly),
	// giving the highlight a seamed/"wired" look instead of one smooth band
	// like every other text-selection UI draws.
	bool haveRun = false;
	RECT run{};
	auto flush = [&]() {
		if (haveRun) FillAlpha(dc, run, RGB(60, 120, 220), 90);
		haveRun = false;
	};
	for (int i = lo; i <= hi && i < static_cast<int>(chars.size()); ++i) {
		const auto& q = chars[i].quad;
		RECT r;
		r.left = pageX + static_cast<int>(q.x0 * sc);
		r.top = pageY + static_cast<int>(q.y0 * sc);
		r.right = pageX + static_cast<int>(q.x1 * sc);
		r.bottom = pageY + static_cast<int>(q.y1 * sc);
		if (!haveRun) { run = r; haveRun = true; }
		else {
			run.left = std::min(run.left, r.left);
			run.top = std::min(run.top, r.top);
			run.right = std::max(run.right, r.right);
			run.bottom = std::max(run.bottom, r.bottom);
		}
		if (chars[i].lineBreakAfter || chars[i].paragraphBreakAfter) flush();
	}
	flush();
}

HCURSOR CanvasView::cursorForSelectAt(int mx, int my)
{
	int page; float px, py;
	if (!hitTestPage(mx, my, page, px, py)) return nullptr;

	// A hyperlink takes priority: hand cursor, like every browser/PDF viewer.
	if (linkAt(page, px, py)) return LoadCursor(nullptr, IDC_HAND);

	for (const auto& a : annotsForPage(page)) {
		if (a.kind != AnnotKind::FreeText) continue;
		if (px >= a.rect.x0 && px <= a.rect.x1 && py >= a.rect.y0 && py <= a.rect.y1)
			return LoadCursor(nullptr, IDC_HAND);
	}
	if (doc_ && doc_->isPdf()) {
		for (const auto& w : widgetsForPage(page)) {
			if (w.kind == WidgetKind::Button || w.kind == WidgetKind::Signature) continue;
			if (px >= w.rect.x0 && px <= w.rect.x1 && py >= w.rect.y0 && py <= w.rect.y1)
				// A text field is typed into (I-beam, like any editable text);
				// checkboxes/radios/combos/list boxes are clicked (hand).
				return LoadCursor(nullptr, w.kind == WidgetKind::Text ? IDC_IBEAM : IDC_HAND);
		}
	}
	// A small padding around each glyph quad makes the I-beam easier to hit
	// without requiring pixel-perfect hover over thin/small characters.
	constexpr float kPad = 1.0f;
	for (const auto& c : charsForPage(page)) {
		if (px >= c.quad.x0 - kPad && px <= c.quad.x1 + kPad && py >= c.quad.y0 && py <= c.quad.y1)
			return LoadCursor(nullptr, IDC_IBEAM);
	}
	return nullptr;
}

void CanvasView::updateLinkHover(int mx, int my)
{
	int page; float px, py;
	std::string text;
	if (hitTestPage(mx, my, page, px, py)) {
		if (const LinkInfo* lk = linkAt(page, px, py)) {
			if (!lk->external) {
				text = "Go to page " + std::to_string(lk->targetPage + 1);
			} else {
				text = lk->uri;
			}
		}
	}
	if (text != hoveredLinkText_) {
		hoveredLinkText_ = std::move(text);
		updateStatus();
	}
}

std::wstring CanvasView::selectedText()
{
	if (selAnchorIdx_ < 0 || selFocusIdx_ < 0) return {};
	int startPage = std::min(selAnchorPage_, selFocusPage_);
	int endPage = std::max(selAnchorPage_, selFocusPage_);
	std::wstring text;
	for (int page = startPage; page <= endPage; ++page) {
		int lo, hi;
		if (!selectionRangeForPage(page, lo, hi)) continue;
		const auto& chars = charsForPage(page);
		int hiClamped = std::min(hi, static_cast<int>(chars.size()) - 1);
		for (int i = lo; i <= hiClamped; ++i) {
			const auto& c = chars[i];
			if (c.unicode >= 0x10000) {
				// Encode as a UTF-16 surrogate pair.
				unsigned int v = static_cast<unsigned int>(c.unicode) - 0x10000;
				text.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
				text.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
			} else {
				text.push_back(static_cast<wchar_t>(c.unicode));
			}
			// Never emit a trailing break after the LAST selected character
			// of the WHOLE (possibly multi-page) selection -- triple-click
			// (select line) lands exactly on a lineBreakAfter char, so
			// without this guard every line-selection copy carried an
			// invisible trailing blank line, which pasted into Word as an
			// extra Enter. A page boundary mid-selection is deliberately
			// NOT forced to break here either: a paragraph that continues
			// from the bottom of one page onto the next has no real
			// line/paragraph break at that character, and should paste as
			// one continuous paragraph, not get an unwanted line split.
			if (page == endPage && i == hiClamped) continue;
			if (c.paragraphBreakAfter) text += L"\r\n\r\n";
			else if (c.lineBreakAfter) text += L"\r\n";
		}
	}
	// MuPDF's text extraction can include a real/synthesized trailing space
	// at the end of a line (a word-gap heuristic, or an actual space glyph
	// before the line wraps) -- invisible when followed by a newline, but
	// triple-click (select line) has no newline after it anymore (see the
	// guard above), so that trailing space pasted as a visible stray
	// character with nothing after it to hide it. Trim it.
	while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) text.pop_back();
	return text;
}

void CanvasView::copySelectionToClipboard()
{
	std::wstring text = selectedText();
	if (text.empty()) return;
	if (!OpenClipboard(hwnd_)) return;
	EmptyClipboard();
	HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
	if (mem) {
		void* p = GlobalLock(mem);
		memcpy(p, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
		GlobalUnlock(mem);
		SetClipboardData(CF_UNICODETEXT, mem);
	}
	CloseClipboard();
}

// --- Inline editing (no modal dialogs for form fields / free-text boxes) ---

void CanvasView::positionInlineEdit()
{
	if (!inlineEdit_) return;
	int sx, sy;
	if (!pageScreenOrigin(inlineEditPage_, sx, sy)) { commitInlineEdit(false); return; }
	float sc = effScale();
	int x0 = sx + static_cast<int>(inlineEditRectPt_.x0 * sc);
	int y0 = sy + static_cast<int>(inlineEditRectPt_.y0 * sc);
	int x1 = sx + static_cast<int>(inlineEditRectPt_.x1 * sc);
	int y1 = sy + static_cast<int>(inlineEditRectPt_.y1 * sc);
	MoveWindow(inlineEdit_, x0, y0, std::max(24, x1 - x0), std::max(18, y1 - y0), TRUE);

	if (inlineShowPopup_) {
		makeInlineBgBrush();
		InvalidateRect(inlineEdit_, nullptr, TRUE);
	}

	if (colorPopup_) {
		UINT dpi = GetDpiForWindow(hwnd_);
		bool showDelete = inlineExistingAnnotIndex_ >= 0;
		int pw = Scale(showDelete ? 210 : 180, dpi), ph = Scale(30, dpi);
		int margin = Scale(4, dpi);
		// Decide above-vs-below in client space (where we know the canvas'
		// visible bounds), then convert once to screen coordinates.
		bool roomAbove = (y0 - ph - margin) >= 0;
		POINT pt = roomAbove ? POINT{ x0, y0 - ph - margin } : POINT{ x0, y1 + margin };
		ClientToScreen(hwnd_, &pt);
		SetWindowPos(colorPopup_, HWND_TOPMOST, pt.x, pt.y, pw, ph, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}

	positionResizeHandles();
}

// Builds a pattern brush from the page pixels currently rendered directly
// behind the free-text box, so the live EDIT control can look "transparent"
// (matching how the committed annotation itself renders -- see
// PdfDocument::setFreeTextAnnot/addTextBox, which never fill a background)
// instead of painting a solid color over whatever is on the page there.
// Native EDIT controls have no real alpha transparency, so this fakes it by
// tiling exactly the captured rect.
//
// When re-editing an *existing* FreeText annotation, that annotation's own
// (not-yet-committed) old appearance is still part of the normal cached page
// render, so grabbing pixels from it would show the old text doubled up
// behind the new typing. Use a one-off render with just that annotation
// hidden instead (PdfDocument::renderPageWithoutAnnot) -- new boxes have
// nothing to hide, so they can use the already-cached page bitmap.
void CanvasView::makeInlineBgBrush()
{
	if (inlineBgBrush_) { DeleteObject(inlineBgBrush_); inlineBgBrush_ = nullptr; }
	if (!doc_) return;
	float sc = effScale();

	HBITMAP pageHb = nullptr;
	int pw = 0, ph = 0;
	PageBitmap freshBmp; // keeps the fresh render's HBITMAP alive for this scope
	if (inlineExistingAnnotIndex_ >= 0) {
		freshBmp = doc_->renderPageWithoutAnnot(inlineEditPage_, sc, inlineExistingAnnotIndex_);
		pageHb = freshBmp.hbmp;
		pw = freshBmp.width; ph = freshBmp.height;
	} else {
		pageHb = ensureRendered(inlineEditPage_, pw, ph);
	}
	if (!pageHb) return;
	int bx0 = std::clamp(static_cast<int>(inlineEditRectPt_.x0 * sc), 0, pw);
	int by0 = std::clamp(static_cast<int>(inlineEditRectPt_.y0 * sc), 0, ph);
	int bx1 = std::clamp(static_cast<int>(inlineEditRectPt_.x1 * sc), 0, pw);
	int by1 = std::clamp(static_cast<int>(inlineEditRectPt_.y1 * sc), 0, ph);
	int bw = std::max(1, bx1 - bx0), bh = std::max(1, by1 - by0);

	HDC screenDC = GetDC(nullptr);
	HDC pageDC = CreateCompatibleDC(screenDC);
	HBITMAP oldPage = static_cast<HBITMAP>(SelectObject(pageDC, pageHb));
	HDC dstDC = CreateCompatibleDC(screenDC);
	HBITMAP dst = CreateCompatibleBitmap(screenDC, bw, bh);
	HBITMAP oldDst = static_cast<HBITMAP>(SelectObject(dstDC, dst));
	BitBlt(dstDC, 0, 0, bw, bh, pageDC, bx0, by0, SRCCOPY);
	SelectObject(dstDC, oldDst);
	SelectObject(pageDC, oldPage);
	DeleteDC(dstDC);
	DeleteDC(pageDC);
	ReleaseDC(nullptr, screenDC);

	inlineBgBrush_ = CreatePatternBrush(dst);
	DeleteObject(dst); // CreatePatternBrush keeps its own copy
}

void CanvasView::createResizeHandles()
{
	HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
	for (int i = 0; i < 8; ++i) {
		resizeHandles_[i] = CreateWindowExW(0, kResizeHandleClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
			0, 0, 8, 8, hwnd_, nullptr, hInst, nullptr);
		SetWindowLongPtrW(resizeHandles_[i], 0, reinterpret_cast<LONG_PTR>(this));
		SetWindowLongPtrW(resizeHandles_[i], sizeof(LONG_PTR), static_cast<LONG_PTR>(i));
	}
}

void CanvasView::destroyResizeHandles()
{
	for (auto& h : resizeHandles_) {
		if (h) { DestroyWindow(h); h = nullptr; }
	}
}

void CanvasView::positionResizeHandles()
{
	if (!resizeHandles_[0]) return;
	int sx, sy;
	if (!pageScreenOrigin(inlineEditPage_, sx, sy)) return;
	float sc = effScale();
	int x0 = sx + static_cast<int>(inlineEditRectPt_.x0 * sc);
	int y0 = sy + static_cast<int>(inlineEditRectPt_.y0 * sc);
	int x1 = sx + static_cast<int>(inlineEditRectPt_.x1 * sc);
	int y1 = sy + static_cast<int>(inlineEditRectPt_.y1 * sc);
	int hs = Scale(4, dpi_); // half-size: 8px handle at 96 DPI
	int xm = (x0 + x1) / 2, ym = (y0 + y1) / 2;
	const int cx[8] = { x0, xm, x1, x0, x1, x0, xm, x1 };
	const int cy[8] = { y0, y0, y0, ym, ym, y1, y1, y1 };
	for (int i = 0; i < 8; ++i)
		MoveWindow(resizeHandles_[i], cx[i] - hs, cy[i] - hs, hs * 2, hs * 2, TRUE);
}

void CanvasView::beginResize(int dir)
{
	if (!inlineEdit_ || !doc_) return;
	activeResizeDir_ = dir;
	resizeBound_ = doc_->pageBound(inlineEditPage_);
	resizeScale_ = effScale();
	pageScreenOrigin(inlineEditPage_, resizeOrgX_, resizeOrgY_);
	SetCapture(hwnd_);
}

void CanvasView::beginMove(int mx, int my)
{
	if (!inlineEdit_ || !doc_) return;
	activeResizeDir_ = kResizeMove;
	resizeBound_ = doc_->pageBound(inlineEditPage_);
	resizeScale_ = effScale();
	pageScreenOrigin(inlineEditPage_, resizeOrgX_, resizeOrgY_);
	float px = resizeBound_.x0 + (mx - resizeOrgX_) / resizeScale_;
	float py = resizeBound_.y0 + (my - resizeOrgY_) / resizeScale_;
	moveGrabDX_ = px - inlineEditRectPt_.x0;
	moveGrabDY_ = py - inlineEditRectPt_.y0;
	SetCapture(hwnd_);
}

void CanvasView::updateResize(int mx, int my)
{
	if (activeResizeDir_ < 0) return;
	float px = resizeBound_.x0 + (mx - resizeOrgX_) / resizeScale_;
	float py = resizeBound_.y0 + (my - resizeOrgY_) / resizeScale_;
	constexpr float kMinW = 20.0f, kMinH = 12.0f;
	PageRectPt r = inlineEditRectPt_;
	switch (activeResizeDir_) {
	case kResizeNW: r.x0 = std::min(px, r.x1 - kMinW); r.y0 = std::min(py, r.y1 - kMinH); break;
	case kResizeN:  r.y0 = std::min(py, r.y1 - kMinH); break;
	case kResizeNE: r.x1 = std::max(px, r.x0 + kMinW); r.y0 = std::min(py, r.y1 - kMinH); break;
	case kResizeW:  r.x0 = std::min(px, r.x1 - kMinW); break;
	case kResizeE:  r.x1 = std::max(px, r.x0 + kMinW); break;
	case kResizeSW: r.x0 = std::min(px, r.x1 - kMinW); r.y1 = std::max(py, r.y0 + kMinH); break;
	case kResizeS:  r.y1 = std::max(py, r.y0 + kMinH); break;
	case kResizeSE: r.x1 = std::max(px, r.x0 + kMinW); r.y1 = std::max(py, r.y0 + kMinH); break;
	case kResizeMove: {
		float w = r.x1 - r.x0, h = r.y1 - r.y0;
		float nx0 = px - moveGrabDX_, ny0 = py - moveGrabDY_;
		r = { nx0, ny0, nx0 + w, ny0 + h };
		break;
	}
	}
	inlineEditRectPt_ = r;
	positionInlineEdit(); // also repositions the handles/move frame
}

void CanvasView::endResize()
{
	activeResizeDir_ = -1;
	ReleaseCapture();
}

void CanvasView::updateInlineEditFont()
{
	if (!inlineEdit_) return;
	// inlineFontSize_ is in PDF points; effScale() is device-px per point.
	float sizePx = std::max(6.0f, inlineFontSize_ * effScale());
	LOGFONTW lf = {};
	lf.lfHeight = -static_cast<int>(std::lround(sizePx));
	lf.lfWeight = FW_NORMAL;
	lf.lfCharSet = DEFAULT_CHARSET;
	wcscpy_s(lf.lfFaceName, L"Segoe UI");
	HFONT f = CreateFontIndirectW(&lf);
	SendMessageW(inlineEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
	if (inlineEditFont_) DeleteObject(inlineEditFont_);
	inlineEditFont_ = f;
}

void CanvasView::adjustInlineFontSize(float deltaPt)
{
	inlineFontSize_ = std::clamp(inlineFontSize_ + deltaPt, 6.0f, 72.0f);
	updateInlineEditFont();
	invalidate();
}

void CanvasView::beginInlineEdit(int page, PageRectPt rectPt, const std::string& utf8,
	const InlineEditOptions& opts, std::function<void(const std::string&, bool)> onDone)
{
	commitInlineEdit(false); // clean up any previous session first

	HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
	DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_BORDER | ES_AUTOHSCROLL;
	if (opts.multiline) style |= ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
	inlineEdit_ = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10,
		hwnd_, nullptr, hInst, nullptr);

	inlineEditPage_ = page;
	inlineEditRectPt_ = rectPt;
	inlineEditDone_ = std::move(onDone);
	inlineIsWidget_ = opts.isWidget;
	inlineShowPopup_ = opts.showPopup;
	inlineAutoExitSelect_ = opts.autoExitSelect;
	inlineExistingAnnotIndex_ = opts.existingAnnotIndex;
	inlineWidgetIndex_ = opts.widgetIndex;
	inlineFontSize_ = opts.fontSize;

	if (inlineShowPopup_) {
		updateInlineEditFont(); // reflect the actual annotation point size
	} else {
		NONCLIENTMETRICSW ncm = { sizeof(ncm) };
		UINT dpi = GetDpiForWindow(hwnd_);
		if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
			HFONT f = CreateFontIndirectW(&ncm.lfMessageFont);
			SendMessageW(inlineEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
			inlineEditFont_ = f;
		}
	}
	SetWindowTextW(inlineEdit_, Utf8ToWide(utf8).c_str());
	SetWindowSubclass(inlineEdit_, InlineEditSubclass, 1, reinterpret_cast<DWORD_PTR>(this));

	if (inlineShowPopup_) {
		// Small floating toolbar (color swatches, font size, delete) positioned
		// just above the box -- matches how modern viewers avoid a dialog here.
		// Owned by the top-level frame (GA_ROOT), not the canvas child window:
		// a popup owned by a non-top-level HWND causes flaky activation
		// (observed as the whole app spuriously minimizing).
		HWND ownerRoot = GetAncestor(hwnd_, GA_ROOT);
		colorPopup_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kColorPopupClass, L"",
			WS_POPUP | WS_VISIBLE, 0, 0, 10, 10, ownerRoot, nullptr, hInst, this);
	}

	// Form fields keep their fixed widget-defined size; only free-text boxes
	// (new or existing) get drag handles.
	if (!opts.isWidget) createResizeHandles();

	positionInlineEdit();
	SetFocus(inlineEdit_);
	SendMessageW(inlineEdit_, EM_SETSEL, 0, -1);
	invalidatePage(page);
	invalidate();
}

void CanvasView::commitInlineEdit(bool commit)
{
	if (!inlineEdit_) return;
	HWND edit = inlineEdit_;
	inlineEdit_ = nullptr; // guard against re-entrancy from focus-kill during destroy
	int n = GetWindowTextLengthW(edit);
	std::wstring buf(static_cast<size_t>(n) + 1, L'\0');
	GetWindowTextW(edit, buf.data(), n + 1);
	buf.resize(n);
	DestroyWindow(edit);
	if (colorPopup_) { DestroyWindow(colorPopup_); colorPopup_ = nullptr; }
	destroyResizeHandles();
	if (activeResizeDir_ >= 0) { activeResizeDir_ = -1; ReleaseCapture(); }
	if (inlineEditFont_) { DeleteObject(inlineEditFont_); inlineEditFont_ = nullptr; }
	if (inlineBgBrush_) { DeleteObject(inlineBgBrush_); inlineBgBrush_ = nullptr; }

	int page = inlineEditPage_;
	inlineEditPage_ = -1;
	inlineExistingAnnotIndex_ = -1;
	inlineWidgetIndex_ = -1;
	auto done = std::move(inlineEditDone_);
	inlineEditDone_ = nullptr;
	if (done) done(WideToUtf8(buf), commit);
	invalidatePage(page);
	invalidate();
	SetFocus(hwnd_);
}

void CanvasView::deleteInlineAnnotAndClose()
{
	if (!inlineEdit_ || inlineExistingAnnotIndex_ < 0) return;
	int page = inlineEditPage_;
	int idx = inlineExistingAnnotIndex_;
	// Close without committing (there's nothing to write back to), then
	// delete the annotation itself.
	inlineEditDone_ = nullptr;
	commitInlineEdit(false);
	std::string err;
	if (doc_ && doc_->deleteAnnot(page, idx, err)) {
		invalidatePage(page);
		invalidate();
		if (onChanged_) onChanged_();
	}
}

PageRectPt CanvasView::autoFitTextRect(int page, const std::wstring& text, float x0, float y0, float fontSizePt) const
{
	float sc = effScale();
	float sizePx = std::max(6.0f, fontSizePt * sc);
	LOGFONTW lf = {};
	lf.lfHeight = -static_cast<int>(std::lround(sizePx));
	lf.lfWeight = FW_NORMAL;
	lf.lfCharSet = DEFAULT_CHARSET;
	wcscpy_s(lf.lfFaceName, L"Segoe UI");
	HFONT f = CreateFontIndirectW(&lf);

	HDC screenDC = GetDC(nullptr);
	HDC memDC = CreateCompatibleDC(screenDC);
	HFONT oldFont = static_cast<HFONT>(SelectObject(memDC, f));

	std::wstring t = text.empty() ? L" " : text;
	RECT rc = { 0, 0, 0, 0 };
	DrawTextW(memDC, t.c_str(), -1, &rc, DT_CALCRECT | DT_NOPREFIX);

	// Clamp width to a sane fraction of the page (a single very long typed
	// line shouldn't blow the box off the page); if that clips the natural
	// width, re-measure height with wrapping at the clamped width so the two
	// numbers stay consistent with each other.
	PageRectPt pageBound = doc_ ? doc_->pageBound(page) : PageRectPt{ 0, 0, 612, 792 };
	int maxWidthPx = static_cast<int>((pageBound.x1 - pageBound.x0) * 0.9f * sc);
	if (maxWidthPx > 0 && rc.right > maxWidthPx) {
		RECT wrc = { 0, 0, maxWidthPx, 0 };
		DrawTextW(memDC, t.c_str(), -1, &wrc, DT_CALCRECT | DT_NOPREFIX | DT_WORDBREAK);
		rc.right = maxWidthPx;
		rc.bottom = wrc.bottom;
	}

	SelectObject(memDC, oldFont);
	DeleteObject(f);
	DeleteDC(memDC);
	ReleaseDC(nullptr, screenDC);

	constexpr float kMarginPt = 4.0f; // "a few pixels" of margin, in PDF points
	float wPt = std::max(20.0f, rc.right / sc + kMarginPt * 2);
	float hPt = std::max(14.0f, rc.bottom / sc + kMarginPt * 2);
	return { x0, y0, x0 + wPt, y0 + hPt };
}

void CanvasView::beginExistingAnnotEdit(int page, const AnnotInfo& ai)
{
	textColor_ = ai.color ? ai.color : textColor_;
	InlineEditOptions opts;
	opts.multiline = true;
	opts.showPopup = true;
	opts.autoExitSelect = false; // stay in Select tool while editing an existing box
	opts.existingAnnotIndex = ai.index;
	opts.fontSize = ai.fontSize > 0 ? ai.fontSize : 12.0f;
	beginInlineEdit(page, ai.rect, ai.contents, opts,
		[this, page, idx = ai.index](const std::string& text, bool committed) {
			if (!committed) return;
			// Auto-fit to the final text, anchored at wherever the box's
			// top-left ended up (move handled), ignoring whatever size was
			// dragged during editing -- the saved box always shrink-wraps.
			PageRectPt finalRect = autoFitTextRect(page, Utf8ToWide(text),
				inlineEditRectPt_.x0, inlineEditRectPt_.y0, inlineFontSize_);
			std::string err;
			doc_->setFreeTextAnnot(page, idx, text, inlineFontSize_, textColor_, finalRect, err);
			invalidatePage(page);
			invalidate();
			if (onChanged_) onChanged_();
		});
}

void CanvasView::beginTextWidgetEdit(int page, int widgetIndex, PageRectPt rect, const std::string& value, bool multiline)
{
	InlineEditOptions opts;
	opts.multiline = multiline;
	opts.isWidget = true;
	opts.widgetIndex = widgetIndex;
	beginInlineEdit(page, rect, value, opts,
		[this, page, widgetIndex](const std::string& text, bool committed) {
			if (!committed) return;
			std::string err2;
			if (doc_->setTextWidget(page, widgetIndex, text, err2)) {
				invalidatePage(page);
				invalidate();
				if (onChanged_) onChanged_();
			}
		});
}

void CanvasView::tabToAdjacentTextWidget(bool forward)
{
	if (!inlineEdit_ || !inlineIsWidget_ || inlineEditPage_ < 0) return;
	int page = inlineEditPage_;
	int curIndex = inlineWidgetIndex_;

	// Commit the current field first (Tab confirms-and-moves-on, same as any
	// other form filler) -- this also invalidates the page's cached widget
	// list, so the lookup below picks up the value just typed.
	commitInlineEdit(true);

	const auto& widgets = widgetsForPage(page);
	// Only text fields get an inline edit box to tab into; walk just those,
	// in the page's own field order, wrapping around. Position relative to
	// the field just left (by its widget index), not by its position in
	// `widgets` -- checkboxes/combos can sit between text fields there.
	std::vector<int> textPositions;
	int curPos = -1;
	for (size_t i = 0; i < widgets.size(); ++i) {
		if (widgets[i].kind != WidgetKind::Text) continue;
		if (widgets[i].index == curIndex) curPos = static_cast<int>(textPositions.size());
		textPositions.push_back(static_cast<int>(i));
	}
	if (textPositions.empty()) return;
	int count = static_cast<int>(textPositions.size());
	int nextPos = curPos < 0 ? (forward ? 0 : count - 1) : (curPos + (forward ? 1 : -1) + count) % count;
	const WidgetInfo& next = widgets[textPositions[nextPos]];
	beginTextWidgetEdit(page, next.index, next.rect, next.value, next.multiline);
}

LRESULT CALLBACK CanvasView::InlineEditSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
	UINT_PTR, DWORD_PTR ref)
{
	auto* self = reinterpret_cast<CanvasView*>(ref);
	// A click/hover within this many px of the control's own edge starts a
	// move-drag instead of the default text-cursor placement -- lets the
	// user reposition a free-text box by grabbing its border, without a
	// separate overlapping window (which would fight the resize handles for
	// clicks in that same strip; see the 8 corner/edge squares created in
	// createResizeHandles()).
	constexpr int kBorderGrab = 4;
	if (self->inlineShowPopup_ && (msg == WM_LBUTTONDOWN || msg == WM_SETCURSOR)) {
		POINT pt;
		if (msg == WM_LBUTTONDOWN) pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		else { GetCursorPos(&pt); ScreenToClient(hwnd, &pt); }
		RECT rc; GetClientRect(hwnd, &rc);
		bool onBorder = pt.x < kBorderGrab || pt.y < kBorderGrab ||
			pt.x >= rc.right - kBorderGrab || pt.y >= rc.bottom - kBorderGrab;
		if (onBorder) {
			if (msg == WM_LBUTTONDOWN) {
				POINT canvasPt = pt;
				ClientToScreen(hwnd, &canvasPt);
				ScreenToClient(self->hwnd(), &canvasPt);
				self->beginMove(canvasPt.x, canvasPt.y);
				return 0;
			}
			SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
			return TRUE;
		}
	}
	if (msg == WM_KEYDOWN) {
		bool multiline = (GetWindowLongPtrW(hwnd, GWL_STYLE) & ES_MULTILINE) != 0;
		if (wp == VK_RETURN && !multiline) { self->commitInlineEdit(true); return 0; }
		if (wp == VK_ESCAPE) { self->commitInlineEdit(false); return 0; }
		if (wp == VK_TAB && self->inlineIsWidget_) {
			bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
			self->tabToAdjacentTextWidget(!shift);
			return 0;
		}
		if (ConsumeCtrlBackspaceWordDelete(hwnd, wp)) return 0;
		if (ConsumeCtrlSelectAll(hwnd, wp)) return 0;
	}
	if (msg == WM_CHAR && wp == 0x7F) return 0; // see ConsumeCtrlBackspaceWordDelete
	// Swallow the WM_CHAR('\t') TranslateMessage queues for the VK_TAB
	// handled above -- tabToAdjacentTextWidget() destroys this control and
	// immediately creates a new one, and Windows can reuse the same HWND
	// value for it; without this, that stray queued char can land in the
	// brand-new field as a literal typed tab.
	if (msg == WM_CHAR && wp == L'\t' && self->inlineIsWidget_) return 0;
	if (msg == WM_KILLFOCUS) {
		// Losing focus to the color popup shouldn't commit/close the edit.
		HWND next = reinterpret_cast<HWND>(wp);
		if (!(self->colorPopup_ && (next == self->colorPopup_ || GetParent(next) == self->colorPopup_)))
			self->commitInlineEdit(true);
	}
	return DefSubclassProc(hwnd, msg, wp, lp);
}

void CanvasView::pagePixelSize(int i, int& w, int& h) const
{
	PageSizePt s = doc_ ? doc_->pageSize(i) : PageSizePt{};
	float sc = effScale();
	w = std::max(1, static_cast<int>(std::lround(s.w * sc)));
	h = std::max(1, static_cast<int>(std::lround(s.h * sc)));
}

void CanvasView::relayout()
{
	layout_.clear();
	canvasW_ = canvasH_ = 0;
	if (!doc_ || !doc_->isOpen()) { updateScrollbars(); return; }

	int gap = Scale(kBasePageGap, dpi_);
	long long newKey = std::llround(effScale() * 1000.0f);
	if (newKey != cacheKey_) { invalidateCache(); cacheKey_ = newKey; }

	if (mode_ == Mode::Continuous) {
		int n = doc_->pageCount();
		int maxW = 0;
		std::vector<std::pair<int, int>> sizes(n);
		for (int i = 0; i < n; ++i) {
			int w, h; pagePixelSize(i, w, h);
			sizes[i] = { w, h };
			maxW = std::max(maxW, w);
		}
		canvasW_ = maxW;
		int y = gap;
		layout_.resize(n);
		for (int i = 0; i < n; ++i) {
			int w = sizes[i].first, h = sizes[i].second;
			layout_[i] = { (maxW - w) / 2, y, w, h };
			y += h + gap;
		}
		canvasH_ = y;
	} else {
		int w, h; pagePixelSize(currentPage_, w, h);
		canvasW_ = w; canvasH_ = h;
		layout_.push_back({ 0, 0, w, h });
	}
	clampScroll();
	updateScrollbars();
}

void CanvasView::applyFit()
{
	if (!doc_ || !doc_->isOpen() || fit_ == Fit::None) return;
	RECT rc; GetClientRect(hwnd_, &rc);
	int cw = rc.right - rc.left;
	int ch = rc.bottom - rc.top;
	int margin = Scale(kBasePageGap, dpi_) * 2;
	float dpiScale = dpi_ / 72.0f;

	// Reference page: in single mode the current page; in continuous the widest.
	float refWpt = 0, refHpt = 0;
	if (mode_ == Mode::SinglePage) {
		PageSizePt s = doc_->pageSize(currentPage_);
		refWpt = s.w; refHpt = s.h;
	} else {
		for (int i = 0; i < doc_->pageCount(); ++i) {
			PageSizePt s = doc_->pageSize(i);
			if (s.w > refWpt) { refWpt = s.w; refHpt = s.h; }
		}
	}
	if (refWpt <= 0 || refHpt <= 0) return;

	if (fit_ == Fit::Width) {
		zoom_ = (cw - margin) / (refWpt * dpiScale);
	} else { // Fit::Page
		float zw = (cw - margin) / (refWpt * dpiScale);
		float zh = (ch - margin) / (refHpt * dpiScale);
		zoom_ = std::min(zw, zh);
	}
	zoom_ = std::clamp(zoom_, kMinZoom, kMaxZoom);
}

void CanvasView::setZoom(float z)
{
	z = std::clamp(z, kMinZoom, kMaxZoom);
	if (std::abs(z - zoom_) < 1e-4f) return;
	// A direct scroll-position write below overrides any in-flight
	// smooth-scroll (see goToPage()/scrollToHit()) -- otherwise a still-
	// running animation timer (e.g. from a scroll or search jump just
	// before this zoom) can tick again afterward and overwrite scrollY_
	// with coordinates computed for the pre-zoom layout, jumping to an
	// unrelated page.
	if (scrollAnimY_ || scrollAnimX_) {
		scrollAnimY_ = scrollAnimX_ = false;
		KillTimer(hwnd_, kScrollAnimTimerId);
	}
	// preserve vertical center anchor
	RECT rc; GetClientRect(hwnd_, &rc);
	int ch = rc.bottom - rc.top;
	float anchor = canvasH_ > 0 ? (scrollY_ + ch * 0.5f) / canvasH_ : 0.0f;
	zoom_ = z;
	relayout();
	scrollY_ = static_cast<int>(anchor * canvasH_ - ch * 0.5f);
	clampScroll();
	updateScrollbars();
	invalidate();
	updateStatus();
}

// Like setZoom(), but preserves the content point under (anchorClientX,
// anchorClientY) instead of the viewport's vertical center -- used for
// Ctrl+wheel zoom so the page zooms from the mouse pointer (like every other
// viewer), not always from a fixed origin toward the top-left.
void CanvasView::setZoomAnchored(float z, int anchorClientX, int anchorClientY)
{
	z = std::clamp(z, kMinZoom, kMaxZoom);
	if (std::abs(z - zoom_) < 1e-4f) return;
	// See setZoom()'s comment -- a stale in-flight smooth-scroll must not be
	// left running across a zoom, or it can overwrite the freshly-computed
	// scroll position a moment later and jump to an unrelated page.
	if (scrollAnimY_ || scrollAnimX_) {
		scrollAnimY_ = scrollAnimX_ = false;
		KillTimer(hwnd_, kScrollAnimTimerId);
	}
	float fracX = canvasW_ > 0 ? (scrollX_ + anchorClientX) / static_cast<float>(canvasW_) : 0.0f;
	float fracY = canvasH_ > 0 ? (scrollY_ + anchorClientY) / static_cast<float>(canvasH_) : 0.0f;
	zoom_ = z;
	relayout();
	scrollX_ = static_cast<int>(fracX * canvasW_ - anchorClientX);
	scrollY_ = static_cast<int>(fracY * canvasH_ - anchorClientY);
	clampScroll();
	updateScrollbars();
	invalidate();
	updateStatus();
}

void CanvasView::clampScroll()
{
	RECT rc; GetClientRect(hwnd_, &rc);
	int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
	int maxX = std::max(0, canvasW_ - cw);
	int maxY = std::max(0, canvasH_ - ch);
	scrollX_ = std::clamp(scrollX_, 0, maxX);
	scrollY_ = std::clamp(scrollY_, 0, maxY);
}

int CanvasView::clampedScrollY(int y) const
{
	RECT rc; GetClientRect(hwnd_, &rc);
	int ch = rc.bottom - rc.top;
	return std::clamp(y, 0, std::max(0, canvasH_ - ch));
}

int CanvasView::clampedScrollX(int x) const
{
	RECT rc; GetClientRect(hwnd_, &rc);
	int cw = rc.right - rc.left;
	return std::clamp(x, 0, std::max(0, canvasW_ - cw));
}

// Animates scrollY_/scrollX_ toward a target over a short ease-out curve --
// used for keyboard-triggered scrolling (arrows, Page Up/Down) so it reads
// as a deliberate motion like Edge/Chrome, rather than an instant jump.
// Mouse wheel and scrollbar drag stay instant (they already track the input
// continuously, so animating them would just add lag).
void CanvasView::beginSmoothScrollY(int targetY)
{
	targetY = clampedScrollY(targetY);
	scrollAnimFromY_ = scrollY_; // restart from wherever it visually is now
	scrollAnimToY_ = targetY;
	scrollAnimStartY_ = GetTickCount();
	scrollAnimY_ = true;
	SetTimer(hwnd_, kScrollAnimTimerId, 10, nullptr);
}

void CanvasView::beginSmoothScrollX(int targetX)
{
	targetX = clampedScrollX(targetX);
	scrollAnimFromX_ = scrollX_;
	scrollAnimToX_ = targetX;
	scrollAnimStartX_ = GetTickCount();
	scrollAnimX_ = true;
	SetTimer(hwnd_, kScrollAnimTimerId, 10, nullptr);
}

void CanvasView::stepScrollAnim()
{
	constexpr DWORD kDurationMs = 180;
	DWORD now = GetTickCount();
	auto ease = [](float t) { return 1.0f - std::pow(1.0f - t, 3.0f); }; // cubic ease-out

	if (scrollAnimY_) {
		float t = std::min(1.0f, (now - scrollAnimStartY_) / static_cast<float>(kDurationMs));
		scrollY_ = scrollAnimFromY_ + static_cast<int>((scrollAnimToY_ - scrollAnimFromY_) * ease(t));
		if (t >= 1.0f) { scrollY_ = scrollAnimToY_; scrollAnimY_ = false; }
	}
	if (scrollAnimX_) {
		float t = std::min(1.0f, (now - scrollAnimStartX_) / static_cast<float>(kDurationMs));
		scrollX_ = scrollAnimFromX_ + static_cast<int>((scrollAnimToX_ - scrollAnimFromX_) * ease(t));
		if (t >= 1.0f) { scrollX_ = scrollAnimToX_; scrollAnimX_ = false; }
	}
	clampScroll();
	updateScrollbars();
	invalidate();
	if (inlineEdit_) positionInlineEdit();
	if (!scrollAnimY_ && !scrollAnimX_) {
		KillTimer(hwnd_, kScrollAnimTimerId);
		updateStatus(); // also updates currentPage_ from scrollY_ in Continuous mode
		if (mode_ == Mode::Continuous && thumbs_) thumbs_->setCurrent(currentPage_);
	}
}

void CanvasView::updateScrollbars()
{
	RECT rc; GetClientRect(hwnd_, &rc);
	int cw = rc.right - rc.left, ch = rc.bottom - rc.top;

	SCROLLINFO si = {}; si.cbSize = sizeof(si); si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	si.nMin = 0; si.nMax = std::max(0, canvasH_ - 1); si.nPage = ch; si.nPos = scrollY_;
	SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);

	si.nMax = std::max(0, canvasW_ - 1); si.nPage = cw; si.nPos = scrollX_;
	SetScrollInfo(hwnd_, SB_HORZ, &si, TRUE);
}

HBITMAP CanvasView::ensureRendered(int i, int& w, int& h)
{
	auto it = cache_.find(i);
	if (it != cache_.end() && it->second.hbmp) {
		w = it->second.width; h = it->second.height;
		return it->second.hbmp;
	}
	if (!doc_) return nullptr;
	PageBitmap bmp = doc_->renderPage(i, effScale());
	HBITMAP hb = bmp.hbmp;
	w = bmp.width; h = bmp.height;
	// Bound cache growth: clear if it gets large (visible pages re-render fast).
	if (cache_.size() > 24) cache_.clear();
	cache_[i] = std::move(bmp);
	return hb;
}

int CanvasView::firstVisiblePage() const
{
	if (mode_ == Mode::SinglePage) return currentPage_;
	for (size_t i = 0; i < layout_.size(); ++i) {
		if (layout_[i].y + layout_[i].h - scrollY_ > 0)
			return static_cast<int>(i);
	}
	return 0;
}

void CanvasView::onPaint()
{
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd_, &ps);
	RECT rc; GetClientRect(hwnd_, &rc);
	int cw = rc.right - rc.left, ch = rc.bottom - rc.top;

	// Double-buffer to a memory DC.
	HDC mem = CreateCompatibleDC(hdc);
	HBITMAP back = CreateCompatibleBitmap(hdc, cw, ch);
	HBITMAP oldBack = static_cast<HBITMAP>(SelectObject(mem, back));

	HBRUSH bg = CreateSolidBrush(Theme(dark_).canvasSurround); // matches Edge's PDF viewer surround
	FillRect(mem, &rc, bg);
	DeleteObject(bg);

	if (printPreviewActive_) {
		onPaintPrintPreview(mem, cw, ch);
	} else if (doc_ && doc_->isOpen()) {
		int offX = (canvasW_ <= cw) ? (cw - canvasW_) / 2 : -scrollX_;
		int offY = (canvasH_ <= ch) ? (ch - canvasH_) / 2 : -scrollY_;

		HDC src = CreateCompatibleDC(mem);
		// Soft drop shadow behind each page: nested rects growing outward from
		// the page edge with falling alpha, drawn before the page bitmap so
		// the bitmap's opaque blit covers the part that overlaps the page
		// itself -- only the halo outside the edge stays visible. Slight
		// down/right bias matches the elevation look of Edge/Chrome's viewer.
		auto drawPageShadow = [&](int px, int py, int pw, int ph) {
			int blur = Scale(6, dpi_);
			int offset = Scale(2, dpi_);
			for (int i = blur; i >= 1; --i) {
				BYTE alpha = static_cast<BYTE>(std::max(1, 10 * (blur - i + 1) / blur));
				RECT r = { px - i + offset, py - i + offset, px + pw + i + offset, py + ph + i + offset };
				FillAlpha(mem, r, RGB(0, 0, 0), alpha);
			}
		};
		auto blitPage = [&](int idx, int px, int py, int pw, int ph) {
			int w = 0, h = 0;
			HBITMAP hb = ensureRendered(idx, w, h);
			if (!hb) return;
			drawPageShadow(px, py, pw, ph);
			HBITMAP oldSrc = static_cast<HBITMAP>(SelectObject(src, hb));
			BitBlt(mem, px, py, w, h, src, 0, 0, SRCCOPY);
			SelectObject(src, oldSrc);
			drawFieldHighlights(mem, idx, px, py);
			drawHighlights(mem, idx, px, py);
			drawTextSelection(mem, idx, px, py);
		};

		if (mode_ == Mode::Continuous) {
			for (size_t i = 0; i < layout_.size(); ++i) {
				int py = layout_[i].y + offY;
				if (py + layout_[i].h < 0 || py > ch) continue; // offscreen
				blitPage(static_cast<int>(i), layout_[i].x + offX, py, layout_[i].w, layout_[i].h);
			}
		} else {
			blitPage(currentPage_, offX, offY, canvasW_, canvasH_);
		}
		DeleteDC(src);
	} else {
		drawEmptyState(mem, rc);
	}

	// Live overlay for the in-progress annotation drag.
	if (dragging_) {
		if (tool_ == Tool::Highlight || tool_ == Tool::Text || tool_ == Tool::Redact) {
			RECT r = { std::min(dragStartX_, dragCurX_), std::min(dragStartY_, dragCurY_),
				std::max(dragStartX_, dragCurX_), std::max(dragStartY_, dragCurY_) };
			if (tool_ == Tool::Highlight) FillAlpha(mem, r, highlightColor_, 60);
			else if (tool_ == Tool::Redact) FillAlpha(mem, r, RGB(0, 0, 0), 180);
			HPEN pen = CreatePen(PS_DOT, 1, RGB(40, 40, 40));
			HGDIOBJ oldPen = SelectObject(mem, pen);
			HGDIOBJ oldBr = SelectObject(mem, GetStockObject(NULL_BRUSH));
			Rectangle(mem, r.left, r.top, r.right, r.bottom);
			SelectObject(mem, oldPen); SelectObject(mem, oldBr);
			DeleteObject(pen);
		} else if (tool_ == Tool::Draw && curStroke_.size() > 1) {
			int wpen = std::max(1, static_cast<int>(std::lround(penWidth_ * dragScale_)));
			HPEN pen = CreatePen(PS_SOLID, wpen, drawColor_);
			HGDIOBJ oldPen = SelectObject(mem, pen);
			for (size_t i = 0; i < curStroke_.size(); ++i) {
				int sx = pageOrgX_ + static_cast<int>((curStroke_[i].x - dragBound_.x0) * dragScale_);
				int sy = pageOrgY_ + static_cast<int>((curStroke_[i].y - dragBound_.y0) * dragScale_);
				if (i == 0) MoveToEx(mem, sx, sy, nullptr);
				else LineTo(mem, sx, sy);
			}
			SelectObject(mem, oldPen);
			DeleteObject(pen);
		}
	}

	BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
	SelectObject(mem, oldBack);
	DeleteObject(back);
	DeleteDC(mem);
	EndPaint(hwnd_, &ps);
}

void CanvasView::onPaintPrintPreview(HDC mem, int cw, int ch)
{
	RECT rc = { 0, 0, cw, ch };
	if (!doc_ || printPreviewPages_.empty()) { drawEmptyState(mem, rc); return; }
	int idx = printPreviewPages_[std::clamp(printPreviewCursor_, 0, static_cast<int>(printPreviewPages_.size()) - 1)];
	HBITMAP sheet = RenderPrintPreviewPage(*doc_, idx, cw, ch, printPreviewGrayscale_, printPreviewLandscape_);
	if (!sheet) return;
	HDC src = CreateCompatibleDC(mem);
	HBITMAP old = static_cast<HBITMAP>(SelectObject(src, sheet));
	BitBlt(mem, 0, 0, cw, ch, src, 0, 0, SRCCOPY);
	SelectObject(src, old);
	DeleteDC(src);
	DeleteObject(sheet);
}

void CanvasView::beginPrintPreview(std::vector<int> pages, bool grayscale, bool landscape)
{
	if (inlineEdit_) commitInlineEdit(false);
	clearTextSelection();
	printPreviewActive_ = true;
	printPreviewPages_ = std::move(pages);
	printPreviewCursor_ = 0;
	printPreviewGrayscale_ = grayscale;
	printPreviewLandscape_ = landscape;
	invalidate();
}

void CanvasView::setPrintPreviewOptions(std::vector<int> pages, bool grayscale, bool landscape)
{
	if (!printPreviewActive_) return;
	printPreviewPages_ = std::move(pages);
	printPreviewCursor_ = std::clamp(printPreviewCursor_, 0, std::max(0, static_cast<int>(printPreviewPages_.size()) - 1));
	printPreviewGrayscale_ = grayscale;
	printPreviewLandscape_ = landscape;
	invalidate();
}

void CanvasView::printPreviewGoTo(int index)
{
	if (!printPreviewActive_ || printPreviewPages_.empty()) return;
	printPreviewCursor_ = std::clamp(index, 0, static_cast<int>(printPreviewPages_.size()) - 1);
	invalidate();
}

void CanvasView::endPrintPreview()
{
	if (!printPreviewActive_) return;
	printPreviewActive_ = false;
	printPreviewPages_.clear();
	printPreviewCursor_ = 0;
	invalidate();
}

void CanvasView::onSize()
{
	if (fit_ != Fit::None) applyFit();
	relayout();
	invalidate();
	if (inlineEdit_) positionInlineEdit();
}

void CanvasView::onVScroll(WPARAM wp)
{
	RECT rc; GetClientRect(hwnd_, &rc);
	int ch = rc.bottom - rc.top;
	int line = Scale(60, dpi_);
	switch (LOWORD(wp)) {
	// Line/page/top/bottom are keyboard- or scrollbar-click-triggered
	// discrete jumps -- animate them (see beginSmoothScrollY). Dragging the
	// thumb below stays instant so it tracks the mouse 1:1.
	case SB_LINEUP:   beginSmoothScrollY(scrollY_ - line); return;
	case SB_LINEDOWN: beginSmoothScrollY(scrollY_ + line); return;
	case SB_PAGEUP:   beginSmoothScrollY(scrollY_ - ch); return;
	case SB_PAGEDOWN: beginSmoothScrollY(scrollY_ + ch); return;
	case SB_TOP:      beginSmoothScrollY(0); return;
	case SB_BOTTOM:   beginSmoothScrollY(canvasH_); return;
	case SB_THUMBTRACK:
	case SB_THUMBPOSITION: {
		SCROLLINFO si = {}; si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS;
		GetScrollInfo(hwnd_, SB_VERT, &si);
		int old = scrollY_;
		scrollAnimY_ = false; // a drag interrupts any in-flight animation
		scrollY_ = si.nTrackPos;
		clampScroll();
		if (scrollY_ != old) {
			updateScrollbars(); invalidate(); updateStatus();
			if (inlineEdit_) positionInlineEdit();
		}
		break;
	}
	}
}

void CanvasView::onHScroll(WPARAM wp)
{
	RECT rc; GetClientRect(hwnd_, &rc);
	int cw = rc.right - rc.left;
	int line = Scale(40, dpi_);
	switch (LOWORD(wp)) {
	case SB_LINELEFT:  beginSmoothScrollX(scrollX_ - line); return;
	case SB_LINERIGHT: beginSmoothScrollX(scrollX_ + line); return;
	case SB_PAGELEFT:  beginSmoothScrollX(scrollX_ - cw); return;
	case SB_PAGERIGHT: beginSmoothScrollX(scrollX_ + cw); return;
	case SB_THUMBTRACK:
	case SB_THUMBPOSITION: {
		SCROLLINFO si = {}; si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS;
		GetScrollInfo(hwnd_, SB_HORZ, &si);
		int old = scrollX_;
		scrollAnimX_ = false;
		scrollX_ = si.nTrackPos;
		clampScroll();
		if (scrollX_ != old) {
			updateScrollbars(); invalidate();
			if (inlineEdit_) positionInlineEdit();
		}
		break;
	}
	}
}

void CanvasView::onWheel(short delta, bool ctrl, bool horiz, POINT screenPt)
{
	if (printPreviewActive_) {
		// The preview shows one full sheet at a time -- there's no scrollable
		// content underneath for the wheel to move, so treat it the same as
		// the panel's prev/next buttons instead of silently doing nothing.
		int steps = delta / WHEEL_DELTA;
		if (steps != 0) printPreviewGoTo(printPreviewCursor_ - steps); // already invalidates
		if (onViewChanged_) onViewChanged_();
		return;
	}
	if (ctrl) {
		POINT pt = screenPt;
		ScreenToClient(hwnd_, &pt);
		setZoomAnchored(delta > 0 ? zoom_ * kZoomStep : zoom_ / kZoomStep, pt.x, pt.y);
		fit_ = Fit::None;
		return;
	}
	if (mode_ == Mode::SinglePage && canvasH_ <= 0) return;
	// In single-page mode at page top/bottom, wheel flips pages.
	int steps = delta / WHEEL_DELTA;
	int amount = Scale(100, dpi_) * -steps; // ~100px/notch, matches typical browser wheel scroll

	// Ease toward a target instead of jumping scrollY_/scrollX_ straight to
	// the new value -- a bare instant add-and-invalidate per wheel notch is
	// what read as "jumping little distances" instead of a normal smooth
	// page-scroll. Accumulate onto the in-flight animation's target (not the
	// current mid-flight position) so spinning the wheel continuously keeps
	// gliding rather than restarting/decelerating on every notch.
	int base = horiz ? (scrollAnimX_ ? scrollAnimToX_ : scrollX_)
	                  : (scrollAnimY_ ? scrollAnimToY_ : scrollY_);
	int target = base + amount;

	if (mode_ == Mode::SinglePage && !horiz) {
		RECT rc; GetClientRect(hwnd_, &rc);
		int ch = rc.bottom - rc.top;
		int maxY = std::max(0, canvasH_ - ch);
		if (target < 0 && currentPage_ > 0) { goToPage(currentPage_ - 1); return; }
		if (target > maxY && currentPage_ < doc_->pageCount() - 1) { goToPage(currentPage_ + 1); return; }
	}
	if (horiz) beginSmoothScrollX(target); else beginSmoothScrollY(target);
	// beginSmoothScrollX/Y already call positionInlineEdit() indirectly via
	// stepScrollAnim() on every timer tick; nothing else to sync here --
	// currentPage_/thumbs_ tracking for Continuous mode now happens when the
	// animation settles (see stepScrollAnim()), same as arrow-key scrolling.
}

void CanvasView::updateStatus()
{
	if (!status_) return;
	// Dark mode draws this part itself (FrameWindow::drawStatusBarItem) --
	// status bars don't reliably support NM_CUSTOMDRAW, so SBT_OWNERDRAW is
	// the documented way to recolor its text. Light mode stays fully native.
	WPARAM part = dark_ ? SBT_OWNERDRAW : 0;
	// Hovering a link takes over the status bar, browser-style (bottom-left
	// URL preview) -- restored to the normal page/zoom text once it clears.
	if (!hoveredLinkText_.empty()) {
		statusText_ = Utf8ToWide(hoveredLinkText_);
		SendMessageW(status_, SB_SETTEXTW, part, reinterpret_cast<LPARAM>(statusText_.c_str()));
		return;
	}
	wchar_t buf[128];
	if (doc_ && doc_->isOpen()) {
		int cur = (mode_ == Mode::Continuous) ? firstVisiblePage() : currentPage_;
		currentPage_ = cur;
		swprintf(buf, 128, L"Page %d / %d      Zoom %d%%",
			cur + 1, doc_->pageCount(), static_cast<int>(std::lround(zoom_ * 100)));
	} else {
		swprintf(buf, 128, L"No document");
	}
	statusText_ = buf;
	SendMessageW(status_, SB_SETTEXTW, part, reinterpret_cast<LPARAM>(statusText_.c_str()));
	if (onViewChanged_) onViewChanged_();
}

LRESULT CALLBACK CanvasView::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	CanvasView* self = reinterpret_cast<CanvasView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (msg == WM_NCCREATE) {
		auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
		self = static_cast<CanvasView*>(cs->lpCreateParams);
		self->hwnd_ = hwnd;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}
	if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

	switch (msg) {
	case WM_PAINT: self->onPaint(); return 0;
	case WM_ERASEBKGND: return 1;
	case WM_SIZE: self->onSize(); return 0;
	case WM_VSCROLL: self->onVScroll(wp); return 0;
	case WM_HSCROLL: self->onHScroll(wp); return 0;
	case WM_MOUSELEAVE:
		self->trackingMouseLeave_ = false;
		if (!self->hoveredLinkText_.empty()) { self->hoveredLinkText_.clear(); self->updateStatus(); }
		return 0;
	case WM_TIMER:
		if (wp == CanvasView::kScrollAnimTimerId) { self->stepScrollAnim(); return 0; }
		break;
	case WM_MOUSEWHEEL:
		// WM_(H)MOUSEWHEEL's lParam is in SCREEN coordinates (unlike almost
		// every other mouse message), needed so Ctrl+wheel zoom can anchor on
		// the actual pointer position instead of the viewport center.
		self->onWheel(GET_WHEEL_DELTA_WPARAM(wp), (LOWORD(wp) & MK_CONTROL) != 0, false,
			POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
		return 0;
	case WM_MOUSEHWHEEL:
		self->onWheel(GET_WHEEL_DELTA_WPARAM(wp), false, true,
			POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
		return 0;
	case WM_LBUTTONDOWN: self->onLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
	case WM_RBUTTONDOWN: self->onRButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
	case WM_MOUSEMOVE: self->onMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
	case WM_LBUTTONUP: self->onLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
	case WM_SETCURSOR:
		if (LOWORD(lp) == HTCLIENT) {
			if (self->tool_ != Tool::Select) {
				SetCursor(LoadCursor(nullptr, IDC_CROSS));
				return TRUE;
			}
			if (self->doc_ && self->doc_->isOpen() && !self->selDragging_ && self->activeResizeDir_ < 0) {
				POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
				HCURSOR c = self->cursorForSelectAt(pt.x, pt.y);
				if (c) { SetCursor(c); return TRUE; }
			} else if (!self->doc_ || !self->doc_->isOpen()) {
				POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
				int cmdId;
				if (self->hitTestEmptyState(pt.x, pt.y, cmdId)) {
					SetCursor(LoadCursor(nullptr, IDC_HAND));
					return TRUE;
				}
			}
		}
		break;
	case WM_GETDLGCODE: return DLGC_WANTARROWS;
	case WM_KEYDOWN:
		// WM_GETDLGCODE/DLGC_WANTARROWS only matters under IsDialogMessage,
		// which the main message loop doesn't use -- arrow keys otherwise
		// reach here and do nothing by default, so scroll explicitly.
		// Page Up/Down scroll by a viewport height (like Edge/Chrome), not
		// jump to the prev/next page -- IDM_GO_PREV/NEXT's VK_PRIOR/VK_NEXT
		// accelerators were removed from app.rc so these reach here instead.
		switch (wp) {
		case VK_UP:    self->onVScroll(SB_LINEUP); return 0;
		case VK_DOWN:  self->onVScroll(SB_LINEDOWN); return 0;
		case VK_LEFT:  self->onHScroll(SB_LINELEFT); return 0;
		case VK_RIGHT: self->onHScroll(SB_LINERIGHT); return 0;
		case VK_PRIOR: self->onVScroll(SB_PAGEUP); return 0;
		case VK_NEXT:  self->onVScroll(SB_PAGEDOWN); return 0;
		case VK_HOME:  self->onVScroll(SB_TOP); return 0;
		case VK_END:   self->onVScroll(SB_BOTTOM); return 0;
		}
		break;
	case WM_CTLCOLOREDIT:
		if (self->inlineEdit_ && reinterpret_cast<HWND>(lp) == self->inlineEdit_ && self->inlineShowPopup_) {
			// Live preview: the free-text box's typed color follows the swatch
			// picker immediately, same as the rendered annotation will use.
			// Background is a pattern brush of the actual page pixels behind
			// the box (see makeInlineBgBrush) so it looks transparent, same
			// as the committed annotation, instead of painting a solid fill.
			HDC dc = reinterpret_cast<HDC>(wp);
			SetTextColor(dc, self->textColor_);
			SetBkMode(dc, TRANSPARENT);
			if (self->inlineBgBrush_) return reinterpret_cast<LRESULT>(self->inlineBgBrush_);
			static HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
			return reinterpret_cast<LRESULT>(white);
		}
		break;
	case WM_DPICHANGED_BEFOREPARENT:
		self->dpi_ = GetDpiForWindow(hwnd);
		if (self->fit_ != Fit::None) self->applyFit();
		self->relayout(); self->invalidate();
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
// ThumbPanel implementation
// ===========================================================================
void ThumbPanel::Register(HINSTANCE hInst)
{
	WNDCLASSW wc = {};
	wc.lpfnWndProc = ThumbPanel::Proc;
	wc.hInstance = hInst;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = nullptr;
	wc.lpszClassName = kThumbClass;
	wc.style = CS_VREDRAW;
	RegisterClassW(&wc);
}

ThumbPanel::ThumbPanel(HWND parent, HINSTANCE hInst)
{
	hwnd_ = CreateWindowExW(0, kThumbClass, L"",
		WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER,
		0, 0, 100, 100, parent, nullptr, hInst, this);
	dpi_ = GetDpiForWindow(hwnd_);
	thumbW_ = Scale(kBaseThumbWidth, dpi_);
}

void ThumbPanel::setDocument(PdfDocument* doc)
{
	doc_ = doc;
	current_ = 0;
	scrollY_ = 0;
	cache_.clear();
	relayout();
	InvalidateRect(hwnd_, nullptr, TRUE);
}

void ThumbPanel::setCurrent(int page)
{
	if (page == current_) return;
	current_ = page;
	// scroll to keep current thumbnail visible
	if (page >= 0 && page < static_cast<int>(slots_.size())) {
		RECT rc; GetClientRect(hwnd_, &rc);
		int ch = rc.bottom - rc.top;
		int top = slots_[page].y, bot = top + slots_[page].h;
		if (top - scrollY_ < 0) scrollY_ = top;
		else if (bot - scrollY_ > ch) scrollY_ = bot - ch;
		clampScroll(); updateScrollbar();
	}
	InvalidateRect(hwnd_, nullptr, FALSE);
}

void ThumbPanel::relayout()
{
	slots_.clear();
	contentH_ = 0;
	// Organize mode can be active on a document that's never been formally
	// "opened" (e.g. Merge into a blank tab: doc_ only holds externalFiles_
	// until rebuildFromPages() commits at Done) -- don't gate on isOpen()
	// in that case, just on having a PdfDocument to pull pages from at all.
	if (!doc_ || (!organizeMode_ && !doc_->isOpen())) { updateScrollbar(); return; }
	int pad = Scale(8, dpi_);
	// Organize mode swaps the plain page-number label for a row of rotate/
	// delete hit-zones (see organizeIconRects()), so it needs more height.
	int rowH = organizeMode_ ? Scale(26, dpi_) : Scale(16, dpi_);
	int innerW = thumbW_ - 2 * pad;
	int y = pad;
	int n = organizeMode_ ? static_cast<int>(order_.size()) : doc_->pageCount();
	slots_.resize(n);
	for (int i = 0; i < n; ++i) {
		PageSizePt s = organizeMode_ ? orderedPageSize(i) : doc_->pageSize(i);
		int th = (s.w > 0) ? static_cast<int>(std::lround(innerW * (s.h / s.w))) : innerW;
		th = std::clamp(th, Scale(20, dpi_), Scale(400, dpi_));
		slots_[i] = { y, th };
		y += th + rowH + pad;
	}
	contentH_ = y;
	clampScroll();
	updateScrollbar();
}

PageSizePt ThumbPanel::orderedPageSize(int i) const
{
	if (!doc_ || i < 0 || i >= static_cast<int>(order_.size())) return {};
	const auto& e = order_[i];
	return e.externalFileIndex < 0 ? doc_->pageSize(e.pageIndex)
		: doc_->externalPageSize(e.externalFileIndex, e.pageIndex);
}

void ThumbPanel::clampScroll()
{
	RECT rc; GetClientRect(hwnd_, &rc);
	int ch = rc.bottom - rc.top;
	scrollY_ = std::clamp(scrollY_, 0, std::max(0, contentH_ - ch));
}

void ThumbPanel::updateScrollbar()
{
	RECT rc; GetClientRect(hwnd_, &rc);
	int ch = rc.bottom - rc.top;
	SCROLLINFO si = {}; si.cbSize = sizeof(si); si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	si.nMin = 0; si.nMax = std::max(0, contentH_ - 1); si.nPage = ch; si.nPos = scrollY_;
	SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

HBITMAP ThumbPanel::ensureThumb(int i, int& w, int& h)
{
	auto it = cache_.find(i);
	if (it != cache_.end() && it->second.hbmp) {
		w = it->second.width; h = it->second.height; return it->second.hbmp;
	}
	if (!doc_) return nullptr;
	int pad = Scale(8, dpi_);
	int innerW = thumbW_ - 2 * pad;
	PageBitmap bmp;
	if (organizeMode_) {
		if (i < 0 || i >= static_cast<int>(order_.size())) return nullptr;
		const auto& e = order_[i];
		PageSizePt s = orderedPageSize(i);
		float scale = (s.w > 0) ? (innerW / s.w) : 0.2f;
		bmp = e.externalFileIndex < 0 ? doc_->renderPage(e.pageIndex, scale)
			: doc_->renderExternalPage(e.externalFileIndex, e.pageIndex, scale);
	} else {
		PageSizePt s = doc_->pageSize(i);
		float scale = (s.w > 0) ? (innerW / s.w) : 0.2f; // device px per point
		bmp = doc_->renderPage(i, scale);
	}
	HBITMAP hb = bmp.hbmp; w = bmp.width; h = bmp.height;
	if (cache_.size() > 40) cache_.clear();
	cache_[i] = std::move(bmp);
	return hb;
}

void ThumbPanel::onPaint()
{
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd_, &ps);
	RECT rc; GetClientRect(hwnd_, &rc);
	int cw = rc.right - rc.left, ch = rc.bottom - rc.top;

	HDC mem = CreateCompatibleDC(hdc);
	HBITMAP back = CreateCompatibleBitmap(hdc, cw, ch);
	HBITMAP oldBack = static_cast<HBITMAP>(SelectObject(mem, back));
	const ThemeColors& th = Theme(dark_);
	HBRUSH bg = CreateSolidBrush(th.thumbBg);
	FillRect(mem, &rc, bg); DeleteObject(bg);

	// Same isOpen() exemption as relayout(): organize mode can be active on
	// a document that's never been formally opened (Merge into a blank tab).
	if (doc_ && (organizeMode_ || doc_->isOpen())) {
		int rowH = organizeMode_ ? Scale(26, dpi_) : Scale(16, dpi_);
		SetBkMode(mem, TRANSPARENT);
		// Page-number labels / organize glyphs were drawing in the DC's default
		// (ancient SYSTEM_FONT) face, which looked dated. Render them in Segoe
		// UI (ClearType) for a modern look, matching the rest of the chrome.
		HFONT thumbFont = CreateFontW(-Scale(11, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
		// Slightly larger face just for the organize rotate/delete glyphs.
		HFONT glyphFont = CreateFontW(-Scale(15, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
			DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
		HGDIOBJ oldFont = SelectObject(mem, thumbFont);
		HDC src = CreateCompatibleDC(mem);
		for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
			int top = slots_[i].y - scrollY_;
			if (top + slots_[i].h + rowH < 0 || top > ch) continue;
			int w = 0, h = 0;
			HBITMAP hb = ensureThumb(i, w, h);
			int tx = (cw - w) / 2;
			if (hb) {
				// selection highlight (not shown while organizing -- the
				// row-swap-as-you-drag already gives positional feedback).
				if (!organizeMode_ && i == current_) {
					RECT hl = { tx - 3, top - 3, tx + w + 3, top + h + 3 };
					HBRUSH sel = CreateSolidBrush(th.thumbSelFill);
					FillRect(mem, &hl, sel); DeleteObject(sel);
					HPEN pen = CreatePen(PS_SOLID, 1, th.thumbSelBorder);
					HGDIOBJ oldPen = SelectObject(mem, pen);
					HGDIOBJ oldBr = SelectObject(mem, GetStockObject(NULL_BRUSH));
					Rectangle(mem, hl.left, hl.top, hl.right, hl.bottom);
					SelectObject(mem, oldPen); SelectObject(mem, oldBr);
					DeleteObject(pen);
				}
				HBITMAP oldSrc = static_cast<HBITMAP>(SelectObject(src, hb));
				BitBlt(mem, tx, top, w, h, src, 0, 0, SRCCOPY);
				SelectObject(src, oldSrc);
			}
			if (organizeMode_) {
				// Pending rotation is shown as a small corner badge rather
				// than actually rotating the rendered thumbnail bitmap --
				// applyRedactions()-style live re-render per drag step would
				// be needlessly expensive for a preview; the real rotation
				// still lands correctly at Apply time (see PagePlanEntry).
				int rot = (i < static_cast<int>(order_.size())) ? order_[i].extraRotation : 0;
				if (rot != 0) {
					wchar_t badge[8]; swprintf(badge, 8, L"%d°", rot);
					RECT br = { tx + w - Scale(28, dpi_), top + Scale(2, dpi_), tx + w - Scale(2, dpi_), top + Scale(18, dpi_) };
					HBRUSH bb = CreateSolidBrush(RGB(40, 40, 40));
					FillRect(mem, &br, bb); DeleteObject(bb);
					SetTextColor(mem, RGB(255, 255, 255));
					DrawTextW(mem, badge, -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				}
				OrganizeIconRects ir = organizeIconRects(i);
				// The rotate/delete glyphs read too faint at the number's size;
				// draw them a couple points larger with the glyph font, then
				// restore the number font.
				HGDIOBJ prevF = SelectObject(mem, glyphFont);
				SetTextColor(mem, th.thumbText);
				DrawTextW(mem, L"↺", -1, &ir.ccw, DT_CENTER | DT_VCENTER | DT_SINGLELINE); // rotate CCW
				DrawTextW(mem, L"↻", -1, &ir.cw, DT_CENTER | DT_VCENTER | DT_SINGLELINE);  // rotate CW
				SetTextColor(mem, RGB(180, 40, 40));
				DrawTextW(mem, L"✕", -1, &ir.del, DT_CENTER | DT_VCENTER | DT_SINGLELINE); // delete
				SelectObject(mem, prevF);
				wchar_t num[16]; swprintf(num, 16, L"%d", i + 1);
				RECT nr = { ir.ccw.right, ir.ccw.top, ir.cw.left, ir.ccw.bottom };
				SetTextColor(mem, th.thumbText);
				DrawTextW(mem, num, -1, &nr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			} else {
				wchar_t num[16]; swprintf(num, 16, L"%d", i + 1);
				RECT lr = { 0, top + h, cw, top + h + rowH };
				SetTextColor(mem, i == current_ ? th.thumbCurrentText : th.thumbText);
				DrawTextW(mem, num, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			}
		}
		DeleteDC(src);
		SelectObject(mem, oldFont);
		DeleteObject(thumbFont);
		DeleteObject(glyphFont);
	}

	BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
	SelectObject(mem, oldBack);
	DeleteObject(back); DeleteDC(mem);
	EndPaint(hwnd_, &ps);
}

void ThumbPanel::onSize() { clampScroll(); updateScrollbar(); InvalidateRect(hwnd_, nullptr, FALSE); }

void ThumbPanel::onVScroll(WPARAM wp)
{
	RECT rc; GetClientRect(hwnd_, &rc);
	int ch = rc.bottom - rc.top;
	int old = scrollY_;
	switch (LOWORD(wp)) {
	case SB_LINEUP: scrollY_ -= Scale(40, dpi_); break;
	case SB_LINEDOWN: scrollY_ += Scale(40, dpi_); break;
	case SB_PAGEUP: scrollY_ -= ch; break;
	case SB_PAGEDOWN: scrollY_ += ch; break;
	case SB_THUMBTRACK: case SB_THUMBPOSITION: {
		SCROLLINFO si = {}; si.cbSize = sizeof(si); si.fMask = SIF_TRACKPOS;
		GetScrollInfo(hwnd_, SB_VERT, &si); scrollY_ = si.nTrackPos; break;
	}
	}
	clampScroll();
	if (scrollY_ != old) { updateScrollbar(); InvalidateRect(hwnd_, nullptr, FALSE); }
}

void ThumbPanel::onWheel(short delta)
{
	scrollY_ -= (delta / WHEEL_DELTA) * Scale(40, dpi_) * 3;
	clampScroll(); updateScrollbar(); InvalidateRect(hwnd_, nullptr, FALSE);
}

void ThumbPanel::onClick(int y)
{
	int docY = y + scrollY_;
	for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
		if (docY >= slots_[i].y && docY < slots_[i].y + slots_[i].h + Scale(16, dpi_)) {
			current_ = i;
			InvalidateRect(hwnd_, nullptr, FALSE);
			if (canvas_) canvas_->goToPage(i);
			return;
		}
	}
}

ThumbPanel::OrganizeIconRects ThumbPanel::organizeIconRects(int i) const
{
	RECT rc; GetClientRect(hwnd_, &rc);
	int cwid = rc.right - rc.left;
	int top = slots_[i].y - scrollY_ + slots_[i].h + Scale(2, dpi_);
	int iconH = Scale(20, dpi_);
	int iconW = Scale(24, dpi_);
	int delW = Scale(22, dpi_);
	int numW = Scale(30, dpi_);
	int pad = Scale(4, dpi_);
	OrganizeIconRects r;
	int x = pad;
	r.ccw = { x, top, x + iconW, top + iconH }; x += iconW + numW;
	r.cw = { x, top, x + iconW, top + iconH };
	r.del = { cwid - pad - delW, top, cwid - pad, top + iconH };
	return r;
}

int ThumbPanel::organizeSlotAt(int y) const
{
	int docY = y + scrollY_;
	int rowH = Scale(26, dpi_);
	for (int i = 0; i < static_cast<int>(slots_.size()); ++i)
		if (docY >= slots_[i].y && docY < slots_[i].y + slots_[i].h + rowH) return i;
	return -1;
}

void ThumbPanel::onOrganizeLButtonDown(int x, int y)
{
	int i = organizeSlotAt(y);
	if (i < 0) return;
	OrganizeIconRects ir = organizeIconRects(i);
	POINT pt{ x, y };
	if (PtInRect(&ir.del, pt)) {
		order_.erase(order_.begin() + i);
		cache_.clear();
		relayout();
		InvalidateRect(hwnd_, nullptr, FALSE);
		return;
	}
	if (PtInRect(&ir.ccw, pt)) {
		order_[i].extraRotation = ((order_[i].extraRotation - 90) % 360 + 360) % 360;
		InvalidateRect(hwnd_, nullptr, FALSE);
		return;
	}
	if (PtInRect(&ir.cw, pt)) {
		order_[i].extraRotation = (order_[i].extraRotation + 90) % 360;
		InvalidateRect(hwnd_, nullptr, FALSE);
		return;
	}
	organizeDragArmed_ = true;
	organizeDragIndex_ = i;
	SetCapture(hwnd_);
}

void ThumbPanel::onOrganizeMouseMove(int x, int y)
{
	(void)x;
	if (!organizeDragArmed_) return;
	int i = organizeSlotAt(y);
	if (i < 0 || i == organizeDragIndex_) return;
	auto item = order_[organizeDragIndex_];
	order_.erase(order_.begin() + organizeDragIndex_);
	order_.insert(order_.begin() + i, item);
	organizeDragIndex_ = i;
	cache_.clear();
	relayout();
	InvalidateRect(hwnd_, nullptr, FALSE);
}

void ThumbPanel::onOrganizeLButtonUp()
{
	if (!organizeDragArmed_) return;
	organizeDragArmed_ = false;
	organizeDragIndex_ = -1;
	ReleaseCapture();
}

void ThumbPanel::enterOrganizeMode(std::vector<PdfDocument::PagePlanEntry> seed)
{
	organizeMode_ = true;
	order_ = std::move(seed);
	organizeDragArmed_ = false;
	organizeDragIndex_ = -1;
	cache_.clear();
	scrollY_ = 0;
	relayout();
	InvalidateRect(hwnd_, nullptr, TRUE);
}

void ThumbPanel::exitOrganizeMode()
{
	organizeMode_ = false;
	order_.clear();
	organizeDragArmed_ = false;
	organizeDragIndex_ = -1;
	cache_.clear();
	relayout();
	InvalidateRect(hwnd_, nullptr, TRUE);
}

void ThumbPanel::organizeInsertFile(int fileIndex)
{
	if (!doc_) return;
	int n = doc_->externalPageCount(fileIndex);
	for (int p = 0; p < n; ++p) order_.push_back({ fileIndex, p, 0 });
	cache_.clear();
	relayout();
	InvalidateRect(hwnd_, nullptr, TRUE);
}

LRESULT CALLBACK ThumbPanel::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	ThumbPanel* self = reinterpret_cast<ThumbPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (msg == WM_NCCREATE) {
		auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
		self = static_cast<ThumbPanel*>(cs->lpCreateParams);
		self->hwnd_ = hwnd;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}
	if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
	switch (msg) {
	case WM_PAINT: self->onPaint(); return 0;
	case WM_ERASEBKGND: return 1;
	case WM_SIZE: self->onSize(); return 0;
	case WM_VSCROLL: self->onVScroll(wp); return 0;
	case WM_MOUSEWHEEL: self->onWheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
	case WM_LBUTTONDOWN:
		if (self->organizeMode_) self->onOrganizeLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		else self->onClick(GET_Y_LPARAM(lp));
		return 0;
	case WM_MOUSEMOVE:
		if (self->organizeMode_) self->onOrganizeMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		return 0;
	case WM_LBUTTONUP:
	case WM_CAPTURECHANGED:
		if (self->organizeMode_) self->onOrganizeLButtonUp();
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
// Toolbar icons -- small hand-drawn vector glyphs (GDI+), monochrome to
// match a flat, modern (Edge-like) toolbar. Drawing our own avoids any
// dependency on a specific icon font's glyph set/codepoints being present.
// ===========================================================================
namespace icons {

// Crisper Fluent neutral (was 0xFF505050, which read washed-out). Dark enough
// to feel modern/legible, not a harsh pure black. Mutable (not constexpr) so
// SetInkColor() can retint every icon -- both the on-demand Gdiplus draws
// (empty-state grid, organize-mode glyphs) and, after a rebuild, the
// pre-rasterized toolbar ImageList -- when the app theme toggles.
Gdiplus::ARGB kInk = 0xFF424242;
void SetInkColor(Gdiplus::ARGB ink) { kInk = ink; }
// Single shared stroke weight so every glyph reads at a consistent thickness
// (~1.25px at the 20px icon size -- a lighter, more modern Fluent line; the
// earlier 0.08 read too heavy). Icons that intentionally deviate (Width's
// graded lines, Opacity's pie) still pass their own value.
constexpr float kStroke = 0.0625f;

// Normalized-coordinate drawing helper: every icon function works in a
// [0,1] x [0,1] unit square regardless of final pixel size, which keeps the
// per-icon code short and each icon trivially resizable.
struct IconPen {
	Gdiplus::Graphics& g;
	float s;
	Gdiplus::Pen pen;
	explicit IconPen(Gdiplus::Graphics& g_, float s_, float widthFrac = kStroke)
		: g(g_), s(s_), pen(Gdiplus::Color(kInk), s_ * widthFrac)
	{
		pen.SetLineJoin(Gdiplus::LineJoinRound);
		pen.SetStartCap(Gdiplus::LineCapRound);
		pen.SetEndCap(Gdiplus::LineCapRound);
	}
	void line(float x0, float y0, float x1, float y1) { g.DrawLine(&pen, x0 * s, y0 * s, x1 * s, y1 * s); }
	void rect(float x, float y, float w, float h) { g.DrawRectangle(&pen, x * s, y * s, w * s, h * s); }
	void ellipse(float x, float y, float w, float h) { g.DrawEllipse(&pen, x * s, y * s, w * s, h * s); }
	void polyline(std::initializer_list<Gdiplus::PointF> pts)
	{
		std::vector<Gdiplus::PointF> p;
		p.reserve(pts.size());
		for (auto& pt : pts) p.push_back(Gdiplus::PointF(pt.X * s, pt.Y * s));
		g.DrawLines(&pen, p.data(), static_cast<int>(p.size()));
	}
};

using DrawFn = std::function<void(Gdiplus::Graphics&, float)>;

// Renders one icon into a 32bpp premultiplied-alpha HBITMAP of size px*px
// for an ILC_COLOR32 image list.
HBITMAP MakeIcon(int px, const DrawFn& draw)
{
	Gdiplus::Bitmap bmp(px, px, PixelFormat32bppPARGB);
	{
		Gdiplus::Graphics g(&bmp);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.Clear(Gdiplus::Color(0, 0, 0, 0));
		draw(g, static_cast<float>(px));
	}
	HBITMAP hbmp = nullptr;
	bmp.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hbmp);
	return hbmp;
}

void DrawOpen(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.polyline({ {0.15f,0.30f}, {0.15f,0.78f}, {0.85f,0.78f}, {0.85f,0.42f},
		{0.50f,0.42f}, {0.42f,0.30f}, {0.15f,0.30f} });
}

void DrawSave(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.rect(0.18f, 0.16f, 0.64f, 0.68f);
	p.rect(0.32f, 0.18f, 0.24f, 0.16f);
	p.rect(0.26f, 0.52f, 0.48f, 0.26f);
}

void DrawSaveAs(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.rect(0.12f, 0.14f, 0.56f, 0.58f);
	p.rect(0.22f, 0.16f, 0.20f, 0.14f);
	p.rect(0.18f, 0.44f, 0.40f, 0.20f);
	IconPen p2(g, s, 0.10f);
	p2.line(0.74f, 0.60f, 0.74f, 0.86f);
	p2.line(0.61f, 0.73f, 0.87f, 0.73f);
}

void DrawPrint(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.rect(0.22f, 0.38f, 0.56f, 0.32f);
	p.rect(0.30f, 0.14f, 0.40f, 0.26f);
	p.rect(0.30f, 0.62f, 0.40f, 0.24f);
}

void DrawFind(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.ellipse(0.16f, 0.16f, 0.44f, 0.44f);
	p.line(0.56f, 0.56f, 0.84f, 0.84f);
}

// Plain minus/plus glyphs (not a magnifying glass) -- matches Edge's PDF
// toolbar exactly, which uses bare +/- for zoom.
void DrawZoomOut(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.line(0.20f, 0.5f, 0.80f, 0.5f);
}

void DrawZoomIn(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.line(0.20f, 0.5f, 0.80f, 0.5f);
	p.line(0.5f, 0.20f, 0.5f, 0.80f);
}

void DrawFitWidth(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.rect(0.32f, 0.12f, 0.36f, 0.76f);
	p.line(0.06f, 0.5f, 0.24f, 0.5f);
	p.polyline({ {0.14f,0.40f}, {0.06f,0.50f}, {0.14f,0.60f} });
	p.line(0.76f, 0.5f, 0.94f, 0.5f);
	p.polyline({ {0.86f,0.40f}, {0.94f,0.50f}, {0.86f,0.60f} });
}

void DrawFitPage(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.rect(0.28f, 0.22f, 0.44f, 0.56f);
	p.line(0.5f, 0.02f, 0.5f, 0.18f);
	p.polyline({ {0.40f,0.10f}, {0.50f,0.02f}, {0.60f,0.10f} });
	p.line(0.5f, 0.82f, 0.5f, 0.98f);
	p.polyline({ {0.40f,0.90f}, {0.50f,0.98f}, {0.60f,0.90f} });
}

void DrawContinuous(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s, 0.08f);
	p.rect(0.22f, 0.08f, 0.56f, 0.22f);
	p.rect(0.22f, 0.39f, 0.56f, 0.22f);
	p.rect(0.22f, 0.70f, 0.56f, 0.22f);
}

void DrawSinglePage(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.rect(0.28f, 0.12f, 0.44f, 0.76f);
}

void DrawThumbs(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s, 0.08f);
	p.rect(0.12f, 0.16f, 0.76f, 0.68f);
	p.line(0.38f, 0.16f, 0.38f, 0.84f);
	p.rect(0.18f, 0.24f, 0.14f, 0.16f);
	p.rect(0.18f, 0.48f, 0.14f, 0.16f);
}

void DrawSelectTool(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.polyline({ {0.20f,0.14f}, {0.20f,0.80f}, {0.38f,0.63f}, {0.50f,0.86f},
		{0.60f,0.81f}, {0.47f,0.58f}, {0.70f,0.55f}, {0.20f,0.14f} });
}

void DrawHighlightTool(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.polyline({ {0.20f,0.68f}, {0.54f,0.28f}, {0.76f,0.48f}, {0.42f,0.86f}, {0.20f,0.68f} });
	p.line(0.14f, 0.90f, 0.42f, 0.90f);
}

void DrawDrawTool(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.line(0.20f, 0.80f, 0.60f, 0.40f);
	p.polyline({ {0.60f,0.40f}, {0.76f,0.20f}, {0.84f,0.28f}, {0.68f,0.48f}, {0.60f,0.40f} });
	p.line(0.16f, 0.84f, 0.24f, 0.84f);
}

void DrawEraseTool(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	// A tilted eraser block outline with a diagonal seam band cutting across
	// the interior (the classic two-tone eraser split) -- reads as "eraser"
	// at toolbar size without needing a second color. The seam's endpoints
	// are deliberately off the outline's own edges (not just a corner clip)
	// so it renders as a distinct interior line instead of silently
	// overlapping an outline segment.
	p.polyline({ {0.22f,0.62f}, {0.52f,0.22f}, {0.86f,0.46f}, {0.56f,0.86f}, {0.22f,0.62f} });
	p.line(0.38f, 0.48f, 0.68f, 0.74f);
}

void DrawTextTool(Gdiplus::Graphics& g, float s)
{
	Gdiplus::FontFamily fam(L"Segoe UI");
	Gdiplus::Font font(&fam, s * 0.56f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
	Gdiplus::SolidBrush br{ Gdiplus::Color(kInk) };
	Gdiplus::StringFormat fmt;
	fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
	fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
	Gdiplus::RectF rc(0, 0, s, s);
	g.DrawString(L"A", 1, &font, rc, &fmt, &br);
}

// A restrained outline ring + single small accent dot -- not the loud
// three-color pie this used to be. Edge's own icon set is fully monochrome;
// this keeps just enough color to still read as "the color picker" at a
// glance, without breaking the flat monochrome look everywhere else.
void DrawColorTool(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.ellipse(0.16f, 0.16f, 0.60f, 0.60f);
	Gdiplus::SolidBrush dot(Gdiplus::Color(255, 205, 60, 55));
	g.FillEllipse(&dot, s * 0.32f, s * 0.32f, s * 0.28f, s * 0.28f);
}

void DrawWidthTool(Gdiplus::Graphics& g, float s)
{
	// Widths were too close together at 18px (rounded to the same visible
	// thickness); spread them out more and floor at 1px so the thinnest
	// line stays visible instead of anti-aliasing away to nothing.
	Gdiplus::Pen p1(Gdiplus::Color(kInk), std::max(1.0f, s * 0.04f));
	Gdiplus::Pen p2(Gdiplus::Color(kInk), std::max(1.0f, s * 0.12f));
	Gdiplus::Pen p3(Gdiplus::Color(kInk), std::max(1.0f, s * 0.24f));
	p1.SetStartCap(Gdiplus::LineCapRound); p1.SetEndCap(Gdiplus::LineCapRound);
	p2.SetStartCap(Gdiplus::LineCapRound); p2.SetEndCap(Gdiplus::LineCapRound);
	p3.SetStartCap(Gdiplus::LineCapRound); p3.SetEndCap(Gdiplus::LineCapRound);
	g.DrawLine(&p1, s * 0.20f, s * 0.24f, s * 0.80f, s * 0.24f);
	g.DrawLine(&p2, s * 0.20f, s * 0.52f, s * 0.80f, s * 0.52f);
	g.DrawLine(&p3, s * 0.20f, s * 0.82f, s * 0.80f, s * 0.82f);
}

// Half-filled circle -- a simpler, lighter-weight "transparency" glyph than
// the old checkerboard, matching the thinner monochrome style of the rest.
void DrawOpacityTool(Gdiplus::Graphics& g, float s)
{
	Gdiplus::RectF rc(s * 0.18f, s * 0.18f, s * 0.54f, s * 0.54f);
	Gdiplus::SolidBrush fill(Gdiplus::Color(200, 0x42, 0x42, 0x42));
	g.FillPie(&fill, rc, 90, 180);
	Gdiplus::Pen pen(Gdiplus::Color(kInk), s * kStroke);
	g.DrawEllipse(&pen, rc);
}

// Two thin "text" lines with a solid black bar over the middle one --
// reads as "blackout/redact" at a glance.
void DrawRedactTool(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.line(0.16f, 0.24f, 0.84f, 0.24f);
	p.line(0.16f, 0.76f, 0.60f, 0.76f);
	Gdiplus::SolidBrush black(Gdiplus::Color(255, 20, 20, 20));
	g.FillRectangle(&black, s * 0.16f, s * 0.42f, s * 0.68f, s * 0.20f);
}

// A small toolbox silhouette (body + arched handle + seam line) for the
// Tools popup-menu button (Organize/Merge/Split/Resize/Flatten/Compress).
void DrawToolsMenu(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.rect(0.14f, 0.42f, 0.72f, 0.42f);
	Gdiplus::RectF arc(s * 0.30f, s * 0.14f, s * 0.40f, s * 0.36f);
	g.DrawArc(&p.pen, arc, 180, 180);
	p.line(0.14f, 0.60f, 0.86f, 0.60f);
}

// Two overlapping page rects (offset diagonally) -- reads as "combine".
void DrawMergeTool(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.rect(0.14f, 0.30f, 0.48f, 0.56f);
	p.rect(0.38f, 0.14f, 0.48f, 0.56f);
}

// A single page cut by a dashed line into two halves with a small gap --
// reads as "split apart".
void DrawSplitTool(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.rect(0.16f, 0.14f, 0.68f, 0.32f);
	p.rect(0.16f, 0.54f, 0.68f, 0.32f);
	Gdiplus::Pen dash(Gdiplus::Color(kInk), s * kStroke);
	dash.SetDashStyle(Gdiplus::DashStyleDash);
	g.DrawLine(&dash, s * 0.10f, s * 0.48f, s * 0.90f, s * 0.48f);
}

// A classic padlock: rounded body + shackle arc -- reads as "protect".
void DrawLockTool(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.rect(0.20f, 0.44f, 0.60f, 0.42f);
	Gdiplus::RectF arc(s * 0.28f, s * 0.14f, s * 0.44f, s * 0.44f);
	g.DrawArc(&p.pen, arc, 180, 180);
	Gdiplus::SolidBrush dot{ Gdiplus::Color(kInk) }; // brace-init avoids a most-vexing-parse with a single identifier arg
	g.FillEllipse(&dot, s * 0.44f, s * 0.58f, s * 0.12f, s * 0.12f);
}

// The same padlock as DrawLockTool, shrunk into the top-left with a "+"
// badge bottom-right (same technique as DrawSaveAs's badge on DrawSave) --
// reads as "add a new password" vs. DrawLockTool's plain "protected".
void DrawSetPasswordTool(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.rect(0.14f, 0.40f, 0.46f, 0.34f);
	Gdiplus::RectF arc(s * 0.20f, s * 0.12f, s * 0.34f, s * 0.34f);
	g.DrawArc(&p.pen, arc, 180, 180);
	Gdiplus::SolidBrush dot{ Gdiplus::Color(kInk) };
	g.FillEllipse(&dot, s * 0.32f, s * 0.50f, s * 0.10f, s * 0.10f);
	IconPen p2(g, s, 0.10f);
	p2.line(0.74f, 0.60f, 0.74f, 0.86f);
	p2.line(0.61f, 0.73f, 0.87f, 0.73f);
}

// A generic source page, an arrow, and a destination page -- reads as
// "turn this file into a PDF page" (plain page rects like DrawMergeTool's,
// just side by side with an arrow instead of overlapping).
void DrawConvertTool(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.rect(0.06f, 0.20f, 0.34f, 0.60f);
	p.rect(0.60f, 0.20f, 0.34f, 0.60f);
	p.line(0.44f, 0.50f, 0.56f, 0.50f);
	p.line(0.50f, 0.44f, 0.56f, 0.50f);
	p.line(0.50f, 0.56f, 0.56f, 0.50f);
}

// A simple globe (circle + one vertical + two horizontal "latitude" arcs) --
// reads as "web page" for the web-to-PDF tool.
void DrawWebPageTool(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.ellipse(0.14f, 0.14f, 0.72f, 0.72f);
	p.ellipse(0.365f, 0.14f, 0.27f, 0.72f);
	p.line(0.14f, 0.34f, 0.86f, 0.34f);
	p.line(0.14f, 0.56f, 0.86f, 0.56f);
}

// Two stacked page rects with a downward arrow between them -- reads as
// "flatten/merge layers down into one", the same visual language as a
// layers panel's "flatten" action in most graphics apps.
void DrawFlattenTool(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.rect(0.18f, 0.10f, 0.64f, 0.30f);
	p.rect(0.18f, 0.58f, 0.64f, 0.30f);
	p.line(0.50f, 0.42f, 0.50f, 0.56f);
	p.line(0.38f, 0.46f, 0.50f, 0.58f);
	p.line(0.62f, 0.46f, 0.50f, 0.58f);
}

// A minimal sun (core + 8 rays) for the light/dark theme toggle button --
// one fixed glyph regardless of which mode is active, like most apps' theme
// toggles.
void DrawThemeToggle(Gdiplus::Graphics& g, float s)
{
	IconPen p(g, s);
	p.ellipse(0.32f, 0.32f, 0.36f, 0.36f);
	constexpr float kCx = 0.5f, kCy = 0.5f, kR1 = 0.32f, kR2 = 0.46f;
	for (int i = 0; i < 8; ++i) {
		float a = i * 3.14159265f / 4.0f;
		float x1 = kCx + kR1 * std::cos(a), y1 = kCy + kR1 * std::sin(a);
		float x2 = kCx + kR2 * std::cos(a), y2 = kCy + kR2 * std::sin(a);
		p.line(x1, y1, x2, y2);
	}
}

} // namespace icons

// ---------------------------------------------------------------------------
// Empty-state "tools grid" (shown instead of the plain drag-and-drop hint
// when a tab has no document open) -- a quick visual index of what the app
// can do. Every tile funnels into IDM_FILE_OPEN except Merge, which (like
// the toolbar's own Merge command) can start straight from a picked file
// list with no document open yet.
// ---------------------------------------------------------------------------
namespace {

struct EmptyTile { icons::DrawFn icon; const wchar_t* label; int cmdId; };

const std::vector<EmptyTile>& emptyStateTiles()
{
	static const std::vector<EmptyTile> tiles = {
		{ icons::DrawOpen,          L"Open a PDF",       IDM_FILE_OPEN },
		{ icons::DrawHighlightTool, L"Annotate",          IDM_FILE_OPEN },
		{ icons::DrawTextTool,      L"Fill Forms",        IDM_FILE_OPEN },
		{ icons::DrawThumbs,        L"Organize Pages",    IDM_FILE_OPEN },
		{ icons::DrawMergeTool,     L"Merge PDFs",        IDM_TOOLS_MERGE },
		{ icons::DrawSplitTool,     L"Split PDF",         IDM_FILE_OPEN },
		{ icons::DrawRedactTool,    L"Redact Content",    IDM_FILE_OPEN },
		{ icons::DrawLockTool,      L"Remove Password",   IDM_FILE_OPEN },
		{ icons::DrawSetPasswordTool, L"Set Password",    IDM_EMPTY_SET_PASSWORD },
		{ icons::DrawConvertTool,   L"Convert to PDF",    IDM_TOOLS_CONVERT },
		{ icons::DrawWebPageTool,   L"Web Page to PDF",   IDM_TOOLS_WEBPDF },
		{ icons::DrawFlattenTool,   L"Flatten to Image",  IDM_EMPTY_FLATTEN_IMAGE },
		{ icons::DrawFlattenTool,   L"Flatten Edits Only", IDM_EMPTY_FLATTEN_EDITS },
	};
	return tiles;
}

// Shared tile-rect math for both painting and hit-testing, so they can never
// drift out of sync with each other.
std::vector<RECT> layoutEmptyStateTiles(const RECT& rc, UINT dpi)
{
	const auto& tiles = emptyStateTiles();
	const int cols = 4;
	const int rows = (static_cast<int>(tiles.size()) + cols - 1) / cols;
	const int tileW = Scale(132, dpi), tileH = Scale(96, dpi);
	const int gapX = Scale(16, dpi), gapY = Scale(16, dpi);
	const int gridW = cols * tileW + (cols - 1) * gapX;
	const int gridH = rows * tileH + (rows - 1) * gapY;
	const int headerH = Scale(84, dpi); // reserved space for the heading/subtitle above the grid
	const int totalH = headerH + gridH;
	const int left = rc.left + ((rc.right - rc.left) - gridW) / 2;
	const int top = rc.top + std::max(0, static_cast<int>((rc.bottom - rc.top) - totalH) / 2) + headerH;

	std::vector<RECT> out;
	out.reserve(tiles.size());
	for (size_t i = 0; i < tiles.size(); ++i) {
		int col = static_cast<int>(i) % cols, row = static_cast<int>(i) / cols;
		int x = left + col * (tileW + gapX);
		int y = top + row * (tileH + gapY);
		out.push_back(RECT{ x, y, x + tileW, y + tileH });
	}
	return out;
}

} // namespace

void CanvasView::drawEmptyState(HDC dc, const RECT& rc)
{
	auto tileRects = layoutEmptyStateTiles(rc, dpi_);
	const auto& tiles = emptyStateTiles();
	if (tileRects.empty()) return;

	HFONT headingFont = CreateFontW(-Scale(20, dpi_), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
	HFONT subFont = CreateFontW(-Scale(12, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
	HFONT labelFont = CreateFontW(-Scale(12, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
	SetBkMode(dc, TRANSPARENT);
	const ThemeColors& th = Theme(dark_);

	// Heading + subtitle, right above the grid's top row.
	int gridTop = tileRects.front().top;
	for (auto& r : tileRects) gridTop = std::min(gridTop, static_cast<int>(r.top));
	int headerBottom = gridTop - Scale(16, dpi_);
	RECT headRc = { rc.left, headerBottom - Scale(56, dpi_), rc.right, headerBottom - Scale(24, dpi_) };
	RECT subRc = { rc.left, headerBottom - Scale(24, dpi_), rc.right, headerBottom };
	HGDIOBJ oldFont = SelectObject(dc, headingFont);
	SetTextColor(dc, th.emptyHeading);
	DrawTextW(dc, L"Open a PDF to get started", -1, &headRc, DT_CENTER | DT_SINGLELINE | DT_BOTTOM);
	SelectObject(dc, subFont);
	SetTextColor(dc, th.emptySubtitle);
	DrawTextW(dc, L"Drag a file here, or pick a tool below", -1, &subRc, DT_CENTER | DT_SINGLELINE | DT_BOTTOM);

	Gdiplus::Graphics g(dc);
	g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
	for (size_t i = 0; i < tiles.size() && i < tileRects.size(); ++i) {
		const RECT& r = tileRects[i];
		int rad = Scale(10, dpi_);
		HPEN pen = CreatePen(PS_SOLID, 1, th.tileBorder);
		HBRUSH fill = CreateSolidBrush(th.tileBg);
		HGDIOBJ oldPen = SelectObject(dc, pen);
		HGDIOBJ oldBr = SelectObject(dc, fill);
		RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
		SelectObject(dc, oldPen); SelectObject(dc, oldBr);
		DeleteObject(pen); DeleteObject(fill);

		float iconSize = static_cast<float>(Scale(28, dpi_));
		float iconX = r.left + ((r.right - r.left) - iconSize) / 2.0f;
		float iconY = r.top + Scale(14, dpi_);
		Gdiplus::GraphicsState st = g.Save();
		g.TranslateTransform(iconX, iconY);
		tiles[i].icon(g, iconSize);
		g.Restore(st);

		RECT labelRc = { r.left + Scale(4, dpi_), r.top + Scale(14, dpi_) + static_cast<int>(iconSize) + Scale(8, dpi_),
			r.right - Scale(4, dpi_), r.bottom - Scale(6, dpi_) };
		SelectObject(dc, labelFont);
		SetTextColor(dc, th.tileText);
		DrawTextW(dc, tiles[i].label, -1, &labelRc, DT_CENTER | DT_TOP | DT_WORDBREAK);
	}

	SelectObject(dc, oldFont);
	DeleteObject(headingFont);
	DeleteObject(subFont);
	DeleteObject(labelFont);
}

bool CanvasView::hitTestEmptyState(int mx, int my, int& cmdId) const
{
	RECT rc; GetClientRect(hwnd_, &rc);
	auto tileRects = layoutEmptyStateTiles(rc, dpi_);
	const auto& tiles = emptyStateTiles();
	for (size_t i = 0; i < tileRects.size() && i < tiles.size(); ++i) {
		const RECT& r = tileRects[i];
		if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
			cmdId = tiles[i].cmdId;
			return true;
		}
	}
	return false;
}

// ===========================================================================
// FrameWindow implementation
// ===========================================================================
HWND FrameWindow::create(int nCmdShow, bool checkForUpdates)
{
	// Resolve the starting theme (saved override, else the system's) before
	// the window is created -- WM_CREATE (fired synchronously inside
	// CreateWindowExW below) calls createChildren(), which rasterizes the
	// toolbar icons using whatever ink color icons::kInk holds right now.
	isDark_ = InitialDarkMode();
	icons::SetInkColor(Theme(isDark_).iconInk);
	ctrlBgBrush_ = CreateSolidBrush(Theme(isDark_).ctrlBg);

	// No classic File/Edit/View menu bar -- the toolbar plus accelerators
	// (Ctrl+O, Ctrl+S, Ctrl+F, ...) cover everything, matching Edge's PDF
	// viewer chrome. IDR_MAINMENU is left in the .rc file unused rather than
	// deleted, in case a "more options" overflow menu wants it later.
	hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, kFrameClass, L"PDFast",
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
		Scale(1100, 96), Scale(800, 96), nullptr, nullptr, hInst_, this);
	if (!hwnd_) return nullptr;
	++g_liveFrameCount;
	// Windows 11 native chrome: rounded corners, Mica backdrop, themed title
	// bar. No-op on older Windows. Applied before ShowWindow so the first
	// paint already has the correct frame.
	ApplyModernWindowChrome(hwnd_, isDark_);
	// Open maximized by default (fills the screen, keeping the title bar and
	// taskbar -- what users mean by "full screen" for a document app). Still
	// honor an explicit minimized/hidden launch (e.g. a shortcut set to run
	// minimized); the CW_USEDEFAULT size above becomes the restored size.
	int show = (nCmdShow == SW_SHOWMINIMIZED || nCmdShow == SW_SHOWMINNOACTIVE ||
	            nCmdShow == SW_HIDE) ? nCmdShow : SW_SHOWMAXIMIZED;
	ShowWindow(hwnd_, show);
	UpdateWindow(hwnd_);

	// Auto-update: sweep away any leftover from a prior self-replace, then
	// kick off a silent background check for a newer GitHub release. The
	// result comes back via WM_APP_UPDATE_RESULT once the message loop runs.
	// Skipped for extra windows (drag-a-tab-out / "Open in New Window") --
	// one check per process launch is enough, and a second window shouldn't
	// pop its own redundant "update available" prompt.
	if (checkForUpdates) {
		updater::CleanupAfterUpdate();
		startUpdateCheck(/*manual=*/false);
	}
	return hwnd_;
}

void FrameWindow::toggleTheme()
{
	isDark_ = !isDark_;
	SaveThemeOverride(isDark_);
	applyTheme();
}

void FrameWindow::applyTheme()
{
	icons::SetInkColor(Theme(isDark_).iconInk);
	rebuildToolbarIcons();
	ApplyTitleBarDarkMode(hwnd_, isDark_);

	const ThemeColors& t = Theme(isDark_);
	if (ctrlBgBrush_) DeleteObject(ctrlBgBrush_);
	ctrlBgBrush_ = CreateSolidBrush(t.ctrlBg);
	// Status bar is fully custom-drawn (background + text) in its
	// NM_CUSTOMDRAW handler when isDark_ -- nothing to do here.

	for (auto& tab : tabs_) {
		if (tab->canvas) tab->canvas->setDarkMode(isDark_);
		if (tab->thumbs) tab->thumbs->setDarkMode(isDark_);
	}

	// Plain InvalidateRect on the frame only repaints the frame's OWN client
	// area -- it does not cascade to child HWNDs (EDIT/STATIC/the tab strip/
	// status bar/canvas/thumbnail panel), which is why they'd otherwise keep
	// showing stale colors until something else happened to repaint them.
	// RDW_ALLCHILDREN forces every descendant to repaint too, so every
	// WM_CTLCOLORSTATIC/EDIT, WM_ERASEBKGND, and custom WM_PAINT along the
	// way picks up the new theme immediately.
	RedrawWindow(hwnd_, nullptr, nullptr,
		RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
}

namespace {
// Ordered list of every toolbar icon glyph. createChildren() adds these to
// toolbarImages_ once at startup (each TBBUTTON's iBitmap is that position,
// baked in below via the named iXxx indices); rebuildToolbarIcons() replays
// the exact same order into a cleared ImageList when the theme toggles, so
// every existing TBBUTTON's index stays valid without touching the button
// list itself -- only the pixels (and therefore the ink color) change.
const std::vector<icons::DrawFn>& ToolbarIconDrawFns()
{
	static const std::vector<icons::DrawFn> fns = {
		icons::DrawOpen, icons::DrawSave, icons::DrawSaveAs, icons::DrawPrint, icons::DrawFind,
		icons::DrawSelectTool, icons::DrawHighlightTool, icons::DrawDrawTool, icons::DrawTextTool,
		icons::DrawRedactTool, icons::DrawColorTool, icons::DrawWidthTool, icons::DrawOpacityTool,
		icons::DrawZoomOut, icons::DrawZoomIn, icons::DrawFitWidth, icons::DrawFitPage,
		icons::DrawToolsMenu, icons::DrawThemeToggle, icons::DrawEraseTool,
	};
	return fns;
}
} // namespace

void FrameWindow::rebuildToolbarIcons()
{
	if (!toolbarImages_ || !toolbar_) return;
	UINT dpi = GetDpiForWindow(hwnd_);
	int iconPx = Scale(20, dpi);
	ImageList_RemoveAll(toolbarImages_);
	for (auto& fn : ToolbarIconDrawFns()) {
		HBITMAP hb = icons::MakeIcon(iconPx, fn);
		ImageList_Add(toolbarImages_, hb, nullptr);
		DeleteObject(hb);
	}
	InvalidateRect(toolbar_, nullptr, TRUE);
}

void FrameWindow::createChildren()
{
	// Icon-only flat toolbar with tooltips, grouped like Edge's PDF viewer
	// chrome: File actions, Find, annotation Tools (+ their color/width/
	// opacity), Zoom/Fit, then Layout toggles. CCS_NOPARENTALIGN|CCS_NORESIZE
	// hand full position/size control to layout() -- the tab strip sits above
	// this row now (Edge-style: tabs right under the title bar, toolbar
	// below), which needs the toolbar OFF its default auto-dock-to-(0,0)
	// behavior. An earlier attempt at exactly this (CCS_NORESIZE alone, no
	// CCS_NOPARENTALIGN, and calling TB_AUTOSIZE before ever giving the
	// control a real width via MoveWindow) collapsed it to 0 height -- giving
	// it a real width first, THEN calling TB_AUTOSIZE (see layout()), avoids
	// that.
	// WS_CLIPSIBLINGS: the page-number box's transient EDIT control
	// (pageEditActive_, created only while actively typing -- see its
	// comment) is a real overlapping sibling while it exists; this keeps the
	// toolbar's own hover/hot-tracking repaint of the button underneath
	// clipped around it instead of painting straight over it.
	toolbar_ = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
		WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | TBSTYLE_WRAPABLE |
		CCS_NODIVIDER | CCS_NOPARENTALIGN | CCS_NORESIZE,
		0, 0, 0, 0, hwnd_, nullptr, hInst_, nullptr);
	SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
	// TBSTYLE_EX_MIXEDBUTTONS is needed for the text-only zoom-% button (see
	// textBtn()) to render its text at all -- without it, that button
	// collapses to a blank gap. It does NOT cause icon buttons to show a
	// caption; that turned out to be because they had an iString set at all
	// (even without BTNS_SHOWTEXT) -- see btn()/radioBtn()'s iString = -1.
	SendMessageW(toolbar_, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS);

	UINT dpi = GetDpiForWindow(hwnd_);
	int iconPx = Scale(20, dpi);
	toolbarImages_ = ImageList_Create(iconPx, iconPx, ILC_COLOR32, 0, 1);
	SendMessageW(toolbar_, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(toolbarImages_));
	// Roomier, even hit targets on a Fluent 4/8 spacing grid: with the 20px
	// icon this yields ~36px-tall square-ish buttons and consistent gaps,
	// giving the rounded hover pills room to breathe.
	SendMessageW(toolbar_, TB_SETPADDING, 0, MAKELPARAM(Scale(10, dpi), Scale(8, dpi)));
	// Build every icon from the single shared ordered list (ToolbarIconDrawFns)
	// so rebuildToolbarIcons() can later replay the exact same order into a
	// cleared ImageList -- these named indices are just that list's positions.
	for (auto& fn : ToolbarIconDrawFns()) {
		HBITMAP hb = icons::MakeIcon(iconPx, fn);
		ImageList_Add(toolbarImages_, hb, nullptr);
		DeleteObject(hb);
	}
	int iOpen = 0, iSave = 1, iSaveAs = 2, iPrint = 3, iFind = 4, iSelect = 5, iHighlight = 6,
		iDraw = 7, iText = 8, iRedact = 9, iColor = 10, iWidth = 11, iOpacity = 12,
		iZoomOut = 13, iZoomIn = 14, iFitWidth = 15, iFitPage = 16, iTools = 17, iThemeToggle = 18,
		iErase = 19;

	auto addStr = [&](const wchar_t* s) -> INT_PTR {
		wchar_t buf[64]; wcscpy_s(buf, s);
		size_t n = wcslen(buf); buf[n + 1] = 0; // double-null for TB_ADDSTRING
		return SendMessageW(toolbar_, TB_ADDSTRINGW, 0, reinterpret_cast<LPARAM>(buf));
	};
	// Icon + tooltip. No iString here -- tooltip text is served on demand
	// via TTN_GETDISPINFOW (see toolbarTips_'s comment for why).
	auto btn = [&](int id, int image, const wchar_t* tip) {
		TBBUTTON b = {};
		b.idCommand = id;
		b.fsState = TBSTATE_ENABLED;
		b.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
		b.iBitmap = image;
		b.iString = -1;
		toolbarTips_[id] = tip;
		SendMessageW(toolbar_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&b));
	};
	// A run of consecutive BTNS_CHECKGROUP buttons forms one mutually-
	// exclusive radio group; a non-group button or separator ends it, so the
	// annotation tools and the Continuous/Single-page pair form two
	// independent groups even though both use this same helper.
	auto radioBtn = [&](int id, int image, const wchar_t* tip) {
		TBBUTTON b = {};
		b.idCommand = id;
		b.fsState = TBSTATE_ENABLED;
		b.fsStyle = BTNS_CHECKGROUP | BTNS_AUTOSIZE;
		b.iBitmap = image;
		b.iString = -1;
		toolbarTips_[id] = tip;
		SendMessageW(toolbar_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&b));
	};
	// Text-only, no icon: shows the current zoom %; click resets to 100%.
	auto textBtn = [&](int id, const wchar_t* text) {
		TBBUTTON b = {};
		b.idCommand = id;
		b.fsState = TBSTATE_ENABLED;
		b.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
		b.iBitmap = I_IMAGENONE;
		b.iString = addStr(text);
		SendMessageW(toolbar_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&b));
	};
	auto sep = [&]() {
		TBBUTTON b = {}; b.fsStyle = BTNS_SEP;
		SendMessageW(toolbar_, TB_ADDBUTTONS, 1, reinterpret_cast<LPARAM>(&b));
	};

	// Grouped left-to-right like Edge's PDF toolbar: annotation tools first,
	// then zoom/fit/page, then file actions on the right.
	radioBtn(IDM_TOOL_SELECT, iSelect, L"Select");
	radioBtn(IDM_TOOL_HIGHLIGHT, iHighlight, L"Highlight");
	radioBtn(IDM_TOOL_DRAW, iDraw, L"Draw");
	radioBtn(IDM_TOOL_ERASE, iErase, L"Erase");
	radioBtn(IDM_TOOL_TEXT, iText, L"Add Text");
	radioBtn(IDM_TOOL_REDACT, iRedact, L"Redact");
	btn(IDM_TOOL_COLOR, iColor, L"Color");
	btn(IDM_TOOL_WIDTH, iWidth, L"Line Width");
	btn(IDM_TOOL_OPACITY, iOpacity, L"Opacity");
	sep();
	btn(IDM_VIEW_ZOOMOUT, iZoomOut, L"Zoom Out");
	// "1200%" (not "100%") purely to reserve enough width for the widest
	// realistic zoom value (kMaxZoom = 1200%) -- this button's own
	// BTNS_SHOWTEXT caption is never actually shown (see the file's own
	// notes on that comctl32 quirk); zoomText_ is drawn directly onto this
	// reserved rect by CDDS_ITEMPREPAINT instead.
	textBtn(IDM_VIEW_ZOOMLABEL, L"1200%");
	btn(IDM_VIEW_ZOOMIN, iZoomIn, L"Zoom In");
	btn(IDM_VIEW_FITWIDTH, iFitWidth, L"Fit Width");
	btn(IDM_VIEW_FITPAGE, iFitPage, L"Fit Page");
	sep();
	// "9999 of 9999" purely to reserve enough width for a realistically huge
	// page count -- pageLabelText_ is drawn directly onto this reserved rect
	// by CDDS_ITEMPREPAINT; clicking it creates the transient pageEditActive_
	// EDIT control (see beginPageNumberEdit()). Moved here from the tab-strip
	// row per user request, to sit inline in the toolbar like Edge's.
	textBtn(IDM_VIEW_PAGELABEL, L"9999 of 9999");
	sep();
	btn(IDM_EDIT_FIND, iFind, L"Find");
	btn(IDM_FILE_PRINT, iPrint, L"Print");
	btn(IDM_FILE_SAVE, iSave, L"Save");
	btn(IDM_FILE_SAVEAS, iSaveAs, L"Save As");
	btn(IDM_FILE_OPEN, iOpen, L"Open");
	sep();
	btn(IDM_TOOLS_MENU, iTools, L"Tools (Merge, Split, Organize, Resize, Flatten, Compress, Set Password, Convert to PDF, Web Page to PDF)");
	sep();
	btn(IDM_VIEW_TOGGLETHEME, iThemeToggle, L"Toggle Light/Dark Theme");
	// Continuous/Single-page and Thumbnails toggles removed from the UI by
	// request; Continuous stays the only (default) scroll mode and the
	// thumbnail panel stays hidden (see showThumbs_'s default).

	SendMessageW(toolbar_, TB_CHECKBUTTON, IDM_TOOL_SELECT, MAKELPARAM(TRUE, 0));
	SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);

	// Document tab strip, below the toolbar. Owner-drawn so each tab can
	// show its own close "x" (fixed-width tabs make that hit-rect
	// predictable -- see tabCloseRect()); TabStripSubclass adds click-on-the-
	// x, middle-click, and right-click-to-close on top of the control's
	// normal click-to-select behavior.
	tabStrip_ = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
		WS_CHILD | WS_VISIBLE | TCS_TOOLTIPS | TCS_OWNERDRAWFIXED | TCS_FIXEDWIDTH,
		0, 0, 0, 0, hwnd_, nullptr, hInst_, nullptr);
	SendMessageW(tabStrip_, TCM_SETITEMSIZE, 0, MAKELPARAM(Scale(160, dpi), Scale(28, dpi)));
	SetWindowSubclass(tabStrip_, TabStripSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
	tabTooltip_ = reinterpret_cast<HWND>(SendMessageW(tabStrip_, TCM_GETTOOLTIPS, 0, 0));

	// Status bar
	status_ = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
		WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
		hwnd_, nullptr, hInst_, nullptr);

	// Search bar (hidden until Ctrl+F). Children of the frame.
	// Use the real system UI font (Segoe UI), DPI-scaled -- not the ancient
	// DEFAULT_GUI_FONT stock object, which renders oversized and dated.
	NONCLIENTMETRICSW ncm = { sizeof(ncm) };
	if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi))
		uiFont_ = CreateFontIndirectW(&ncm.lfMessageFont);
	HFONT guiFont = uiFont_ ? uiFont_ : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	SendMessageW(tabStrip_, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
	searchLabel_ = CreateWindowExW(0, L"STATIC", L"",
		WS_CHILD | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SEARCH_LABEL), hInst_, nullptr);
	searchEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SEARCH_EDIT), hInst_, nullptr);
	searchPrev_ = CreateWindowExW(0, L"BUTTON", L"Previous",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SEARCH_PREV), hInst_, nullptr);
	searchNext_ = CreateWindowExW(0, L"BUTTON", L"Next",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SEARCH_NEXT), hInst_, nullptr);
	searchClose_ = CreateWindowExW(0, L"BUTTON", L"Close",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SEARCH_CLOSE), hInst_, nullptr);
	for (HWND h : { searchLabel_, searchEdit_, searchPrev_, searchNext_, searchClose_ })
		SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
	// Subclass edit for Enter (find next) / Shift+Enter (prev) / Esc (close).
	SetWindowSubclass(searchEdit_, EditSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
	searchBarH_ = Scale(34, GetDpiForWindow(hwnd_));

	// Password prompt bar (hidden until a PDF that needs one is opened).
	pwdLabel_ = CreateWindowExW(0, L"STATIC", L"Enter the password to open this PDF:",
		WS_CHILD | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PWD_LABEL), hInst_, nullptr);
	pwdEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | ES_AUTOHSCROLL | ES_PASSWORD, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PWD_EDIT), hInst_, nullptr);
	pwdUnlock_ = CreateWindowExW(0, L"BUTTON", L"Unlock",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PWD_UNLOCK), hInst_, nullptr);
	pwdCancel_ = CreateWindowExW(0, L"BUTTON", L"Cancel",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PWD_CANCEL), hInst_, nullptr);
	for (HWND h : { pwdLabel_, pwdEdit_, pwdUnlock_, pwdCancel_ })
		SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
	SetWindowSubclass(pwdEdit_, PwdEditSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
	pwdBarH_ = Scale(34, GetDpiForWindow(hwnd_));

	// Protection info bar (hidden unless the open document is encrypted).
	protLabel_ = CreateWindowExW(0, L"STATIC", L"",
		WS_CHILD | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PROT_LABEL), hInst_, nullptr);
	protRemovePwd_ = CreateWindowExW(0, L"BUTTON", L"Remove Password",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PROT_REMOVE_PWD), hInst_, nullptr);
	protRemoveRestrictions_ = CreateWindowExW(0, L"BUTTON", L"Remove Restrictions",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PROT_REMOVE_RESTRICTIONS), hInst_, nullptr);
	protClose_ = CreateWindowExW(0, L"BUTTON", L"Close",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PROT_CLOSE), hInst_, nullptr);
	for (HWND h : { protLabel_, protRemovePwd_, protRemoveRestrictions_, protClose_ })
		SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
	protBarH_ = Scale(34, GetDpiForWindow(hwnd_));

	// Shared operation-result bar (hidden until a one-shot document
	// operation completes).
	opResultLabel_ = CreateWindowExW(0, L"STATIC", L"",
		WS_CHILD | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_OPRESULT_LABEL), hInst_, nullptr);
	opResultSave_ = CreateWindowExW(0, L"BUTTON", L"Save",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_OPRESULT_SAVE), hInst_, nullptr);
	opResultSaveAs_ = CreateWindowExW(0, L"BUTTON", L"Save a Copy...",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_OPRESULT_SAVEAS), hInst_, nullptr);
	opResultClose_ = CreateWindowExW(0, L"BUTTON", L"Not Now",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_OPRESULT_CLOSE), hInst_, nullptr);
	for (HWND h : { opResultLabel_, opResultSave_, opResultSaveAs_, opResultClose_ })
		SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
	opResultBarH_ = Scale(34, GetDpiForWindow(hwnd_));

	// Split bar (hidden until Tools > Split PDF...).
	splitLabel_ = CreateWindowExW(0, L"STATIC", L"Page ranges (e.g. 1-3, 4, 7-10):",
		WS_CHILD | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SPLIT_LABEL), hInst_, nullptr);
	splitEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SPLIT_EDIT), hInst_, nullptr);
	splitButton_ = CreateWindowExW(0, L"BUTTON", L"Split",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SPLIT_BUTTON), hInst_, nullptr);
	splitClose_ = CreateWindowExW(0, L"BUTTON", L"Close",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SPLIT_CLOSE), hInst_, nullptr);
	splitResult_ = CreateWindowExW(0, L"STATIC", L"",
		WS_CHILD | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SPLIT_RESULT), hInst_, nullptr);
	for (HWND h : { splitLabel_, splitEdit_, splitButton_, splitClose_, splitResult_ })
		SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
	SetWindowSubclass(splitEdit_, SplitEditSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
	splitBarH_ = Scale(34, GetDpiForWindow(hwnd_));

	// Set-password bar (hidden until Tools > Set Password...).
	setPwdLabel_ = CreateWindowExW(0, L"STATIC", L"New password to open this PDF:",
		WS_CHILD | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SETPWD_LABEL), hInst_, nullptr);
	setPwdEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | ES_AUTOHSCROLL | ES_PASSWORD, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SETPWD_EDIT), hInst_, nullptr);
	setPwdButton_ = CreateWindowExW(0, L"BUTTON", L"Set Password",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SETPWD_BUTTON), hInst_, nullptr);
	setPwdClose_ = CreateWindowExW(0, L"BUTTON", L"Cancel",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_SETPWD_CLOSE), hInst_, nullptr);
	for (HWND h : { setPwdLabel_, setPwdEdit_, setPwdButton_, setPwdClose_ })
		SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
	SetWindowSubclass(setPwdEdit_, SetPwdEditSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
	setPwdBarH_ = Scale(34, GetDpiForWindow(hwnd_));

	// Web-to-PDF bar (hidden until Tools > Web Page to PDF...).
	webPdfLabel_ = CreateWindowExW(0, L"STATIC", L"Web page URL:",
		WS_CHILD | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_WEBPDF_LABEL), hInst_, nullptr);
	webPdfEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_WEBPDF_EDIT), hInst_, nullptr);
	webPdfButton_ = CreateWindowExW(0, L"BUTTON", L"Convert",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_WEBPDF_BUTTON), hInst_, nullptr);
	webPdfClose_ = CreateWindowExW(0, L"BUTTON", L"Cancel",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_WEBPDF_CLOSE), hInst_, nullptr);
	for (HWND h : { webPdfLabel_, webPdfEdit_, webPdfButton_, webPdfClose_ })
		SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
	SetWindowSubclass(webPdfEdit_, WebPdfEditSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
	webPdfBarH_ = Scale(34, GetDpiForWindow(hwnd_));

	// Redact bar (hidden unless the active tool is Redact).
	redactLabel_ = CreateWindowExW(0, L"STATIC", L"",
		WS_CHILD | SS_CENTERIMAGE, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_REDACT_LABEL), hInst_, nullptr);
	redactApply_ = CreateWindowExW(0, L"BUTTON", L"Apply Redactions",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_REDACT_APPLY), hInst_, nullptr);
	redactClear_ = CreateWindowExW(0, L"BUTTON", L"Clear All",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_REDACT_CLEAR), hInst_, nullptr);
	redactDone_ = CreateWindowExW(0, L"BUTTON", L"Done",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_REDACT_DONE), hInst_, nullptr);
	for (HWND h : { redactLabel_, redactApply_, redactClear_, redactDone_ })
		SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
	redactBarH_ = Scale(34, GetDpiForWindow(hwnd_));

	// Print side panel (hidden until File > Print). A vertical stack of
	// plain native controls, same idiom as the horizontal bars above --
	// positioned in layout() instead of laid out here.
	auto mkStatic = [&](const wchar_t* text, int id) {
		return CreateWindowExW(0, L"STATIC", text, WS_CHILD, 0, 0, 0, 0, hwnd_,
			reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst_, nullptr);
	};
	auto mkRadio = [&](const wchar_t* text, int id, bool group) {
		HWND h = CreateWindowExW(0, L"BUTTON", text,
			WS_CHILD | BS_AUTORADIOBUTTON | (group ? WS_GROUP : 0), 0, 0, 0, 0, hwnd_,
			reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst_, nullptr);
		// A themed (visual-styles) button ignores whatever WM_CTLCOLORBTN
		// returns -- the theme engine draws it regardless (same constraint
		// noted on ctrlBgBrush_'s WM_CTLCOLORSTATIC/EDIT handler, which is
		// why THAT handler explicitly leaves BUTTON children alone). Opting
		// these specific radios out of theming reverts them to classic
		// rendering, which DOES respect a custom text/background color, so
		// their labels can actually be recolored for dark mode below.
		SetWindowTheme(h, L"", L"");
		return h;
	};
	printTitle_ = mkStatic(L"Print", IDC_PRINT_TITLE);
	printPrinterLabel_ = mkStatic(L"Printer", IDC_PRINT_PRINTER_LABEL);
	printPrinterCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
		WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PRINT_PRINTER_COMBO), hInst_, nullptr);
	printCopiesLabel_ = mkStatic(L"Copies", IDC_PRINT_COPIES_LABEL);
	printCopiesEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1",
		WS_CHILD | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PRINT_COPIES_EDIT), hInst_, nullptr);
	printRangeLabel_ = mkStatic(L"Pages", IDC_PRINT_RANGE_LABEL);
	printRangeAll_ = mkRadio(L"All", IDC_PRINT_RANGE_ALL, true);
	printRangeCurrent_ = mkRadio(L"Current page", IDC_PRINT_RANGE_CURRENT, false);
	printRangeCustom_ = mkRadio(L"Custom range", IDC_PRINT_RANGE_CUSTOM, false);
	printRangeEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PRINT_RANGE_EDIT), hInst_, nullptr);
	printOrientLabel_ = mkStatic(L"Orientation", IDC_PRINT_ORIENT_LABEL);
	printOrientPortrait_ = mkRadio(L"Portrait", IDC_PRINT_ORIENT_PORTRAIT, true);
	printOrientLandscape_ = mkRadio(L"Landscape", IDC_PRINT_ORIENT_LANDSCAPE, false);
	printColorLabel_ = mkStatic(L"Color", IDC_PRINT_COLOR_LABEL);
	printColorColor_ = mkRadio(L"Color", IDC_PRINT_COLOR_COLOR, true);
	printColorGray_ = mkRadio(L"Grayscale", IDC_PRINT_COLOR_GRAY, false);
	printPageNavLabel_ = mkStatic(L"", IDC_PRINT_PAGENAV_LABEL);
	printPageNavPrev_ = CreateWindowExW(0, L"BUTTON", L"◀", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PRINT_PAGENAV_PREV), hInst_, nullptr);
	printPageNavNext_ = CreateWindowExW(0, L"BUTTON", L"▶", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PRINT_PAGENAV_NEXT), hInst_, nullptr);
	printGo_ = CreateWindowExW(0, L"BUTTON", L"Print",
		WS_CHILD | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PRINT_GO), hInst_, nullptr);
	printCancel_ = CreateWindowExW(0, L"BUTTON", L"Cancel",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_PRINT_CANCEL), hInst_, nullptr);
	SendMessageW(printRangeAll_, BM_SETCHECK, BST_CHECKED, 0);
	SendMessageW(printOrientPortrait_, BM_SETCHECK, BST_CHECKED, 0);
	SendMessageW(printColorColor_, BM_SETCHECK, BST_CHECKED, 0);
	for (HWND h : { printTitle_, printPrinterLabel_, printPrinterCombo_, printCopiesLabel_, printCopiesEdit_,
		printRangeLabel_, printRangeAll_, printRangeCurrent_, printRangeCustom_, printRangeEdit_,
		printOrientLabel_, printOrientPortrait_, printOrientLandscape_,
		printColorLabel_, printColorColor_, printColorGray_,
		printPageNavLabel_, printPageNavPrev_, printPageNavNext_, printGo_, printCancel_ })
		SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);

	// Rich-text-editor side panel (hidden until "Edit as Rich Text..."),
	// same right-docked idiom as the print panel above (and mutually
	// exclusive with it -- see showTextEditorPanel()).
	LoadLibraryW(L"Msftedit.dll"); // registers MSFTEDIT_CLASS; needed once, before creating the rich edit below
	textPanelTitle_ = mkStatic(L"Edit as Rich Text", IDC_TEXTPANEL_TITLE);
	// BS_CHECKBOX|BS_PUSHLIKE (not BS_AUTOCHECKBOX) so the button draws with a
	// pressed/sunken look when checked, matching what a bold/italic/underline
	// toggle should look like, but WITHOUT the control auto-flipping its own
	// check state on click -- textPanelToggleFormat()/updateTextFormatButtons()
	// drive the check state explicitly from the rich edit's actual character
	// format, which is the only way to stay correct for a mixed-format
	// selection (where "currently on" isn't a simple flip).
	textBoldBtn_ = CreateWindowExW(0, L"BUTTON", L"B", WS_CHILD | BS_CHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_TEXTPANEL_BOLD), hInst_, nullptr);
	textItalicBtn_ = CreateWindowExW(0, L"BUTTON", L"I", WS_CHILD | BS_CHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_TEXTPANEL_ITALIC), hInst_, nullptr);
	textUnderlineBtn_ = CreateWindowExW(0, L"BUTTON", L"U", WS_CHILD | BS_CHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_TEXTPANEL_UNDERLINE), hInst_, nullptr);
	textCaseLabel_ = mkStatic(L"Change case", IDC_TEXTPANEL_CASE_LABEL);
	textCaseCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
		WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_TEXTPANEL_CASE_COMBO), hInst_, nullptr);
	for (const wchar_t* item : { L"UPPERCASE", L"lowercase", L"Title Case", L"Sentence case" })
		SendMessageW(textCaseCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
	textRichEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
		WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
		0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(IDC_TEXTPANEL_RICHEDIT), hInst_, nullptr);
	{
		// A comfortable default reading size -- the rich edit's built-in
		// default is a small legacy font that looks out of place next to
		// real UI text.
		CHARFORMAT2W cf = { sizeof(cf) };
		cf.dwMask = CFM_FACE | CFM_SIZE;
		wcscpy_s(cf.szFaceName, L"Segoe UI");
		cf.yHeight = 200; // twips: 10pt
		SendMessageW(textRichEdit_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&cf));
	}
	// EN_SELCHANGE is opt-in for a rich edit -- needed so the B/I/U buttons
	// can follow the caret (see updateTextFormatButtons()) instead of only
	// updating right after a click.
	SendMessageW(textRichEdit_, EM_SETEVENTMASK, 0,
		SendMessageW(textRichEdit_, EM_GETEVENTMASK, 0, 0) | ENM_SELCHANGE);
	textCopyBtn_ = CreateWindowExW(0, L"BUTTON", L"Copy to Clipboard",
		WS_CHILD | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_TEXTPANEL_COPY), hInst_, nullptr);
	textCloseBtn_ = CreateWindowExW(0, L"BUTTON", L"Close",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_TEXTPANEL_CLOSE), hInst_, nullptr);
	for (HWND h : { textPanelTitle_, textBoldBtn_, textItalicBtn_, textUnderlineBtn_,
		textCaseLabel_, textCaseCombo_, textRichEdit_, textCopyBtn_, textCloseBtn_ })
		SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);

	// Organize side panel's bottom action strip (hidden outside organize mode).
	organizeInsert_ = CreateWindowExW(0, L"BUTTON", L"Insert...",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_ORGANIZE_INSERT), hInst_, nullptr);
	organizeDone_ = CreateWindowExW(0, L"BUTTON", L"Done",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_ORGANIZE_DONE), hInst_, nullptr);
	organizeCancel_ = CreateWindowExW(0, L"BUTTON", L"Cancel",
		WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd_,
		reinterpret_cast<HMENU>(IDC_ORGANIZE_CANCEL), hInst_, nullptr);
	for (HWND h : { organizeInsert_, organizeDone_, organizeCancel_ })
		SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont), TRUE);
	organizeBarH_ = Scale(34, GetDpiForWindow(hwnd_));

	// Zoom-%/page-number readouts are now plain owner-drawn text (see
	// zoomText_'s comment) -- no permanent child windows to create here.
	// pageEditActive_ is created on demand by beginPageNumberEdit().

	// Start with a single empty tab -- Open/drag-drop reuses it (browser-
	// style: an empty tab gets reused, a non-empty one doesn't).
	switchToTab(newTab());
}

void FrameWindow::showSearchBar(bool show)
{
	searchVisible_ = show;
	int sw = show ? SW_SHOW : SW_HIDE;
	for (HWND h : { searchLabel_, searchEdit_, searchPrev_, searchNext_, searchClose_ })
		ShowWindow(h, sw);
	layout();
	if (show) {
		SetFocus(searchEdit_);
		SendMessageW(searchEdit_, EM_SETSEL, 0, -1);
	} else {
		if (canvas_) { canvas_->clearSearch(); SetFocus(canvas_->hwnd()); }
	}
}

void FrameWindow::runSearch()
{
	if (!canvas_) return;
	wchar_t buf[256];
	GetWindowTextW(searchEdit_, buf, 256);
	// First search for a new term finds all + jumps to the first; pressing
	// Enter again on the same term advances to the next match.
	if (buf[0] && lastSearch_ == buf && canvas_->hitCount() > 0) {
		canvas_->findNext();
	} else {
		int count = canvas_->search(buf);
		lastSearch_ = buf;
		if (count == 0 && buf[0]) MessageBeep(MB_OK);
	}
	updateSearchLabel();
}

void FrameWindow::liveSearch()
{
	if (!canvas_) return;
	wchar_t buf[256];
	GetWindowTextW(searchEdit_, buf, 256);
	// Fresh search on each edit; don't beep or advance while typing.
	canvas_->search(buf);
	lastSearch_ = buf;
	updateSearchLabel();
}

void FrameWindow::updateSearchLabel()
{
	if (!canvas_) return;
	wchar_t buf[64];
	int total = canvas_->hitCount();
	if (total <= 0) wcscpy_s(buf, L"No matches");
	else swprintf(buf, 64, L"%d / %d", canvas_->currentHitOrdinal() + 1, total);
	SetWindowTextW(searchLabel_, buf);
}

void FrameWindow::showPasswordBar(bool show)
{
	pwdVisible_ = show;
	int sw = show ? SW_SHOW : SW_HIDE;
	for (HWND h : { pwdLabel_, pwdEdit_, pwdUnlock_, pwdCancel_ })
		ShowWindow(h, sw);
	if (show) {
		SetWindowTextW(pwdLabel_, L"Enter the password to open this PDF:");
		SetWindowTextW(pwdEdit_, L"");
	} else {
		pendingPwdPath_.clear();
	}
	layout();
	if (show) SetFocus(pwdEdit_);
}

void FrameWindow::submitPassword()
{
	if (!doc_ || pendingPwdPath_.empty()) return;
	wchar_t buf[128] = {};
	GetWindowTextW(pwdEdit_, buf, 128);
	std::string upw = WideToUtf8(buf);
	if (doc_->authenticate(upw.c_str())) {
		std::wstring path = pendingPwdPath_;
		showPasswordBar(false);
		finishOpenDocument(path.c_str());
	} else {
		SetWindowTextW(pwdLabel_, L"Incorrect password. Try again:");
		SetWindowTextW(pwdEdit_, L"");
		SetFocus(pwdEdit_);
		MessageBeep(MB_ICONWARNING);
	}
}

void FrameWindow::cancelPasswordPrompt()
{
	pendingSetPasswordAfterOpen_ = false;
	showPasswordBar(false);
	// The tab was created (or reused) for this open attempt but never got a
	// document -- closeTab() is safe to call here since dirty()/isOpen() are
	// both false, so promptSaveIfDirty() won't pop anything.
	closeTab(activeTab_);
}

// Whenever the active tab's document is encrypted (needed a password to
// open, and/or an owner password still restricts copy/print/etc.), offer to
// strip that protection. See removeProtection()/PdfDocument::removeProtection
// for why "remove password" and "remove restrictions" are the same action.
void FrameWindow::updateProtectionBar()
{
	DocTab* at = (activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size())) ? tabs_[activeTab_].get() : nullptr;
	bool show = at && !at->protDismissed && doc_ && doc_->isOpen() && doc_->isPdf() && doc_->isEncrypted();
	protVisible_ = show;
	int sw = show ? SW_SHOW : SW_HIDE;
	for (HWND h : { protLabel_, protRemovePwd_, protRemoveRestrictions_, protClose_ })
		ShowWindow(h, sw);
	if (show) {
		SetWindowTextW(protLabel_, doc_->neededPassword()
			? L"This PDF is password-protected."
			: L"This PDF has restricted permissions (e.g. copying may be blocked).");
	}
	layout();
}

void FrameWindow::removeProtection()
{
	if (!doc_ || !doc_->isOpen() || !doc_->isPdf()) return;
	DocTab* at = tabs_[activeTab_].get();
	if (at->path.empty()) return;
	if (canvas_) canvas_->flushPendingEdit();
	std::string err;
	if (!doc_->removeProtection(at->path.c_str(), err)) {
		std::wstring wmsg(err.begin(), err.end());
		MessageBoxW(hwnd_, wmsg.empty() ? L"Failed to remove protection." : wmsg.c_str(),
			L"Remove Protection", MB_OK | MB_ICONERROR);
		return;
	}
	at->protDismissed = true;
	updateProtectionBar();
}

void FrameWindow::showSetPasswordBar(bool show)
{
	if (show && (!doc_ || !doc_->isOpen() || !doc_->isPdf())) return;
	setPwdVisible_ = show;
	int sw = show ? SW_SHOW : SW_HIDE;
	for (HWND h : { setPwdLabel_, setPwdEdit_, setPwdButton_, setPwdClose_ })
		ShowWindow(h, sw);
	if (show) {
		SetWindowTextW(setPwdLabel_, L"New password to open this PDF:");
		SetWindowTextW(setPwdEdit_, L"");
	}
	layout();
	if (show) SetFocus(setPwdEdit_);
}

void FrameWindow::runSetPassword()
{
	if (!doc_ || !doc_->isOpen() || !doc_->isPdf()) return;
	DocTab* at = tabs_[activeTab_].get();
	if (at->path.empty()) {
		SetWindowTextW(setPwdLabel_, L"Save this document first, then set a password.");
		return;
	}
	wchar_t buf[128] = {};
	GetWindowTextW(setPwdEdit_, buf, 128);
	if (!buf[0]) {
		SetWindowTextW(setPwdLabel_, L"Enter a password:");
		MessageBeep(MB_ICONWARNING);
		return;
	}
	std::string pw = WideToUtf8(buf);
	if (canvas_) canvas_->flushPendingEdit();
	std::string err;
	if (!doc_->setPassword(at->path.c_str(), pw.c_str(), err)) {
		std::wstring wmsg(err.begin(), err.end());
		MessageBoxW(hwnd_, wmsg.empty() ? L"Failed to set password." : wmsg.c_str(),
			L"Set Password", MB_OK | MB_ICONERROR);
		return;
	}
	showSetPasswordBar(false);
	// Confirms success and offers the matching undo -- the protection bar's
	// existing "Remove Password"/"Remove Restrictions" now apply to the
	// password we just set.
	at->protDismissed = false;
	updateProtectionBar();
}

// --- Auto-update -----------------------------------------------------------

void FrameWindow::startUpdateCheck(bool manual)
{
	HWND hwnd = hwnd_;
	// Detached worker: the GitHub API round-trip must not block the UI. It
	// hands the result back via a posted message so all UI happens on the
	// main thread. The heap UpdateInfo is owned by the message handler.
	std::thread([hwnd, manual] {
		auto* info = new updater::UpdateInfo(updater::CheckForUpdate());
		if (!PostMessageW(hwnd, WM_APP_UPDATE_RESULT, manual ? 1 : 0,
				reinterpret_cast<LPARAM>(info)))
			delete info; // window gone -- don't leak
	}).detach();
}

bool FrameWindow::promptSaveAllIfDirty()
{
	for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
		switchToTab(i);
		if (!promptSaveIfDirty()) return false;
	}
	return true;
}

void FrameWindow::onUpdateResult(const updater::UpdateInfo& info, bool manual)
{
	if (info.checkFailed) {
		if (manual)
			MessageBoxW(hwnd_, L"Couldn't check for updates. Please check your internet connection and try again.",
				L"Check for Updates", MB_OK | MB_ICONWARNING);
		return; // silent on the automatic launch check
	}
	if (!info.available) {
		if (manual) {
			std::wstring msg = L"You're on the latest version (" L"" APP_VERSION_STR L").";
			MessageBoxW(hwnd_, msg.c_str(), L"Check for Updates", MB_OK | MB_ICONINFORMATION);
		}
		return;
	}

	std::wstring prompt = L"Version " + info.latestVersion + L" is available (you have "
		L"" APP_VERSION_STR L").\n\nUpdate now? The app will briefly close and reopen.";
	if (MessageBoxW(hwnd_, prompt.c_str(), L"Update Available", MB_YESNO | MB_ICONINFORMATION) != IDYES)
		return;

	// Don't lose unsaved work across the relaunch.
	if (!promptSaveAllIfDirty()) return;

	// No .exe in the release (or self-replace not possible) -- fall back to the
	// browser so the user can grab it manually.
	if (info.downloadUrl.empty()) {
		ShellExecuteW(hwnd_, L"open", info.releaseUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		return;
	}

	updater::ApplyResult r = updater::DownloadAndApply(hwnd_, info);
	if (r == updater::ApplyResult::Relaunching) {
		DestroyWindow(hwnd_); // tabs already saved above; new version is launching
	} else if (r == updater::ApplyResult::Failed) {
		int open = MessageBoxW(hwnd_,
			L"The update couldn't be installed automatically. Open the download page in your browser?",
			L"Update Failed", MB_YESNO | MB_ICONERROR);
		if (open == IDYES)
			ShellExecuteW(hwnd_, L"open", info.releaseUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
	// Canceled: nothing to do.
}

void FrameWindow::showOpResultBar(bool show)
{
	opResultVisible_ = show;
	int sw = show ? SW_SHOW : SW_HIDE;
	for (HWND h : { opResultLabel_, opResultSave_, opResultSaveAs_, opResultClose_ })
		ShowWindow(h, sw);
	if (show) SetWindowTextW(opResultLabel_, L"Done. Save changes to the original file, or save a copy?");
	layout();
}

void FrameWindow::showSplitBar(bool show)
{
	splitVisible_ = show;
	int sw = show ? SW_SHOW : SW_HIDE;
	for (HWND h : { splitLabel_, splitEdit_, splitButton_, splitClose_, splitResult_ })
		ShowWindow(h, sw);
	if (show) {
		SetWindowTextW(splitEdit_, L"");
		SetWindowTextW(splitResult_, L"");
	}
	layout();
	if (show) SetFocus(splitEdit_);
}

void FrameWindow::runSplit()
{
	if (!doc_ || !doc_->isOpen() || !doc_->isPdf()) return;
	DocTab* at = tabs_[activeTab_].get();
	if (at->path.empty()) {
		SetWindowTextW(splitResult_, L"Save this document first, then split.");
		return;
	}
	wchar_t buf[256] = {};
	GetWindowTextW(splitEdit_, buf, 256);
	std::wstring rangesText = buf;
	if (rangesText.empty()) {
		SetWindowTextW(splitResult_, L"Enter at least one page range.");
		return;
	}

	// Parse comma-separated ranges: "N" or "N-M", 1-based inclusive.
	int pageCount = doc_->pageCount();
	struct Range { int lo, hi; }; // 0-based, inclusive
	std::vector<Range> ranges;
	auto parseToken = [&](std::wstring tok) -> bool {
		size_t a = tok.find_first_not_of(L" \t");
		if (a == std::wstring::npos) return true; // blank token (e.g. trailing comma): ignore
		size_t b = tok.find_last_not_of(L" \t");
		tok = tok.substr(a, b - a + 1);
		size_t dash = tok.find(L'-');
		int lo, hi;
		if (dash != std::wstring::npos) {
			lo = _wtoi(tok.substr(0, dash).c_str());
			hi = _wtoi(tok.substr(dash + 1).c_str());
		} else {
			lo = hi = _wtoi(tok.c_str());
		}
		if (lo < 1 || hi < lo || hi > pageCount) return false;
		ranges.push_back({ lo - 1, hi - 1 });
		return true;
	};
	bool parseOk = true;
	size_t start = 0;
	while (start <= rangesText.size()) {
		size_t comma = rangesText.find(L',', start);
		std::wstring tok = (comma == std::wstring::npos) ? rangesText.substr(start) : rangesText.substr(start, comma - start);
		if (!parseToken(tok)) { parseOk = false; break; }
		if (comma == std::wstring::npos) break;
		start = comma + 1;
	}
	if (!parseOk || ranges.empty()) {
		SetWindowTextW(splitResult_, L"Couldn't parse those page ranges.");
		return;
	}

	if (canvas_) canvas_->flushPendingEdit();
	size_t slash = at->path.find_last_of(L'\\');
	std::wstring folder = (slash == std::wstring::npos) ? L"." : at->path.substr(0, slash);
	std::wstring base = at->name;
	size_t dot = base.find_last_of(L'.');
	if (dot != std::wstring::npos) base = base.substr(0, dot);

	int ok = 0, fail = 0;
	for (auto& r : ranges) {
		std::vector<int> pages;
		for (int p = r.lo; p <= r.hi; ++p) pages.push_back(p);
		wchar_t suffix[32];
		if (r.lo == r.hi) swprintf(suffix, 32, L"_page-%d.pdf", r.lo + 1);
		else swprintf(suffix, 32, L"_pages-%d-%d.pdf", r.lo + 1, r.hi + 1);
		std::wstring outPath = folder + L"\\" + base + suffix;
		std::string err;
		if (doc_->exportPages(pages, outPath.c_str(), err)) ++ok; else ++fail;
	}
	wchar_t result[160];
	swprintf(result, 160, L"Created %d file(s) next to the original%s.", ok, fail > 0 ? L" (some failed)" : L"");
	SetWindowTextW(splitResult_, result);
}

void FrameWindow::updateRedactBar()
{
	bool show = canvas_ && doc_ && doc_->isOpen() && doc_->isPdf() && canvas_->tool() == CanvasView::Tool::Redact;
	redactBarVisible_ = show;
	int sw = show ? SW_SHOW : SW_HIDE;
	for (HWND h : { redactLabel_, redactApply_, redactClear_, redactDone_ })
		ShowWindow(h, sw);
	if (show) {
		int n = doc_->pendingRedactionCount();
		wchar_t buf[160];
		swprintf(buf, 160, L"Redact tool: drag a box over content to mark it. %d mark%s pending.",
			n, n == 1 ? L"" : L"s");
		SetWindowTextW(redactLabel_, buf);
		EnableWindow(redactApply_, n > 0);
		EnableWindow(redactClear_, n > 0);
	}
	layout();
}

void FrameWindow::showPrintPanel(bool show)
{
	if (show == printPanelVisible_) return;
	if (show) {
		if (!doc_ || !doc_->isOpen() || !canvas_) return;
		if (textPanelVisible_) showTextEditorPanel(false); // one right-hand panel at a time
		// Populate the printer list fresh every time the panel opens (cheap,
		// and picks up printers installed/removed since last time).
		SendMessageW(printPrinterCombo_, CB_RESETCONTENT, 0, 0);
		std::vector<std::wstring> printers = ListPrinterNames();
		std::wstring def = DefaultPrinterName();
		int defIndex = 0;
		for (size_t i = 0; i < printers.size(); ++i) {
			SendMessageW(printPrinterCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(printers[i].c_str()));
			if (printers[i] == def) defIndex = static_cast<int>(i);
		}
		if (!printers.empty()) SendMessageW(printPrinterCombo_, CB_SETCURSEL, defIndex, 0);
		// Reset the rest of the panel to sensible defaults each time it opens.
		SetWindowTextW(printCopiesEdit_, L"1");
		SendMessageW(printRangeAll_, BM_SETCHECK, BST_CHECKED, 0);
		SendMessageW(printRangeCurrent_, BM_SETCHECK, BST_UNCHECKED, 0);
		SendMessageW(printRangeCustom_, BM_SETCHECK, BST_UNCHECKED, 0);
		SetWindowTextW(printRangeEdit_, L"");
		EnableWindow(printRangeEdit_, FALSE);
		SendMessageW(printOrientPortrait_, BM_SETCHECK, BST_CHECKED, 0);
		SendMessageW(printOrientLandscape_, BM_SETCHECK, BST_UNCHECKED, 0);
		SendMessageW(printColorColor_, BM_SETCHECK, BST_CHECKED, 0);
		SendMessageW(printColorGray_, BM_SETCHECK, BST_UNCHECKED, 0);
		printPanelVisible_ = true;
		layout();
		updatePrintPreviewFromPanel();
	} else {
		printPanelVisible_ = false;
		if (canvas_) canvas_->endPrintPreview();
		layout();
	}
}

PrintSettings FrameWindow::readPrintSettingsFromPanel() const
{
	PrintSettings s;
	int sel = static_cast<int>(SendMessageW(printPrinterCombo_, CB_GETCURSEL, 0, 0));
	if (sel >= 0) {
		wchar_t buf[512];
		if (SendMessageW(printPrinterCombo_, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(buf)) > 0)
			s.printerName = buf;
	}
	wchar_t copiesBuf[16];
	GetWindowTextW(printCopiesEdit_, copiesBuf, 16);
	s.copies = std::max(1, _wtoi(copiesBuf));
	if (SendMessageW(printRangeCurrent_, BM_GETCHECK, 0, 0) == BST_CHECKED)
		s.rangeMode = PrintRangeMode::Current;
	else if (SendMessageW(printRangeCustom_, BM_GETCHECK, 0, 0) == BST_CHECKED)
		s.rangeMode = PrintRangeMode::Custom;
	else
		s.rangeMode = PrintRangeMode::All;
	wchar_t rangeBuf[128];
	GetWindowTextW(printRangeEdit_, rangeBuf, 128);
	s.customRange = rangeBuf;
	s.landscape = SendMessageW(printOrientLandscape_, BM_GETCHECK, 0, 0) == BST_CHECKED;
	s.grayscale = SendMessageW(printColorGray_, BM_GETCHECK, 0, 0) == BST_CHECKED;
	return s;
}

void FrameWindow::updatePrintPreviewFromPanel()
{
	if (!printPanelVisible_ || !doc_ || !doc_->isOpen() || !canvas_) return;
	EnableWindow(printRangeEdit_, SendMessageW(printRangeCustom_, BM_GETCHECK, 0, 0) == BST_CHECKED);
	PrintSettings s = readPrintSettingsFromPanel();
	int curPage = canvas_->currentPage();
	std::vector<int> pages = ResolvePrintPages(s, doc_->pageCount(), curPage);
	if (!canvas_->printPreviewActive())
		canvas_->beginPrintPreview(pages, s.grayscale, s.landscape);
	else
		canvas_->setPrintPreviewOptions(pages, s.grayscale, s.landscape);
	int n = canvas_->printPreviewPageCount();
	wchar_t buf[64];
	if (n <= 0) wcscpy_s(buf, L"No pages");
	else swprintf(buf, 64, L"Page %d of %d", canvas_->printPreviewCursor() + 1, n);
	SetWindowTextW(printPageNavLabel_, buf);
	EnableWindow(printPageNavPrev_, n > 1 && canvas_->printPreviewCursor() > 0);
	EnableWindow(printPageNavNext_, n > 1 && canvas_->printPreviewCursor() < n - 1);
	EnableWindow(printGo_, n > 0);
}

void FrameWindow::doExecutePrint()
{
	if (!doc_ || !doc_->isOpen() || !canvas_) return;
	PrintSettings s = readPrintSettingsFromPanel();
	std::vector<int> pages = ResolvePrintPages(s, doc_->pageCount(), canvas_->currentPage());
	if (pages.empty()) {
		MessageBoxW(hwnd_, L"No pages to print -- check the page range.", L"Print", MB_OK | MB_ICONWARNING);
		return;
	}
	std::string err;
	bool ok = ExecutePrintJob(hwnd_, *doc_, s, pages, err);
	if (!ok) {
		std::wstring werr(err.begin(), err.end());
		MessageBoxW(hwnd_, werr.empty() ? L"Printing failed." : werr.c_str(), L"Print", MB_OK | MB_ICONERROR);
		return; // leave the panel open so the user can fix settings and retry
	}
	showPrintPanel(false);
}

void FrameWindow::showTextEditorPanel(bool show, const std::wstring& initialText)
{
	if (show) {
		if (printPanelVisible_) showPrintPanel(false); // one right-hand panel at a time
		SetWindowTextW(textRichEdit_, initialText.c_str());
		SendMessageW(textRichEdit_, EM_SETSEL, 0, 0);
		textPanelVisible_ = true;
		layout();
		SetFocus(textRichEdit_);
		updateTextFormatButtons();
	} else {
		if (!textPanelVisible_) return;
		textPanelVisible_ = false;
		SetWindowTextW(textRichEdit_, L""); // don't hold onto the last selection's text once closed
		layout();
	}
}

void FrameWindow::textPanelToggleFormat(DWORD mask, DWORD effect)
{
	CHARFORMAT2W cf = { sizeof(cf) };
	SendMessageW(textRichEdit_, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
	bool currentlyOn = (cf.dwMask & mask) && (cf.dwEffects & effect);
	cf.dwMask = mask;
	cf.dwEffects = currentlyOn ? 0 : effect;
	SendMessageW(textRichEdit_, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
	SetFocus(textRichEdit_);
	updateTextFormatButtons();
}

void FrameWindow::updateTextFormatButtons()
{
	CHARFORMAT2W cf = { sizeof(cf) };
	SendMessageW(textRichEdit_, EM_GETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
	// A mixed-format selection reports the bit absent from dwMask (not just
	// dwEffects) -- treated as "off" here, same as GDI/Word's own toolbars.
	auto isOn = [&](DWORD mask, DWORD effect) { return (cf.dwMask & mask) && (cf.dwEffects & effect); };
	SendMessageW(textBoldBtn_, BM_SETCHECK, isOn(CFM_BOLD, CFE_BOLD) ? BST_CHECKED : BST_UNCHECKED, 0);
	SendMessageW(textItalicBtn_, BM_SETCHECK, isOn(CFM_ITALIC, CFE_ITALIC) ? BST_CHECKED : BST_UNCHECKED, 0);
	SendMessageW(textUnderlineBtn_, BM_SETCHECK, isOn(CFM_UNDERLINE, CFE_UNDERLINE) ? BST_CHECKED : BST_UNCHECKED, 0);
}

void FrameWindow::textPanelApplyCase(int mode)
{
	DWORD selStart = 0, selEnd = 0;
	SendMessageW(textRichEdit_, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));
	if (selStart == selEnd) {
		// Nothing selected -- apply to the whole box, matching what a user
		// would expect from picking a case with no explicit selection
		// (there's nothing else the command could mean).
		SendMessageW(textRichEdit_, EM_SETSEL, 0, -1);
		SendMessageW(textRichEdit_, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));
	}
	int len = static_cast<int>(selEnd - selStart);
	if (len <= 0) return;
	std::wstring text(static_cast<size_t>(len) + 1, L'\0');
	SendMessageW(textRichEdit_, EM_GETSELTEXT, 0, reinterpret_cast<LPARAM>(text.data()));
	text.resize(wcslen(text.c_str()));

	switch (mode) {
	case 0: // UPPERCASE
		CharUpperBuffW(text.data(), static_cast<DWORD>(text.size()));
		break;
	case 1: // lowercase
		CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
		break;
	case 2: { // Title Case
		bool startOfWord = true;
		for (auto& ch : text) {
			if (iswspace(ch)) { startOfWord = true; continue; }
			if (startOfWord) CharUpperBuffW(&ch, 1);
			else CharLowerBuffW(&ch, 1);
			startOfWord = false;
		}
		break;
	}
	case 3: { // Sentence case
		bool startOfSentence = true;
		for (auto& ch : text) {
			if (iswspace(ch)) continue;
			if (startOfSentence) { CharUpperBuffW(&ch, 1); startOfSentence = false; }
			else CharLowerBuffW(&ch, 1);
			if (ch == L'.' || ch == L'!' || ch == L'?') startOfSentence = true;
		}
		break;
	}
	}
	SendMessageW(textRichEdit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()));
	SetFocus(textRichEdit_);
}

void FrameWindow::textPanelCopy()
{
	// Select-all + WM_COPY leans on the RichEdit control's own clipboard
	// handling, which puts BOTH a plain-text and an RTF (formatted)
	// rendering on the clipboard in one step -- no need to hand-generate RTF
	// ourselves. The caret position is restored after so repeated copies
	// while still editing don't leave the whole box visibly selected.
	DWORD selStart = 0, selEnd = 0;
	SendMessageW(textRichEdit_, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));
	SendMessageW(textRichEdit_, EM_SETSEL, 0, -1);
	SendMessageW(textRichEdit_, WM_COPY, 0, 0);
	SendMessageW(textRichEdit_, EM_SETSEL, selStart, selEnd);
	SetFocus(textRichEdit_);
}

void FrameWindow::applyRedactionsCmd()
{
	if (!doc_ || !doc_->isOpen()) return;
	if (doc_->pendingRedactionCount() <= 0) return;
	// The one deliberate modal in this whole feature set: applying a
	// redaction permanently strips content, even before the next save, so
	// it gets a real confirmation rather than the inline-bar pattern used
	// everywhere else.
	int r = MessageBoxW(hwnd_,
		L"This permanently removes the marked content from the document. This cannot be undone. Continue?",
		L"Apply Redactions", MB_YESNO | MB_ICONWARNING);
	if (r != IDYES) return;
	if (canvas_) canvas_->flushPendingEdit();
	std::string err;
	if (!doc_->applyRedactions(err)) {
		std::wstring wmsg(err.begin(), err.end());
		MessageBoxW(hwnd_, wmsg.empty() ? L"Failed to apply redactions." : wmsg.c_str(),
			L"Apply Redactions", MB_OK | MB_ICONERROR);
		return;
	}
	if (canvas_) canvas_->refreshAfterSave();
	if (thumbs_) thumbs_->refreshAfterSave();
	selectTool(IDM_TOOL_SELECT); // also refreshes/hides the redact bar
	showOpResultBar(true);
}

void FrameWindow::clearRedactions()
{
	if (!doc_ || !doc_->isOpen()) return;
	std::string err;
	if (doc_->clearPendingRedactions(err)) {
		if (canvas_) canvas_->refreshAfterSave();
		if (thumbs_) thumbs_->refreshAfterSave();
	}
	updateRedactBar();
}

void FrameWindow::enterOrganizeMode(std::vector<PdfDocument::PagePlanEntry> seed)
{
	if (!doc_ || !doc_->isOpen() || !doc_->isPdf() || !thumbs_) return;
	if (canvas_) canvas_->flushPendingEdit();
	if (seed.empty()) {
		for (int p = 0; p < doc_->pageCount(); ++p) seed.push_back({ -1, p, 0 });
	}
	organizeShownThumbsBefore_ = showThumbs_;
	thumbs_->setDocument(doc_); // no-op if already wired; cheap and idempotent
	thumbs_->enterOrganizeMode(std::move(seed));
	layout();
}

void FrameWindow::organizeInsertPages()
{
	if (!doc_ || !thumbs_ || !thumbs_->organizeMode()) return;
	auto files = PickFilesAcrossFolders(hwnd_, L"PDF Documents (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0", L"Insert Pages");
	int skipped = 0;
	for (auto& f : files) {
		std::string err;
		int idx = doc_->openExternalFile(f.c_str(), err);
		if (idx < 0) { ++skipped; continue; }
		thumbs_->organizeInsertFile(idx);
	}
	if (skipped > 0) {
		wchar_t msg[160];
		swprintf(msg, 160, L"%d file(s) could not be added (password-protected or unreadable).", skipped);
		MessageBoxW(hwnd_, msg, L"Insert Pages", MB_OK | MB_ICONWARNING);
	}
	layout();
}

void FrameWindow::organizeDone()
{
	if (!doc_ || !thumbs_ || !thumbs_->organizeMode()) return;
	auto order = thumbs_->organizeOrder();
	if (order.empty()) {
		MessageBoxW(hwnd_, L"A document needs at least one page.", L"Organize Pages", MB_OK | MB_ICONWARNING);
		return;
	}
	std::string err;
	if (!doc_->rebuildFromPages(order, err)) {
		std::wstring wmsg(err.begin(), err.end());
		MessageBoxW(hwnd_, wmsg.empty() ? L"Failed to apply changes." : wmsg.c_str(),
			L"Organize Pages", MB_OK | MB_ICONERROR);
		return;
	}
	thumbs_->exitOrganizeMode();
	showThumbs_ = organizeShownThumbsBefore_;
	// Merge into a blank tab never went through finishOpenDocument(), so the
	// canvas/thumbs were never pointed at this tab's PdfDocument and the tab
	// has no name yet. Wire them up now (idempotent for an existing doc) and
	// give the untitled result a default name so Save-As has something to
	// pre-fill / the title bar has something to show.
	DocTab* at = tabs_[activeTab_].get();
	if (canvas_) canvas_->setDocument(doc_);
	if (thumbs_) thumbs_->setDocument(doc_);
	if (at->name.empty()) at->name = L"Untitled.pdf";
	if (canvas_) canvas_->refreshAfterSave();
	if (thumbs_) thumbs_->refreshAfterSave();
	updatePageEditBox();
	updateTitle();
	layout();
	showOpResultBar(true);
}

void FrameWindow::organizeCancel()
{
	if (!doc_ || !thumbs_ || !thumbs_->organizeMode()) return;
	doc_->closeExternalFiles();
	thumbs_->exitOrganizeMode();
	showThumbs_ = organizeShownThumbsBefore_;
	layout();
}

void FrameWindow::doMerge()
{
	auto files = PickFilesAcrossFolders(hwnd_, L"PDF Documents (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0", L"Select PDF Files");
	if (files.empty()) return;
	// Like openDocument(): never disturb an already-open document -- merge
	// always lands in a fresh tab (or reuses the current one if it's
	// already empty), seeded only with the picked files' pages. The user
	// can still drag the tab's own pages in via "Insert Pages..." from
	// inside the organize panel if they actually want to fold them in too.
	if (doc_ && doc_->isOpen()) switchToTab(newTab());
	if (!thumbs_ || !doc_) return;

	std::vector<PdfDocument::PagePlanEntry> seed;
	int skipped = 0;
	for (auto& f : files) {
		std::string err;
		int idx = doc_->openExternalFile(f.c_str(), err);
		if (idx < 0) { ++skipped; continue; }
		int n = doc_->externalPageCount(idx);
		for (int p = 0; p < n; ++p) seed.push_back({ idx, p, 0 });
	}
	if (skipped > 0) {
		wchar_t msg[160];
		swprintf(msg, 160, L"%d file(s) could not be added (password-protected or unreadable).", skipped);
		MessageBoxW(hwnd_, msg, L"Merge PDFs", MB_OK | MB_ICONWARNING);
	}
	if (seed.empty()) return;
	organizeShownThumbsBefore_ = showThumbs_;
	// This tab's PdfDocument was never doc_->open()'d (doc_->isOpen() stays
	// false until rebuildFromPages() commits at Done) -- ThumbPanel only
	// ever learns its `doc_` pointer via setDocument(), which normally
	// happens in finishOpenDocument(). Without this, the panel has no
	// PdfDocument to render external-file thumbnails from and stays blank.
	thumbs_->setDocument(doc_);
	thumbs_->enterOrganizeMode(std::move(seed));
	layout();
}

void FrameWindow::doConvertToPdf()
{
	static const wchar_t kFilter[] =
		L"Convertible Files (*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.txt;*.md;*.docx;*.pdf)\0"
		L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.txt;*.md;*.docx;*.pdf\0"
		L"Images (*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff)\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff\0"
		L"Text/Markdown (*.txt;*.md)\0*.txt;*.md\0"
		L"Word Documents (*.docx)\0*.docx\0"
		L"PDF Documents (*.pdf)\0*.pdf\0"
		L"All Files (*.*)\0*.*\0";
	auto files = PickFilesAcrossFolders(hwnd_, kFilter, L"Select Files to Convert to PDF");
	if (files.empty()) return;

	// Output lands next to the first picked file, named after its stem --
	// never silently overwrites an unrelated existing file of that name.
	const std::wstring& first = files[0];
	size_t slash = first.find_last_of(L'\\');
	std::wstring folder = (slash == std::wstring::npos) ? L"." : first.substr(0, slash);
	std::wstring base = (slash == std::wstring::npos) ? first : first.substr(slash + 1);
	size_t dot = base.find_last_of(L'.');
	if (dot != std::wstring::npos) base = base.substr(0, dot);
	std::wstring outPath = folder + L"\\" + base + L".pdf";
	for (int n = 1; GetFileAttributesW(outPath.c_str()) != INVALID_FILE_ATTRIBUTES && n <= 20; ++n) {
		wchar_t suffix[32];
		swprintf(suffix, 32, n == 1 ? L"_converted.pdf" : L"_converted%d.pdf", n);
		outPath = folder + L"\\" + base + suffix;
	}

	std::string err;
	std::vector<std::wstring> skipped;
	bool ok = PdfDocument::ConvertFilesToPdf(files, outPath.c_str(), err, &skipped);
	if (!ok) {
		std::wstring wmsg(err.begin(), err.end());
		MessageBoxW(hwnd_, wmsg.empty() ? L"Failed to convert the selected file(s)." : wmsg.c_str(),
			L"Convert to PDF", MB_OK | MB_ICONERROR);
		return;
	}
	if (!skipped.empty()) {
		wchar_t msg[160];
		swprintf(msg, 160, L"%d file(s) could not be converted and were skipped (unsupported type or unreadable).",
			static_cast<int>(skipped.size()));
		MessageBoxW(hwnd_, msg, L"Convert to PDF", MB_OK | MB_ICONWARNING);
	}
	openDocument(outPath.c_str());
}

void FrameWindow::showWebPdfBar(bool show)
{
	webPdfVisible_ = show;
	int sw = show ? SW_SHOW : SW_HIDE;
	for (HWND h : { webPdfLabel_, webPdfEdit_, webPdfButton_, webPdfClose_ })
		ShowWindow(h, sw);
	if (show) SetWindowTextW(webPdfEdit_, L"");
	layout();
	if (show) SetFocus(webPdfEdit_);
}

void FrameWindow::runWebToPdf()
{
	wchar_t buf[2048] = {};
	GetWindowTextW(webPdfEdit_, buf, 2048);
	std::wstring url = buf;
	size_t a = url.find_first_not_of(L" \t");
	if (a == std::wstring::npos) { MessageBeep(MB_ICONWARNING); return; }
	size_t b = url.find_last_not_of(L" \t");
	url = url.substr(a, b - a + 1);
	if (url.find(L"://") == std::wstring::npos) url = L"https://" + url; // bare "example.com" -> a real URL

	// Default filename from the URL's host (e.g. "https://example.com/x" ->
	// "example.com.pdf"), falling back to something generic if that parse
	// comes up empty (e.g. a bare "https://").
	std::wstring host = url;
	size_t schemeEnd = host.find(L"://");
	if (schemeEnd != std::wstring::npos) host = host.substr(schemeEnd + 3);
	size_t slash = host.find_first_of(L"/?#");
	if (slash != std::wstring::npos) host = host.substr(0, slash);
	for (wchar_t c : std::wstring(L"\\/:*?\"<>|")) std::replace(host.begin(), host.end(), c, L'_');
	if (host.empty()) host = L"webpage";

	// host (e.g. "example.com") contains a dot, so the Save dialog would
	// treat ".com" as an already-given extension and skip appending
	// lpstrDefExt -- append ".pdf" explicitly instead of relying on that.
	wchar_t file[MAX_PATH] = L"";
	wcscpy_s(file, (host + L".pdf").c_str());
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hwnd_;
	ofn.lpstrFilter = L"PDF Documents (*.pdf)\0*.pdf\0";
	ofn.lpstrFile = file;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrDefExt = L"pdf";
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
	ofn.lpstrTitle = L"Save Web Page as PDF";
	if (!GetSaveFileNameW(&ofn)) return; // bar stays open -- user can retry or Cancel

	// This blocks the UI thread for as long as the page takes to load and
	// print (see ConvertWebPageToPdf's header comment) -- flip the button to
	// a busy state so it's clear something is happening, since there's no
	// real progress to report.
	EnableWindow(webPdfEdit_, FALSE);
	EnableWindow(webPdfButton_, FALSE);
	SetWindowTextW(webPdfButton_, L"Converting...");
	UpdateWindow(hwnd_);

	std::string err;
	bool ok = ConvertWebPageToPdf(url.c_str(), file, err);

	SetWindowTextW(webPdfButton_, L"Convert");
	EnableWindow(webPdfEdit_, TRUE);
	EnableWindow(webPdfButton_, TRUE);

	if (!ok) {
		std::wstring wmsg(err.begin(), err.end());
		MessageBoxW(hwnd_, wmsg.empty() ? L"Failed to convert the page." : wmsg.c_str(),
			L"Web Page to PDF", MB_OK | MB_ICONERROR);
		return;
	}
	showWebPdfBar(false);
	openDocument(file);
}

LRESULT CALLBACK FrameWindow::EditSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
	UINT_PTR, DWORD_PTR ref)
{
	auto* self = reinterpret_cast<FrameWindow*>(ref);
	if (msg == WM_KEYDOWN) {
		if (wp == VK_RETURN) {
			bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
			if (shift && self->canvas_ && self->canvas_->hitCount() > 0) {
				self->canvas_->findPrev();
				self->updateSearchLabel();
			} else {
				self->runSearch();
			}
			return 0;
		}
		if (wp == VK_ESCAPE) { self->showSearchBar(false); return 0; }
		// EM_REPLACESEL below fires its own EN_CHANGE, which arms the normal
		// search-as-you-type debounce timer -- no need to call liveSearch()
		// here directly.
		if (ConsumeCtrlBackspaceWordDelete(hwnd, wp)) return 0;
		if (ConsumeCtrlSelectAll(hwnd, wp)) return 0;
	}
	// The stock edit proc turns the WM_KEYDOWN(Ctrl+VK_BACK) handled above
	// into a WM_CHAR(0x7F) right after -- swallow it too, or it re-inserts
	// the same "DEL" control character we just deleted.
	if (msg == WM_CHAR && (wp == VK_RETURN || wp == 0x1B || wp == 0x7F)) return 0; // swallow beep
	return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK FrameWindow::PwdEditSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
	UINT_PTR, DWORD_PTR ref)
{
	auto* self = reinterpret_cast<FrameWindow*>(ref);
	if (msg == WM_KEYDOWN) {
		if (wp == VK_RETURN) { self->submitPassword(); return 0; }
		if (wp == VK_ESCAPE) { self->cancelPasswordPrompt(); return 0; }
		if (ConsumeCtrlBackspaceWordDelete(hwnd, wp)) return 0;
		if (ConsumeCtrlSelectAll(hwnd, wp)) return 0;
	}
	if (msg == WM_CHAR && (wp == VK_RETURN || wp == 0x1B || wp == 0x7F)) return 0; // swallow beep
	return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK FrameWindow::SplitEditSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
	UINT_PTR, DWORD_PTR ref)
{
	auto* self = reinterpret_cast<FrameWindow*>(ref);
	if (msg == WM_KEYDOWN) {
		if (wp == VK_RETURN) { self->runSplit(); return 0; }
		if (wp == VK_ESCAPE) { self->showSplitBar(false); return 0; }
		if (ConsumeCtrlBackspaceWordDelete(hwnd, wp)) return 0;
		if (ConsumeCtrlSelectAll(hwnd, wp)) return 0;
	}
	if (msg == WM_CHAR && (wp == VK_RETURN || wp == 0x1B || wp == 0x7F)) return 0; // swallow beep
	return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK FrameWindow::SetPwdEditSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
	UINT_PTR, DWORD_PTR ref)
{
	auto* self = reinterpret_cast<FrameWindow*>(ref);
	if (msg == WM_KEYDOWN) {
		if (wp == VK_RETURN) { self->runSetPassword(); return 0; }
		if (wp == VK_ESCAPE) { self->showSetPasswordBar(false); return 0; }
		if (ConsumeCtrlBackspaceWordDelete(hwnd, wp)) return 0;
		if (ConsumeCtrlSelectAll(hwnd, wp)) return 0;
	}
	if (msg == WM_CHAR && (wp == VK_RETURN || wp == 0x1B || wp == 0x7F)) return 0; // swallow beep
	return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK FrameWindow::WebPdfEditSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
	UINT_PTR, DWORD_PTR ref)
{
	auto* self = reinterpret_cast<FrameWindow*>(ref);
	if (msg == WM_KEYDOWN) {
		if (wp == VK_RETURN) { self->runWebToPdf(); return 0; }
		if (wp == VK_ESCAPE) { self->showWebPdfBar(false); return 0; }
		if (ConsumeCtrlBackspaceWordDelete(hwnd, wp)) return 0;
		if (ConsumeCtrlSelectAll(hwnd, wp)) return 0;
	}
	if (msg == WM_CHAR && (wp == VK_RETURN || wp == 0x1B || wp == 0x7F)) return 0; // swallow beep
	return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK FrameWindow::PageEditSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
	UINT_PTR, DWORD_PTR ref)
{
	auto* self = reinterpret_cast<FrameWindow*>(ref);
	if (msg == WM_KEYDOWN && wp == VK_RETURN) {
		self->endPageNumberEdit(true);
		return 0;
	}
	if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
		self->endPageNumberEdit(false);
		return 0;
	}
	if (msg == WM_KEYDOWN && ConsumeCtrlBackspaceWordDelete(hwnd, wp)) return 0;
	if (msg == WM_KEYDOWN && ConsumeCtrlSelectAll(hwnd, wp)) return 0;
	if (msg == WM_CHAR && (wp == VK_RETURN || wp == 0x1B || wp == 0x7F)) return 0; // swallow beep
	if (msg == WM_KILLFOCUS) {
		// No Enter pressed -- discard the half-typed value; endPageNumberEdit
		// destroys the box and reverts to the plain owner-drawn "N of M" text.
		self->endPageNumberEdit(false);
	}
	return DefSubclassProc(hwnd, msg, wp, lp);
}

// Creates the transient page-number EDIT control over the toolbar's reserved
// IDM_VIEW_PAGELABEL rect and focuses it -- see pageEditActive_'s comment on
// why this is created on demand instead of permanently overlaid.
void FrameWindow::beginPageNumberEdit()
{
	if (pageEditActive_ || !canvas_ || !doc_ || !doc_->isOpen() || !toolbar_) return;
	RECT pr = {};
	if (!SendMessageW(toolbar_, TB_GETRECT, IDM_VIEW_PAGELABEL, reinterpret_cast<LPARAM>(&pr))) return;
	MapWindowPoints(toolbar_, hwnd_, reinterpret_cast<POINT*>(&pr), 2);
	UINT dpi = GetDpiForWindow(hwnd_);
	int totalW = pr.right - pr.left;
	int editW = PageEditBoxWidth(totalW, dpi);
	int editH = Scale(22, dpi);
	int py = pr.top + ((pr.bottom - pr.top) - editH) / 2;
	// Flat themed border (WS_BORDER + SetWindowTheme "Explorer") instead of
	// the classic sunken WS_EX_CLIENTEDGE look, to match the flat modern
	// chrome everywhere else.
	pageEditActive_ = CreateWindowExW(0, L"EDIT", L"",
		WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_CENTER | ES_NUMBER,
		pr.left, py, editW, editH, hwnd_, reinterpret_cast<HMENU>(IDC_PAGE_EDIT), hInst_, nullptr);
	if (!pageEditActive_) return;
	SetWindowTheme(pageEditActive_, L"Explorer", nullptr);
	HFONT f = uiFont_ ? uiFont_ : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	SendMessageW(pageEditActive_, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
	SetWindowSubclass(pageEditActive_, PageEditSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
	wchar_t buf[16];
	swprintf(buf, 16, L"%d", canvas_->currentPage() + 1);
	SetWindowTextW(pageEditActive_, buf);
	SetFocus(pageEditActive_);
	SendMessageW(pageEditActive_, EM_SETSEL, 0, -1);
}

// Destroys the transient page-number EDIT control, optionally jumping to the
// typed value first (Enter), or just discarding it (Escape/focus-loss).
void FrameWindow::endPageNumberEdit(bool commit)
{
	if (!pageEditActive_) return;
	if (commit && canvas_ && doc_ && doc_->isOpen()) {
		wchar_t buf[16] = {};
		GetWindowTextW(pageEditActive_, buf, 16);
		int n = _wtoi(buf);
		if (n >= 1 && n <= doc_->pageCount()) canvas_->goToPage(n - 1);
	}
	HWND old = pageEditActive_;
	pageEditActive_ = nullptr;
	DestroyWindow(old);
	updatePageEditBox(); // refresh pageLabelText_ to the (possibly just-jumped) current page
	if (canvas_) SetFocus(canvas_->hwnd());
}

void FrameWindow::updatePageEditBox()
{
	if (!doc_ || !doc_->isOpen()) {
		pageLabelText_.clear();
	} else {
		wchar_t buf[40];
		swprintf(buf, 40, L"%d of %d", canvas_->currentPage() + 1, doc_->pageCount());
		pageLabelText_ = buf;
	}
	if (toolbar_) InvalidateRect(toolbar_, nullptr, FALSE);
}

void FrameWindow::layout()
{
	RECT rc; GetClientRect(hwnd_, &rc);
	UINT dpi = GetDpiForWindow(hwnd_);
	int fullW = rc.right - rc.left;

	SendMessageW(status_, WM_SIZE, 0, 0);
	layoutStatusParts();

	int tabStripH = Scale(30, dpi);

	// Tab strip sits right below the title bar (Edge-style), toolbar below
	// that -- the reverse of comctl32's own default toolbar behavior (which
	// auto-docks to (0,0) full-width), so the toolbar is created with
	// CCS_NOPARENTALIGN|CCS_NORESIZE (see its creation comment) and fully
	// positioned/sized here instead. It needs a real width via MoveWindow
	// BEFORE TB_AUTOSIZE, since that's what TB_AUTOSIZE uses to figure out
	// button wrapping (TBSTYLE_WRAPABLE) and its own resulting height.
	MoveWindow(toolbar_, 0, tabStripH, fullW, Scale(40, dpi), TRUE);
	SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
	RECT tb; GetWindowRect(toolbar_, &tb);
	int tbH = tb.bottom - tb.top;
	// TB_AUTOSIZE only recomputes height for the width already in place --
	// re-set the position with that final height so anything below it lines
	// up exactly (matters if WRAPABLE ever kicks in and grows past one row).
	MoveWindow(toolbar_, 0, tabStripH, fullW, tbH, TRUE);

	// The zoom-%/page-number readouts are plain owner-drawn text now (see
	// zoomText_'s comment) -- nothing to position here. If the transient
	// page-number EDIT control happens to be up during a resize, keep it
	// aligned with its reserved rect using the same math beginPageNumberEdit()
	// used to create it.
	if (pageEditActive_) {
		RECT pr = {};
		if (SendMessageW(toolbar_, TB_GETRECT, IDM_VIEW_PAGELABEL, reinterpret_cast<LPARAM>(&pr))) {
			MapWindowPoints(toolbar_, hwnd_, reinterpret_cast<POINT*>(&pr), 2);
			int totalW = pr.right - pr.left;
			int editW = PageEditBoxWidth(totalW, dpi);
			int editH = Scale(22, dpi);
			int py = pr.top + ((pr.bottom - pr.top) - editH) / 2;
			MoveWindow(pageEditActive_, pr.left, py, editW, editH, TRUE);
		}
	}

	MoveWindow(tabStrip_, 0, 0, fullW, tabStripH, TRUE);
	updateTabWidths(fullW);

	RECT sb; GetWindowRect(status_, &sb);
	int sbH = sb.bottom - sb.top;

	int top = tabStripH + tbH;
	int fullW0 = fullW;
	if (searchVisible_) {
		int y = top + Scale(4, dpi);
		int h = Scale(24, dpi);
		int pad = Scale(6, dpi);
		int x = pad;
		int editW = Scale(240, dpi);
		int btnW = Scale(76, dpi);
		int closeW = Scale(60, dpi);
		MoveWindow(searchEdit_, x, y, editW, h, TRUE); x += editW + pad;
		MoveWindow(searchPrev_, x, y, btnW, h, TRUE); x += btnW + pad;
		MoveWindow(searchNext_, x, y, btnW, h, TRUE); x += btnW + pad;
		MoveWindow(searchClose_, x, y, closeW, h, TRUE); x += closeW + pad;
		// The label shows the match counter to the right of the buttons.
		MoveWindow(searchLabel_, x, y, std::max(0, fullW0 - x - pad), h, TRUE);
		top += searchBarH_;
	}
	if (pwdVisible_) {
		int y = top + Scale(4, dpi);
		int h = Scale(24, dpi);
		int pad = Scale(6, dpi);
		int x = pad;
		int lblW = Scale(260, dpi);
		int editW = Scale(160, dpi);
		int btnW = Scale(76, dpi);
		MoveWindow(pwdLabel_, x, y, lblW, h, TRUE); x += lblW + pad;
		MoveWindow(pwdEdit_, x, y, editW, h, TRUE); x += editW + pad;
		MoveWindow(pwdUnlock_, x, y, btnW, h, TRUE); x += btnW + pad;
		MoveWindow(pwdCancel_, x, y, btnW, h, TRUE);
		top += pwdBarH_;
	}
	if (protVisible_) {
		int y = top + Scale(4, dpi);
		int h = Scale(24, dpi);
		int pad = Scale(6, dpi);
		int btnW1 = Scale(120, dpi);
		int btnW2 = Scale(140, dpi);
		int closeW = Scale(60, dpi);
		// Buttons anchor to the right; the label fills the remaining space.
		int x = fullW0 - pad - closeW;
		MoveWindow(protClose_, x, y, closeW, h, TRUE); x -= pad + btnW2;
		MoveWindow(protRemoveRestrictions_, x, y, btnW2, h, TRUE); x -= pad + btnW1;
		MoveWindow(protRemovePwd_, x, y, btnW1, h, TRUE); x -= pad;
		MoveWindow(protLabel_, Scale(6, dpi), y, std::max(0, x - Scale(6, dpi)), h, TRUE);
		top += protBarH_;
	}
	if (opResultVisible_) {
		int y = top + Scale(4, dpi);
		int h = Scale(24, dpi);
		int pad = Scale(6, dpi);
		int btnW1 = Scale(70, dpi);
		int btnW2 = Scale(120, dpi);
		int closeW = Scale(80, dpi);
		int x = fullW0 - pad - closeW;
		MoveWindow(opResultClose_, x, y, closeW, h, TRUE); x -= pad + btnW2;
		MoveWindow(opResultSaveAs_, x, y, btnW2, h, TRUE); x -= pad + btnW1;
		MoveWindow(opResultSave_, x, y, btnW1, h, TRUE); x -= pad;
		MoveWindow(opResultLabel_, Scale(6, dpi), y, std::max(0, x - Scale(6, dpi)), h, TRUE);
		top += opResultBarH_;
	}
	if (splitVisible_) {
		int y = top + Scale(4, dpi);
		int h = Scale(24, dpi);
		int pad = Scale(6, dpi);
		int x = pad;
		int lblW = Scale(220, dpi);
		int editW = Scale(160, dpi);
		int btnW = Scale(60, dpi);
		int closeW = Scale(60, dpi);
		MoveWindow(splitLabel_, x, y, lblW, h, TRUE); x += lblW + pad;
		MoveWindow(splitEdit_, x, y, editW, h, TRUE); x += editW + pad;
		MoveWindow(splitButton_, x, y, btnW, h, TRUE); x += btnW + pad;
		MoveWindow(splitClose_, x, y, closeW, h, TRUE); x += closeW + pad;
		MoveWindow(splitResult_, x, y, std::max(0, fullW0 - x - pad), h, TRUE);
		top += splitBarH_;
	}
	if (setPwdVisible_) {
		int y = top + Scale(4, dpi);
		int h = Scale(24, dpi);
		int pad = Scale(6, dpi);
		int x = pad;
		int lblW = Scale(220, dpi);
		int editW = Scale(160, dpi);
		int btnW = Scale(100, dpi);
		int closeW = Scale(60, dpi);
		MoveWindow(setPwdLabel_, x, y, lblW, h, TRUE); x += lblW + pad;
		MoveWindow(setPwdEdit_, x, y, editW, h, TRUE); x += editW + pad;
		MoveWindow(setPwdButton_, x, y, btnW, h, TRUE); x += btnW + pad;
		MoveWindow(setPwdClose_, x, y, closeW, h, TRUE);
		top += setPwdBarH_;
	}
	if (webPdfVisible_) {
		int y = top + Scale(4, dpi);
		int h = Scale(24, dpi);
		int pad = Scale(6, dpi);
		int x = pad;
		int lblW = Scale(90, dpi);
		int btnW = Scale(76, dpi);
		int closeW = Scale(60, dpi);
		MoveWindow(webPdfLabel_, x, y, lblW, h, TRUE); x += lblW + pad;
		int editW = std::max(0, fullW0 - x - pad - btnW - pad - closeW - pad);
		MoveWindow(webPdfEdit_, x, y, editW, h, TRUE); x += editW + pad;
		MoveWindow(webPdfButton_, x, y, btnW, h, TRUE); x += btnW + pad;
		MoveWindow(webPdfClose_, x, y, closeW, h, TRUE);
		top += webPdfBarH_;
	}
	if (redactBarVisible_) {
		int y = top + Scale(4, dpi);
		int h = Scale(24, dpi);
		int pad = Scale(6, dpi);
		int btnW1 = Scale(130, dpi);
		int btnW2 = Scale(80, dpi);
		int btnW3 = Scale(60, dpi);
		int x = fullW0 - pad - btnW3;
		MoveWindow(redactDone_, x, y, btnW3, h, TRUE); x -= pad + btnW2;
		MoveWindow(redactClear_, x, y, btnW2, h, TRUE); x -= pad + btnW1;
		MoveWindow(redactApply_, x, y, btnW1, h, TRUE); x -= pad;
		MoveWindow(redactLabel_, Scale(6, dpi), y, std::max(0, x - Scale(6, dpi)), h, TRUE);
		top += redactBarH_;
	}
	int bottom = (rc.bottom - rc.top) - sbH;
	int midH = std::max(0, bottom - top);

	bool organizing = thumbs_ && thumbs_->organizeMode();
	// Wider than the plain nav-thumbnail column -- organize mode's rows also
	// carry rotate/delete hit-zones (see ThumbPanel::organizeIconRects) and
	// the action strip below needs room for three buttons.
	int thumbW = organizing ? Scale(kBaseThumbWidth + 80, dpi)
		: (showThumbs_ ? Scale(kBaseThumbWidth + 24, dpi) : 0);
	int thumbMidH = midH;
	if (organizing) thumbMidH = std::max(0, midH - organizeBarH_);
	if (thumbs_) {
		ShowWindow(thumbs_->hwnd(), (showThumbs_ || organizing) ? SW_SHOW : SW_HIDE);
		if (showThumbs_ || organizing)
			MoveWindow(thumbs_->hwnd(), 0, top, thumbW, thumbMidH, TRUE);
	}
	int osw = organizing ? SW_SHOW : SW_HIDE;
	for (HWND h : { organizeInsert_, organizeDone_, organizeCancel_ }) ShowWindow(h, osw);
	if (organizing) {
		int y = top + thumbMidH + Scale(4, dpi);
		int h = Scale(24, dpi);
		int pad = Scale(4, dpi);
		// "Insert..." needs more room than the two short labels.
		int totalW = std::max(0, thumbW - pad * 4);
		int bwInsert = totalW * 4 / 10;
		int bwDone = totalW * 3 / 10;
		int bwCancel = totalW - bwInsert - bwDone;
		int x = pad;
		MoveWindow(organizeInsert_, x, y, bwInsert, h, TRUE); x += bwInsert + pad;
		MoveWindow(organizeDone_, x, y, bwDone, h, TRUE); x += bwDone + pad;
		MoveWindow(organizeCancel_, x, y, bwCancel, h, TRUE);
	}
	// Print and text-editor panels are mutually exclusive (see
	// showPrintPanel()/showTextEditorPanel()) and share one right-hand
	// column width so switching between them doesn't jitter the canvas size.
	int rightPanelW = (printPanelVisible_ || textPanelVisible_) ? Scale(300, dpi) : 0;
	int printPanelW = rightPanelW;
	if (printPanelVisible_) {
		int pad = Scale(10, dpi);
		int rowH = Scale(22, dpi);
		int gap = Scale(6, dpi);
		int px = fullW - printPanelW + pad;
		int pw = printPanelW - pad * 2;
		int y = top + pad;
		auto row = [&](HWND h, int w = -1) {
			MoveWindow(h, px, y, w < 0 ? pw : w, rowH, TRUE);
			y += rowH + gap;
		};
		row(printTitle_);
		y += gap;
		row(printPrinterLabel_);
		row(printPrinterCombo_);
		y += gap;
		int halfW = (pw - gap) / 2;
		MoveWindow(printCopiesLabel_, px, y, pw, rowH, TRUE); y += rowH;
		MoveWindow(printCopiesEdit_, px, y, Scale(70, dpi), rowH, TRUE); y += rowH + gap * 2;
		row(printRangeLabel_);
		row(printRangeAll_);
		row(printRangeCurrent_);
		row(printRangeCustom_);
		row(printRangeEdit_);
		y += gap;
		row(printOrientLabel_);
		MoveWindow(printOrientPortrait_, px, y, halfW, rowH, TRUE);
		MoveWindow(printOrientLandscape_, px + halfW + gap, y, halfW, rowH, TRUE);
		y += rowH + gap * 2;
		row(printColorLabel_);
		MoveWindow(printColorColor_, px, y, halfW, rowH, TRUE);
		MoveWindow(printColorGray_, px + halfW + gap, y, halfW, rowH, TRUE);
		y += rowH + gap * 3;
		int navBtnW = Scale(32, dpi);
		int navLabelW = std::max(0, pw - navBtnW * 2 - gap * 2);
		MoveWindow(printPageNavPrev_, px, y, navBtnW, rowH, TRUE);
		MoveWindow(printPageNavLabel_, px + navBtnW + gap, y, navLabelW, rowH, TRUE);
		MoveWindow(printPageNavNext_, px + navBtnW + gap + navLabelW + gap, y, navBtnW, rowH, TRUE);
		y += rowH + gap * 3;
		int btnH = Scale(28, dpi);
		MoveWindow(printGo_, px, y, halfW, btnH, TRUE);
		MoveWindow(printCancel_, px + halfW + gap, y, halfW, btnH, TRUE);
	}
	int psw = printPanelVisible_ ? SW_SHOW : SW_HIDE;
	for (HWND h : { printTitle_, printPrinterLabel_, printPrinterCombo_, printCopiesLabel_, printCopiesEdit_,
		printRangeLabel_, printRangeAll_, printRangeCurrent_, printRangeCustom_, printRangeEdit_,
		printOrientLabel_, printOrientPortrait_, printOrientLandscape_,
		printColorLabel_, printColorColor_, printColorGray_,
		printPageNavLabel_, printPageNavPrev_, printPageNavNext_, printGo_, printCancel_ })
		ShowWindow(h, psw);

	if (textPanelVisible_) {
		int pad = Scale(10, dpi);
		int rowH = Scale(22, dpi);
		int gap = Scale(6, dpi);
		int px = fullW - rightPanelW + pad;
		int pw = rightPanelW - pad * 2;
		int y = top + pad;
		MoveWindow(textPanelTitle_, px, y, pw, rowH, TRUE);
		y += rowH + gap;
		int fmtBtnW = (pw - gap * 2) / 3;
		MoveWindow(textBoldBtn_, px, y, fmtBtnW, rowH, TRUE);
		MoveWindow(textItalicBtn_, px + fmtBtnW + gap, y, fmtBtnW, rowH, TRUE);
		MoveWindow(textUnderlineBtn_, px + (fmtBtnW + gap) * 2, y, pw - (fmtBtnW + gap) * 2, rowH, TRUE);
		y += rowH + gap * 2;
		MoveWindow(textCaseLabel_, px, y, pw, rowH, TRUE);
		y += rowH + gap;
		MoveWindow(textCaseCombo_, px, y, pw, rowH * 6, TRUE); // extra height = dropdown list allowance
		y += rowH + gap * 2;
		int bottomH = Scale(28, dpi);
		// Reserve room for BOTH bottom buttons (copy + close), not just one --
		// this previously reserved only a single bottomH + pad*2, so the close
		// button ended up drawn bottomH+gap-pad past the panel's bottom edge,
		// overlapping the status bar and becoming unclickable.
		int richH = std::max(0, midH - (y - top) - bottomH * 2 - gap - pad);
		MoveWindow(textRichEdit_, px, y, pw, richH, TRUE);
		y += richH + pad;
		MoveWindow(textCopyBtn_, px, y, pw, bottomH, TRUE);
		y += bottomH + gap;
		MoveWindow(textCloseBtn_, px, y, pw, bottomH, TRUE);
	}
	int tsw = textPanelVisible_ ? SW_SHOW : SW_HIDE;
	for (HWND h : { textPanelTitle_, textBoldBtn_, textItalicBtn_, textUnderlineBtn_,
		textCaseLabel_, textCaseCombo_, textRichEdit_, textCopyBtn_, textCloseBtn_ })
		ShowWindow(h, tsw);

	if (canvas_)
		MoveWindow(canvas_->hwnd(), thumbW, top, fullW - thumbW - rightPanelW, midH, TRUE);
}

int FrameWindow::newTab()
{
	auto tab = std::make_unique<DocTab>();
	tab->doc = std::make_unique<PdfDocument>();
	tab->thumbs = std::make_unique<ThumbPanel>(hwnd_, hInst_);
	tab->canvas = std::make_unique<CanvasView>(hwnd_, hInst_);
	tab->canvas->setStatusBar(status_);
	tab->canvas->setThumbPanel(tab->thumbs.get());
	tab->thumbs->setCanvas(tab->canvas.get());
	tab->canvas->setOnChanged([this] { updateTitle(); updateRedactBar(); });
	tab->canvas->setOnExitTextTool([this] { selectTool(IDM_TOOL_SELECT); });
	tab->canvas->setOnViewChanged([this] {
		updateZoomLabel();
		updatePageEditBox();
		// Wheel-driven print-preview paging (see CanvasView::onWheel) moves
		// the preview cursor directly on the canvas; keep the panel's "Page
		// N of M" readout and prev/next enabled-state in sync with it.
		if (printPanelVisible_ && canvas_ && canvas_->printPreviewActive()) updatePrintPreviewFromPanel();
	});
	tab->canvas->setOnEmptyStateAction([this](int cmdId) { onCommand(cmdId); });
	tab->canvas->setOnOpenTextEditor([this](const std::wstring& text) { showTextEditorPanel(true, text); });
	tab->canvas->setDarkMode(isDark_);
	tab->thumbs->setDarkMode(isDark_);
	// Both start hidden; switchToTab() shows whichever becomes active.
	ShowWindow(tab->canvas->hwnd(), SW_HIDE);
	ShowWindow(tab->thumbs->hwnd(), SW_HIDE);

	int idx = static_cast<int>(tabs_.size());
	wchar_t label[] = L"New Tab";
	TCITEMW item = {};
	item.mask = TCIF_TEXT;
	item.pszText = label;
	SendMessageW(tabStrip_, TCM_INSERTITEMW, idx, reinterpret_cast<LPARAM>(&item));

	tabs_.push_back(std::move(tab));
	return idx;
}

void FrameWindow::switchToTab(int idx)
{
	if (idx < 0 || idx >= static_cast<int>(tabs_.size())) return;
	// A pending password prompt belongs to whichever tab is being left --
	// abandon it rather than carrying stale UI state to the new tab.
	if (pwdVisible_) showPasswordBar(false);
	// Same for an in-progress Organize session: nothing was committed to
	// doc_ yet, so this is a clean discard, not a lost-work situation.
	if (thumbs_ && thumbs_->organizeMode()) organizeCancel();
	// And the print panel -- its live preview lives on the CURRENT tab's
	// CanvasView, which is about to stop being canvas_. Nothing was
	// committed either (Print executes immediately, it doesn't leave
	// pending state), so this is a clean discard too.
	if (printPanelVisible_) showPrintPanel(false);
	// Same for the text-editor panel: its content came from whichever tab's
	// selection was active, so it doesn't make sense to carry it across a
	// tab switch either (and nothing here is "in progress" -- Copy already
	// happened if the user wanted it).
	if (textPanelVisible_) showTextEditorPanel(false);
	if (activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size()) && activeTab_ != idx) {
		ShowWindow(tabs_[activeTab_]->canvas->hwnd(), SW_HIDE);
		ShowWindow(tabs_[activeTab_]->thumbs->hwnd(), SW_HIDE);
	}
	activeTab_ = idx;
	DocTab* t = tabs_[idx].get();
	doc_ = t->doc.get();
	canvas_ = t->canvas.get();
	thumbs_ = t->thumbs.get();
	ShowWindow(canvas_->hwnd(), SW_SHOW);

	if (static_cast<int>(SendMessageW(tabStrip_, TCM_GETCURSEL, 0, 0)) != idx)
		SendMessageW(tabStrip_, TCM_SETCURSEL, idx, 0);

	layout();
	updateTitle();
	updateZoomLabel();
	updatePageEditBox();
	updateProtectionBar();
	syncMenuChecks();

	// Sync the Select/Highlight/Draw/Text/Redact toolbar buttons to this tab's tool.
	int toolId = IDM_TOOL_SELECT;
	switch (canvas_->tool()) {
	case CanvasView::Tool::Highlight: toolId = IDM_TOOL_HIGHLIGHT; break;
	case CanvasView::Tool::Draw: toolId = IDM_TOOL_DRAW; break;
	case CanvasView::Tool::Erase: toolId = IDM_TOOL_ERASE; break;
	case CanvasView::Tool::Text: toolId = IDM_TOOL_TEXT; break;
	case CanvasView::Tool::Redact: toolId = IDM_TOOL_REDACT; break;
	default: break;
	}
	for (int b : { IDM_TOOL_SELECT, IDM_TOOL_HIGHLIGHT, IDM_TOOL_DRAW, IDM_TOOL_ERASE, IDM_TOOL_TEXT, IDM_TOOL_REDACT })
		SendMessageW(toolbar_, TB_CHECKBUTTON, b, MAKELPARAM(b == toolId, 0));
	updateRedactBar();

	InvalidateRect(hwnd_, nullptr, FALSE);
	SetFocus(canvas_->hwnd());
}

void FrameWindow::closeTab(int idx)
{
	if (idx < 0 || idx >= static_cast<int>(tabs_.size())) return;
	// Tear the print panel down before anything else: it holds a live
	// preview on canvas_'s CanvasView, and closing the tab that's CURRENTLY
	// active (the common case) skips the switchToTab() call below entirely
	// (activeTab_ == idx already), so that call's own printPanelVisible_
	// guard would otherwise only run later, after this tab's canvas has
	// already been DestroyWindow()'d -- a use-after-free on canvas_.
	if (printPanelVisible_) showPrintPanel(false);
	if (textPanelVisible_) showTextEditorPanel(false);
	// Make it active first so promptSaveIfDirty()/saveDocument() (which
	// operate on doc_/canvas_) see the right document.
	if (activeTab_ != idx) switchToTab(idx);
	if (!promptSaveIfDirty()) return; // user cancelled

	// Destroy the child windows before the tab's unique_ptrs go away --
	// CanvasView/ThumbPanel don't destroy their own HWND in their
	// destructor, and a lingering window whose GWLP_USERDATA now points at
	// freed memory would crash on the next message it receives.
	DestroyWindow(tabs_[idx]->canvas->hwnd());
	DestroyWindow(tabs_[idx]->thumbs->hwnd());
	SendMessageW(tabStrip_, TCM_DELETEITEM, idx, 0);
	tabs_.erase(tabs_.begin() + idx);
	clearTabMultiSelect(); // indices below would otherwise drift out of sync

	if (tabs_.empty()) {
		// Closing the last tab closes this window (browser-tab convention
		// would leave a blank tab behind; a single-purpose document app
		// instead quits, like closing the last document in Acrobat/Preview).
		// The whole app only exits once every such window is gone -- see
		// g_liveFrameCount's comment. The closed tab was already prompted-
		// for-save above, so there's nothing left to check here.
		DestroyWindow(hwnd_);
		return;
	}
	activeTab_ = -1; // force switchToTab to treat this as a real switch
	switchToTab(std::min(idx, static_cast<int>(tabs_.size()) - 1));
}

void FrameWindow::detachTabToNewWindow(int idx)
{
	detachTabsToNewWindow({ idx });
}

// Handles both a lone tab (idx alone) and a multi-selected group together --
// every selected tab's file lands as its own tab in ONE new window,
// preserving their relative order.
void FrameWindow::detachTabsToNewWindow(std::vector<int> indices)
{
	std::sort(indices.begin(), indices.end());
	indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
	indices.erase(std::remove_if(indices.begin(), indices.end(),
		[&](int i) { return i < 0 || i >= static_cast<int>(tabs_.size()); }), indices.end());
	if (indices.empty()) return;
	if (indices.size() == tabs_.size()) return; // would just recreate an equivalent window with everything in it

	// Moving the live CanvasView/ThumbPanel/PdfDocument to another top-level
	// window (reparenting HWNDs, re-pointing every callback that captured
	// `this`) is a lot of fragile surface for little benefit -- instead close
	// each tab here and reopen its file fresh in the new window, exactly like
	// a normal Open would. Requires a real path, so an unsaved/untitled tab
	// (e.g. a fresh Merge result before its first Save) can't be detached
	// this way yet -- those are silently skipped (with a warning if any were).
	std::vector<std::wstring> paths;
	int skippedUnsaved = 0;
	for (int idx : indices) {
		switchToTab(idx);
		if (tabs_[idx]->path.empty()) { ++skippedUnsaved; continue; }
		if (!promptSaveIfDirty()) return; // user cancelled -- abort the whole move
		paths.push_back(tabs_[idx]->path);
	}
	if (paths.empty()) { MessageBeep(MB_ICONWARNING); return; }
	if (skippedUnsaved > 0) {
		wchar_t msg[160];
		swprintf(msg, 160, L"%d unsaved tab(s) don't have a file yet and were left where they are. Save them first to move them.",
			skippedUnsaved);
		MessageBoxW(hwnd_, msg, L"Open in New Window", MB_OK | MB_ICONINFORMATION);
	}

	// Remove highest-index-first so earlier indices in `indices` stay valid.
	for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
		int idx = *it;
		if (tabs_[idx]->path.empty()) continue; // left in place above
		DestroyWindow(tabs_[idx]->canvas->hwnd());
		DestroyWindow(tabs_[idx]->thumbs->hwnd());
		SendMessageW(tabStrip_, TCM_DELETEITEM, idx, 0);
		tabs_.erase(tabs_.begin() + idx);
	}
	clearTabMultiSelect();
	if (tabs_.empty()) { DestroyWindow(hwnd_); return; }
	activeTab_ = -1;
	switchToTab(std::min(indices[0], static_cast<int>(tabs_.size()) - 1));

	SpawnNewWindow(hInst_, paths);
}

void FrameWindow::cycleTab(int dir)
{
	int n = static_cast<int>(tabs_.size());
	if (n < 2) return;
	switchToTab(((activeTab_ + dir) % n + n) % n);
}

void FrameWindow::updateTabLabel(int idx)
{
	if (idx < 0 || idx >= static_cast<int>(tabs_.size())) return;
	DocTab* t = tabs_[idx].get();
	std::wstring label = t->name.empty() ? L"New Tab" : t->name;
	if (label.size() > 24) label = label.substr(0, 22) + L"...";
	if (t->doc && t->doc->dirty()) label = L"* " + label;
	TCITEMW item = {};
	item.mask = TCIF_TEXT;
	item.pszText = const_cast<LPWSTR>(label.c_str());
	SendMessageW(tabStrip_, TCM_SETITEMW, idx, reinterpret_cast<LPARAM>(&item));
}

// Moves the DocTab at `from` to position `to` and mirrors the move in the
// native tab control. The control is owner-drawn and the only per-item state
// it holds is the label text (drawTabItem() re-reads it via TCM_GETITEMW each
// paint), so delete+reinsert loses nothing.
void FrameWindow::reorderTab(int from, int to)
{
	int n = static_cast<int>(tabs_.size());
	if (from == to || from < 0 || to < 0 || from >= n || to >= n) return;

	DocTab* activeDoc = (activeTab_ >= 0 && activeTab_ < n) ? tabs_[activeTab_].get() : nullptr;

	wchar_t label[128] = {};
	TCITEMW item = {};
	item.mask = TCIF_TEXT;
	item.pszText = label;
	item.cchTextMax = 128;
	SendMessageW(tabStrip_, TCM_GETITEMW, from, reinterpret_cast<LPARAM>(&item));

	auto moved = std::move(tabs_[from]);
	tabs_.erase(tabs_.begin() + from);
	tabs_.insert(tabs_.begin() + to, std::move(moved));

	SendMessageW(tabStrip_, TCM_DELETEITEM, from, 0);
	SendMessageW(tabStrip_, TCM_INSERTITEMW, to, reinterpret_cast<LPARAM>(&item));

	// tabs_ indices shifted around the moved element -- re-find whichever tab
	// was active (by identity, not old index) so canvas_/doc_/selection stay
	// pointed at the same document the user was looking at.
	if (activeDoc) {
		for (int i = 0; i < n; ++i) {
			if (tabs_[i].get() == activeDoc) { activeTab_ = i; break; }
		}
	}
	SendMessageW(tabStrip_, TCM_SETCURSEL, activeTab_, 0);
	InvalidateRect(tabStrip_, nullptr, FALSE);
}

// Like reorderTab(), but moves an entire multi-selected block of tabs to
// `to` as one contiguous unit, preserving their relative order -- used when
// dragging a tab that's part of an active multi-selection.
void FrameWindow::reorderTabGroup(std::vector<int> indices, int to)
{
	std::sort(indices.begin(), indices.end());
	indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
	int n = static_cast<int>(tabs_.size());
	if (indices.empty() || to < 0 || to >= n) return;

	DocTab* activeDoc = (activeTab_ >= 0 && activeTab_ < n) ? tabs_[activeTab_].get() : nullptr;

	// Same owner-drawn-control convention as reorderTab(): its only real
	// per-item state is the label text, so delete+reinsert loses nothing.
	std::vector<std::wstring> labels;
	for (int idx : indices) {
		wchar_t label[128] = {};
		TCITEMW item = {}; item.mask = TCIF_TEXT; item.pszText = label; item.cchTextMax = 128;
		SendMessageW(tabStrip_, TCM_GETITEMW, idx, reinterpret_cast<LPARAM>(&item));
		labels.emplace_back(label);
	}

	std::vector<std::unique_ptr<DocTab>> moving(indices.size());
	for (size_t k = 0; k < indices.size(); ++k) moving[k] = std::move(tabs_[indices[k]]);
	for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
		tabs_.erase(tabs_.begin() + *it);
		SendMessageW(tabStrip_, TCM_DELETEITEM, *it, 0);
	}

	// `to` was a position in the pre-removal indexing -- shift it down by
	// however many removed tabs sat at or before it, then clamp into the
	// now-shorter vector's valid insertion range.
	int adjTo = to;
	for (int idx : indices) if (idx < to) --adjTo;
	adjTo = std::clamp(adjTo, 0, static_cast<int>(tabs_.size()));

	for (size_t k = 0; k < moving.size(); ++k) {
		int pos = adjTo + static_cast<int>(k);
		tabs_.insert(tabs_.begin() + pos, std::move(moving[k]));
		TCITEMW item = {};
		item.mask = TCIF_TEXT;
		item.pszText = const_cast<LPWSTR>(labels[k].c_str());
		SendMessageW(tabStrip_, TCM_INSERTITEMW, pos, reinterpret_cast<LPARAM>(&item));
	}

	if (activeDoc) {
		for (int i = 0; i < static_cast<int>(tabs_.size()); ++i)
			if (tabs_[i].get() == activeDoc) { activeTab_ = i; break; }
	}
	multiSelectedTabs_.clear();
	for (size_t k = 0; k < moving.size(); ++k) multiSelectedTabs_.push_back(adjTo + static_cast<int>(k));
	SendMessageW(tabStrip_, TCM_SETCURSEL, activeTab_, 0);
	InvalidateRect(tabStrip_, nullptr, FALSE);
}

bool FrameWindow::isTabMultiSelected(int idx) const
{
	return std::find(multiSelectedTabs_.begin(), multiSelectedTabs_.end(), idx) != multiSelectedTabs_.end();
}

void FrameWindow::clearTabMultiSelect()
{
	if (multiSelectedTabs_.empty()) return;
	multiSelectedTabs_.clear();
	InvalidateRect(tabStrip_, nullptr, FALSE);
}

// Ctrl+click: toggle a tab in/out of the multi-selection without switching
// the active/shown tab (Explorer-style -- marking tabs for a bulk move
// shouldn't disrupt what's currently on screen).
void FrameWindow::toggleTabMultiSelect(int idx)
{
	if (idx < 0 || idx >= static_cast<int>(tabs_.size())) return;
	// A bare multi-selection of just the active tab isn't meaningful (every
	// group operation already treats a lone tab as "no selection"); seed it
	// with the active tab too so Ctrl+clicking a second tab immediately forms
	// a real 2-tab group instead of feeling like nothing happened.
	if (multiSelectedTabs_.empty()) multiSelectedTabs_.push_back(activeTab_);
	auto it = std::find(multiSelectedTabs_.begin(), multiSelectedTabs_.end(), idx);
	if (it != multiSelectedTabs_.end()) multiSelectedTabs_.erase(it);
	else multiSelectedTabs_.push_back(idx);
	if (multiSelectedTabs_.size() < 2) multiSelectedTabs_.clear(); // back down to "no selection"
	tabSelectAnchor_ = idx;
	InvalidateRect(tabStrip_, nullptr, FALSE);
}

// Shift+click: select every tab between the last plain-clicked tab and this
// one, inclusive.
void FrameWindow::selectTabRange(int fromIdx, int toIdx)
{
	int n = static_cast<int>(tabs_.size());
	if (fromIdx < 0 || fromIdx >= n || toIdx < 0 || toIdx >= n) return;
	int lo = std::min(fromIdx, toIdx), hi = std::max(fromIdx, toIdx);
	multiSelectedTabs_.clear();
	if (lo != hi) for (int i = lo; i <= hi; ++i) multiSelectedTabs_.push_back(i);
	InvalidateRect(tabStrip_, nullptr, FALSE);
}

// Shrinks the (TCS_FIXEDWIDTH) tab item width so all open tabs fit within
// barW, down to a floor that still leaves room for an ellipsized label and
// the close "x" -- instead of comctl32's default of keeping every tab at a
// fixed width and showing overflow spin-arrows once they no longer fit.
void FrameWindow::updateTabWidths(int barW)
{
	if (!tabStrip_) return;
	UINT dpi = GetDpiForWindow(hwnd_);
	int n = static_cast<int>(tabs_.size());
	if (n <= 0) n = 1;
	int maxW = Scale(160, dpi);
	int minW = Scale(56, dpi);
	int w = std::clamp(barW / n, minW, maxW);
	SendMessageW(tabStrip_, TCM_SETITEMSIZE, 0, MAKELPARAM(w, Scale(28, dpi)));
}

LRESULT CALLBACK FrameWindow::TabStripSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
	UINT_PTR, DWORD_PTR ref)
{
	auto* self = reinterpret_cast<FrameWindow*>(ref);
	// The trailing area past the last tab (and, in dark mode, the momentary
	// flash before an owner-drawn item repaints) is the control's own
	// background -- WM_DRAWITEM only covers actual tab items, not that gap.
	if (msg == WM_ERASEBKGND && self->isDark_) {
		HDC dc = reinterpret_cast<HDC>(wp);
		RECT rc; GetClientRect(hwnd, &rc);
		HBRUSH b = CreateSolidBrush(kDarkTheme.tabInactiveBg);
		FillRect(dc, &rc, b);
		DeleteObject(b);
		return 1;
	}
	if (msg == WM_LBUTTONDOWN) {
		POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		TCHITTESTINFO hit = {}; hit.pt = pt;
		int idx = TabCtrl_HitTest(hwnd, &hit);
		if (idx >= 0) {
			RECT closeRc = self->tabCloseRect(idx);
			if (PtInRect(&closeRc, pt)) {
				self->closeTab(idx);
				return 0; // swallow -- don't let the default proc also select it
			}
			bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
			bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
			if (ctrl) {
				// Toggle membership only -- don't touch the active/shown tab
				// or arm a drag, and don't let the default proc change the
				// control's own selection either.
				self->toggleTabMultiSelect(idx);
				return 0;
			}
			if (shift && self->tabSelectAnchor_ >= 0) {
				self->selectTabRange(self->tabSelectAnchor_, idx);
				return 0;
			}
			// Plain click: if this tab isn't already part of an active
			// multi-selection, that selection no longer applies.
			if (!self->isTabMultiSelected(idx)) self->clearTabMultiSelect();
			self->tabSelectAnchor_ = idx;
			// Arm drag-to-reorder tracking, but still fall through to
			// DefSubclassProc so the control's normal click-to-select
			// behavior also runs on this same button-down.
			self->tabDragIndex_ = idx;
			self->tabDragArmed_ = true;
			SetCapture(hwnd);
		}
	}
	if (msg == WM_MOUSEMOVE && self->tabDragArmed_ && (wp & MK_LBUTTON)) {
		POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		// A multi-selected group being dragged moves/tears off together;
		// otherwise just the one tab under the cursor, as before.
		bool asGroup = self->isTabMultiSelected(self->tabDragIndex_) && self->multiSelectedTabs_.size() > 1;
		// Dragging far enough above/below the strip (mouse capture keeps
		// these coordinates coming even once the cursor leaves the control)
		// tears the tab(s) off into their own window, Chrome/Edge-style,
		// instead of just reordering within the strip.
		RECT rc; GetClientRect(hwnd, &rc);
		int tearThreshold = Scale(50, GetDpiForWindow(hwnd));
		if (pt.y < rc.top - tearThreshold || pt.y > rc.bottom + tearThreshold) {
			int idx = self->tabDragIndex_;
			std::vector<int> group = asGroup ? self->multiSelectedTabs_ : std::vector<int>{ idx };
			self->tabDragArmed_ = false;
			self->tabDragIndex_ = -1;
			ReleaseCapture();
			self->detachTabsToNewWindow(group);
			return 0;
		}
		TCHITTESTINFO hit = {}; hit.pt = pt;
		int idx = TabCtrl_HitTest(hwnd, &hit);
		if (idx >= 0 && idx != self->tabDragIndex_) {
			if (asGroup) {
				DocTab* draggedDoc = (self->tabDragIndex_ >= 0 && self->tabDragIndex_ < static_cast<int>(self->tabs_.size()))
					? self->tabs_[self->tabDragIndex_].get() : nullptr;
				self->reorderTabGroup(self->multiSelectedTabs_, idx);
				if (draggedDoc) {
					for (int i = 0; i < static_cast<int>(self->tabs_.size()); ++i)
						if (self->tabs_[i].get() == draggedDoc) { self->tabDragIndex_ = i; break; }
				}
			} else {
				self->reorderTab(self->tabDragIndex_, idx);
				self->tabDragIndex_ = idx;
			}
		}
		return 0;
	}
	if ((msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED) && self->tabDragArmed_) {
		self->tabDragArmed_ = false;
		self->tabDragIndex_ = -1;
		if (msg == WM_LBUTTONUP) ReleaseCapture();
		// Fall through to DefSubclassProc either way -- this only clears our
		// own drag-tracking state, it doesn't need to swallow the message.
	}
	if (msg == WM_MBUTTONUP || msg == WM_RBUTTONUP) {
		TCHITTESTINFO hit = {};
		hit.pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		int idx = TabCtrl_HitTest(hwnd, &hit);
		if (idx >= 0) {
			if (msg == WM_MBUTTONUP) {
				self->closeTab(idx);
			} else {
				bool asGroup = self->isTabMultiSelected(idx) && self->multiSelectedTabs_.size() > 1;
				std::vector<int> group = asGroup ? self->multiSelectedTabs_ : std::vector<int>{ idx };
				HMENU m = CreatePopupMenu();
				AppendMenuW(m, MF_STRING, IDM_TAB_CLOSE, L"Close Tab");
				wchar_t openLabel[48];
				if (asGroup) swprintf(openLabel, 48, L"Open %d Tabs in New Window", static_cast<int>(group.size()));
				else wcscpy_s(openLabel, L"Open in New Window");
				AppendMenuW(m, MF_STRING, IDM_TAB_OPEN_NEW_WINDOW, openLabel);
				POINT pt = hit.pt;
				ClientToScreen(hwnd, &pt);
				self->switchToTab(idx);
				int sel = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, self->hwnd_, nullptr);
				DestroyMenu(m);
				if (sel == IDM_TAB_CLOSE) self->closeTab(idx);
				else if (sel == IDM_TAB_OPEN_NEW_WINDOW) self->detachTabsToNewWindow(group);
			}
			return 0;
		}
	}
	return DefSubclassProc(hwnd, msg, wp, lp);
}

RECT FrameWindow::tabCloseRect(int idx) const
{
	RECT rc = {};
	SendMessageW(tabStrip_, TCM_GETITEMRECT, idx, reinterpret_cast<LPARAM>(&rc));
	UINT dpi = GetDpiForWindow(hwnd_);
	int closeSize = Scale(14, dpi);
	int margin = Scale(8, dpi);
	RECT closeRc;
	closeRc.right = rc.right - margin;
	closeRc.left = closeRc.right - closeSize;
	closeRc.top = rc.top + (rc.bottom - rc.top - closeSize) / 2;
	closeRc.bottom = closeRc.top + closeSize;
	return closeRc;
}

void FrameWindow::drawTabItem(const DRAWITEMSTRUCT* dis)
{
	HDC dc = dis->hDC;
	RECT rc = dis->rcItem;
	bool selected = (dis->itemState & ODS_SELECTED) != 0;
	const ThemeColors& th = Theme(isDark_);

	HBRUSH bg = CreateSolidBrush(selected ? th.tabActiveBg : th.tabInactiveBg);
	FillRect(dc, &rc, bg);
	DeleteObject(bg);
	if (selected) {
		HBRUSH accent = CreateSolidBrush(th.tabAccent);
		RECT under = { rc.left, rc.bottom - Scale(2, GetDpiForWindow(hwnd_)), rc.right, rc.bottom };
		FillRect(dc, &under, accent);
		DeleteObject(accent);
	}
	// Multi-tab selection (Ctrl/Shift+click, see multiSelectedTabs_) gets its
	// own translucent tint independent of the single "active/shown" tab's
	// accent bar above -- several tabs can be marked at once, only one is
	// ever actually on screen.
	if (isTabMultiSelected(static_cast<int>(dis->itemID)))
		FillAlpha(dc, rc, th.tabAccent, 60);
	HPEN gridPen = CreatePen(PS_SOLID, 1, th.tabBorder);
	HGDIOBJ oldPen = SelectObject(dc, gridPen);
	MoveToEx(dc, rc.right - 1, rc.top + 4, nullptr);
	LineTo(dc, rc.right - 1, rc.bottom - 4);
	SelectObject(dc, oldPen);
	DeleteObject(gridPen);

	wchar_t text[64] = {};
	TCITEMW item = {};
	item.mask = TCIF_TEXT;
	item.pszText = text;
	item.cchTextMax = 64;
	SendMessageW(tabStrip_, TCM_GETITEMW, dis->itemID, reinterpret_cast<LPARAM>(&item));

	RECT closeRc = tabCloseRect(static_cast<int>(dis->itemID));
	UINT dpi = GetDpiForWindow(hwnd_);
	RECT textRc = rc;
	textRc.left += Scale(10, dpi);
	textRc.right = closeRc.left - Scale(4, dpi);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, th.tabText);
	HFONT oldFont = uiFont_ ? static_cast<HFONT>(SelectObject(dc, uiFont_)) : nullptr;
	DrawTextW(dc, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
	if (oldFont) SelectObject(dc, oldFont);

	// Close "x", drawn as a plain glyph (no hover state tracked) -- deliberately
	// more muted than the tab's own label text, in both themes.
	HPEN xpen = CreatePen(PS_SOLID, Scale(1, dpi), isDark_ ? RGB(150, 150, 150) : RGB(100, 100, 100));
	HGDIOBJ oldXPen = SelectObject(dc, xpen);
	MoveToEx(dc, closeRc.left, closeRc.top, nullptr);
	LineTo(dc, closeRc.right, closeRc.bottom);
	MoveToEx(dc, closeRc.right, closeRc.top, nullptr);
	LineTo(dc, closeRc.left, closeRc.bottom);
	SelectObject(dc, oldXPen);
	DeleteObject(xpen);
}

// Part 0 (page/zoom) is only reached here when SBT_OWNERDRAW was set -- i.e.
// only in dark mode (see CanvasView::updateStatus). Part 1 (file path) is
// ALWAYS owner-drawn regardless of theme, so it can use DT_PATH_ELLIPSIS
// (see updateStatusPath()'s comment). Status bar controls don't reliably
// send NM_CUSTOMDRAW like other common controls do, so SBT_OWNERDRAW +
// WM_DRAWITEM (the same mechanism the tab strip already uses) is the
// documented, reliable way to hand-draw their text. The sizegrip corner is
// drawn separately by the control itself, unaffected by this.
void FrameWindow::drawStatusBarItem(const DRAWITEMSTRUCT* dis)
{
	const ThemeColors& th = Theme(isDark_);
	HBRUSH bg = CreateSolidBrush(th.statusBg);
	FillRect(dis->hDC, &dis->rcItem, bg);
	DeleteObject(bg);

	SetTextColor(dis->hDC, th.statusText);
	SetBkMode(dis->hDC, TRANSPARENT);
	RECT tr = dis->rcItem;
	UINT dpi = GetDpiForWindow(status_);

	if (dis->itemID == 1) {
		std::wstring path = (activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size()))
			? tabs_[activeTab_]->path : std::wstring();
		tr.left += Scale(6, dpi);
		tr.right -= Scale(6, dpi);
		DrawTextW(dis->hDC, path.c_str(), -1, &tr,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
		return;
	}

	// SB_GETTEXTW can't be used here: once a part is marked SBT_OWNERDRAW,
	// its text slot holds opaque app data, not a retrievable string (see
	// CanvasView::statusText_'s comment) -- read the mirrored text instead.
	if (canvas_) {
		tr.left += Scale(6, dpi);
		DrawTextW(dis->hDC, canvas_->currentStatusText().c_str(), -1, &tr,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	}
}

void FrameWindow::openDocument(const wchar_t* path)
{
	// Reuse the current tab if it has no document loaded yet (the initial
	// empty tab, or one just cleared); otherwise open into a fresh tab,
	// browser-style, so an already-open document is never disturbed.
	if (doc_ && doc_->isOpen())
		switchToTab(newTab());

	std::string err; bool needsPw = false;
	bool ok = doc_->open(path, err, needsPw);
	if (!ok && needsPw) {
		pendingPwdPath_ = path;
		showPasswordBar(true);
		return;
	}
	if (!ok) {
		pendingSetPasswordAfterOpen_ = false;
		pendingFlattenAfterOpen_ = 0;
		std::wstring wmsg(err.begin(), err.end());
		MessageBoxW(hwnd_, wmsg.empty() ? L"Failed to open document." : wmsg.c_str(),
			L"Open PDF", MB_OK | MB_ICONERROR);
		return;
	}
	finishOpenDocument(path);
}

void FrameWindow::finishOpenDocument(const wchar_t* path)
{
	canvas_->setDocument(doc_);
	thumbs_->setDocument(doc_);

	DocTab* at = tabs_[activeTab_].get();
	at->path = path;
	const wchar_t* name = wcsrchr(path, L'\\');
	at->name = (name ? name + 1 : path);
	at->protDismissed = false;
	updateTitle();
	updateProtectionBar();
	bool chainToSetPassword = false;
	if (pendingSetPasswordAfterOpen_) {
		pendingSetPasswordAfterOpen_ = false;
		// Only chains into the bar for a PDF that doesn't already have a
		// password -- one that needed one just got its own offer to remove
		// it, via updateProtectionBar() above.
		chainToSetPassword = doc_ && doc_->isPdf() && !doc_->isEncrypted();
	}
	if (pendingFlattenAfterOpen_) {
		int which = pendingFlattenAfterOpen_;
		pendingFlattenAfterOpen_ = 0;
		if (which == 1) doFlatten();
		else doFlattenEdits();
	}
	if (chainToSetPassword) showSetPasswordBar(true); // moves focus to its own edit box
	else SetFocus(canvas_->hwnd());
}

void FrameWindow::toggleThumbs()
{
	showThumbs_ = !showThumbs_;
	syncMenuChecks();
	layout();
}

void FrameWindow::updateZoomLabel()
{
	if (!canvas_) return;
	wchar_t buf[16];
	swprintf(buf, 16, L"%d%%", static_cast<int>(std::lround(canvas_->zoom() * 100)));
	zoomText_ = buf;
	if (toolbar_) InvalidateRect(toolbar_, nullptr, FALSE);
}

void FrameWindow::syncMenuChecks()
{
	bool cont = canvas_ && canvas_->mode() == CanvasView::Mode::Continuous;
	// No menu bar (see FrameWindow::create) and no more Continuous/Single-
	// page/Thumbnails toolbar buttons to keep in sync either -- this is now
	// only meaningful if IDR_MAINMENU is ever wired back up.
	HMENU m = GetMenu(hwnd_);
	if (!m) return;
	CheckMenuItem(m, IDM_VIEW_CONTINUOUS, MF_BYCOMMAND | (cont ? MF_CHECKED : MF_UNCHECKED));
	CheckMenuItem(m, IDM_VIEW_SINGLEPAGE, MF_BYCOMMAND | (cont ? MF_UNCHECKED : MF_CHECKED));
	CheckMenuItem(m, IDM_VIEW_THUMBS, MF_BYCOMMAND | (showThumbs_ ? MF_CHECKED : MF_UNCHECKED));
}

void FrameWindow::onCommand(int id)
{
	switch (id) {
	case IDM_FILE_OPEN: {
		std::wstring p = OpenFileDialog(hwnd_);
		if (!p.empty()) openDocument(p.c_str());
		break;
	}
	case IDM_EMPTY_SET_PASSWORD: {
		std::wstring p = OpenFileDialog(hwnd_);
		if (!p.empty()) { pendingSetPasswordAfterOpen_ = true; openDocument(p.c_str()); }
		break;
	}
	case IDM_EMPTY_FLATTEN_IMAGE: {
		std::wstring p = OpenFileDialog(hwnd_);
		if (!p.empty()) { pendingFlattenAfterOpen_ = 1; openDocument(p.c_str()); }
		break;
	}
	case IDM_EMPTY_FLATTEN_EDITS: {
		std::wstring p = OpenFileDialog(hwnd_);
		if (!p.empty()) { pendingFlattenAfterOpen_ = 2; openDocument(p.c_str()); }
		break;
	}
	case IDM_FILE_EXIT: if (promptSaveIfDirty()) DestroyWindow(hwnd_); break;
	case IDM_FILE_PRINT:
		if (doc_ && doc_->isOpen()) showPrintPanel(true);
		break;
	// BS_AUTORADIOBUTTON only auto-unchecks its siblings under a real dialog
	// procedure (DefDlgProc) -- this frame is a plain window (DefWindowProc),
	// so each radio group's exclusivity has to be done by hand here.
	case IDC_PRINT_RANGE_ALL:
	case IDC_PRINT_RANGE_CURRENT:
	case IDC_PRINT_RANGE_CUSTOM:
		for (HWND h : { printRangeAll_, printRangeCurrent_, printRangeCustom_ })
			SendMessageW(h, BM_SETCHECK, h == GetDlgItem(hwnd_, id) ? BST_CHECKED : BST_UNCHECKED, 0);
		// An empty custom-range box resolves to zero pages, which would blank
		// the preview entirely until the user finishes typing -- pre-fill it
		// with whatever page the preview is already showing so switching to
		// Custom never itself blanks anything, and select the text so typing
		// immediately replaces it.
		if (id == IDC_PRINT_RANGE_CUSTOM && GetWindowTextLengthW(printRangeEdit_) == 0 && canvas_) {
			wchar_t buf[16];
			swprintf(buf, 16, L"%d", canvas_->printPreviewCursor() + 1);
			SetWindowTextW(printRangeEdit_, buf);
			SendMessageW(printRangeEdit_, EM_SETSEL, 0, -1);
		}
		updatePrintPreviewFromPanel();
		if (id == IDC_PRINT_RANGE_CUSTOM) SetFocus(printRangeEdit_);
		break;
	case IDC_PRINT_ORIENT_PORTRAIT:
	case IDC_PRINT_ORIENT_LANDSCAPE:
		for (HWND h : { printOrientPortrait_, printOrientLandscape_ })
			SendMessageW(h, BM_SETCHECK, h == GetDlgItem(hwnd_, id) ? BST_CHECKED : BST_UNCHECKED, 0);
		updatePrintPreviewFromPanel();
		break;
	case IDC_PRINT_COLOR_COLOR:
	case IDC_PRINT_COLOR_GRAY:
		for (HWND h : { printColorColor_, printColorGray_ })
			SendMessageW(h, BM_SETCHECK, h == GetDlgItem(hwnd_, id) ? BST_CHECKED : BST_UNCHECKED, 0);
		updatePrintPreviewFromPanel();
		break;
	case IDC_PRINT_PRINTER_COMBO:
	case IDC_PRINT_COPIES_EDIT:
	case IDC_PRINT_RANGE_EDIT:
		updatePrintPreviewFromPanel();
		break;
	case IDC_PRINT_PAGENAV_PREV:
		if (canvas_) { canvas_->printPreviewGoTo(canvas_->printPreviewCursor() - 1); updatePrintPreviewFromPanel(); }
		break;
	case IDC_PRINT_PAGENAV_NEXT:
		if (canvas_) { canvas_->printPreviewGoTo(canvas_->printPreviewCursor() + 1); updatePrintPreviewFromPanel(); }
		break;
	case IDC_PRINT_GO: doExecutePrint(); break;
	case IDC_PRINT_CANCEL: showPrintPanel(false); break;
	case IDC_TEXTPANEL_BOLD: textPanelToggleFormat(CFM_BOLD, CFE_BOLD); break;
	case IDC_TEXTPANEL_ITALIC: textPanelToggleFormat(CFM_ITALIC, CFE_ITALIC); break;
	case IDC_TEXTPANEL_UNDERLINE: textPanelToggleFormat(CFM_UNDERLINE, CFE_UNDERLINE); break;
	case IDC_TEXTPANEL_COPY: textPanelCopy(); break;
	case IDC_TEXTPANEL_CLOSE: showTextEditorPanel(false); break;
	case IDM_EDIT_COPY: if (canvas_) canvas_->copySelectionToClipboard(); break;
	case IDM_EDIT_SELECTALL: if (canvas_) canvas_->selectAll(); break;
	case IDM_EDIT_FIND: showSearchBar(true); break;
	case IDM_EDIT_FINDNEXT:
		if (canvas_ && canvas_->hitCount() > 0) { canvas_->findNext(); updateSearchLabel(); }
		else showSearchBar(true);
		break;
	case IDM_EDIT_FINDPREV:
		if (canvas_ && canvas_->hitCount() > 0) { canvas_->findPrev(); updateSearchLabel(); }
		else showSearchBar(true);
		break;
	case IDC_SEARCH_NEXT:
		if (canvas_ && canvas_->hitCount() > 0) { canvas_->findNext(); updateSearchLabel(); }
		else runSearch();
		break;
	case IDC_SEARCH_PREV:
		if (canvas_ && canvas_->hitCount() > 0) { canvas_->findPrev(); updateSearchLabel(); }
		break;
	case IDC_SEARCH_CLOSE: showSearchBar(false); break;
	case IDC_PWD_UNLOCK: submitPassword(); break;
	case IDC_PWD_CANCEL: cancelPasswordPrompt(); break;
	case IDC_PROT_REMOVE_PWD:
	case IDC_PROT_REMOVE_RESTRICTIONS: removeProtection(); break;
	case IDC_PROT_CLOSE:
		if (activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size()))
			tabs_[activeTab_]->protDismissed = true;
		updateProtectionBar();
		break;
	case IDC_OPRESULT_SAVE: saveDocument(false); showOpResultBar(false); break;
	case IDC_OPRESULT_SAVEAS: saveDocument(true); showOpResultBar(false); break;
	case IDC_OPRESULT_CLOSE: showOpResultBar(false); break;
	case IDC_SPLIT_BUTTON: runSplit(); break;
	case IDC_SPLIT_CLOSE: showSplitBar(false); break;
	case IDC_SETPWD_BUTTON: runSetPassword(); break;
	case IDC_SETPWD_CLOSE: showSetPasswordBar(false); break;
	case IDC_WEBPDF_BUTTON: runWebToPdf(); break;
	case IDC_WEBPDF_CLOSE: showWebPdfBar(false); break;
	case IDC_REDACT_APPLY: applyRedactionsCmd(); break;
	case IDC_REDACT_CLEAR: clearRedactions(); break;
	case IDC_REDACT_DONE: selectTool(IDM_TOOL_SELECT); break;
	case IDM_TOOLS_MENU: showToolsMenu(); break;
	case IDM_TOOLS_ORGANIZE: enterOrganizeMode({}); break;
	case IDM_TOOLS_MERGE: doMerge(); break;
	case IDM_TOOLS_SPLIT: showSplitBar(true); break;
	case IDM_TOOLS_RESIZE_A4: doResizeToA4(); break;
	case IDM_TOOLS_FLATTEN: doFlatten(); break;
	case IDM_TOOLS_FLATTEN_EDITS: doFlattenEdits(); break;
	case IDM_TOOLS_COMPRESS: doCompress(); break;
	case IDM_TOOLS_SET_PASSWORD: showSetPasswordBar(true); break;
	case IDM_TOOLS_CONVERT: doConvertToPdf(); break;
	case IDM_TOOLS_WEBPDF: showWebPdfBar(true); break;
	case IDM_VIEW_TOGGLETHEME: toggleTheme(); break;
	case IDM_HELP_CHECKUPDATE: startUpdateCheck(/*manual=*/true); break;
	case IDC_ORGANIZE_INSERT: organizeInsertPages(); break;
	case IDC_ORGANIZE_DONE: organizeDone(); break;
	case IDC_ORGANIZE_CANCEL: organizeCancel(); break;
	case IDM_TOOL_SELECT:
	case IDM_TOOL_HIGHLIGHT:
	case IDM_TOOL_DRAW:
	case IDM_TOOL_ERASE:
	case IDM_TOOL_TEXT:
	case IDM_TOOL_REDACT: selectTool(id); break;
	case IDM_TOOL_COLOR: chooseColor(); break;
	case IDM_TOOL_WIDTH: chooseWidth(); break;
	case IDM_TOOL_OPACITY: chooseOpacity(); break;
	case IDM_FILE_SAVE: saveDocument(false); break;
	case IDM_FILE_SAVEAS: saveDocument(true); break;
	case IDM_VIEW_ZOOMIN: canvas_->zoomIn(); break;
	case IDM_VIEW_ZOOMLABEL: canvas_->actualSize(); break;
	case IDM_VIEW_PAGELABEL: beginPageNumberEdit(); break;
	case IDM_VIEW_ZOOMOUT: canvas_->zoomOut(); break;
	case IDM_VIEW_ACTUALSIZE: canvas_->actualSize(); break;
	case IDM_VIEW_FITWIDTH: canvas_->setFit(CanvasView::Fit::Width); break;
	case IDM_VIEW_FITPAGE: canvas_->setFit(CanvasView::Fit::Page); break;
	case IDM_VIEW_CONTINUOUS: canvas_->setMode(CanvasView::Mode::Continuous); syncMenuChecks(); break;
	case IDM_VIEW_SINGLEPAGE: canvas_->setMode(CanvasView::Mode::SinglePage); syncMenuChecks(); break;
	case IDM_VIEW_THUMBS: toggleThumbs(); break;
	case IDM_TAB_NEXT: cycleTab(+1); break;
	case IDM_TAB_PREV: cycleTab(-1); break;
	case IDM_TAB_CLOSE: closeTab(activeTab_); break;
	case IDM_GO_PREV: if (canvas_) canvas_->goToPage(canvas_->currentPage() - 1); break;
	case IDM_GO_NEXT: if (canvas_) canvas_->goToPage(canvas_->currentPage() + 1); break;
	case IDM_HELP_ABOUT:
		MessageBoxW(hwnd_,
			L"PDFast\nA fast, lightweight native Windows PDF viewer and editor.\n"
			L"Rendering by MuPDF (statically linked).",
			L"About PDFast", MB_OK | MB_ICONINFORMATION);
		break;
	}
}

void FrameWindow::selectTool(int id)
{
	CanvasView::Tool t = CanvasView::Tool::Select;
	switch (id) {
	case IDM_TOOL_HIGHLIGHT: t = CanvasView::Tool::Highlight; break;
	case IDM_TOOL_DRAW: t = CanvasView::Tool::Draw; break;
	case IDM_TOOL_ERASE: t = CanvasView::Tool::Erase; break;
	case IDM_TOOL_TEXT: t = CanvasView::Tool::Text; break;
	case IDM_TOOL_REDACT: t = CanvasView::Tool::Redact; break;
	default: t = CanvasView::Tool::Select; break;
	}
	if (canvas_) canvas_->setTool(t);
	for (int b : { IDM_TOOL_SELECT, IDM_TOOL_HIGHLIGHT, IDM_TOOL_DRAW, IDM_TOOL_ERASE, IDM_TOOL_TEXT, IDM_TOOL_REDACT })
		SendMessageW(toolbar_, TB_CHECKBUTTON, b, MAKELPARAM(b == id, 0));
	updateRedactBar();
}

void FrameWindow::chooseColor()
{
	if (!canvas_) return;
	static COLORREF custom[16] = {};
	CHOOSECOLORW cc = { sizeof(cc) };
	cc.hwndOwner = hwnd_;
	cc.lpCustColors = custom;
	cc.rgbResult = canvas_->color();
	cc.Flags = CC_FULLOPEN | CC_RGBINIT;
	if (ChooseColorW(&cc)) canvas_->setColor(cc.rgbResult);
}

int FrameWindow::popupUnderButton(int buttonId, HMENU menu)
{
	RECT r = {};
	SendMessageW(toolbar_, TB_GETRECT, buttonId, reinterpret_cast<LPARAM>(&r));
	MapWindowPoints(toolbar_, nullptr, reinterpret_cast<POINT*>(&r), 2);
	int sel = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
		r.left, r.bottom, 0, hwnd_, nullptr);
	DestroyMenu(menu);
	return sel;
}

void FrameWindow::showToolsMenu()
{
	HMENU m = CreatePopupMenu();
	AppendMenuW(m, MF_STRING, IDM_TOOLS_ORGANIZE, L"Organize Pages");
	AppendMenuW(m, MF_STRING, IDM_TOOLS_MERGE, L"Merge PDFs...");
	AppendMenuW(m, MF_STRING, IDM_TOOLS_SPLIT, L"Split PDF...");
	AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(m, MF_STRING, IDM_TOOLS_RESIZE_A4, L"Resize Pages to A4");
	AppendMenuW(m, MF_STRING, IDM_TOOLS_FLATTEN, L"Flatten to Image (Read-Only)");
	AppendMenuW(m, MF_STRING, IDM_TOOLS_FLATTEN_EDITS, L"Flatten Edits Only (Keep Text Selectable)");
	AppendMenuW(m, MF_STRING, IDM_TOOLS_COMPRESS, L"Compress PDF");
	AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(m, MF_STRING, IDM_TOOLS_SET_PASSWORD, L"Set Password...");
	AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(m, MF_STRING, IDM_TOOLS_CONVERT, L"Convert to PDF...");
	AppendMenuW(m, MF_STRING, IDM_TOOLS_WEBPDF, L"Web Page to PDF...");
	AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(m, MF_STRING, IDM_HELP_CHECKUPDATE, L"Check for Updates…");
	int sel = popupUnderButton(IDM_TOOLS_MENU, m);
	if (sel != 0) onCommand(sel);
}

void FrameWindow::doResizeToA4()
{
	if (!doc_ || !doc_->isOpen() || !doc_->isPdf()) return;
	if (canvas_) canvas_->flushPendingEdit();
	std::string err;
	if (!doc_->resizeToA4(err)) {
		std::wstring wmsg(err.begin(), err.end());
		MessageBoxW(hwnd_, wmsg.empty() ? L"Failed to resize pages." : wmsg.c_str(),
			L"Resize to A4", MB_OK | MB_ICONERROR);
		return;
	}
	if (canvas_) canvas_->refreshAfterSave();
	if (thumbs_) thumbs_->refreshAfterSave();
	updatePageEditBox();
	showOpResultBar(true);
}

void FrameWindow::doFlatten()
{
	if (!doc_ || !doc_->isOpen() || !doc_->isPdf()) return;
	if (canvas_) canvas_->flushPendingEdit();
	std::string err;
	if (!doc_->flattenToImages(err)) {
		std::wstring wmsg(err.begin(), err.end());
		MessageBoxW(hwnd_, wmsg.empty() ? L"Failed to flatten pages." : wmsg.c_str(),
			L"Flatten", MB_OK | MB_ICONERROR);
		return;
	}
	if (canvas_) canvas_->refreshAfterSave();
	if (thumbs_) thumbs_->refreshAfterSave();
	showOpResultBar(true);
}

void FrameWindow::doFlattenEdits()
{
	if (!doc_ || !doc_->isOpen() || !doc_->isPdf()) return;
	if (canvas_) canvas_->flushPendingEdit();
	std::string err;
	if (!doc_->flattenAnnotationsToContent(err)) {
		std::wstring wmsg(err.begin(), err.end());
		MessageBoxW(hwnd_, wmsg.empty() ? L"Failed to flatten annotations." : wmsg.c_str(),
			L"Flatten Edits", MB_OK | MB_ICONERROR);
		return;
	}
	if (canvas_) canvas_->refreshAfterSave();
	if (thumbs_) thumbs_->refreshAfterSave();
	showOpResultBar(true);
}

void FrameWindow::doCompress()
{
	if (!doc_ || !doc_->isOpen() || !doc_->isPdf()) return;
	if (canvas_) canvas_->flushPendingEdit();
	std::string err;
	if (!doc_->compress(err)) {
		std::wstring wmsg(err.begin(), err.end());
		MessageBoxW(hwnd_, wmsg.empty() ? L"Failed to compress document." : wmsg.c_str(),
			L"Compress", MB_OK | MB_ICONERROR);
		return;
	}
	if (canvas_) canvas_->refreshAfterSave();
	if (thumbs_) thumbs_->refreshAfterSave();
	showOpResultBar(true);
}

void FrameWindow::chooseWidth()
{
	if (!canvas_) return;
	static const int widths[] = { 1, 2, 3, 5, 8 };
	HMENU m = CreatePopupMenu();
	for (int i = 0; i < 5; ++i) {
		wchar_t b[16]; swprintf(b, 16, L"%d pt", widths[i]);
		AppendMenuW(m, MF_STRING, IDM_WIDTH_BASE + i, b);
	}
	int sel = popupUnderButton(IDM_TOOL_WIDTH, m);
	if (sel >= IDM_WIDTH_BASE && sel < IDM_WIDTH_BASE + 5)
		canvas_->setPenWidth(static_cast<float>(widths[sel - IDM_WIDTH_BASE]));
}

void FrameWindow::chooseOpacity()
{
	if (!canvas_) return;
	static const int pct[] = { 25, 40, 60, 100 };
	HMENU m = CreatePopupMenu();
	for (int i = 0; i < 4; ++i) {
		wchar_t b[16]; swprintf(b, 16, L"%d%%", pct[i]);
		AppendMenuW(m, MF_STRING, IDM_OPACITY_BASE + i, b);
	}
	int sel = popupUnderButton(IDM_TOOL_OPACITY, m);
	if (sel >= IDM_OPACITY_BASE && sel < IDM_OPACITY_BASE + 4)
		canvas_->setOpacity(pct[sel - IDM_OPACITY_BASE] / 100.0f);
}

// Two status bar parts: [0] the existing "Page X / N   Zoom Y%" (left,
// CanvasView::updateStatus() owns its text), [1] the active tab's full file
// path (right, filling the rest of the bar). Part 0's width is a fixed
// budget wide enough for its longest realistic text; part -1 means "extends
// to the status bar's own right edge" (SB_SETPARTS convention), which also
// keeps the sizegrip corner working normally.
void FrameWindow::layoutStatusParts()
{
	if (!status_) return;
	UINT dpi = GetDpiForWindow(status_);
	int leftW = Scale(240, dpi);
	int parts[2] = { leftW, -1 };
	SendMessageW(status_, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
}

// Always owner-drawn (regardless of theme, unlike part 0 which stays fully
// native in light mode) so a long path can be drawn with DT_PATH_ELLIPSIS
// (elides from the middle -- "C:\...\test.pdf" -- far more readable for a
// path than clipping or a trailing "..."), which the native status bar
// rendering can't do on its own.
void FrameWindow::updateStatusPath()
{
	if (!status_) return;
	std::wstring path = (activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size()))
		? tabs_[activeTab_]->path : std::wstring();
	SendMessageW(status_, SB_SETTEXTW, 1 | SBT_OWNERDRAW, reinterpret_cast<LPARAM>(path.c_str()));
}

void FrameWindow::updateTitle()
{
	DocTab* at = (activeTab_ >= 0 && activeTab_ < static_cast<int>(tabs_.size())) ? tabs_[activeTab_].get() : nullptr;
	std::wstring t;
	if (!at || at->name.empty()) t = L"PDFast";
	else {
		if (at->doc && at->doc->dirty()) t += L"*";
		t += at->name + L" - PDFast";
	}
	SetWindowTextW(hwnd_, t.c_str());
	if (activeTab_ >= 0) updateTabLabel(activeTab_);
	updateStatusPath();
}

void FrameWindow::saveDocument(bool saveAs)
{
	if (!doc_ || !doc_->isOpen()) return;
	if (!doc_->isPdf()) {
		MessageBoxW(hwnd_, L"Only PDF files can be saved with edits.", L"Save", MB_OK | MB_ICONINFORMATION);
		return;
	}
	if (canvas_) canvas_->flushPendingEdit(); // commit in-progress typing first

	DocTab* at = tabs_[activeTab_].get();
	std::wstring target = at->path;
	if (saveAs || at->path.empty()) {
		wchar_t file[MAX_PATH] = L"";
		if (!at->name.empty()) wcscpy_s(file, at->name.c_str());
		OPENFILENAMEW ofn = {};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = hwnd_;
		ofn.lpstrFilter = L"PDF Documents (*.pdf)\0*.pdf\0";
		ofn.lpstrFile = file;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrDefExt = L"pdf";
		ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
		ofn.lpstrTitle = L"Save PDF As";
		if (!GetSaveFileNameW(&ofn)) return;
		target = file;
	}

	// Always a full, clean rewrite (never incremental): simpler and more
	// robust than patching the original file in place, and PdfDocument::save
	// itself never touches the on-disk original until the new version has
	// been independently verified to open and render correctly.
	std::string err;
	if (doc_->save(target.c_str(), /*incremental=*/false, err)) {
		at->path = target;
		const wchar_t* name = wcsrchr(target.c_str(), L'\\');
		at->name = name ? name + 1 : target;
		updateTitle();
		// save() reopened the document's internal handle; drop stale caches.
		if (canvas_) canvas_->refreshAfterSave();
		if (thumbs_) thumbs_->refreshAfterSave();
	} else {
		std::wstring wmsg(err.begin(), err.end());
		MessageBoxW(hwnd_, wmsg.empty() ? L"Failed to save." : wmsg.c_str(),
			L"Save", MB_OK | MB_ICONERROR);
	}
}

bool FrameWindow::promptSaveIfDirty()
{
	if (canvas_) canvas_->flushPendingEdit(); // in-progress typing counts as a change
	if (!doc_ || !doc_->dirty()) return true;
	int r = MessageBoxW(hwnd_, L"Save changes to this document?", L"PDFast",
		MB_YESNOCANCEL | MB_ICONWARNING);
	if (r == IDCANCEL) return false;
	if (r == IDYES) {
		saveDocument(false);
		return !doc_->dirty(); // proceed only if the save actually succeeded
	}
	return true; // IDNO: discard
}

LRESULT CALLBACK FrameWindow::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	FrameWindow* self = reinterpret_cast<FrameWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (msg == WM_NCCREATE) {
		auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
		self = static_cast<FrameWindow*>(cs->lpCreateParams);
		self->hwnd_ = hwnd;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
	}
	if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

	switch (msg) {
	case WM_CREATE: self->createChildren(); return 0;
	case WM_SIZE: self->layout(); return 0;
	case WM_ERASEBKGND: {
		// Only intervene in dark mode -- in light mode, fall through (break)
		// to the default handling, which uses the class brush exactly as
		// before this feature existed, so light mode is pixel-identical.
		if (!self->isDark_) break;
		HDC dc = reinterpret_cast<HDC>(wp);
		RECT rc; GetClientRect(hwnd, &rc);
		HBRUSH b = CreateSolidBrush(kDarkTheme.frameBg);
		FillRect(dc, &rc, b);
		DeleteObject(b);
		return 1;
	}
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLOREDIT: {
		// Recolors every STATIC label and EDIT box that sits directly on the
		// frame -- the secondary bars (search/password/protection/split) plus
		// the always-visible zoom-%/page-number controls -- in dark mode.
		// Native BUTTON children are left themed/light (no supported API to
		// recolor a themed push-button face without private uxtheme calls).
		if (!self->isDark_) break;
		HDC dc = reinterpret_cast<HDC>(wp);
		SetTextColor(dc, kDarkTheme.ctrlText);
		SetBkMode(dc, TRANSPARENT);
		return reinterpret_cast<LRESULT>(self->ctrlBgBrush_);
	}
	case WM_CTLCOLORBTN:
		// Same recoloring as WM_CTLCOLORSTATIC/EDIT above, but for the print
		// panel's radio button labels specifically -- those had
		// SetWindowTheme(h, L"", L"") applied at creation (see mkRadio's
		// comment) precisely so this actually takes effect for them, unlike
		// ordinary themed push buttons elsewhere in the app.
		if (!self->isDark_) break;
		SetTextColor(reinterpret_cast<HDC>(wp), kDarkTheme.ctrlText);
		SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
		return reinterpret_cast<LRESULT>(self->ctrlBgBrush_);
	case WM_COMMAND:
		// Search-as-you-type: debounce edit changes so large docs stay smooth.
		if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == IDC_SEARCH_EDIT) {
			SetTimer(hwnd, 1 /*IDT_SEARCH*/, 150, nullptr);
			return 0;
		}
		// onCommand only takes the id (see below), so the case-converter
		// combo's notification code has to be checked here -- it must react
		// only to an actual selection change, not every WM_COMMAND a combo
		// box sends for that id (CBN_DROPDOWN/CBN_CLOSEUP too), or opening
		// the dropdown alone would spuriously re-apply the case conversion.
		if (LOWORD(wp) == IDC_TEXTPANEL_CASE_COMBO) {
			if (HIWORD(wp) == CBN_SELCHANGE) {
				int sel = static_cast<int>(SendMessageW(self->textCaseCombo_, CB_GETCURSEL, 0, 0));
				if (sel >= 0) self->textPanelApplyCase(sel);
			}
			return 0;
		}
		self->onCommand(LOWORD(wp));
		return 0;
	case WM_TIMER:
		if (wp == 1) { KillTimer(hwnd, 1); self->liveSearch(); }
		return 0;
	case WM_NOTIFY: {
		auto* nm = reinterpret_cast<NMHDR*>(lp);
		if (nm->hwndFrom == self->tabStrip_ && nm->code == TCN_SELCHANGE) {
			self->switchToTab(static_cast<int>(SendMessageW(self->tabStrip_, TCM_GETCURSEL, 0, 0)));
			return 0;
		}
		if (nm->hwndFrom == self->textRichEdit_ && nm->code == EN_SELCHANGE) {
			self->updateTextFormatButtons();
			return 0;
		}
		if (nm->code == TTN_GETDISPINFOW || nm->code == TTN_NEEDTEXTW) {
			auto* di = reinterpret_cast<NMTTDISPINFOW*>(lp);
			// Tab strip tooltips: owner-drawn tabs paint their own caption
			// rather than relying on the control's own pszText, so there's
			// nothing for the native truncation-tooltip behavior to show
			// automatically. Serve the full file name on demand instead --
			// idFrom here is the tab's own zero-based index (how
			// TCS_TOOLTIPS registers each tab's hit-rect as its own
			// "tool"), a completely different id space from the toolbar's
			// command ids below, hence checking hwndFrom against the tab
			// strip's own tooltip control first.
			if (nm->hwndFrom == self->tabTooltip_) {
				int idx = static_cast<int>(nm->idFrom);
				if (idx >= 0 && idx < static_cast<int>(self->tabs_.size()) && !self->tabs_[idx]->name.empty())
					di->lpszText = const_cast<LPWSTR>(self->tabs_[idx]->name.c_str());
				else
					di->lpszText = const_cast<LPWSTR>(L"");
				return 0;
			}
			// Toolbar tooltips: our icon-only buttons have no iString (see
			// toolbarTips_), so serve the text on demand here. idFrom is the
			// button's command id for a toolbar-owned tooltip.
			int ttId = static_cast<int>(nm->idFrom);
			// The zoom-%/page-number placeholders are a live, constantly-
			// relevant readout, not a button that needs an explanatory
			// tooltip -- and critically, THIS was the actual root cause of
			// the "readout disappears while hovered" bug several rounds of
			// paint/z-order fixes elsewhere failed to resolve: these two IDs
			// were never in toolbarTips_, so lpszText was left as whatever
			// stale/garbage value happened to already be in this reused
			// NMTTDISPINFOW struct, and the tooltip control showed it anyway
			// -- an (often blank-looking) popup sitting right over the
			// readout for as long as the tooltip stayed up. An explicit
			// empty string suppresses the tooltip outright instead.
			if (ttId == IDM_VIEW_ZOOMLABEL || ttId == IDM_VIEW_PAGELABEL) {
				di->lpszText = const_cast<LPWSTR>(L"");
				return 0;
			}
			auto it = self->toolbarTips_.find(ttId);
			if (it != self->toolbarTips_.end())
				di->lpszText = const_cast<LPWSTR>(it->second.c_str());
			return 0;
		}
		// Status bar dark-mode recoloring is done via SBT_OWNERDRAW + WM_DRAWITEM
		// (see drawStatusBarItem) -- status bar controls don't reliably send
		// NM_CUSTOMDRAW the way toolbar/tab controls do.
		if (nm->hwndFrom == self->toolbar_ && nm->code == NM_CUSTOMDRAW) {
			// Recolor the toolbar to a flat white/near-white look (Edge's PDF
			// viewer chrome) instead of the default themed gray button face.
			// This is the standard, Microsoft-documented way to recolor a
			// toolbar's background/hover without switching every button to
			// full owner-draw.
			auto* cd = reinterpret_cast<LPNMTBCUSTOMDRAW>(lp);
			const ThemeColors& th = Theme(self->isDark_);
			switch (cd->nmcd.dwDrawStage) {
			case CDDS_PREPAINT: {
				RECT rc; GetClientRect(self->toolbar_, &rc);
				// pageEditActive_ (see its comment) is the one remaining real
				// sibling window that can overlap this toolbar, and only
				// while the user is actively typing a page number. Excluding
				// its current rect from this DC's clip region keeps this
				// PREPAINT fill (which covers the toolbar's FULL client rect
				// on every redraw, including one triggered by hovering some
				// unrelated button) from ever painting over it, on top of the
				// WS_CLIPSIBLINGS style already set on the toolbar itself.
				if (self->pageEditActive_) {
					RECT wr; GetWindowRect(self->pageEditActive_, &wr);
					MapWindowPoints(HWND_DESKTOP, self->toolbar_, reinterpret_cast<POINT*>(&wr), 2);
					ExcludeClipRect(cd->nmcd.hdc, wr.left, wr.top, wr.right, wr.bottom);
				}
				HBRUSH bg = CreateSolidBrush(th.toolbarBg);
				FillRect(cd->nmcd.hdc, &rc, bg);
				DeleteObject(bg);
				// This is a normal WNDPROC, so the custom-draw result must be the
				// procedure's RETURN value -- DWLP_MSGRESULT is a dialog-proc
				// mechanism and is ignored here. (Returning it wrong is why an
				// earlier attempt's per-item drawing never ran.)
				// NOTE: an earlier attempt also requested CDRF_NOTIFYPOSTPAINT
				// here and, on receiving it, force-invalidated+UpdateWindow'd
				// pageEditActive_ on every single toolbar paint cycle as a
				// "belt and suspenders" measure against it being painted over,
				// then removed it after that broke live typing -- forcing a
				// SYNCHRONOUS repaint (UpdateWindow) fought the control's own
				// caret/redraw timing and left it rendering blank while typing.
				// Reintroduced below in a narrower form: the clip-region
				// exclusion above turns out NOT to be sufficient on its own --
				// confirmed via a live repro that merely hovering the mouse
				// over this reserved rect (without any click/typing) still
				// blanks the edit box, on every OS/comctl32 build tested,
				// which the exclusion alone should have prevented. Requesting
				// CDRF_NOTIFYPOSTPAINT and doing an ASYNC-only
				// InvalidateRect(..., FALSE) (no UpdateWindow) below reasserts
				// the control after whatever painted over it, without forcing
				// a synchronous repaint that could race live typing.
				return self->pageEditActive_
					? (CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT)
					: CDRF_NOTIFYITEMDRAW;
			}
			case CDDS_POSTPAINT: {
				if (self->pageEditActive_) InvalidateRect(self->pageEditActive_, nullptr, FALSE);
				return CDRF_DODEFAULT;
			}
			case CDDS_ITEMPREPAINT: {
				// Fluent-style state layer: a soft rounded "pill" behind the
				// icon for hover / pressed / active(checked), instead of the
				// dated light-blue Aero highlight. On a *themed* toolbar the
				// TBCDRF_NOBACKGROUND/NOMARK flags are ignored (the theme's
				// DrawThemeBackground repaints the blue over us), so we fully
				// own-draw the button -- pill + glyph -- and CDRF_SKIPDEFAULT.
				// Separators (and anything we can't resolve) fall through to
				// the default drawing, which renders them on the white bg.
				UINT id = static_cast<UINT>(cd->nmcd.dwItemSpec);
				// The zoom-%/page-number buttons are pure width-reserving
				// placeholders (see textBtn() call sites) -- draw their text
				// directly here instead of showing/hiding a pill (there's
				// nothing to hover-highlight, they're a plain readout, not an
				// action button -- clicking them is handled via onCommand()
				// same as any other button, no visual press state needed).
				// This used to be a real STATIC/EDIT control permanently
				// overlaid on top of this exact rect, which had a confirmed
				// bug: this toolbar's own hover repaint would race the
				// overlay's own repaint and could leave it blank for as long
				// as it stayed hovered. Owner-drawing the text directly here,
				// in the SAME window that would otherwise paint over it,
				// removes the race entirely -- there's no second window to
				// race against, exactly like the status bar's own page/zoom
				// text never had this problem.
				if (id == IDM_VIEW_ZOOMLABEL || id == IDM_VIEW_PAGELABEL) {
					RECT r = cd->nmcd.rc;
					std::wstring text = (id == IDM_VIEW_ZOOMLABEL) ? self->zoomText_ : self->pageLabelText_;
					// While pageEditActive_ is up, it already covers the left
					// portion of this rect (see beginPageNumberEdit()) --
					// keep showing "of N" in the REMAINING portion instead of
					// blanking the whole reserved rect (that looked like the
					// readout going blank all over again the moment you
					// clicked to edit it).
					if (id == IDM_VIEW_PAGELABEL && self->pageEditActive_) {
						size_t sp = text.find(L' ');
						text = (sp != std::wstring::npos) ? text.substr(sp + 1) : std::wstring();
						UINT dpi2 = GetDpiForWindow(self->toolbar_);
						r.left += PageEditBoxWidth(r.right - r.left, dpi2);
					}
					if (!text.empty()) {
						SetBkMode(cd->nmcd.hdc, TRANSPARENT);
						SetTextColor(cd->nmcd.hdc, th.ctrlText);
						HFONT old = self->uiFont_
							? static_cast<HFONT>(SelectObject(cd->nmcd.hdc, self->uiFont_)) : nullptr;
						DrawTextW(cd->nmcd.hdc, text.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
						if (old) SelectObject(cd->nmcd.hdc, old);
					}
					return CDRF_SKIPDEFAULT;
				}
				TBBUTTONINFOW bi = {};
				bi.cbSize = sizeof(bi);
				bi.dwMask = TBIF_IMAGE | TBIF_STYLE;
				LRESULT gi = SendMessageW(self->toolbar_, TB_GETBUTTONINFOW, id,
					reinterpret_cast<LPARAM>(&bi));
				if (gi < 0 || (bi.fsStyle & BTNS_SEP)) {
					return CDRF_DODEFAULT;
				}

				UINT st = cd->nmcd.uItemState;
				COLORREF fill = 0;
				bool paint = true;
				if (st & CDIS_SELECTED)        fill = th.toolbarPressedPill;
				else if (st & CDIS_CHECKED)
					fill = (st & CDIS_HOT) ? th.toolbarPressedPill : th.toolbarActivePill;
				else if (st & CDIS_HOT)        fill = th.toolbarHoverPill;
				else paint = false;

				RECT r = cd->nmcd.rc;
				UINT dpi = GetDpiForWindow(self->toolbar_);
				if (paint) {
					RECT pr = r;
					InflateRect(&pr, -Scale(2, dpi), -Scale(2, dpi));
					int rad = Scale(5, dpi) * 2;
					HRGN rgn = CreateRoundRectRgn(pr.left, pr.top, pr.right + 1, pr.bottom + 1, rad, rad);
					HBRUSH br = CreateSolidBrush(fill);
					FillRgn(cd->nmcd.hdc, rgn, br);
					DeleteObject(br);
					DeleteObject(rgn);
				}
				if (bi.iImage >= 0) {
					int cx = 0, cy = 0;
					ImageList_GetIconSize(self->toolbarImages_, &cx, &cy);
					int ix = r.left + ((r.right - r.left) - cx) / 2;
					int iy = r.top + ((r.bottom - r.top) - cy) / 2;
					ImageList_Draw(self->toolbarImages_, bi.iImage, cd->nmcd.hdc, ix, iy, ILD_NORMAL);
				}
				return CDRF_SKIPDEFAULT;
			}
			}
			break;
		}
		break;
	}
	case WM_DRAWITEM: {
		auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
		if (dis->CtlType == ODT_TAB && dis->hwndItem == self->tabStrip_) {
			self->drawTabItem(dis);
			return TRUE;
		}
		if (dis->hwndItem == self->status_) {
			self->drawStatusBarItem(dis);
			return TRUE;
		}
		break;
	}
	case WM_DROPFILES: {
		HDROP drop = reinterpret_cast<HDROP>(wp);
		UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
		wchar_t path[MAX_PATH];
		for (UINT i = 0; i < n; ++i)
			if (DragQueryFileW(drop, i, path, MAX_PATH)) self->openDocument(path);
		DragFinish(drop);
		return 0;
	}
	case WM_COPYDATA: {
		// A second PDFast instance forwarded a file to open here (single-instance
		// routing -- see RunViewer). Open it as a tab and surface the window.
		auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
		if (cds && cds->dwData == kCopyDataOpenFile && cds->lpData && cds->cbData >= sizeof(wchar_t)) {
			std::wstring path(reinterpret_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
			while (!path.empty() && path.back() == L'\0') path.pop_back();
			if (!path.empty()) {
				if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
				self->openDocument(path.c_str());
				SetForegroundWindow(hwnd);
			}
			return TRUE;
		}
		break;
	}
	case WM_APP_UPDATE_RESULT: {
		// Posted from the background update-check thread; we own the UpdateInfo.
		std::unique_ptr<updater::UpdateInfo> info(reinterpret_cast<updater::UpdateInfo*>(lp));
		self->onUpdateResult(*info, wp != 0);
		return 0;
	}
	case WM_CLOSE:
		// Each tab may have its own unsaved changes -- check every one (this
		// also flips the visible active tab as it goes, which is expected:
		// the user sees exactly which document each prompt is about).
		for (int i = 0; i < static_cast<int>(self->tabs_.size()); ++i) {
			self->switchToTab(i);
			if (!self->promptSaveIfDirty()) return 0; // user cancelled -- abort closing
		}
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		if (self->uiFont_) { DeleteObject(self->uiFont_); self->uiFont_ = nullptr; }
		if (self->toolbarImages_) { ImageList_Destroy(self->toolbarImages_); self->toolbarImages_ = nullptr; }
		if (self->ctrlBgBrush_) { DeleteObject(self->ctrlBgBrush_); self->ctrlBgBrush_ = nullptr; }
		// Only quit the whole app once every top-level window is gone -- a
		// second window (from dragging a tab out / "Open in New Window")
		// closing on its own must not end the process out from under the
		// others. See g_liveFrameCount's comment.
		if (--g_liveFrameCount <= 0) PostQuitMessage(0);
		return 0;
	case WM_NCDESTROY:
		// The very last message this HWND ever receives -- safe to free
		// `self` now (every FrameWindow is heap-allocated and self-owning;
		// see SpawnNewWindow's comment). Must not touch `self` afterward.
		delete self;
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
// Default-app registration ("open .pdf files with this app by default").
// Windows requires explicit user consent for changing default apps (since
// Windows 8) -- writing registry keys alone never silently makes an app the
// default. This registers PDF Viewer as a *valid candidate* under the
// current user's own registry hive (no admin rights, easily undone from
// Settings > Default apps), then offers, once, to open the OS's own
// association UI so the user can actually confirm it.
// ===========================================================================
namespace {

const wchar_t kProgId[] = L"PDFast.pdf";
const wchar_t kAppRegKey[] = L"Software\\PDFast";
const wchar_t kAppName[] = L"PDFast";
const wchar_t kExeRegName[] = L"PDFast.exe"; // HKCU\...\Classes\Applications\<this> for "Open with"

void SetRegString(HKEY root, const std::wstring& subkey, const wchar_t* name, const std::wstring& value)
{
	HKEY key;
	if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
		RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
			static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
		RegCloseKey(key);
	}
}

void RegisterFileAssociation()
{
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	std::wstring openCmd = L"\"" + std::wstring(exePath) + L"\" \"%1\"";

	std::wstring progIdKey = L"Software\\Classes\\" + std::wstring(kProgId);
	SetRegString(HKEY_CURRENT_USER, progIdKey, nullptr, L"PDF Document");
	SetRegString(HKEY_CURRENT_USER, progIdKey + L"\\shell\\open\\command", nullptr, openCmd);
	SetRegString(HKEY_CURRENT_USER, progIdKey + L"\\DefaultIcon", nullptr, std::wstring(exePath) + L",0");

	std::wstring capKey = std::wstring(kAppRegKey) + L"\\Capabilities";
	const wchar_t* desc = L"A fast, lightweight native PDF viewer and editor.";
	SetRegString(HKEY_CURRENT_USER, capKey, L"ApplicationName", kAppName);
	SetRegString(HKEY_CURRENT_USER, capKey, L"ApplicationDescription", desc);
	SetRegString(HKEY_CURRENT_USER, capKey + L"\\FileAssociations", L".pdf", kProgId);

	SetRegString(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", kAppName, capKey);

	// Populate the right-click "Open with" list. This needs the per-application
	// registration under Classes\Applications\<exe> (friendly name + open verb +
	// declared supported types) AND the .pdf's OpenWithProgIds pointing at our
	// ProgID -- the RegisteredApplications/Capabilities block above only makes
	// us a *Default Apps* candidate, it does not by itself surface us in
	// "Open with".
	std::wstring appKey = L"Software\\Classes\\Applications\\" + std::wstring(kExeRegName);
	SetRegString(HKEY_CURRENT_USER, appKey, L"FriendlyAppName", kAppName);
	SetRegString(HKEY_CURRENT_USER, appKey + L"\\shell\\open\\command", nullptr, openCmd);
	SetRegString(HKEY_CURRENT_USER, appKey + L"\\DefaultIcon", nullptr, std::wstring(exePath) + L",0");
	SetRegString(HKEY_CURRENT_USER, appKey + L"\\SupportedTypes", L".pdf", L"");
	SetRegString(HKEY_CURRENT_USER, L"Software\\Classes\\.pdf\\OpenWithProgIds", kProgId, L"");

	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// Create (or self-heal) a Start Menu shortcut so PDFast shows up in the Start
// menu and in search -- there's no installer to do it, so the app plants its
// own .lnk on launch. Idempotent: if a shortcut already points at the current
// exe we leave it; if the exe was moved we repoint it (matching the file-
// association self-heal behavior). Under the *user's* Programs folder, so no
// admin rights are needed.
void EnsureStartMenuShortcut()
{
	wchar_t exePath[MAX_PATH];
	if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return;

	PWSTR programs = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)) || !programs) return;
	std::wstring lnkPath = std::wstring(programs) + L"\\" + std::wstring(kAppName) + L".lnk";
	CoTaskMemFree(programs);

	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	bool comOwned = SUCCEEDED(hr);
	if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
		IShellLinkW* link = nullptr;
		if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
			IPersistFile* pf = nullptr;
			if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf)))) {
				bool exists = false, sameTarget = false;
				if (SUCCEEDED(pf->Load(lnkPath.c_str(), STGM_READ))) {
					exists = true;
					wchar_t cur[MAX_PATH] = {};
					if (SUCCEEDED(link->GetPath(cur, MAX_PATH, nullptr, 0)))
						sameTarget = (_wcsicmp(cur, exePath) == 0);
				}
				if (!exists || !sameTarget) {
					std::wstring dir(exePath);
					size_t slash = dir.find_last_of(L"\\/");
					if (slash != std::wstring::npos) dir.resize(slash);
					link->SetPath(exePath);
					link->SetDescription(L"Fast, lightweight PDF viewer and editor");
					link->SetIconLocation(exePath, 0);
					link->SetWorkingDirectory(dir.c_str());
					pf->Save(lnkPath.c_str(), TRUE);
				}
				pf->Release();
			}
			link->Release();
		}
	}
	if (comOwned) CoUninitialize();
}

bool AlreadyOfferedDefaultPrompt()
{
	HKEY key;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kAppRegKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
	DWORD val = 0, size = sizeof(val), type = 0;
	bool has = RegQueryValueExW(key, L"AskedSetDefault", nullptr, &type,
		reinterpret_cast<BYTE*>(&val), &size) == ERROR_SUCCESS && type == REG_DWORD;
	RegCloseKey(key);
	return has && val != 0;
}

void MarkOfferedDefaultPrompt()
{
	HKEY key;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kAppRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
		DWORD val = 1;
		RegSetValueExW(key, L"AskedSetDefault", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
		RegCloseKey(key);
	}
}

// Opens the Windows Settings "Default apps" screen, deep-linked to PDFast's own
// page when possible. NOTE: the old COM API IApplicationAssociationRegistrationUI
// ::LaunchAdvancedAssociationUI is DEPRECATED on Windows 10/11 -- it no longer
// shows an association picker, it just pops a "go to Settings > Apps > Default
// apps" message box and returns S_OK, so we must NOT use it. Modern Windows also
// forbids apps from setting a per-extension default programmatically (anti-
// hijack), so the user has to make the final choice in Settings; the best we can
// do is take them straight there. `registeredAppUser=<name>` jumps to our app's
// page (name matches HKCU\Software\RegisteredApplications); if a build doesn't
// understand the parameter it simply opens the general Default-apps page.
void LaunchDefaultAppsUI(HWND owner)
{
	std::wstring deep = L"ms-settings:defaultapps?registeredAppUser=" + std::wstring(kAppName);
	HINSTANCE r = ShellExecuteW(owner, L"open", deep.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(r) <= 32)
		ShellExecuteW(owner, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
}

void OfferSetAsDefault(HWND owner)
{
	RegisterFileAssociation();
	if (AlreadyOfferedDefaultPrompt()) return;
	MarkOfferedDefaultPrompt();
	int r = MessageBoxW(owner,
		L"Would you like to set PDFast as your default app for opening PDF files?",
		L"PDFast", MB_YESNO | MB_ICONQUESTION);
	if (r == IDYES) LaunchDefaultAppsUI(owner);
}

} // namespace

// Used by "drag a tab out of the strip" and the tab context menu's "Open in
// New Window" -- the window class is already registered and common controls
// already initialized by the time either of those can happen (both require
// an existing window with an open document), so this just repeats the
// window-creation half of RunViewer() without any of the once-per-process
// setup (single-instance mutex, Start Menu shortcut, update check).
FrameWindow* SpawnNewWindow(HINSTANCE hInst, const std::vector<std::wstring>& paths)
{
	FrameWindow* fw = new FrameWindow(hInst);
	if (!fw->create(SW_SHOWNORMAL, /*checkForUpdates=*/false)) { delete fw; return nullptr; }
	for (const auto& path : paths) if (!path.empty()) fw->openDocument(path.c_str());
	return fw;
}

// ===========================================================================
// Entry
// ===========================================================================
int RunViewer(HINSTANCE hInstance, const wchar_t* optionalPath, int nCmdShow)
{
	// Single instance: if PDFast is already running, hand our file path to that
	// instance (as a new tab) and exit, instead of spawning another window.
	// The named mutex is process-lifetime; only the FIRST instance keeps it.
	HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, L"PDFast.SingleInstance.Mutex");
	if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
		// The first instance may still be creating its window; retry briefly.
		HWND existing = nullptr;
		for (int i = 0; i < 50 && !existing; ++i) {
			existing = FindWindowW(kFrameClass, nullptr);
			if (!existing) Sleep(20);
		}
		if (existing) {
			if (optionalPath && *optionalPath) {
				COPYDATASTRUCT cds = {};
				cds.dwData = kCopyDataOpenFile;
				cds.cbData = static_cast<DWORD>((wcslen(optionalPath) + 1) * sizeof(wchar_t));
				cds.lpData = const_cast<wchar_t*>(optionalPath);
				SendMessageW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
			}
			// We're the freshly-launched (foreground) process, so we're allowed
			// to hand the foreground to the existing window.
			if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
			SetForegroundWindow(existing);
		}
		CloseHandle(instanceMutex);
		return 0;
	}

	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
	InitCommonControlsEx(&icc);

	CanvasView::Register(hInstance);
	ThumbPanel::Register(hInstance);

	WNDCLASSW wc = {};
	wc.lpfnWndProc = FrameWindow::Proc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	wc.lpszClassName = kFrameClass;
	wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
	RegisterClassW(&wc);

	// Heap-allocated and self-deleting (see g_liveFrameCount's comment) --
	// not owned by a smart pointer here, since additional windows can be
	// spawned later (SpawnNewWindow) that must outlive this function's stack
	// frame the same way this first one does.
	FrameWindow* frame = new FrameWindow(hInstance);
	if (!frame->create(nCmdShow)) { delete frame; return 1; }

	if (optionalPath && *optionalPath) frame->openDocument(optionalPath);

	// Plant/refresh the Start Menu shortcut (no installer does it for us) so
	// PDFast is reachable from Start and search. Cheap + idempotent per launch.
	EnsureStartMenuShortcut();

	// One-time (per user) offer to register as the default .pdf handler.
	// Registers the file association candidate unconditionally (cheap,
	// idempotent, HKCU-only) -- this also populates the right-click "Open with"
	// list; the actual "make it default" confirmation always goes through
	// Windows' own UI, never silently.
	OfferSetAsDefault(frame->hwnd());

	HACCEL accel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDR_ACCEL));

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0)) {
		// Ctrl+C and Ctrl+A are also in our accelerator table (copy/select-all
		// for PDF page text), but native EDIT controls (search box, inline
		// annotation/form-field editors, page-jump box, ...) need to handle
		// those themselves -- e.g. Ctrl+A in the Find box must select that
		// box's text, not the whole PDF page. TranslateAcceleratorW runs
		// before the keystroke would ever reach the focused control's own
		// WM_KEYDOWN handler, so if one of those has focus, skip translation
		// entirely and let it fall through untranslated instead of hijacking
		// the keystroke into our page-text command.
		bool skipAccel = false;
		if (msg.message == WM_KEYDOWN && (msg.wParam == 'C' || msg.wParam == 'A') &&
			(GetKeyState(VK_CONTROL) & 0x8000)) {
			wchar_t cls[32] = {};
			HWND focused = GetFocus();
			if (focused && GetClassNameW(focused, cls, 32) && _wcsicmp(cls, L"Edit") == 0)
				skipAccel = true;
		}
		// Accelerators must be translated against whichever top-level
		// FrameWindow is actually active, not always the first one created --
		// dragging a tab out / "Open in New Window" means more than one can
		// now exist on this same thread's message queue.
		HWND active = GetActiveWindow();
		if (skipAccel || !active || !TranslateAcceleratorW(active, accel, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	return static_cast<int>(msg.wParam);
}
