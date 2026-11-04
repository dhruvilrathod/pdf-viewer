// Console harness for the OCR layer (PdfDocument::pageNeedsOcr / ocrScanPage /
// the save-time text layer). No GUI, so it can run while the app is in use.
//
//   pdfviewer_ocrtest info  <in.pdf>                 which pages would be OCR'd
//   pdfviewer_ocrtest scan  <in.pdf> [page]          recognize and print the text
//   pdfviewer_ocrtest bake  <in.pdf> <out.pdf>       recognize everything needed, save
//                                                    with the text layer, re-open and check
//   pdfviewer_ocrtest make  <file>... <out.pdf>      wrap images / PDFs into one PDF
#include "pdf_document.h"

extern "C" {
#include <mupdf/pdf.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::string Narrow(const std::wstring& w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring PageText(const std::vector<PageChar>& chars)
{
	std::wstring t;
	for (const auto& c : chars) {
		t.push_back(static_cast<wchar_t>(c.unicode));
		if (c.lineBreakAfter) t.push_back(L'\n');
	}
	return t;
}

bool Open(PdfDocument& doc, const wchar_t* path)
{
	std::string err;
	bool needsPw = false;
	if (!doc.open(path, err, needsPw)) {
		std::printf("open failed: %s%s\n", err.c_str(), needsPw ? " (password)" : "");
		return false;
	}
	return true;
}

double Ms(std::chrono::steady_clock::time_point a)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
	SetConsoleOutputCP(CP_UTF8);
	setvbuf(stdout, nullptr, _IONBF, 0); // a crash must not eat the lines before it
	if (argc < 3) {
		std::printf("usage: pdfviewer_ocrtest info|scan|bake|make ...\n");
		return 2;
	}
	std::wstring cmd = argv[1];

	if (cmd == L"make") {
		if (argc < 4) return 2;
		std::string err;
		std::vector<std::wstring> inputs(argv + 2, argv + argc - 1);
		if (!PdfDocument::ConvertFilesToPdf(inputs, argv[argc - 1], err)) {
			std::printf("make failed: %s\n", err.c_str());
			return 1;
		}
		std::printf("wrote %s\n", Narrow(argv[argc - 1]).c_str());
		return 0;
	}

	PdfDocument doc;
	if (!Open(doc, argv[2])) return 1;
	std::printf("OCR engine available: %s\n", PdfDocument::ocrAvailable() ? "yes" : "NO");
	std::printf("pages: %d, scanned-document probe: %s\n", doc.pageCount(), doc.probeNeedsOcr() ? "YES (OCR on)" : "no (OCR off)");

	if (cmd == L"info") {
		for (int p = 0; p < doc.pageCount(); ++p) {
			auto t0 = std::chrono::steady_clock::now();
			bool need = doc.pageNeedsOcr(p);
			std::printf("  page %d: %s  (%.1f ms)\n", p + 1, need ? "needs OCR" : "has text / no OCR", Ms(t0));
		}
		return 0;
	}

	if (cmd == L"scan") {
		doc.setOcrLayerEnabled(true);
		int only = argc >= 4 ? _wtoi(argv[3]) - 1 : -1;
		for (int p = 0; p < doc.pageCount(); ++p) {
			if (only >= 0 && p != only) continue;
			if (only < 0 && !doc.pageNeedsOcr(p)) { std::printf("-- page %d: skipped (no OCR needed)\n", p + 1); continue; }
			auto t0 = std::chrono::steady_clock::now();
			std::string err;
			if (!doc.ocrScanPage(p, err)) { std::printf("-- page %d: OCR failed: %s\n", p + 1, err.c_str()); continue; }
			double ms = Ms(t0);
			auto chars = doc.pageChars(p);
			std::printf("-- page %d: %zu chars in %.0f ms\n%s\n", p + 1, chars.size(), ms, Narrow(PageText(chars)).c_str());
		}
		return 0;
	}

	if (cmd == L"render") {
		// How much of each page is actually inked: a blank (all-white) page prints 0.
		for (int p = 0; p < doc.pageCount(); ++p) {
			PageBitmap bm = doc.renderPage(p, 0.5f);
			if (!bm.hbmp) { std::printf("-- page %d: render FAILED\n", p + 1); continue; }
			BITMAPINFO bi = {};
			bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
			bi.bmiHeader.biWidth = bm.width; bi.bmiHeader.biHeight = -bm.height;
			bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
			std::vector<unsigned char> px(static_cast<size_t>(bm.width) * bm.height * 4);
			HDC dc = GetDC(nullptr);
			GetDIBits(dc, bm.hbmp, 0, bm.height, px.data(), &bi, DIB_RGB_COLORS);
			ReleaseDC(nullptr, dc);
			size_t dark = 0;
			for (size_t i = 0; i < px.size(); i += 4) if (px[i] < 200 || px[i + 1] < 200 || px[i + 2] < 200) ++dark;
			std::printf("-- page %d: %zu non-white pixels of %zu\n", p + 1, dark, px.size() / 4);
		}
		return 0;
	}

	if (cmd == L"dump") {
		// Structure of page N (default 1): resources and the start of the content stream.
		int pn = argc >= 4 ? _wtoi(argv[3]) - 1 : 0;
		fz_context* ctx = doc.ctx();
		pdf_document* pdf = pdf_document_from_fz_document(ctx, doc.doc());
		fz_try(ctx) {
			pdf_obj* page = pdf_lookup_page_obj(ctx, pdf, pn);
			pdf_obj* res = pdf_dict_get_inheritable(ctx, page, PDF_NAME(Resources));
			std::printf("page %d resources:\n", pn + 1);
			for (int i = 0; i < pdf_dict_len(ctx, res); ++i) {
				pdf_obj* v = pdf_dict_get_val(ctx, res, i);
				std::printf("  /%s (%d entries)\n", pdf_to_name(ctx, pdf_dict_get_key(ctx, res, i)), pdf_is_dict(ctx, v) ? pdf_dict_len(ctx, v) : 0);
				if (pdf_dict_get_key(ctx, res, i) == PDF_NAME(Font) || pdf_dict_get_key(ctx, res, i) == PDF_NAME(XObject)) {
					for (int k = 0; k < pdf_dict_len(ctx, v) && k < 12; ++k) {
						pdf_obj* o = pdf_dict_get_val(ctx, v, k);
						pdf_obj* st = pdf_dict_get(ctx, o, PDF_NAME(Subtype));
						pdf_obj* bf = pdf_dict_get(ctx, o, PDF_NAME(BaseFont));
						pdf_obj* w = pdf_dict_get(ctx, o, PDF_NAME(Width));
						std::printf("      %s: %s %s %s\n", pdf_to_name(ctx, pdf_dict_get_key(ctx, v, k)), pdf_to_name(ctx, st),
							bf ? pdf_to_name(ctx, bf) : "", w ? "(sized)" : "");
					}
				}
			}
			fz_buffer* buf = pdf_load_raw_stream(ctx, pdf_dict_get(ctx, page, PDF_NAME(Contents)));
			(void)buf;
		}
		fz_catch(ctx) { std::printf("dump error: %s\n", fz_caught_message(ctx)); }
		fz_buffer* all = nullptr;
		fz_try(ctx) {
			pdf_obj* page = pdf_lookup_page_obj(ctx, pdf, pn);
			pdf_obj* c = pdf_dict_get(ctx, page, PDF_NAME(Contents));
			all = fz_new_buffer(ctx, 4096);
			if (pdf_is_array(ctx, c)) {
				for (int i = 0; i < pdf_array_len(ctx, c); ++i) {
					fz_buffer* b = pdf_load_stream(ctx, pdf_array_get(ctx, c, i));
					fz_append_buffer(ctx, all, b); fz_append_string(ctx, all, "\n"); fz_drop_buffer(ctx, b);
				}
			} else if (c) {
				fz_buffer* b = pdf_load_stream(ctx, c);
				fz_append_buffer(ctx, all, b); fz_drop_buffer(ctx, b);
			}
			unsigned char* data = nullptr;
			size_t n = fz_buffer_storage(ctx, all, &data);
			std::printf("content stream: %zu bytes. First 700:\n%.*s\n", n, (int)std::min<size_t>(n, 700), (const char*)data);
		}
		fz_always(ctx) { fz_drop_buffer(ctx, all); }
		fz_catch(ctx) { std::printf("contents error: %s\n", fz_caught_message(ctx)); }
		return 0;
	}

	if (cmd == L"chars") {
		// What the viewer's selection sees: per page, the char count and a sample.
		for (int p = 0; p < doc.pageCount(); ++p) {
			auto chars = doc.pageChars(p);
			auto runs = doc.textRuns(p);
			int vis = 0;
			for (const auto& r : runs) if (r.visible) ++vis;
			std::printf("-- page %d: %zu chars, %zu runs (%d visible), size %.0fx%.0f\n", p + 1, chars.size(), runs.size(), vis,
				doc.pageSize(p).w, doc.pageSize(p).h);
			std::wstring t = PageText(chars);
			std::printf("%s\n", Narrow(t.substr(0, 400)).c_str());
			for (size_t i = 0; i < runs.size() && i < 3; ++i)
				std::printf("   run %zu: font='%s' size=%.1f visible=%d bbox=(%.0f,%.0f,%.0f,%.0f)\n", i, runs[i].fontName.c_str(),
					runs[i].size, (int)runs[i].visible, runs[i].bbox.x0, runs[i].bbox.y0, runs[i].bbox.x1, runs[i].bbox.y1);
		}
		return 0;
	}

	if (cmd == L"redact") {
		// OCR the page, redact the first line, save, and make sure its words are gone
		// from the saved file's text layer (not just painted over).
		if (argc < 4) return 2;
		doc.setOcrLayerEnabled(true);
		std::string err;
		if (!doc.ocrScanPage(0, err)) { std::printf("ocr failed: %s\n", err.c_str()); return 1; }
		auto chars = doc.pageChars(0);
		PageRectPt r = chars[0].quad;
		for (const auto& c : chars) {
			r.x0 = std::min(r.x0, c.quad.x0); r.y0 = std::min(r.y0, c.quad.y0);
			r.x1 = std::max(r.x1, c.quad.x1); r.y1 = std::max(r.y1, c.quad.y1);
			if (c.lineBreakAfter) break; // first line only
		}
		if (!doc.addRedaction(0, r, err)) { std::printf("addRedaction failed: %s\n", err.c_str()); return 1; }
		if (!doc.applyRedactions(false, err)) { std::printf("applyRedactions failed: %s\n", err.c_str()); return 1; }
		if (!doc.save(argv[3], false, err)) { std::printf("save failed: %s\n", err.c_str()); return 1; }
		PdfDocument re;
		if (!Open(re, argv[3])) return 1;
		std::string text = Narrow(PageText(re.pageChars(0)));
		std::printf("text of saved file:\n%s\n", text.c_str());
		std::printf("first line 'Invoice' still present: %s\n", text.find("Invoice") != std::string::npos ? "YES (LEAK)" : "no (good)");
		std::printf("other lines kept: %s\n", text.find("quick brown fox") != std::string::npos ? "yes (good)" : "NO");
		return 0;
	}

	if (cmd == L"bake") {
		if (argc < 4) return 2;
		doc.setOcrLayerEnabled(true);
		int done = 0;
		for (int p; (p = doc.nextOcrPage(0)) >= 0;) {
			std::string err;
			if (!doc.ocrScanPage(p, err)) std::printf("page %d failed: %s\n", p + 1, err.c_str());
			else ++done;
		}
		std::printf("recognized %d page(s); layer pending: %s\n", done, doc.hasOcrToSave() ? "yes" : "no");
		auto beforeChars = doc.pageChars(0);
		std::wstring before = PageText(beforeChars);
		std::string err;
		if (!doc.save(argv[3], false, err)) { std::printf("save failed: %s\n", err.c_str()); return 1; }
		std::printf("saved. pending after save: %s\n", doc.hasOcrToSave() ? "yes" : "no");

		PdfDocument re;
		if (!Open(re, argv[3])) return 1;
		std::printf("reopened: probe says %s\n", re.probeNeedsOcr() ? "STILL SCANNED (bad)" : "has text (good)");
		for (int p = 0; p < re.pageCount(); ++p) {
			std::wstring after = PageText(re.pageChars(p));
			std::printf("-- page %d text from saved file (%zu chars):\n%s\n", p + 1, after.size(), Narrow(after).c_str());
		}
		auto afterChars = re.pageChars(0);
		if (afterChars.size() == beforeChars.size() && !afterChars.empty()) {
			float maxDx = 0, maxDy = 0;
			for (size_t i = 0; i < afterChars.size(); ++i) {
				const auto &a = beforeChars[i].quad, &b = afterChars[i].quad;
				maxDx = std::max(maxDx, std::fabs((a.x0 + a.x1) / 2 - (b.x0 + b.x1) / 2));
				maxDy = std::max(maxDy, std::fabs((a.y0 + a.y1) / 2 - (b.y0 + b.y1) / 2));
			}
			std::printf("char-box centre drift after save: max dx=%.2f pt, max dy=%.2f pt (page is %.0f x %.0f pt)\n",
				maxDx, maxDy, re.pageSize(0).w, re.pageSize(0).h);
		} else {
			std::printf("char count differs after save: %zu before, %zu after\n", beforeChars.size(), afterChars.size());
		}
		bool same = PageText(afterChars) == before;
		std::printf("page 1 text identical before/after save: %s\n", same ? "yes" : "NO");
		// Search the saved file for the first word we saw.
		std::wstring first = before.substr(0, before.find_first_of(L" \n"));
		if (!first.empty()) {
			auto hits = re.searchPage(0, Narrow(first).c_str());
			std::printf("search '%s' in saved file: %zu hit(s)\n", Narrow(first).c_str(), hits.size());
		}
		return 0;
	}
	return 2;
}
