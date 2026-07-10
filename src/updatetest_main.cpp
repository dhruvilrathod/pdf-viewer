// Console harness for the updater's network + parse path (no GUI). Point it at
// any public GitHub repo that publishes releases to confirm the GitHub API
// request, JSON extraction, and version compare all work end to end.
//
//   pdfviewer_updatetest <owner/repo> [currentVersion]
//
// e.g.  pdfviewer_updatetest cli/cli 2.0.0
#include <cstdio>
#include <string>
#include "updater.h"

static std::string narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

int wmain(int argc, wchar_t** argv)
{
	if (argc < 2) {
		std::fwprintf(stderr, L"usage: %s <owner/repo> [currentVersion]\n", argv[0]);
		return 1;
	}
	std::wstring repo = argv[1];
	std::wstring current = argc > 2 ? argv[2] : L"0.0.0";

	std::wstring tag, exe;
	std::string err;
	if (!updater::FetchLatestRelease(repo, tag, exe, err)) {
		std::printf("FetchLatestRelease FAILED: %s\n", err.c_str());
		return 1;
	}
	std::printf("latest tag      : %s\n", narrow(tag).c_str());
	std::printf(".exe asset url  : %s\n", exe.empty() ? "(none found)" : narrow(exe).c_str());
	bool newer = updater::IsNewer(tag, current);
	std::printf("current version : %s\n", narrow(current).c_str());
	std::printf("is newer?       : %s\n", newer ? "YES (would prompt to update)" : "no");
	return 0;
}
