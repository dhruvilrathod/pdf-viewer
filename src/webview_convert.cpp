#include "webview_convert.h"

#include <windows.h>
#include <wrl.h>

#include "WebView2.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kHiddenClassName[] = L"PDFastWebView2Host";

// Shared by every stage of the async chain below. All the lambdas that
// reference this are local to ConvertWebPageToPdf() and never outlive it --
// the function's own message-pump loop blocks until `done` is set, so
// nothing here is ever touched after the function returns.
struct ConvertState {
	HWND hwnd = nullptr;
	std::wstring url;
	std::wstring pdfPath;
	bool done = false;
	bool ok = false;
	std::string err;
	ComPtr<ICoreWebView2Controller> controller;
};

} // namespace

bool ConvertWebPageToPdf(const wchar_t* url, const wchar_t* pdfPath, std::string& err)
{
	HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (!(SUCCEEDED(hrInit) || hrInit == RPC_E_CHANGED_MODE)) {
		err = "could not initialize COM";
		return false;
	}
	bool comOwned = SUCCEEDED(hrInit);

	// A minimal hidden window to host the WebView2 control -- it's never
	// shown; WebView2 just needs a real HWND to attach its own (also never
	// shown) child window to. Printing works fine without ever displaying it.
	static bool classRegistered = false;
	if (!classRegistered) {
		WNDCLASSW wc = {};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = kHiddenClassName;
		RegisterClassW(&wc);
		classRegistered = true;
	}
	HWND hwnd = CreateWindowExW(0, kHiddenClassName, L"", WS_POPUP, 0, 0, 1024, 768,
		nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!hwnd) {
		err = "could not create a hidden host window";
		if (comOwned) CoUninitialize();
		return false;
	}

	ConvertState state;
	state.hwnd = hwnd;
	state.url = url;
	state.pdfPath = pdfPath;

	wchar_t tempDir[MAX_PATH];
	GetTempPathW(MAX_PATH, tempDir);
	// WebView2 needs a writable profile folder -- keep it in %TEMP%, not
	// next to the exe, so this works even if the app is somewhere read-only.
	std::wstring userDataFolder = std::wstring(tempDir) + L"PDFast_WebView2";

	// The async chain, innermost (last to run) declared first so each outer
	// stage can reference the one after it by name instead of nesting
	// lambdas five deep. Every stage sets state.err + state.done on failure
	// and simply returns S_OK either way -- S_OK here means "the callback
	// itself ran fine", not "the operation it kicked off succeeded".

	auto onPrintDone = [&state](HRESULT errorCode, BOOL result) -> HRESULT {
		state.ok = SUCCEEDED(errorCode) && result;
		if (!state.ok) state.err = "WebView2 failed to print the page to PDF";
		state.done = true;
		return S_OK;
	};

	auto onNavigationCompleted = [&state, &onPrintDone](
		ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
		BOOL success = FALSE;
		if (args) args->get_IsSuccess(&success);
		if (!success) {
			state.err = "could not load the page (check the URL and your network connection)";
			state.done = true;
			return S_OK;
		}
		// PrintToPdf was added in ICoreWebView2_7 -- older installed
		// Runtimes won't expose it.
		ComPtr<ICoreWebView2_7> wv7;
		sender->QueryInterface(IID_PPV_ARGS(&wv7));
		if (!wv7) {
			state.err = "this system's WebView2 Runtime is too old to print to PDF";
			state.done = true;
			return S_OK;
		}
		HRESULT hr = wv7->PrintToPdf(state.pdfPath.c_str(), nullptr,
			Callback<ICoreWebView2PrintToPdfCompletedHandler>(onPrintDone).Get());
		if (FAILED(hr)) {
			state.err = "could not start printing to PDF";
			state.done = true;
		}
		return S_OK;
	};

	auto onControllerCreated = [&state, &onNavigationCompleted](
		HRESULT errorCode, ICoreWebView2Controller* controller) -> HRESULT {
		if (FAILED(errorCode) || !controller) {
			state.err = "could not create the WebView2 control";
			state.done = true;
			return S_OK;
		}
		state.controller = controller; // keeps it alive; also used to Close() during teardown
		ComPtr<ICoreWebView2> webview;
		controller->get_CoreWebView2(&webview);
		if (!webview) {
			state.err = "could not create the WebView2 control";
			state.done = true;
			return S_OK;
		}
		EventRegistrationToken token;
		webview->add_NavigationCompleted(
			Callback<ICoreWebView2NavigationCompletedEventHandler>(onNavigationCompleted).Get(), &token);
		webview->Navigate(state.url.c_str());
		return S_OK;
	};

	auto onEnvironmentCreated = [&state, &onControllerCreated](
		HRESULT errorCode, ICoreWebView2Environment* env) -> HRESULT {
		if (FAILED(errorCode) || !env) {
			state.err = "WebView2 Runtime is not installed (required to convert web pages to PDF)";
			state.done = true;
			return S_OK;
		}
		HRESULT hr = env->CreateCoreWebView2Controller(state.hwnd,
			Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(onControllerCreated).Get());
		if (FAILED(hr)) {
			state.err = "could not create the WebView2 control";
			state.done = true;
		}
		return S_OK;
	};

	HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataFolder.c_str(), nullptr,
		Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(onEnvironmentCreated).Get());
	if (FAILED(hr)) {
		err = "WebView2 Runtime is not installed (required to convert web pages to PDF)";
		DestroyWindow(hwnd);
		if (comOwned) CoUninitialize();
		return false;
	}

	// Pump this thread's message loop until the whole chain above has run to
	// completion (or timed out) -- WebView2's callbacks only fire while
	// messages are being pumped, same as a modal dialog's own nested loop.
	MSG msg;
	DWORD startTick = GetTickCount();
	const DWORD kTimeoutMs = 60000; // generous: slow pages/first-run WebView2 init can take a while
	while (!state.done && (GetTickCount() - startTick) < kTimeoutMs) {
		if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		} else {
			MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
		}
	}
	if (!state.done) err = "timed out waiting for the page to load/print";
	else err = state.err;

	if (state.controller) state.controller->Close();
	state.controller.Reset();
	DestroyWindow(hwnd);
	if (comOwned) CoUninitialize();
	return state.done && state.ok;
}
