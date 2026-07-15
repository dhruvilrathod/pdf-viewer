// Web page to PDF via a hidden Microsoft Edge WebView2 control's built-in
// PrintToPdf -- there's no small/fast way to render a real web page
// ourselves (that's a full HTML/CSS/JS engine), so this hands the work to
// the system's own Edge instead of bundling one.
#pragma once

#include <string>

// Loads `url` in a hidden WebView2 control and prints it to a standalone PDF
// at `pdfPath` (CoreWebView2::PrintToPdf, default print settings). Requires
// the WebView2 Runtime to be installed system-wide -- it ships with Edge and
// is present on essentially all current Windows 10/11 machines, but isn't
// bundled by this app (that runtime is much larger than this whole app's
// size budget). Returns false and fills `err` with a user-facing message if
// the runtime is missing, navigation fails (bad URL, no network), or
// printing fails. Runs synchronously on the calling thread, pumping its own
// local message loop while WebView2's async callbacks fire (same principle
// as a modal dialog's nested loop) -- like ConvertDocxToPdf, no progress UI
// or cancellation, and a slow page/first-run WebView2 init can take a while
// (bounded by an internal ~60s timeout).
bool ConvertWebPageToPdf(const wchar_t* url, const wchar_t* pdfPath, std::string& err);
