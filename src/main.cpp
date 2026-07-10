#include <windows.h>
#include <shellapi.h>
#include <string>
#include "viewer_window.h"

#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow)
{
	// Fallback DPI awareness in case the manifest isn't honored.
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// GDI+ is used only to draw the toolbar's vector icons (anti-aliased
	// lines/shapes) -- it's a system DLL (gdiplus.dll ships with Windows),
	// not an external dependency the single-exe distribution needs to carry.
	Gdiplus::GdiplusStartupInput gdiplusInput;
	ULONG_PTR gdiplusToken = 0;
	Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

	// A single file path may be passed on the command line (file association).
	std::wstring path;
	if (lpCmdLine && *lpCmdLine) {
		int argc = 0;
		LPWSTR* argv = CommandLineToArgvW(lpCmdLine, &argc);
		if (argv) {
			if (argc >= 1 && argv[0] && *argv[0]) path = argv[0];
			LocalFree(argv);
		}
	}

	int rc = RunViewer(hInstance, path.empty() ? nullptr : path.c_str(), nCmdShow);

	Gdiplus::GdiplusShutdown(gdiplusToken);
	return rc;
}
