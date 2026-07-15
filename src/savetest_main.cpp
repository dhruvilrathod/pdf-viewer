// Diagnostic tool (not part of the shipped app): exercises PdfDocument's
// exact edit+save code path with no GUI/window involved, to isolate save
// corruption bugs independent of any Win32 input/focus issues.
#include <cstdio>
#include <string>
#include <vector>
#include "pdf_document.h"
#include "webview_convert.h"

namespace {
std::string ToUtf8(const wchar_t* w)
{
	if (!w) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (n <= 0) return {};
	std::string s(static_cast<size_t>(n - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
	if (argc < 3) {
		std::fwprintf(stderr, L"usage: %s input.pdf output.pdf [incremental]\n", argv[0]);
		std::fwprintf(stderr, L"       %s input.pdf output.pdf --pwtest <password>\n", argv[0]);
		std::fwprintf(stderr, L"       %s input.pdf outbase --pagetest\n", argv[0]);
		return 1;
	}
	const wchar_t* input = argv[1];
	const wchar_t* output = argv[2];

	// --pagetest mode: exercises the merge/split/organize/resize/flatten/
	// compress/redact PdfDocument methods headlessly against `input`, using
	// `input` itself as both the primary document and (via openExternalFile)
	// the "other file" for merge/insert testing, so a single sample PDF is
	// enough. `output` is used as a filename prefix for the various outputs
	// this writes (e.g. "<output>_rebuilt.pdf").
	if (argc > 3 && wcscmp(argv[3], L"--pagetest") == 0) {
		auto openFresh = [&](PdfDocument& d) -> bool {
			std::string e; bool npw = false;
			if (!d.open(input, e, npw)) { std::printf("  open failed: %s\n", e.c_str()); return false; }
			return true;
		};
		auto outPath = [&](const wchar_t* suffix) { return std::wstring(output) + suffix; };
		int failures = 0;

		// --- rebuildFromPages (backs Organize's Done and Merge) -----------
		{
			std::printf("[rebuildFromPages]\n");
			PdfDocument doc;
			if (!openFresh(doc)) return 1;
			int origCount = doc.pageCount();
			std::string e;
			int ext = doc.openExternalFile(input, e);
			std::printf("  openExternalFile -> %d (err='%s')\n", ext, e.c_str());
			if (ext < 0) { std::printf("  FAIL: could not open external file\n"); ++failures; }
			else {
				std::printf("  externalPageCount=%d (expect %d)\n", doc.externalPageCount(ext), origCount);
				PageBitmap eb = doc.renderExternalPage(ext, 0, 1.0f);
				std::printf("  renderExternalPage(0) -> %dx%d\n", eb.width, eb.height);

				std::vector<PdfDocument::PagePlanEntry> plan;
				for (int p = origCount - 1; p >= 0; --p) plan.push_back({ -1, p, 0 }); // this doc, reversed
				for (int p = 0; p < doc.externalPageCount(ext); ++p) plan.push_back({ ext, p, 90 }); // + external, rotated

				if (!doc.rebuildFromPages(plan, e)) {
					std::printf("  FAIL: rebuildFromPages: %s\n", e.c_str());
					++failures;
				} else {
					std::printf("  rebuildFromPages OK: pageCount=%d (expect %d), dirty=%d\n",
						doc.pageCount(), origCount * 2, doc.dirty());
					if (doc.pageCount() != origCount * 2) ++failures;
					bool renderOk = true;
					for (int p = 0; p < doc.pageCount(); ++p) {
						PageBitmap b = doc.renderPage(p, 1.0f);
						if (!b.hbmp) { renderOk = false; std::printf("  RENDER FAILED on rebuilt page %d\n", p); }
					}
					std::printf("  all rebuilt pages rendered: %s\n", renderOk ? "yes" : "NO (bug!)");
					if (!renderOk) ++failures;

					std::wstring rebuiltPath = outPath(L"_rebuilt.pdf");
					if (!doc.save(rebuiltPath.c_str(), false, e)) {
						std::printf("  FAIL: save rebuilt: %s\n", e.c_str());
						++failures;
					} else {
						PdfDocument doc2;
						std::string e2; bool npw2 = false;
						if (!doc2.open(rebuiltPath.c_str(), e2, npw2)) {
							std::printf("  FAIL: reopen rebuilt: %s\n", e2.c_str());
							++failures;
						} else {
							std::printf("  reopened rebuilt OK: pageCount=%d\n", doc2.pageCount());
							for (int p = 0; p < doc2.pageCount(); ++p) {
								PageBitmap b = doc2.renderPage(p, 1.0f);
								if (!b.hbmp) { std::printf("  RENDER FAILED on reopened rebuilt page %d\n", p); ++failures; }
							}
						}
					}
				}
			}
		}

		// --- rebuildFromPages into a BLANK document (backs Merge into an
		// empty tab -- doc_ is null / never open()'d, all pages external) ----
		{
			std::printf("[rebuildFromPages(blank/merge)]\n");
			PdfDocument doc; // deliberately NOT opened -- mimics a fresh Merge tab
			std::string e;
			int ext = doc.openExternalFile(input, e);
			std::printf("  openExternalFile -> %d (err='%s'), isOpen(before)=%d\n", ext, e.c_str(), doc.isOpen());
			if (ext < 0) { std::printf("  FAIL: could not open external file\n"); ++failures; }
			else {
				int extCount = doc.externalPageCount(ext);
				std::vector<PdfDocument::PagePlanEntry> plan;
				for (int p = 0; p < extCount; ++p) plan.push_back({ ext, p, 0 });
				if (!doc.rebuildFromPages(plan, e)) {
					std::printf("  FAIL: rebuildFromPages(blank): %s\n", e.c_str());
					++failures;
				} else {
					std::printf("  rebuildFromPages(blank) OK: isOpen=%d pageCount=%d (expect %d)\n",
						doc.isOpen(), doc.pageCount(), extCount);
					if (!doc.isOpen()) { std::printf("  FAIL: isOpen() false after blank merge\n"); ++failures; }
					if (doc.pageCount() != extCount) ++failures;
					for (int p = 0; p < doc.pageCount(); ++p) {
						PageBitmap b = doc.renderPage(p, 1.0f);
						if (!b.hbmp) { std::printf("  RENDER FAILED on blank-merge page %d\n", p); ++failures; }
					}
					std::wstring mergedPath = outPath(L"_merged.pdf");
					if (!doc.save(mergedPath.c_str(), false, e)) {
						std::printf("  FAIL: save blank-merge: %s\n", e.c_str());
						++failures;
					} else {
						PdfDocument doc2; std::string e2; bool npw2 = false;
						if (!doc2.open(mergedPath.c_str(), e2, npw2))
							{ std::printf("  FAIL: reopen blank-merge: %s\n", e2.c_str()); ++failures; }
						else std::printf("  reopened blank-merge OK: pageCount=%d\n", doc2.pageCount());
					}
				}
			}
		}

		// --- exportPages (backs Split) -------------------------------------
		{
			std::printf("[exportPages]\n");
			PdfDocument doc;
			if (!openFresh(doc)) return 1;
			std::vector<int> pages;
			for (int p = 0; p < doc.pageCount() && p < 2; ++p) pages.push_back(p);
			std::wstring splitPath = outPath(L"_split.pdf");
			std::string e;
			if (!doc.exportPages(pages, splitPath.c_str(), e)) {
				std::printf("  FAIL: exportPages: %s\n", e.c_str());
				++failures;
			} else {
				PdfDocument doc2;
				std::string e2; bool npw2 = false;
				if (!doc2.open(splitPath.c_str(), e2, npw2)) {
					std::printf("  FAIL: reopen split output: %s\n", e2.c_str());
					++failures;
				} else {
					std::printf("  exportPages OK: pageCount=%d (expect %d)\n",
						doc2.pageCount(), static_cast<int>(pages.size()));
					if (doc2.pageCount() != static_cast<int>(pages.size())) ++failures;
					for (int p = 0; p < doc2.pageCount(); ++p) {
						PageBitmap b = doc2.renderPage(p, 1.0f);
						if (!b.hbmp) { std::printf("  RENDER FAILED on split page %d\n", p); ++failures; }
					}
				}
			}
		}

		// --- resizeToA4 -----------------------------------------------------
		{
			std::printf("[resizeToA4]\n");
			PdfDocument doc;
			if (!openFresh(doc)) return 1;
			std::string e;
			if (!doc.resizeToA4(e)) {
				std::printf("  FAIL: resizeToA4: %s\n", e.c_str());
				++failures;
			} else {
				PageSizePt sz = doc.pageSize(0);
				std::printf("  resizeToA4 OK: page0 size=%.1fx%.1f (expect ~595x842 or ~842x595)\n", sz.w, sz.h);
				std::wstring a4Path = outPath(L"_a4.pdf");
				if (!doc.save(a4Path.c_str(), false, e)) {
					std::printf("  FAIL: save a4: %s\n", e.c_str());
					++failures;
				} else {
					PdfDocument doc2;
					std::string e2; bool npw2 = false;
					if (!doc2.open(a4Path.c_str(), e2, npw2)) {
						std::printf("  FAIL: reopen a4: %s\n", e2.c_str());
						++failures;
					} else {
						for (int p = 0; p < doc2.pageCount(); ++p) {
							PageBitmap b = doc2.renderPage(p, 1.0f);
							if (!b.hbmp) { std::printf("  RENDER FAILED on a4 page %d\n", p); ++failures; }
						}
						std::printf("  reopened a4 OK: pageCount=%d, page0=%.1fx%.1f\n",
							doc2.pageCount(), doc2.pageSize(0).w, doc2.pageSize(0).h);
					}
				}
			}
		}

		// --- flattenToImages --------------------------------------------------
		{
			std::printf("[flattenToImages]\n");
			PdfDocument doc;
			if (!openFresh(doc)) return 1;
			auto charsBefore = doc.pageChars(0);
			std::string e;
			if (!doc.flattenToImages(e)) {
				std::printf("  FAIL: flattenToImages: %s\n", e.c_str());
				++failures;
			} else {
				auto charsAfter = doc.pageChars(0);
				std::printf("  flattenToImages OK: pageChars(0) %d -> %d (expect 0)\n",
					static_cast<int>(charsBefore.size()), static_cast<int>(charsAfter.size()));
				if (!charsAfter.empty()) ++failures;
				std::wstring flatPath = outPath(L"_flat.pdf");
				if (!doc.save(flatPath.c_str(), false, e)) {
					std::printf("  FAIL: save flat: %s\n", e.c_str());
					++failures;
				} else {
					PdfDocument doc2;
					std::string e2; bool npw2 = false;
					if (!doc2.open(flatPath.c_str(), e2, npw2)) {
						std::printf("  FAIL: reopen flat: %s\n", e2.c_str());
						++failures;
					} else {
						for (int p = 0; p < doc2.pageCount(); ++p) {
							PageBitmap b = doc2.renderPage(p, 1.0f);
							if (!b.hbmp) { std::printf("  RENDER FAILED on flat page %d\n", p); ++failures; }
						}
						std::printf("  reopened flat OK: pageCount=%d\n", doc2.pageCount());
					}
				}
			}
		}

		// --- flattenAnnotationsToContent ---------------------------------------
		{
			std::printf("[flattenAnnotationsToContent]\n");
			PdfDocument doc;
			if (!openFresh(doc)) return 1;
			auto charsBefore = doc.pageChars(0);
			auto annotsBefore = doc.pageAnnots(0);
			std::string e;
			if (!doc.flattenAnnotationsToContent(e)) {
				std::printf("  FAIL: flattenAnnotationsToContent: %s\n", e.c_str());
				++failures;
			} else {
				auto charsAfter = doc.pageChars(0);
				auto annotsAfter = doc.pageAnnots(0);
				// pageChars() extracts from the page's own content stream only
				// (fz_run_page_contents, not fz_run_page -- see its comment), so
				// it never included annotation text before flattening even
				// though the annotation was visibly rendered. Baking a FreeText
				// annotation's appearance into page content is expected to make
				// its text newly extractable, so the count can legitimately
				// grow; it must never shrink (that would mean original page
				// text got clobbered instead of appended-to).
				std::printf("  flattenAnnotationsToContent OK: pageChars(0) %d -> %d (expect >=), annots %d -> %d (expect 0)\n",
					static_cast<int>(charsBefore.size()), static_cast<int>(charsAfter.size()),
					static_cast<int>(annotsBefore.size()), static_cast<int>(annotsAfter.size()));
				if (charsAfter.size() < charsBefore.size()) ++failures;
				if (!annotsAfter.empty()) ++failures;
				std::wstring flatPath = outPath(L"_flatannot.pdf");
				if (!doc.save(flatPath.c_str(), false, e)) {
					std::printf("  FAIL: save flatannot: %s\n", e.c_str());
					++failures;
				} else {
					PdfDocument doc2;
					std::string e2; bool npw2 = false;
					if (!doc2.open(flatPath.c_str(), e2, npw2)) {
						std::printf("  FAIL: reopen flatannot: %s\n", e2.c_str());
						++failures;
					} else {
						for (int p = 0; p < doc2.pageCount(); ++p) {
							PageBitmap b = doc2.renderPage(p, 1.0f);
							if (!b.hbmp) { std::printf("  RENDER FAILED on flatannot page %d\n", p); ++failures; }
						}
						std::printf("  reopened flatannot OK: pageCount=%d, pageChars(0)=%d\n",
							doc2.pageCount(), static_cast<int>(doc2.pageChars(0).size()));
					}
				}
			}
		}

		// --- compress -----------------------------------------------------
		{
			std::printf("[compress]\n");
			PdfDocument doc;
			if (!openFresh(doc)) return 1;
			std::string e;
			if (!doc.compress(e)) {
				std::printf("  FAIL: compress: %s\n", e.c_str());
				++failures;
			} else {
				std::wstring cPath = outPath(L"_compressed.pdf");
				if (!doc.save(cPath.c_str(), false, e)) {
					std::printf("  FAIL: save compressed: %s\n", e.c_str());
					++failures;
				} else {
					PdfDocument doc2;
					std::string e2; bool npw2 = false;
					if (!doc2.open(cPath.c_str(), e2, npw2)) {
						std::printf("  FAIL: reopen compressed: %s\n", e2.c_str());
						++failures;
					} else {
						for (int p = 0; p < doc2.pageCount(); ++p) {
							PageBitmap b = doc2.renderPage(p, 1.0f);
							if (!b.hbmp) { std::printf("  RENDER FAILED on compressed page %d\n", p); ++failures; }
						}
						HANDLE h1 = CreateFileW(input, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
						HANDLE h2 = CreateFileW(cPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
						LARGE_INTEGER s1{}, s2{};
						if (h1 != INVALID_HANDLE_VALUE) { GetFileSizeEx(h1, &s1); CloseHandle(h1); }
						if (h2 != INVALID_HANDLE_VALUE) { GetFileSizeEx(h2, &s2); CloseHandle(h2); }
						std::printf("  reopened compressed OK: pageCount=%d, size %lld -> %lld bytes\n",
							doc2.pageCount(), static_cast<long long>(s1.QuadPart), static_cast<long long>(s2.QuadPart));
					}
				}
			}
		}

		// --- addRedaction / pendingRedactionCount / applyRedactions --------
		{
			std::printf("[redact]\n");
			PdfDocument doc;
			if (!openFresh(doc)) return 1;
			PageRectPt bound = doc.pageBound(0);
			PageRectPt mark{ bound.x0 + 10, bound.y0 + 10, bound.x0 + 200, bound.y0 + 60 };
			auto charsBefore = doc.pageChars(0);
			std::string e;
			if (!doc.addRedaction(0, mark, e)) {
				std::printf("  FAIL: addRedaction: %s\n", e.c_str());
				++failures;
			} else {
				int pending = doc.pendingRedactionCount();
				std::printf("  pendingRedactionCount=%d (expect 1)\n", pending);
				if (pending != 1) ++failures;
				if (!doc.applyRedactions(e)) {
					std::printf("  FAIL: applyRedactions: %s\n", e.c_str());
					++failures;
				} else {
					int pendingAfter = doc.pendingRedactionCount();
					auto charsAfter = doc.pageChars(0);
					std::printf("  applyRedactions OK: pendingAfter=%d (expect 0), pageChars(0) %d -> %d\n",
						pendingAfter, static_cast<int>(charsBefore.size()), static_cast<int>(charsAfter.size()));
					if (pendingAfter != 0) ++failures;
					std::wstring rPath = outPath(L"_redacted.pdf");
					if (!doc.save(rPath.c_str(), false, e)) {
						std::printf("  FAIL: save redacted: %s\n", e.c_str());
						++failures;
					} else {
						PdfDocument doc2;
						std::string e2; bool npw2 = false;
						if (!doc2.open(rPath.c_str(), e2, npw2)) {
							std::printf("  FAIL: reopen redacted: %s\n", e2.c_str());
							++failures;
						} else {
							for (int p = 0; p < doc2.pageCount(); ++p) {
								PageBitmap b = doc2.renderPage(p, 1.0f);
								if (!b.hbmp) { std::printf("  RENDER FAILED on redacted page %d\n", p); ++failures; }
							}
							std::printf("  reopened redacted OK: pageCount=%d\n", doc2.pageCount());
						}
					}
				}
			}
		}

		std::printf("=== %s ===\n", failures == 0 ? "ALL PAGETEST CHECKS PASSED" : "PAGETEST FAILURES PRESENT");
		return failures == 0 ? 0 : 1;
	}

	// --pwtest mode: exercises open(needsPassword)/authenticate()/isEncrypted()/
	// removeProtection() instead of the widget-editing flow below, which
	// assumes an unencrypted, form-bearing PDF.
	if (argc > 3 && wcscmp(argv[3], L"--pwtest") == 0) {
		const wchar_t* pw = argc > 4 ? argv[4] : L"";
		PdfDocument doc;
		std::string err;
		bool needsPw = false;
		bool ok = doc.open(input, err, needsPw);
		std::printf("open: ok=%d needsPw=%d err=%s\n", ok, needsPw, err.c_str());
		if (!ok && needsPw) {
			std::string upw = ToUtf8(pw);
			bool authed = doc.authenticate(upw.c_str());
			std::printf("authenticate(\"%s\"): %d\n", upw.c_str(), authed);
			if (!authed) return 1;
		} else if (!ok) {
			return 1;
		}
		std::printf("isOpen=%d pageCount=%d isEncrypted=%d neededPassword=%d\n",
			doc.isOpen(), doc.pageCount(), doc.isEncrypted(), doc.neededPassword());
		for (int p = 0; p < doc.pageCount(); ++p) {
			PageBitmap b = doc.renderPage(p, 1.0f);
			if (!b.hbmp) { std::printf("RENDER FAILED on page %d\n", p); return 2; }
		}
		std::printf("all pages rendered OK while still encrypted\n");

		if (!doc.removeProtection(output, err)) {
			std::printf("removeProtection FAILED: %s\n", err.c_str());
			return 3;
		}
		std::printf("removeProtection OK -- isOpen=%d isEncrypted=%d neededPassword=%d pageCount=%d\n",
			doc.isOpen(), doc.isEncrypted(), doc.neededPassword(), doc.pageCount());

		// Reopen the written file in a fresh context to confirm it's really
		// unprotected now and needs no password/authenticate() call at all.
		PdfDocument doc2;
		std::string err2; bool needsPw2 = false;
		bool ok2 = doc2.open(output, err2, needsPw2);
		std::printf("reopen output: ok=%d needsPw=%d isEncrypted=%d pageCount=%d\n",
			ok2, needsPw2, doc2.isEncrypted(), doc2.pageCount());
		for (int p = 0; p < doc2.pageCount(); ++p) {
			PageBitmap b = doc2.renderPage(p, 1.0f);
			if (!b.hbmp) { std::printf("RENDER FAILED on reopened page %d\n", p); return 4; }
		}
		std::printf("all pages re-rendered OK from unprotected output\n");
		return (ok2 && !needsPw2 && !doc2.isEncrypted()) ? 0 : 5;
	}

	// --setpwtest mode: exercises setPassword() on an unencrypted input --
	// confirms the output requires the given password to reopen, rejects a
	// wrong password, and accepts the right one.
	if (argc > 3 && wcscmp(argv[3], L"--setpwtest") == 0) {
		const wchar_t* pw = argc > 4 ? argv[4] : L"secret";
		std::string upw = ToUtf8(pw);
		PdfDocument doc;
		std::string err; bool needsPw = false;
		if (!doc.open(input, err, needsPw)) {
			std::printf("open FAILED: %s\n", err.c_str());
			return 1;
		}
		if (!doc.setPassword(output, upw.c_str(), err)) {
			std::printf("setPassword FAILED: %s\n", err.c_str());
			return 2;
		}
		std::printf("setPassword OK -- isEncrypted=%d neededPassword=%d pageCount=%d isOpen=%d\n",
			doc.isEncrypted(), doc.neededPassword(), doc.pageCount(), doc.isOpen());
		if (doc.pageCount() == 0 || !doc.isOpen()) {
			std::printf("LIVE DOCUMENT STATE BROKEN after setPassword (pageCount=0/not open)\n");
			return 7;
		}
		// The live in-memory doc should still render right after setPassword,
		// without needing to close/reopen the tab.
		for (int p = 0; p < doc.pageCount(); ++p) {
			PageBitmap b = doc.renderPage(p, 1.0f);
			if (!b.hbmp) { std::printf("LIVE RENDER FAILED on page %d right after setPassword\n", p); return 8; }
		}
		std::printf("live document still renders OK right after setPassword\n");

		PdfDocument doc2;
		std::string err2; bool needsPw2 = false;
		bool ok2 = doc2.open(output, err2, needsPw2);
		std::printf("reopen output: ok=%d needsPw=%d\n", ok2, needsPw2);
		if (ok2 || !needsPw2) { std::printf("EXPECTED the output to require a password\n"); return 3; }
		if (doc2.authenticate("definitely-wrong-password")) {
			std::printf("WRONG PASSWORD WAS ACCEPTED\n"); return 4;
		}
		if (!doc2.authenticate(upw.c_str())) {
			std::printf("CORRECT PASSWORD WAS REJECTED\n"); return 5;
		}
		std::printf("authenticated OK -- pageCount=%d\n", doc2.pageCount());
		for (int p = 0; p < doc2.pageCount(); ++p) {
			PageBitmap b = doc2.renderPage(p, 1.0f);
			if (!b.hbmp) { std::printf("RENDER FAILED on page %d\n", p); return 6; }
		}
		std::printf("all pages rendered OK after authenticating with the new password\n");
		return 0;
	}

	// --converttest mode: exercises PdfDocument::ConvertFilesToPdf() headlessly.
	// `input` is unused (ConvertFilesToPdf is static -- no PdfDocument needed);
	// `output` is the PDF to build; argv[5..] are the source files to convert
	// (images/.txt/.md). Reopens+renders the result to confirm validity.
	if (argc > 4 && wcscmp(argv[3], L"--converttest") == 0) {
		std::vector<std::wstring> sources;
		for (int i = 4; i < argc; ++i) sources.push_back(argv[i]);
		std::string err;
		std::vector<std::wstring> skipped;
		bool ok = PdfDocument::ConvertFilesToPdf(sources, output, err, &skipped);
		std::printf("ConvertFilesToPdf: ok=%d err=%s skipped=%zu\n", ok, err.c_str(), skipped.size());
		for (auto& s : skipped) std::wprintf(L"  skipped: %s\n", s.c_str());
		if (!ok) return 1;

		PdfDocument doc;
		std::string e2; bool npw = false;
		if (!doc.open(output, e2, npw)) { std::printf("REOPEN FAILED: %s\n", e2.c_str()); return 2; }
		std::printf("reopened: %d pages\n", doc.pageCount());
		for (int p = 0; p < doc.pageCount(); ++p) {
			PageBitmap b = doc.renderPage(p, 1.0f);
			if (!b.hbmp) { std::printf("RENDER FAILED on page %d\n", p); return 3; }
		}
		std::printf("all pages rendered OK\n");
		return 0;
	}

	// --webtest mode: exercises ConvertWebPageToPdf() headlessly. `input` is
	// the URL to convert (unused otherwise); `output` is the PDF to write.
	if (argc > 3 && wcscmp(argv[3], L"--webtest") == 0) {
		std::string err;
		bool ok = ConvertWebPageToPdf(input, output, err);
		std::printf("ConvertWebPageToPdf: ok=%d err=%s\n", ok, err.c_str());
		if (!ok) return 1;

		PdfDocument doc;
		std::string e2; bool npw = false;
		if (!doc.open(output, e2, npw)) { std::printf("REOPEN FAILED: %s\n", e2.c_str()); return 2; }
		std::printf("reopened: %d pages\n", doc.pageCount());
		for (int p = 0; p < doc.pageCount(); ++p) {
			PageBitmap b = doc.renderPage(p, 1.0f);
			if (!b.hbmp) { std::printf("RENDER FAILED on page %d\n", p); return 3; }
		}
		std::printf("all pages rendered OK\n");
		return 0;
	}

	// --compresstest mode: opens `input`, runs compress(), saves to `output`,
	// and prints the before/after byte sizes + ratio. Point it at a real
	// scanned PDF to measure the image-rewriter compression.
	if (argc > 3 && wcscmp(argv[3], L"--compresstest") == 0) {
		PdfDocument doc;
		std::string e; bool npw = false;
		if (!doc.open(input, e, npw)) { std::printf("open failed: %s\n", e.c_str()); return 1; }
		std::printf("opened: %d pages\n", doc.pageCount());
		if (!doc.compress(e)) { std::printf("compress FAILED: %s\n", e.c_str()); return 1; }
		if (!doc.save(output, false, e)) { std::printf("save FAILED: %s\n", e.c_str()); return 1; }
		auto fileSize = [](const wchar_t* p) -> long long {
			HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
			LARGE_INTEGER s{}; if (h != INVALID_HANDLE_VALUE) { GetFileSizeEx(h, &s); CloseHandle(h); }
			return s.QuadPart;
		};
		long long before = fileSize(input), after = fileSize(output);
		std::printf("size: %lld -> %lld bytes (%.1f%% of original)\n",
			before, after, before ? (100.0 * after / before) : 0.0);
		// Confirm the compressed file still opens and every page renders.
		PdfDocument doc2; std::string e2; bool npw2 = false;
		if (!doc2.open(output, e2, npw2)) { std::printf("REOPEN FAILED: %s\n", e2.c_str()); return 2; }
		for (int p = 0; p < doc2.pageCount(); ++p) {
			PageBitmap b = doc2.renderPage(p, 1.0f);
			if (!b.hbmp) { std::printf("RENDER FAILED on page %d\n", p); return 3; }
		}
		std::printf("compressed file reopened + all pages rendered OK\n");
		return 0;
	}

	bool incremental = argc > 3 && wcscmp(argv[3], L"1") == 0;

	PdfDocument doc;
	std::string err;
	bool needsPw = false;
	if (!doc.open(input, err, needsPw)) {
		std::printf("open failed: %s (needsPw=%d)\n", err.c_str(), needsPw);
		return 1;
	}
	std::printf("opened: %d pages, isPdf=%d\n", doc.pageCount(), doc.isPdf());

	// Simulate real GUI usage: render pages (like thumbnails/canvas do),
	// then edit several different widget kinds across pages, re-rendering
	// after each edit (as the canvas does on invalidatePage), then save --
	// this interleaving is what a plain isolated edit+save test misses.
	for (int p = 0; p < doc.pageCount(); ++p) {
		PageBitmap bmp = doc.renderPage(p, 1.0f);
		std::printf("prerendered page %d (%dx%d)\n", p, bmp.width, bmp.height);
	}

	int editedText = 0, editedCheck = 0, editedChoice = 0;
	for (int p = 0; p < doc.pageCount(); ++p) {
		auto widgets = doc.pageWidgets(p);
		for (const auto& w : widgets) {
			if (w.kind == WidgetKind::Text && editedText < 3) {
				char buf[64]; std::snprintf(buf, sizeof(buf), "SAVE TEST %d", editedText);
				if (!doc.setTextWidget(p, w.index, buf, err))
					std::printf("setTextWidget p%d i%d failed: %s\n", p, w.index, err.c_str());
				else { std::printf("edited text p%d i%d\n", p, w.index); ++editedText; }
				PageBitmap b2 = doc.renderPage(p, 1.0f); (void)b2;
			} else if (w.kind == WidgetKind::Checkbox && editedCheck < 3) {
				if (!doc.toggleWidget(p, w.index, err))
					std::printf("toggleWidget p%d i%d failed: %s\n", p, w.index, err.c_str());
				else { std::printf("toggled checkbox p%d i%d\n", p, w.index); ++editedCheck; }
				PageBitmap b2 = doc.renderPage(p, 1.0f); (void)b2;
			} else if ((w.kind == WidgetKind::Combo || w.kind == WidgetKind::ListBox) && editedChoice < 3 && !w.options.empty()) {
				if (!doc.setChoiceWidget(p, w.index, w.options.back(), err))
					std::printf("setChoiceWidget p%d i%d failed: %s\n", p, w.index, err.c_str());
				else { std::printf("set choice p%d i%d = %s\n", p, w.index, w.options.back().c_str()); ++editedChoice; }
				PageBitmap b2 = doc.renderPage(p, 1.0f); (void)b2;
			}
		}
	}
	std::printf("total edits: text=%d checkbox=%d choice=%d\n", editedText, editedCheck, editedChoice);
	if (editedText == 0 && editedCheck == 0 && editedChoice == 0) {
		std::printf("no widgets edited\n");
		return 1;
	}
	int foundPage = 0, foundIdx = -1; // for the post-reopen spot check below

	// --- New Phase-3 APIs: create/edit/delete a FreeText annotation, and
	// extract page text (backs the "click existing text box to edit it",
	// "font size", "delete", and "select PDF text / copy" features). ---
	{
		PageRectPt rect{ 50, 50, 250, 90 };
		std::string aerr;
		if (!doc.addTextBox(0, rect, "Hello annotation", "Helv", 14.0f, 0x0000FF, aerr))
			std::printf("addTextBox failed: %s\n", aerr.c_str());
		else
			std::printf("addTextBox OK\n");

		auto annots = doc.pageAnnots(0);
		std::printf("page 0 has %d markup annotation(s)\n", static_cast<int>(annots.size()));
		int freeTextIdx = -1;
		for (const auto& a : annots) {
			if (a.kind == AnnotKind::FreeText && a.contents == "Hello annotation") {
				freeTextIdx = a.index;
				std::printf("  found FreeText idx=%d size=%.1f color=%06lX text='%s'\n",
					a.index, a.fontSize, a.color, a.contents.c_str());
			}
		}
		if (freeTextIdx >= 0) {
			AnnotInfo hit = doc.annotAt(0, { 150, 70 });
			std::printf("annotAt hit-test -> index=%d kind=%d\n", hit.index, static_cast<int>(hit.kind));

			PageRectPt resizedRect{ 50, 50, 300, 120 };
			if (!doc.setFreeTextAnnot(0, freeTextIdx, "Edited annotation", 18.0f, 0xFF0000, resizedRect, aerr))
				std::printf("setFreeTextAnnot failed: %s\n", aerr.c_str());
			else
				std::printf("setFreeTextAnnot OK\n");

			auto annots2 = doc.pageAnnots(0);
			for (const auto& a : annots2)
				if (a.index == freeTextIdx)
					std::printf("  after edit: size=%.1f color=%06lX text='%s' rect=(%.0f,%.0f,%.0f,%.0f)\n",
						a.fontSize, a.color, a.contents.c_str(), a.rect.x0, a.rect.y0, a.rect.x1, a.rect.y1);

			// Exercise the "transparent inline edit background" render path:
			// hide freeTextIdx for one render, then confirm the flag was
			// restored (annotation still present/visible in a normal render).
			PageBitmap hiddenRender = doc.renderPageWithoutAnnot(0, 1.0f, freeTextIdx);
			std::printf("renderPageWithoutAnnot -> %dx%d\n", hiddenRender.width, hiddenRender.height);
			auto annotsAfterHide = doc.pageAnnots(0);
			bool stillThere = false;
			for (const auto& a : annotsAfterHide) if (a.index == freeTextIdx) stillThere = true;
			std::printf("annotation still listed after hide/restore: %s\n", stillThere ? "yes" : "NO (bug!)");

			if (!doc.deleteAnnot(0, freeTextIdx, aerr))
				std::printf("deleteAnnot failed: %s\n", aerr.c_str());
			else
				std::printf("deleteAnnot OK\n");

			auto annots3 = doc.pageAnnots(0);
			std::printf("page 0 has %d markup annotation(s) after delete\n", static_cast<int>(annots3.size()));
		} else {
			std::printf("FreeText annotation not found after creation!\n");
		}

		auto chars = doc.pageChars(0);
		std::printf("pageChars(0) returned %d characters\n", static_cast<int>(chars.size()));
		if (!chars.empty()) {
			std::string preview;
			for (size_t i = 0; i < chars.size() && i < 40; ++i) preview += static_cast<char>(chars[i].unicode < 128 ? chars[i].unicode : '?');
			std::printf("  first chars: '%s'\n", preview.c_str());
		}
	}

	if (!doc.save(output, incremental, err)) {
		std::printf("save failed: %s\n", err.c_str());
		return 1;
	}
	std::printf("saved (incremental=%d) to output\n", incremental ? 1 : 0);

	// Reopen the saved file in a FRESH PdfDocument/context to verify it's valid.
	PdfDocument doc2;
	std::string err2;
	bool needsPw2 = false;
	if (!doc2.open(output, err2, needsPw2)) {
		std::printf("REOPEN FAILED: %s (needsPw=%d)\n", err2.c_str(), needsPw2);
		return 2;
	}
	std::printf("reopened OK: %d pages\n", doc2.pageCount());
	for (int p = 0; p < doc2.pageCount(); ++p) {
		for (const auto& w : doc2.pageWidgets(p)) {
			if (w.value.rfind("SAVE TEST", 0) == 0)
				std::printf("  p%d i%d value='%s'\n", p, w.index, w.value.c_str());
		}
	}
	// Also fully render every page post-save to catch any structural damage
	// that only manifests when MuPDF tries to interpret content/resources.
	for (int p = 0; p < doc2.pageCount(); ++p) {
		PageBitmap b = doc2.renderPage(p, 1.0f);
		if (!b.hbmp) { std::printf("RENDER FAILED on page %d after reopen\n", p); return 3; }
	}
	std::printf("all pages re-rendered OK after reopen\n");
	(void)foundPage;
	return 0;
}
