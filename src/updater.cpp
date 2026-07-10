#include "updater.h"
#include "version.h"

#include <winhttp.h>
#include <shellapi.h>
#include <commctrl.h>

#include <atomic>
#include <cwctype>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace updater {
namespace {

// RAII for WinHTTP handles.
struct WHandle {
	HINTERNET h = nullptr;
	~WHandle() { if (h) WinHttpCloseHandle(h); }
	operator HINTERNET() const { return h; }
};

std::wstring widen(const std::string& s)
{
	if (s.empty()) return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
	return w;
}

// Streams an HTTPS GET of `url` to `sink` (called with each chunk plus running
// totals; return false to abort). Follows redirects (WinHTTP default), which
// GitHub release-asset URLs rely on. `accept` is an optional extra header line.
using ByteSink = std::function<bool(const char* data, DWORD n, DWORD64 total, DWORD64 sofar)>;

bool httpsGet(const std::wstring& url, const std::wstring& accept, const ByteSink& sink, std::string& err)
{
	URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof(uc);
	wchar_t host[256] = {}, path[4096] = {}, extra[4096] = {};
	uc.lpszHostName = host; uc.dwHostNameLength = ARRAYSIZE(host) - 1;
	uc.lpszUrlPath = path; uc.dwUrlPathLength = ARRAYSIZE(path) - 1;
	uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = ARRAYSIZE(extra) - 1;
	if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "bad URL"; return false; }
	std::wstring fullPath = std::wstring(path) + extra;

	WHandle session{ WinHttpOpen(L"pdfviewer-updater/1.0",
		WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
	if (!session.h) { err = "WinHttpOpen failed"; return false; }
	WHandle connect{ WinHttpConnect(session, host, uc.nPort, 0) };
	if (!connect.h) { err = "WinHttpConnect failed"; return false; }
	DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
	WHandle req{ WinHttpOpenRequest(connect, L"GET", fullPath.c_str(), nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags) };
	if (!req.h) { err = "WinHttpOpenRequest failed"; return false; }
	if (!accept.empty())
		WinHttpAddRequestHeaders(req, accept.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);

	if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
		err = "request send failed (no network?)"; return false;
	}
	if (!WinHttpReceiveResponse(req, nullptr)) { err = "no response"; return false; }

	DWORD code = 0, sz = sizeof(code);
	WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
	if (code != 200) { err = "HTTP status " + std::to_string(code); return false; }

	DWORD64 total = 0; DWORD clen = 0, clenSz = sizeof(clen);
	if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &clen, &clenSz, WINHTTP_NO_HEADER_INDEX))
		total = clen;

	DWORD64 sofar = 0;
	for (;;) {
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(req, &avail)) { err = "read failed"; return false; }
		if (avail == 0) break;
		std::vector<char> buf(avail);
		DWORD read = 0;
		if (!WinHttpReadData(req, buf.data(), avail, &read)) { err = "read failed"; return false; }
		if (read == 0) break;
		sofar += read;
		if (!sink(buf.data(), read, total, sofar)) { err = "canceled"; return false; }
	}
	return true;
}

// Minimal JSON string-value extraction -- GitHub's release JSON is stable and
// flat enough that a full parser would be overkill here. Finds "key":"value"
// and returns value (handling the few escapes that can appear).
std::string jsonString(const std::string& body, const char* key)
{
	std::string pat = std::string("\"") + key + "\"";
	size_t p = body.find(pat);
	if (p == std::string::npos) return "";
	p = body.find(':', p + pat.size());
	if (p == std::string::npos) return "";
	p = body.find('"', p);
	if (p == std::string::npos) return "";
	++p;
	std::string out;
	while (p < body.size()) {
		char ch = body[p++];
		if (ch == '\\' && p < body.size()) {
			char e = body[p++];
			out += (e == 'n') ? '\n' : (e == 't') ? '\t' : e; // \/ \" \\ pass through as the escaped char
		} else if (ch == '"') {
			break;
		} else {
			out += ch;
		}
	}
	return out;
}

// The first release asset whose download URL ends in ".exe".
std::string firstExeAsset(const std::string& body)
{
	const std::string key = "\"browser_download_url\"";
	size_t p = 0;
	while ((p = body.find(key, p)) != std::string::npos) {
		size_t c = body.find(':', p + key.size());
		size_t q = (c == std::string::npos) ? std::string::npos : body.find('"', c);
		if (q == std::string::npos) break;
		std::string url;
		size_t i = q + 1;
		while (i < body.size()) {
			char ch = body[i++];
			if (ch == '\\' && i < body.size()) url += body[i++];
			else if (ch == '"') break;
			else url += ch;
		}
		if (url.size() >= 4) {
			std::string tail = url.substr(url.size() - 4);
			for (auto& ch : tail) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			if (tail == ".exe") return url;
		}
		p = i;
	}
	return "";
}

