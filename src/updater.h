#pragma once

// Self-update via GitHub Releases. No third-party libraries: the network I/O
// uses WinHTTP (a stock Windows system DLL), keeping pdfviewer.exe a single
// self-contained binary. See updater.cpp for the mechanics.

#include <windows.h>
#include <functional>
#include <string>

namespace updater {

// The GitHub repository the updater checks. Format: "owner/repo".
// >>> CHANGE THIS to your own repo before publishing a release. <<<
inline constexpr wchar_t kGitHubRepo[] = L"dhruvilrathod/pdf-viewer";

struct UpdateInfo {
	bool available = false;      // a newer release than this build exists
	bool checkFailed = false;    // network / parse error (no verdict)
	std::wstring latestVersion;  // e.g. L"1.2.0" (tag with any leading 'v' stripped)
	std::wstring downloadUrl;    // browser_download_url of the .exe asset
	std::wstring releaseUrl;     // release page (browser fallback)
};

// Low-level: hits the GitHub API for `repo`'s latest release and returns its
// tag + the .exe asset's download URL. No UI; safe on any thread. `err` gets
// a short human-readable reason on failure. Exposed so it can be exercised by
// the pdfviewer_updatetest console tool against a real repo.
bool FetchLatestRelease(const std::wstring& repo, std::wstring& tagOut,
	std::wstring& exeUrlOut, std::string& err);

// Compares `latestTag` (e.g. "v1.2.0") against `currentVersion` (e.g. "1.0.0")
// numerically (major.minor.patch). Returns true if latestTag is strictly newer.
bool IsNewer(const std::wstring& latestTag, const std::wstring& currentVersion);

// Convenience: FetchLatestRelease(kGitHubRepo) + IsNewer(tag, APP_VERSION_STR),
// packaged into an UpdateInfo. Synchronous; call from a worker thread.
UpdateInfo CheckForUpdate();

// Downloads `url` to `destPath` (following redirects). `progress` is called
// with 0..100 (or -1 if the server doesn't report a size); return false from
// it to cancel. Returns true only on a complete download.
bool DownloadFile(const std::wstring& url, const std::wstring& destPath,
	const std::function<bool(int)>& progress);

enum class ApplyResult { Relaunching, Failed, Canceled };

// Shows a small modal progress window over `owner`, downloads the update,
// swaps it in for the running exe, and relaunches. On success returns
// Relaunching -- the caller must then exit this process promptly. Everything
// is left untouched on failure/cancel.
ApplyResult DownloadAndApply(HWND owner, const UpdateInfo& info);

// Full path of the currently running executable.
std::wstring SelfPath();

// Deletes the "<exe>.old" file a previous update left behind. Call once at
// startup (harmless if there's nothing to clean).
void CleanupAfterUpdate();

} // namespace updater