void parseVer(const std::wstring& s, int v[3])
{
	v[0] = v[1] = v[2] = 0;
	size_t i = 0;
	while (i < s.size() && !std::iswdigit(s[i])) ++i; // skip leading 'v' etc.
	swscanf_s(s.c_str() + i, L"%d.%d.%d", &v[0], &v[1], &v[2]);
}

bool applyAndRelaunch(const std::wstring& newExe)
{
	std::wstring self = SelfPath();
	std::wstring old = self + L".old";
	DeleteFileW(old.c_str()); // clear any stale leftover first
	// Renaming a *running* exe is allowed on Windows (rename != write); this is
	// the standard self-replace trick -- move ourselves aside, move the new
	// build into our name, relaunch it, and let the next start delete the .old.
	if (!MoveFileExW(self.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
	if (!MoveFileExW(newExe.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		MoveFileExW(old.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING); // roll back
		return false;
	}
	HINSTANCE h = ShellExecuteW(nullptr, L"open", self.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	return reinterpret_cast<INT_PTR>(h) > 32;
}

// --- Download progress window ---------------------------------------------

constexpr UINT WM_DL_PROGRESS = WM_APP + 101;
constexpr UINT WM_DL_DONE = WM_APP + 102;
constexpr wchar_t kProgClass[] = L"PdfUpdateProgress";

struct DlState {
	std::wstring url, dest;
	HWND wnd = nullptr, bar = nullptr, text = nullptr;
	std::atomic<bool> cancel{ false };
	std::atomic<bool> done{ false };
	std::atomic<bool> ok{ false };
};

LRESULT CALLBACK ProgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	auto* st = reinterpret_cast<DlState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	switch (msg) {
	case WM_NCCREATE:
		SetWindowLongPtrW(hwnd, GWLP_USERDATA,
			reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
		return DefWindowProcW(hwnd, msg, wp, lp);
	case WM_DL_PROGRESS:
		if (st && st->bar) {
			int pct = static_cast<int>(wp);
			if (pct >= 0) SendMessageW(st->bar, PBM_SETPOS, static_cast<WPARAM>(pct), 0);
		}
		return 0;
	case WM_DL_DONE:
		if (st) { st->ok = (wp != 0); st->done = true; }
		return 0;
	case WM_COMMAND:
		if (st && LOWORD(wp) == 1 /*Cancel*/) {
			st->cancel = true;
			if (st->text) SetWindowTextW(st->text, L"Cancelling…");
			EnableWindow(reinterpret_cast<HWND>(lp), FALSE);
		}
		return 0;
	case WM_CLOSE:
		if (st) st->cancel = true; // treat the X like Cancel; don't destroy mid-download
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

void ensureProgClass()
{
	static bool done = false;
	if (done) return;
	WNDCLASSW wc = {};
	wc.lpfnWndProc = ProgProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	wc.lpszClassName = kProgClass;
	RegisterClassW(&wc);
	done = true;
}

} // namespace

std::wstring SelfPath()
{
	std::vector<wchar_t> buf(1024);
	DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
	return std::wstring(buf.data(), n);
}

void CleanupAfterUpdate()
{
	std::wstring old = SelfPath() + L".old";
	// The just-replaced old process may take a moment to fully exit and release
	// the file; retry briefly, then give up (next launch will get it).
	for (int i = 0; i < 12; ++i) {
		if (DeleteFileW(old.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return;
		Sleep(150);
	}
}

bool FetchLatestRelease(const std::wstring& repo, std::wstring& tagOut,
	std::wstring& exeUrlOut, std::string& err)
{
	std::wstring url = L"https://api.github.com/repos/" + repo + L"/releases/latest";
	std::string body;
	ByteSink sink = [&](const char* d, DWORD n, DWORD64, DWORD64) { body.append(d, n); return true; };
	if (!httpsGet(url, L"Accept: application/vnd.github+json\r\n", sink, err)) return false;

	std::string tag = jsonString(body, "tag_name");
	if (tag.empty()) { err = "no tag_name in response"; return false; }
	tagOut = widen(tag);
	exeUrlOut = widen(firstExeAsset(body)); // may be empty -> caller offers the browser
	return true;
}

bool IsNewer(const std::wstring& latestTag, const std::wstring& currentVersion)
{
	int a[3], b[3];
	parseVer(latestTag, a);
	parseVer(currentVersion, b);
	for (int i = 0; i < 3; ++i)
		if (a[i] != b[i]) return a[i] > b[i];
	return false;
}

UpdateInfo CheckForUpdate()
{
	UpdateInfo info;
	std::wstring tag, exe;
	std::string err;
	if (!FetchLatestRelease(kGitHubRepo, tag, exe, err)) { info.checkFailed = true; return info; }
	// Strip a leading 'v' for display.
	std::wstring ver = tag;
	if (!ver.empty() && (ver[0] == L'v' || ver[0] == L'V')) ver.erase(0, 1);
	info.latestVersion = ver;
	info.downloadUrl = exe;
	info.releaseUrl = L"https://github.com/" + std::wstring(kGitHubRepo) + L"/releases/latest";
	info.available = IsNewer(tag, L"" APP_VERSION_STR);
	return info;
}

bool DownloadFile(const std::wstring& url, const std::wstring& destPath,
	const std::function<bool(int)>& progress)
{
	HANDLE f = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	std::string err;
	bool ok = httpsGet(url, L"", [&](const char* d, DWORD n, DWORD64 total, DWORD64 sofar) {
		DWORD written = 0;
		if (!WriteFile(f, d, n, &written, nullptr) || written != n) return false;
		int pct = total ? static_cast<int>(sofar * 100 / total) : -1;
		return progress ? progress(pct) : true;
	}, err);
	CloseHandle(f);
	if (!ok) DeleteFileW(destPath.c_str());
	return ok;
}

ApplyResult DownloadAndApply(HWND owner, const UpdateInfo& info)
{
	if (info.downloadUrl.empty()) return ApplyResult::Failed;

	std::wstring self = SelfPath();
	size_t slash = self.find_last_of(L"\\/");
	std::wstring dir = (slash == std::wstring::npos) ? L"." : self.substr(0, slash);
	std::wstring dest = dir + L"\\pdfviewer.update.exe";

	DlState st;
	st.url = info.downloadUrl;
	st.dest = dest;

	ensureProgClass();
	UINT dpi = GetDpiForWindow(owner ? owner : GetDesktopWindow());
	auto sc = [&](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
	int w = sc(400), h = sc(130);
	RECT orc{};
	if (owner) GetWindowRect(owner, &orc); else GetWindowRect(GetDesktopWindow(), &orc);
	int x = orc.left + ((orc.right - orc.left) - w) / 2;
	int y = orc.top + ((orc.bottom - orc.top) - h) / 2;

	if (owner) EnableWindow(owner, FALSE);
	st.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kProgClass, L"Updating PDF Viewer",
		WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
		x, y, w, h, owner, nullptr, GetModuleHandleW(nullptr), &st);
	if (!st.wnd) { if (owner) EnableWindow(owner, TRUE); return ApplyResult::Failed; }

	HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	st.text = CreateWindowW(L"STATIC", L"Downloading the latest version…",
		WS_CHILD | WS_VISIBLE, sc(14), sc(10), w - sc(28), sc(20), st.wnd, nullptr, nullptr, nullptr);
	st.bar = CreateWindowW(PROGRESS_CLASSW, L"",
		WS_CHILD | WS_VISIBLE, sc(14), sc(36), w - sc(28), sc(20), st.wnd, nullptr, nullptr, nullptr);
	HWND cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
		w - sc(90), sc(66), sc(74), sc(24), st.wnd, reinterpret_cast<HMENU>(1), nullptr, nullptr);
	SendMessageW(st.text, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	SendMessageW(st.bar, PBM_SETRANGE32, 0, 100);

	std::thread worker([&st] {
		bool ok = DownloadFile(st.url, st.dest, [&st](int pct) {
			PostMessageW(st.wnd, WM_DL_PROGRESS, static_cast<WPARAM>(pct), 0);
			return !st.cancel.load();
		});
		st.ok = ok;
		PostMessageW(st.wnd, WM_DL_DONE, ok ? 1 : 0, 0);
	});

	// Local modal message pump until the worker signals completion.
	MSG msg;
	while (!st.done.load() && GetMessageW(&msg, nullptr, 0, 0)) {
		if (!IsDialogMessageW(st.wnd, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	worker.join();

	if (owner) EnableWindow(owner, TRUE);
	DestroyWindow(st.wnd);

	if (st.cancel.load()) { DeleteFileW(dest.c_str()); return ApplyResult::Canceled; }
	if (!st.ok.load()) { DeleteFileW(dest.c_str()); return ApplyResult::Failed; }
	if (!applyAndRelaunch(dest)) { DeleteFileW(dest.c_str()); return ApplyResult::Failed; }
	return ApplyResult::Relaunching;
}

} // namespace updater
