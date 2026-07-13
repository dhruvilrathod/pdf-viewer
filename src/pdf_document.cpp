#include "pdf_document.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include <mupdf/pdf.h>
}

namespace {

// Convert a wide (UTF-16) path to UTF-8 for MuPDF, which takes UTF-8 paths
// and re-widens them internally for the Win32 file APIs.
std::string toUtf8(const wchar_t* w)
{
	if (!w) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (n <= 0) return {};
	std::string s(static_cast<size_t>(n - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

// ---------------------------------------------------------------------------
// Windows system-font loader for MuPDF.
//
// When a PDF references a font that is NOT embedded in the file (extremely
// common -- e.g. plain "Arial", "Calibri", "Times New Roman"), MuPDF by default
// falls back to its own built-in base14 look-alikes, whose letterforms (notably
// 's') and stroke weight differ from the real font. Installing this hook lets
// MuPDF pull the ACTUAL installed Windows font instead, so those PDFs render the
// way Adobe/Edge/Chrome show them. (Embedded fonts are unaffected -- MuPDF only
// calls this when it needs a substitute.)
// ---------------------------------------------------------------------------

// Turn a PDF/PostScript font name into a Windows family name, inferring style.
// Handles subset prefixes ("ABCDEF+Arial"), style separators ("Arial-BoldMT",
// "Arial,Italic"), PS decorations ("MT"/"PS"), and a few base14 aliases.
std::wstring cleanFontFamily(const char* rawName, int& bold, int& italic)
{
	std::string n = rawName ? rawName : "";
	if (n.size() > 7 && n[6] == '+') n = n.substr(7); // subset tag "ABCDEF+"

	std::string low = n;
	std::transform(low.begin(), low.end(), low.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (low.find("bold") != std::string::npos) bold = 1;
	if (low.find("italic") != std::string::npos || low.find("oblique") != std::string::npos) italic = 1;

	size_t cut = n.find_first_of(",-"); // "Arial,Bold" / "Arial-BoldMT"
	if (cut != std::string::npos) n = n.substr(0, cut);

	auto endsWith = [&](const char* s) {
		size_t l = std::strlen(s);
		return n.size() >= l && _stricmp(n.c_str() + n.size() - l, s) == 0;
	};
	for (bool more = true; more; ) {
		if (endsWith("PSMT")) n.resize(n.size() - 4);
		else if (endsWith("MT")) n.resize(n.size() - 2);
		else if (endsWith("PS")) n.resize(n.size() - 2);
		else more = false;
	}

	static const struct { const char* ps; const wchar_t* win; } kAliases[] = {
		{ "Helvetica", L"Arial" }, { "Arial", L"Arial" },
		{ "TimesNewRoman", L"Times New Roman" }, { "Times", L"Times New Roman" },
		{ "Courier", L"Courier New" }, { "CourierNew", L"Courier New" },
	};
	for (auto& a : kAliases)
		if (_stricmp(n.c_str(), a.ps) == 0) return a.win;

	int wn = MultiByteToWideChar(CP_UTF8, 0, n.c_str(), -1, nullptr, 0);
	if (wn <= 0) return {};
	std::wstring w(static_cast<size_t>(wn - 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, n.c_str(), -1, w.data(), wn);
	return w;
}

fz_font* loadWindowsFont(fz_context* ctx, const char* name, int bold, int italic, int /*needs_exact_metrics*/)
{
	std::wstring family = cleanFontFamily(name, bold, italic);
	if (family.empty()) return nullptr;

	HDC hdc = CreateCompatibleDC(nullptr);
	if (!hdc) return nullptr;
	HFONT hf = CreateFontW(0, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, italic ? TRUE : FALSE,
		FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
		DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family.c_str());
	if (!hf) { DeleteDC(hdc); return nullptr; }
	HGDIOBJ oldFont = SelectObject(hdc, hf);

	// Guard against GDI silently substituting an unrelated family (e.g. the PDF
	// names a font that isn't installed): if the face GDI actually resolved to
	// doesn't match what we asked for, bail and let MuPDF use its own fallback
	// rather than shipping a random system face (which would be worse, e.g. for
	// CJK-named fonts we don't have installed).
	wchar_t actual[LF_FACESIZE] = {};
	GetTextFaceW(hdc, LF_FACESIZE, actual);

	fz_font* font = nullptr;
	if (_wcsicmp(actual, family.c_str()) == 0) {
		DWORD size = GetFontData(hdc, 0, 0, nullptr, 0); // whole file
		if (size != GDI_ERROR && size > 0) {
			std::vector<unsigned char> data(size);
			if (GetFontData(hdc, 0, 0, data.data(), size) == size) {
				std::string fname = toUtf8(family.c_str());
				fz_buffer* buf = nullptr;
				fz_var(buf);
				fz_try(ctx) {
					buf = fz_new_buffer_from_copied_data(ctx, data.data(), size);
					font = fz_new_font_from_buffer(ctx, fname.c_str(), buf, 0, 0);
				}
				fz_always(ctx) {
					fz_drop_buffer(ctx, buf);
				}
				fz_catch(ctx) {
					font = nullptr;
				}
			}
		}
	}

	SelectObject(hdc, oldFont);
	DeleteObject(hf);
	DeleteDC(hdc);
	return font;
}

// Install the Windows font hook on a context. Latin/named lookups go through
// loadWindowsFont; CJK-ordering and script-fallback hooks are left default
// (MuPDF's built-in Noto handling) since those aren't the substitution problem.
void installSystemFontHook(fz_context* ctx)
{
	if (ctx) fz_install_load_system_font_funcs(ctx, loadWindowsFont, nullptr, nullptr);
}

// Build a 32-bit top-down BGRA DIB section from an RGB (3-component) pixmap.
HBITMAP pixmapToDIB(fz_context* ctx, fz_pixmap* pix, int& outW, int& outH)
{
	int w = fz_pixmap_width(ctx, pix);
	int h = fz_pixmap_height(ctx, pix);
	int n = fz_pixmap_components(ctx, pix);
	ptrdiff_t stride = fz_pixmap_stride(ctx, pix);
	const unsigned char* src = fz_pixmap_samples(ctx, pix);
	if (w <= 0 || h <= 0 || n < 3) return nullptr;

	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = w;
	bmi.bmiHeader.biHeight = -h; // negative => top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	void* bits = nullptr;
	HBITMAP hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!hbmp || !bits) { if (hbmp) DeleteObject(hbmp); return nullptr; }

	auto* dst = static_cast<unsigned char*>(bits);
	for (int y = 0; y < h; ++y) {
		const unsigned char* srow = src + static_cast<ptrdiff_t>(y) * stride;
		unsigned char* drow = dst + static_cast<ptrdiff_t>(y) * w * 4;
		for (int x = 0; x < w; ++x) {
			const unsigned char* sp = srow + static_cast<ptrdiff_t>(x) * n;
			unsigned char* dp = drow + static_cast<ptrdiff_t>(x) * 4;
			dp[0] = sp[2]; // B
			dp[1] = sp[1]; // G
			dp[2] = sp[0]; // R
			dp[3] = 255;   // A (opaque)
		}
	}
	outW = w; outH = h;
	return hbmp;
}

} // namespace

PdfDocument::PdfDocument()
{
	ctx_ = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
	if (ctx_) {
		installSystemFontHook(ctx_); // use real installed fonts for non-embedded refs
		fz_try(ctx_) {
			fz_register_document_handlers(ctx_);
		}
		fz_catch(ctx_) {
			fz_drop_context(ctx_);
			ctx_ = nullptr;
		}
	}
}

PdfDocument::~PdfDocument()
{
	close();
	if (ctx_) { fz_drop_context(ctx_); ctx_ = nullptr; }
}

bool PdfDocument::open(const wchar_t* path, std::string& error, bool& needsPassword)
{
	needsPassword = false;
	neededPassword_ = false;
	if (!ctx_) { error = "MuPDF context not initialized"; return false; }

	close();
	std::string upath = toUtf8(path);

	fz_try(ctx_) {
		doc_ = fz_open_document(ctx_, upath.c_str());
	}
	fz_catch(ctx_) {
		doc_ = nullptr;
		error = fz_caught_message(ctx_);
		return false;
	}

	openedPath_ = path;

	if (fz_needs_password(ctx_, doc_)) {
		needsPassword = true;
		neededPassword_ = true;
		authed_ = false;
		return false; // caller must authenticate()
	}

	authed_ = true;
	loadInfo();
	return true;
}

bool PdfDocument::reopenCurrentPath(std::string& err)
{
	if (!ctx_ || openedPath_.empty()) { err = "no path to reopen"; return false; }
	std::string upath = toUtf8(openedPath_.c_str());
	if (doc_) { fz_drop_document(ctx_, doc_); doc_ = nullptr; }
	fz_try(ctx_) {
		doc_ = fz_open_document(ctx_, upath.c_str());
	}
	fz_catch(ctx_) {
		doc_ = nullptr;
		err = fz_caught_message(ctx_);
		authed_ = false;
		return false;
	}
	authed_ = true; // editing requires we were already authenticated before
	loadInfo();
	return true;
}

bool PdfDocument::authenticate(const char* password)
{
	if (!ctx_ || !doc_) return false;
	int ok = 0;
	fz_try(ctx_) {
		ok = fz_authenticate_password(ctx_, doc_, password ? password : "");
	}
	fz_catch(ctx_) {
		ok = 0;
	}
	if (ok) {
		authed_ = true;
		loadInfo();
	}
	return ok != 0;
}

bool PdfDocument::isEncrypted() const
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) return false;
	// Check the trailer directly rather than pdf_document_permissions(): that
	// reports full access (0xFFFFFFFC) once authenticated as the *owner*,
	// even though the file is still structurally encrypted and will demand
	// a password again on next open -- which is exactly the common case
	// right after unlocking a password-protected PDF in this app.
	bool encrypted = false;
	fz_try(ctx_) {
		encrypted = pdf_dict_gets(ctx_, pdf_trailer(ctx_, pdf), "Encrypt") != nullptr;
	}
	fz_catch(ctx_) {
		encrypted = false;
	}
	return encrypted;
}

void PdfDocument::loadInfo()
{
	if (!ctx_ || !doc_) return;
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pageCount_ = 0;
	sizes_.clear();
	bounds_.clear();
	isPdf_ = (pdf_document_from_fz_document(ctx_, doc_) != nullptr);
	fz_try(ctx_) {
		pageCount_ = fz_count_pages(ctx_, doc_);
		sizes_.reserve(pageCount_);
		bounds_.reserve(pageCount_);
		for (int i = 0; i < pageCount_; ++i) {
			fz_page* page = fz_load_page(ctx_, doc_, i);
			fz_rect r = fz_bound_page(ctx_, page);
			fz_drop_page(ctx_, page);
			sizes_.push_back({ r.x1 - r.x0, r.y1 - r.y0 });
			bounds_.push_back({ r.x0, r.y0, r.x1, r.y1 });
		}
	}
	fz_catch(ctx_) {
		// leave whatever we managed to read
	}
}

PageSizePt PdfDocument::pageSize(int index) const
{
	if (index < 0 || index >= static_cast<int>(sizes_.size())) return {};
	return sizes_[index];
}

PageRectPt PdfDocument::pageBound(int index) const
{
	if (index < 0 || index >= static_cast<int>(bounds_.size())) return {};
	return bounds_[index];
}

std::vector<PageRectPt> PdfDocument::searchPage(int index, const char* needle, int maxHits)
{
	std::vector<PageRectPt> out;
	if (!ctx_ || !doc_ || !authed_ || !needle || !*needle) return out;
	if (index < 0 || index >= pageCount_) return out;
	if (maxHits <= 0) return out;

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::vector<fz_quad> quads(static_cast<size_t>(maxHits));
	int n = 0;
	fz_try(ctx_) {
		n = fz_search_page_number(ctx_, doc_, index, needle, nullptr, quads.data(), maxHits);
	}
	fz_catch(ctx_) {
		n = 0;
	}
	out.reserve(n);
	for (int i = 0; i < n; ++i) {
		fz_rect r = fz_rect_from_quad(quads[i]);
		out.push_back({ r.x0, r.y0, r.x1, r.y1 });
	}
	return out;
}

PageBitmap PdfDocument::renderPage(int index, float scale)
{
	PageBitmap result;
	if (!ctx_ || !doc_ || !authed_) return result;
	if (index < 0 || index >= pageCount_) return result;
	if (scale <= 0.0f) return result;

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	fz_page* page = nullptr;
	fz_pixmap* pix = nullptr;
	fz_try(ctx_) {
		page = fz_load_page(ctx_, doc_, index);
		fz_matrix ctm = fz_scale(scale, scale);
		pix = fz_new_pixmap_from_page(ctx_, page, ctm, fz_device_rgb(ctx_), 0);
		int w = 0, h = 0;
		HBITMAP hbmp = pixmapToDIB(ctx_, pix, w, h);
		if (hbmp) { result.hbmp = hbmp; result.width = w; result.height = h; }
	}
	fz_always(ctx_) {
		if (pix) fz_drop_pixmap(ctx_, pix);
		if (page) fz_drop_page(ctx_, page);
	}
	fz_catch(ctx_) {
		// return empty on failure
	}
	return result;
}

void PdfDocument::close()
{
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	closeExternalFiles();
	if (ctx_ && doc_) { fz_drop_document(ctx_, doc_); }
	doc_ = nullptr;
	authed_ = false;
	isPdf_ = false;
	dirty_ = false;
	pageCount_ = 0;
	sizes_.clear();
	bounds_.clear();
	openedPath_.clear();
}

// --- Editing ---------------------------------------------------------------
namespace {

// Every caller passes a COLORREF (built via Win32's RGB() macro: R in the
// low byte, B in the high byte -- 0x00BBGGRR), not a packed 0xRRGGBB
// integer. Use GetRValue/GetGValue/GetBValue rather than manual shifts, or
// red and blue end up swapped in the saved PDF.
void colorToFloat(unsigned long rgb, float out[3])
{
	out[0] = GetRValue(rgb) / 255.0f;
	out[1] = GetGValue(rgb) / 255.0f;
	out[2] = GetBValue(rgb) / 255.0f;
}

fz_rect normRect(PageRectPt r)
{
	fz_rect out;
	out.x0 = std::min(r.x0, r.x1); out.x1 = std::max(r.x0, r.x1);
	out.y0 = std::min(r.y0, r.y1); out.y1 = std::max(r.y0, r.y1);
	return out;
}

} // namespace

std::vector<PageRectPt> PdfDocument::textQuadsInRect(int page, PageRectPt rect)
{
	std::vector<PageRectPt> out;
	if (!ctx_ || !doc_ || !authed_) return out;
	if (page < 0 || page >= pageCount_) return out;

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	fz_rect sel = normRect(rect);
	fz_page* pg = nullptr;
	fz_stext_page* stext = nullptr;
	fz_try(ctx_) {
		pg = fz_load_page(ctx_, doc_, page);
		fz_stext_options opts = {};
		stext = fz_new_stext_page_from_page(ctx_, pg, &opts);
		for (fz_stext_block* b = stext->first_block; b; b = b->next) {
			if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
			for (fz_stext_line* ln = b->u.t.first_line; ln; ln = ln->next) {
				for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next) {
					fz_rect cq = fz_rect_from_quad(ch->quad);
					float cx = (cq.x0 + cq.x1) * 0.5f, cy = (cq.y0 + cq.y1) * 0.5f;
					if (cx >= sel.x0 && cx <= sel.x1 && cy >= sel.y0 && cy <= sel.y1)
						out.push_back({ cq.x0, cq.y0, cq.x1, cq.y1 });
				}
			}
		}
	}
	fz_always(ctx_) {
		if (stext) fz_drop_stext_page(ctx_, stext);
		if (pg) fz_drop_page(ctx_, pg);
	}
	fz_catch(ctx_) {
		out.clear();
	}
	return out;
}

std::vector<PageChar> PdfDocument::pageChars(int page)
{
	std::vector<PageChar> out;
	if (!ctx_ || !doc_ || !authed_) return out;
	if (page < 0 || page >= pageCount_) return out;

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	fz_page* pg = nullptr;
	fz_stext_page* stext = nullptr;
	fz_try(ctx_) {
		pg = fz_load_page(ctx_, doc_, page);
		fz_stext_options opts = {};
		stext = fz_new_stext_page_from_page(ctx_, pg, &opts);
		for (fz_stext_block* b = stext->first_block; b; b = b->next) {
			if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
			for (fz_stext_line* ln = b->u.t.first_line; ln; ln = ln->next) {
				for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next) {
					fz_rect cq = fz_rect_from_quad(ch->quad);
					PageChar pc;
					pc.quad = { cq.x0, cq.y0, cq.x1, cq.y1 };
					pc.unicode = ch->c;
					out.push_back(pc);
				}
				if (!out.empty()) out.back().lineBreakAfter = true;
			}
			if (!out.empty()) out.back().paragraphBreakAfter = true;
		}
	}
	fz_always(ctx_) {
		if (stext) fz_drop_stext_page(ctx_, stext);
		if (pg) fz_drop_page(ctx_, pg);
	}
	fz_catch(ctx_) {
		out.clear();
	}
	return out;
}

bool PdfDocument::addHighlight(int page, const std::vector<PageRectPt>& quads,
	unsigned long color, float opacity, std::string& err)
{
	if (quads.empty()) { err = "no text selected"; return false; }
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pdf_page* pg = nullptr;
	pdf_annot* annot = nullptr;
	bool ok = false;
	fz_try(ctx_) {
		pg = pdf_load_page(ctx_, pdf, page);
		annot = pdf_create_annot(ctx_, pg, PDF_ANNOT_HIGHLIGHT);
		float col[3]; colorToFloat(color, col);
		pdf_set_annot_color(ctx_, annot, 3, col);
		pdf_set_annot_opacity(ctx_, annot, opacity);
		for (const auto& q : quads)
			pdf_add_annot_quad_point(ctx_, annot, fz_quad_from_rect(normRect(q)));
		pdf_update_annot(ctx_, annot);
		ok = true;
	}
	fz_always(ctx_) {
		if (annot) pdf_drop_annot(ctx_, annot);
		if (pg) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg));
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_); ok = false;
	}
	if (ok) dirty_ = true;
	return ok;
}

bool PdfDocument::addInk(int page, const std::vector<std::vector<PagePointF>>& strokes,
	unsigned long color, float widthPt, std::string& err)
{
	bool hasPoints = false;
	for (const auto& s : strokes) if (!s.empty()) { hasPoints = true; break; }
	if (!hasPoints) { err = "empty stroke"; return false; }
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pdf_page* pg = nullptr;
	pdf_annot* annot = nullptr;
	bool ok = false;
	fz_try(ctx_) {
		pg = pdf_load_page(ctx_, pdf, page);
		annot = pdf_create_annot(ctx_, pg, PDF_ANNOT_INK);
		float col[3]; colorToFloat(color, col);
		pdf_set_annot_color(ctx_, annot, 3, col);
		pdf_set_annot_border_width(ctx_, annot, widthPt);
		for (const auto& stroke : strokes) {
			if (stroke.empty()) continue;
			pdf_add_annot_ink_list_stroke(ctx_, annot);
			for (const auto& p : stroke) {
				fz_point pt = { p.x, p.y };
				pdf_add_annot_ink_list_stroke_vertex(ctx_, annot, pt);
			}
		}
		pdf_update_annot(ctx_, annot);
		ok = true;
	}
	fz_always(ctx_) {
		if (annot) pdf_drop_annot(ctx_, annot);
		if (pg) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg));
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_); ok = false;
	}
	if (ok) dirty_ = true;
	return ok;
}

bool PdfDocument::addTextBox(int page, PageRectPt rect, const std::string& utf8,
	const std::string& font, float sizePt, unsigned long color, std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pdf_page* pg = nullptr;
	pdf_annot* annot = nullptr;
	bool ok = false;
	fz_try(ctx_) {
		pg = pdf_load_page(ctx_, pdf, page);
		annot = pdf_create_annot(ctx_, pg, PDF_ANNOT_FREE_TEXT);
		pdf_set_annot_rect(ctx_, annot, normRect(rect));
		float col[3]; colorToFloat(color, col);
		const char* f = font.empty() ? "Helv" : font.c_str();
		pdf_set_annot_default_appearance(ctx_, annot, f, sizePt, 3, col);
		pdf_set_annot_contents(ctx_, annot, utf8.c_str());
		pdf_update_annot(ctx_, annot);
		ok = true;
	}
	fz_always(ctx_) {
		if (annot) pdf_drop_annot(ctx_, annot);
		if (pg) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg));
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_); ok = false;
	}
	if (ok) dirty_ = true;
	return ok;
}

bool PdfDocument::addRedaction(int page, PageRectPt rect, std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pdf_page* pg = nullptr;
	pdf_annot* annot = nullptr;
	bool ok = false;
	fz_try(ctx_) {
		pg = pdf_load_page(ctx_, pdf, page);
		annot = pdf_create_annot(ctx_, pg, PDF_ANNOT_REDACT);
		pdf_set_annot_rect(ctx_, annot, normRect(rect));
		// Redact annotations don't support an interior/fill color (only a
		// border color) -- pdf_set_annot_interior_color throws for this
		// subtype. The black-box fill itself comes from applyRedactions()'s
		// black_boxes option at apply time; before that, the border is the
		// only visual cue this app draws for a pending mark.
		float black[3] = { 0.0f, 0.0f, 0.0f };
		pdf_set_annot_color(ctx_, annot, 3, black);
		pdf_update_annot(ctx_, annot);
		ok = true;
	}
	fz_always(ctx_) {
		if (annot) pdf_drop_annot(ctx_, annot);
		if (pg) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg));
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_); ok = false;
	}
	if (ok) dirty_ = true;
	return ok;
}

namespace {

WidgetInfo describeWidget(fz_context* ctx, pdf_annot* w, int idx)
{
	WidgetInfo info;
	fz_rect r = pdf_bound_widget(ctx, w);
	info.index = idx;
	info.rect = { r.x0, r.y0, r.x1, r.y1 };
	switch (pdf_widget_type(ctx, w)) {
	case PDF_WIDGET_TYPE_TEXT: info.kind = WidgetKind::Text; break;
	case PDF_WIDGET_TYPE_CHECKBOX: info.kind = WidgetKind::Checkbox; break;
	case PDF_WIDGET_TYPE_RADIOBUTTON: info.kind = WidgetKind::Radio; break;
	case PDF_WIDGET_TYPE_COMBOBOX: info.kind = WidgetKind::Combo; break;
	case PDF_WIDGET_TYPE_LISTBOX: info.kind = WidgetKind::ListBox; break;
	case PDF_WIDGET_TYPE_BUTTON: info.kind = WidgetKind::Button; break;
	case PDF_WIDGET_TYPE_SIGNATURE: info.kind = WidgetKind::Signature; break;
	default: info.kind = WidgetKind::None; break;
	}
	const char* val = pdf_annot_field_value(ctx, w);
	if (val) info.value = val;
	if (info.kind == WidgetKind::Text)
		info.multiline = (pdf_annot_field_flags(ctx, w) & PDF_TX_FIELD_IS_MULTILINE) != 0;
	if (info.kind == WidgetKind::Combo || info.kind == WidgetKind::ListBox) {
		int n = pdf_choice_widget_options(ctx, w, 0, nullptr);
		if (n > 0) {
			std::vector<const char*> opts(n);
			pdf_choice_widget_options(ctx, w, 0, opts.data());
			for (int i = 0; i < n; ++i) if (opts[i]) info.options.push_back(opts[i]);
		}
	}
	return info;
}

} // namespace

std::vector<WidgetInfo> PdfDocument::pageWidgets(int page)
{
	std::vector<WidgetInfo> out;
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) return out;

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pdf_page* pg = nullptr;
	fz_try(ctx_) {
		pg = pdf_load_page(ctx_, pdf, page);
		int idx = 0;
		for (pdf_annot* w = pdf_first_widget(ctx_, pg); w; w = pdf_next_widget(ctx_, w), ++idx) {
			if (pdf_widget_type(ctx_, w) == PDF_WIDGET_TYPE_SIGNATURE) continue;
			out.push_back(describeWidget(ctx_, w, idx));
		}
	}
	fz_always(ctx_) { if (pg) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
	fz_catch(ctx_) { out.clear(); }
	return out;
}

WidgetInfo PdfDocument::widgetAt(int page, PagePointF pt)
{
	WidgetInfo info;
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) return info;

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pdf_page* pg = nullptr;
	fz_try(ctx_) {
		pg = pdf_load_page(ctx_, pdf, page);
		int idx = 0;
		for (pdf_annot* w = pdf_first_widget(ctx_, pg); w; w = pdf_next_widget(ctx_, w), ++idx) {
			fz_rect r = pdf_bound_widget(ctx_, w);
			if (pt.x < r.x0 || pt.x > r.x1 || pt.y < r.y0 || pt.y > r.y1) continue;
			info = describeWidget(ctx_, w, idx);
			break;
		}
	}
	fz_always(ctx_) {
		if (pg) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg));
	}
	fz_catch(ctx_) {
		info = WidgetInfo{};
	}
	return info;
}

namespace {
// Walk to the widget at `index` on an already-loaded page.
pdf_annot* widgetByIndex(fz_context* ctx, pdf_page* pg, int index)
{
	int i = 0;
	for (pdf_annot* w = pdf_first_widget(ctx, pg); w; w = pdf_next_widget(ctx, w), ++i)
		if (i == index) return w;
	return nullptr;
}
} // namespace

bool PdfDocument::setTextWidget(int page, int widgetIndex, const std::string& utf8, std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pdf_page* pg = nullptr;
	bool ok = false;
	fz_try(ctx_) {
		pg = pdf_load_page(ctx_, pdf, page);
		pdf_annot* w = widgetByIndex(ctx_, pg, widgetIndex);
		if (w) { pdf_set_text_field_value(ctx_, w, utf8.c_str()); pdf_update_annot(ctx_, w); ok = true; }
	}
	fz_always(ctx_) { if (pg) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
	fz_catch(ctx_) { err = fz_caught_message(ctx_); ok = false; }
	if (ok) dirty_ = true;
	return ok;
}

bool PdfDocument::toggleWidget(int page, int widgetIndex, std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pdf_page* pg = nullptr;
	bool ok = false;
	fz_try(ctx_) {
		pg = pdf_load_page(ctx_, pdf, page);
		pdf_annot* w = widgetByIndex(ctx_, pg, widgetIndex);
		if (w) { pdf_toggle_widget(ctx_, w); pdf_update_annot(ctx_, w); ok = true; }
	}
	fz_always(ctx_) { if (pg) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
	fz_catch(ctx_) { err = fz_caught_message(ctx_); ok = false; }
	if (ok) dirty_ = true;
	return ok;
}

bool PdfDocument::setChoiceWidget(int page, int widgetIndex, const std::string& value, std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pdf_page* pg = nullptr;
	bool ok = false;
	fz_try(ctx_) {
		pg = pdf_load_page(ctx_, pdf, page);
		pdf_annot* w = widgetByIndex(ctx_, pg, widgetIndex);
		if (w) {
			const char* v = value.c_str();
			pdf_choice_widget_set_value(ctx_, w, 1, &v);
			pdf_update_annot(ctx_, w);
			ok = true;
		}
	}
	fz_always(ctx_) { if (pg) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
	fz_catch(ctx_) { err = fz_caught_message(ctx_); ok = false; }
	if (ok) dirty_ = true;
	return ok;
}

namespace {

AnnotInfo describeAnnot(fz_context* ctx, pdf_annot* a, int idx)
{
	AnnotInfo info;
	info.index = idx;
	fz_rect r = pdf_bound_annot(ctx, a);
	info.rect = { r.x0, r.y0, r.x1, r.y1 };
	switch (pdf_annot_type(ctx, a)) {
	case PDF_ANNOT_FREE_TEXT: {
		info.kind = AnnotKind::FreeText;
		const char* c = pdf_annot_contents(ctx, a);
		if (c) info.contents = c;
		const char* font = nullptr; float size = 12.0f; int n = 0; float col[4] = {};
		pdf_annot_default_appearance(ctx, a, &font, &size, &n, col);
		info.fontSize = size;
		info.font = font ? font : "Helv";
		// Build a proper COLORREF (RGB() macro layout) to match what
		// colorToFloat() expects back on the way in -- not a packed
		// 0xRRGGBB integer, which would swap red and blue for every
		// caller that treats info.color as a COLORREF (e.g. textColor_).
		if (n >= 3) info.color = RGB(static_cast<int>(col[0] * 255),
			static_cast<int>(col[1] * 255), static_cast<int>(col[2] * 255));
		break;
	}
	case PDF_ANNOT_HIGHLIGHT: info.kind = AnnotKind::Highlight; break;
	case PDF_ANNOT_INK: info.kind = AnnotKind::Ink; break;
	default: info.kind = AnnotKind::Other; break;
	}
	return info;
}

// Walk to the annotation at `index` on an already-loaded page (mirrors
// widgetByIndex, but over pdf_first_annot/pdf_next_annot).
pdf_annot* annotByIndex(fz_context* ctx, pdf_page* pg, int index)
{
	int i = 0;
	for (pdf_annot* a = pdf_first_annot(ctx, pg); a; a = pdf_next_annot(ctx, a), ++i)
		if (i == index) return a;
	return nullptr;
}

} // namespace

std::vector<AnnotInfo> PdfDocument::pageAnnots(int page)
{
	std::vector<AnnotInfo> out;
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) return out;
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pdf_page* pg = nullptr;
	fz_try(ctx_) {
		pg = pdf_load_page(ctx_, pdf, page);
		int idx = 0;
		for (pdf_annot* a = pdf_first_annot(ctx_, pg); a; a = pdf_next_annot(ctx_, a), ++idx) {
			AnnotInfo info = describeAnnot(ctx_, a, idx);
			if (info.kind != AnnotKind::Other) out.push_back(info);
		}
	}
	fz_always(ctx_) { if (pg) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
	fz_catch(ctx_) { out.clear(); }
	return out;
}

AnnotInfo PdfDocument::annotAt(int page, PagePointF pt)
{
	AnnotInfo info;
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) return info;
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pdf_page* pg = nullptr;
	fz_try(ctx_) {
		pg = pdf_load_page(ctx_, pdf, page);
		int idx = 0;
		// Last match wins: annotations are enumerated bottom-to-top, so the
		// last one containing the point is the one actually drawn on top.
		for (pdf_annot* a = pdf_first_annot(ctx_, pg); a; a = pdf_next_annot(ctx_, a), ++idx) {
			fz_rect r = pdf_bound_annot(ctx_, a);
			if (pt.x < r.x0 || pt.x > r.x1 || pt.y < r.y0 || pt.y > r.y1) continue;
			AnnotInfo cand = describeAnnot(ctx_, a, idx);
			if (cand.kind != AnnotKind::Other) info = cand;
		}
	}
	fz_always(ctx_) { if (pg) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
	fz_catch(ctx_) { info = AnnotInfo{}; }
	return info;
}

bool PdfDocument::setFreeTextAnnot(int page, int annotIndex, const std::string& utf8,
	float fontSize, unsigned long color, PageRectPt rect, std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pdf_page* pg = nullptr;
	bool ok = false;
	fz_try(ctx_) {
		pg = pdf_load_page(ctx_, pdf, page);
		pdf_annot* a = annotByIndex(ctx_, pg, annotIndex);
		if (a && pdf_annot_type(ctx_, a) == PDF_ANNOT_FREE_TEXT) {
			float col[3]; colorToFloat(color, col);
			pdf_set_annot_rect(ctx_, a, normRect(rect));
			pdf_set_annot_default_appearance(ctx_, a, "Helv", fontSize, 3, col);
			pdf_set_annot_contents(ctx_, a, utf8.c_str());
			pdf_update_annot(ctx_, a);
			ok = true;
		}
	}
	fz_always(ctx_) { if (pg) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
	fz_catch(ctx_) { err = fz_caught_message(ctx_); ok = false; }
	if (ok) dirty_ = true;
	return ok;
}

bool PdfDocument::deleteAnnot(int page, int annotIndex, std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	pdf_page* pg = nullptr;
	bool ok = false;
	fz_try(ctx_) {
		pg = pdf_load_page(ctx_, pdf, page);
		pdf_annot* a = annotByIndex(ctx_, pg, annotIndex);
		if (a) { pdf_delete_annot(ctx_, pg, a); ok = true; }
	}
	fz_always(ctx_) { if (pg) fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
	fz_catch(ctx_) { err = fz_caught_message(ctx_); ok = false; }
	if (ok) dirty_ = true;
	return ok;
}

PageBitmap PdfDocument::renderPageWithoutAnnot(int index, float scale, int annotIndex)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf || annotIndex < 0) return renderPage(index, scale);

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	int savedFlags = 0;
	bool hidAnnot = false;
	fz_try(ctx_) {
		pdf_page* pg = pdf_load_page(ctx_, pdf, index);
		fz_try(ctx_) {
			pdf_annot* a = annotByIndex(ctx_, pg, annotIndex);
			if (a) {
				savedFlags = pdf_annot_flags(ctx_, a);
				pdf_set_annot_flags(ctx_, a, savedFlags | PDF_ANNOT_IS_HIDDEN);
				hidAnnot = true;
			}
		}
		fz_always(ctx_) { fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
		fz_catch(ctx_) { fz_rethrow(ctx_); }
	}
	fz_catch(ctx_) { hidAnnot = false; }

	// A fresh (uncached at the PdfDocument level) render with the annotation
	// being edited hidden, so an inline "transparent" edit box doesn't show
	// that annotation's own old text doubled up behind the live typing.
	PageBitmap result = renderPage(index, scale);

	if (hidAnnot) {
		fz_try(ctx_) {
			pdf_page* pg = pdf_load_page(ctx_, pdf, index);
			fz_try(ctx_) {
				pdf_annot* a = annotByIndex(ctx_, pg, annotIndex);
				if (a) pdf_set_annot_flags(ctx_, a, savedFlags);
			}
			fz_always(ctx_) { fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
			fz_catch(ctx_) { fz_rethrow(ctx_); }
		}
		fz_catch(ctx_) { /* best effort restore */ }
	}
	return result;
}

// --- Page assembly (merge/split/organize) -----------------------------------

int PdfDocument::openExternalFile(const wchar_t* path, std::string& err)
{
	if (!ctx_) { err = "no context"; return -1; }
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::string upath = toUtf8(path);
	fz_document* d = nullptr;
	fz_try(ctx_) {
		d = fz_open_document(ctx_, upath.c_str());
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_);
		return -1;
	}
	if (fz_needs_password(ctx_, d)) {
		err = "password-protected files are not supported here";
		fz_drop_document(ctx_, d);
		return -1;
	}
	if (!pdf_document_from_fz_document(ctx_, d)) {
		err = "not a PDF";
		fz_drop_document(ctx_, d);
		return -1;
	}
	ExternalFile f;
	f.doc = d;
	f.path = path;
	externalFiles_.push_back(std::move(f));
	return static_cast<int>(externalFiles_.size()) - 1;
}

int PdfDocument::externalPageCount(int fileIndex) const
{
	if (fileIndex < 0 || fileIndex >= static_cast<int>(externalFiles_.size())) return 0;
	int n = 0;
	fz_try(ctx_) { n = fz_count_pages(ctx_, externalFiles_[fileIndex].doc); }
	fz_catch(ctx_) { n = 0; }
	return n;
}

PageSizePt PdfDocument::externalPageSize(int fileIndex, int pageIndex) const
{
	PageSizePt out;
	if (fileIndex < 0 || fileIndex >= static_cast<int>(externalFiles_.size())) return out;
	fz_document* d = externalFiles_[fileIndex].doc;
	fz_try(ctx_) {
		fz_page* page = fz_load_page(ctx_, d, pageIndex);
		fz_rect r = fz_bound_page(ctx_, page);
		fz_drop_page(ctx_, page);
		out = { r.x1 - r.x0, r.y1 - r.y0 };
	}
	fz_catch(ctx_) {
		out = {};
	}
	return out;
}

PageBitmap PdfDocument::renderExternalPage(int fileIndex, int pageIndex, float scale)
{
	PageBitmap result;
	if (fileIndex < 0 || fileIndex >= static_cast<int>(externalFiles_.size())) return result;
	if (scale <= 0.0f) return result;
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	fz_document* d = externalFiles_[fileIndex].doc;
	fz_page* page = nullptr;
	fz_pixmap* pix = nullptr;
	fz_try(ctx_) {
		page = fz_load_page(ctx_, d, pageIndex);
		fz_matrix ctm = fz_scale(scale, scale);
		pix = fz_new_pixmap_from_page(ctx_, page, ctm, fz_device_rgb(ctx_), 0);
		int w = 0, h = 0;
		HBITMAP hbmp = pixmapToDIB(ctx_, pix, w, h);
		if (hbmp) { result.hbmp = hbmp; result.width = w; result.height = h; }
	}
	fz_always(ctx_) {
		if (pix) fz_drop_pixmap(ctx_, pix);
		if (page) fz_drop_page(ctx_, page);
	}
	fz_catch(ctx_) {
		// return empty on failure
	}
	return result;
}

void PdfDocument::closeExternalFiles()
{
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	for (auto& f : externalFiles_) if (f.doc) fz_drop_document(ctx_, f.doc);
	externalFiles_.clear();
}

namespace {

// Like MuPDF's own pdf_graft_mapped_page (third_party/mupdf/source/pdf/
// pdf-graft.c), but also copies /Annots -- the library version deliberately
// omits it ("do not include items that reference other pages"), which would
// silently drop every highlight/ink/free-text/redaction on merge/split/
// reorder. Known limitation: the document-level /AcroForm field tree isn't
// reconstructed, so form widgets keep their appearance/value but may lose
// live fillability.
void graftPageWithAnnots(fz_context* ctx, pdf_graft_map* map, pdf_document* dst,
	int page_to, pdf_document* src, int page_from, int extraRotation)
{
	pdf_obj* page_ref = pdf_lookup_page_obj(ctx, src, page_from);
	pdf_obj* page_dict = nullptr;
	pdf_obj* ref = nullptr;
	fz_try(ctx) {
		page_dict = pdf_new_dict(ctx, dst, 8);
		pdf_dict_put(ctx, page_dict, PDF_NAME(Type), PDF_NAME(Page));

		static pdf_obj* const inheritable[] = {
			PDF_NAME(Resources), PDF_NAME(MediaBox), PDF_NAME(CropBox), PDF_NAME(UserUnit),
		};
		for (pdf_obj* key : inheritable) {
			pdf_obj* obj = pdf_dict_get_inheritable(ctx, page_ref, key);
			if (obj) pdf_dict_put_drop(ctx, page_dict, key, pdf_graft_mapped_object(ctx, map, obj));
		}
		pdf_obj* contents = pdf_dict_get(ctx, page_ref, PDF_NAME(Contents));
		if (contents) pdf_dict_put_drop(ctx, page_dict, PDF_NAME(Contents), pdf_graft_mapped_object(ctx, map, contents));
		pdf_obj* annots = pdf_dict_get(ctx, page_ref, PDF_NAME(Annots));
		if (annots) pdf_dict_put_drop(ctx, page_dict, PDF_NAME(Annots), pdf_graft_mapped_object(ctx, map, annots));

		pdf_obj* rotObj = pdf_dict_get_inheritable(ctx, page_ref, PDF_NAME(Rotate));
		int rotate = rotObj ? pdf_to_int(ctx, rotObj) : 0;
		rotate = ((rotate + extraRotation) % 360 + 360) % 360;
		if (rotate != 0) pdf_dict_put_int(ctx, page_dict, PDF_NAME(Rotate), rotate);

		ref = pdf_add_object(ctx, dst, page_dict);
		pdf_insert_page(ctx, dst, page_to, ref);
	}
	fz_always(ctx) {
		pdf_drop_obj(ctx, page_dict);
		pdf_drop_obj(ctx, ref);
	}
	fz_catch(ctx) {
		fz_rethrow(ctx);
	}
}

// Builds a brand-new pdf_document from an ordered page plan, grafting each
// entry (with annotations) from `selfPdf` or `externalDocs[externalFileIndex]`
// as appropriate. One pdf_graft_map per distinct source document (a map is
// tied to a single source -- see pdf_new_graft_map's contract). Caller owns
// the returned document.
pdf_document* buildFromPlan(fz_context* ctx, pdf_document* selfPdf,
	const std::vector<fz_document*>& externalDocs,
	const std::vector<PdfDocument::PagePlanEntry>& entries)
{
	pdf_document* newDoc = pdf_create_document(ctx);
	fz_try(ctx) {
		std::unordered_map<pdf_document*, pdf_graft_map*> maps;
		for (const auto& e : entries) {
			pdf_document* src = selfPdf;
			if (e.externalFileIndex >= 0) {
				if (e.externalFileIndex >= static_cast<int>(externalDocs.size())) continue;
				src = pdf_document_from_fz_document(ctx, externalDocs[e.externalFileIndex]);
				if (!src) continue;
			}
			pdf_graft_map*& m = maps[src];
			if (!m) m = pdf_new_graft_map(ctx, newDoc);
			graftPageWithAnnots(ctx, m, newDoc, -1, src, e.pageIndex, e.extraRotation);
		}
		for (auto& kv : maps) pdf_drop_graft_map(ctx, kv.second);
	}
	fz_catch(ctx) {
		pdf_drop_document(ctx, newDoc);
		fz_rethrow(ctx);
	}
	return newDoc;
}

} // namespace

bool PdfDocument::rebuildFromPages(const std::vector<PagePlanEntry>& entries, std::string& err)
{
	if (!ctx_) { err = "no context"; return false; }
	std::lock_guard<std::recursive_mutex> lock(mutex_);

	// selfPdf may legitimately be null here: Merge into a blank tab never
	// opens a base document, so doc_ is null and every plan entry references
	// an external file instead. Only reject if an entry actually needs this
	// document's own pages (externalFileIndex < 0) but there are none.
	pdf_document* selfPdf = doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (entries.empty()) { err = "no pages"; return false; }
	for (const auto& e : entries) {
		if (e.externalFileIndex < 0 && !selfPdf) { err = "not a PDF"; return false; }
	}

	std::vector<fz_document*> externalDocs;
	externalDocs.reserve(externalFiles_.size());
	for (auto& f : externalFiles_) externalDocs.push_back(f.doc);

	pdf_document* newDoc = nullptr;
	fz_try(ctx_) {
		newDoc = buildFromPlan(ctx_, selfPdf, externalDocs, entries);
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_);
		return false;
	}

	// Adopt the freshly built document as this one's live handle -- pdf_document's
	// first member is literally `fz_document super` (mupdf/pdf/document.h), so
	// taking its address is the standard, well-defined way to get back the
	// generic fz_document* handle this class stores.
	if (doc_) fz_drop_document(ctx_, doc_);
	doc_ = &newDoc->super;
	// A freshly built document is always readable -- set authed_ so isOpen()
	// reports true (it stays false on a blank Merge tab that was never
	// open()'d, which would otherwise keep the canvas from ever rendering).
	authed_ = true;
	closeExternalFiles();
	loadInfo();
	dirty_ = true;
	return true;
}

namespace {

// Opens `utf8path` in a brand-new, independent fz_context (not sharing any
// cache/state with the live document) and confirms it actually parses and
// renders. Used to verify a just-written PDF before it's allowed to replace
// the user's original file.
bool verifyPdfFile(const std::string& utf8path, int expectedPageCount)
{
	fz_context* vctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
	if (!vctx) return false;
	installSystemFontHook(vctx);
	bool ok = false;
	fz_try(vctx) {
		fz_register_document_handlers(vctx);
		fz_document* vdoc = fz_open_document(vctx, utf8path.c_str());
		fz_try(vctx) {
			int n = fz_count_pages(vctx, vdoc);
			if (n == expectedPageCount && n > 0) {
				fz_page* page = fz_load_page(vctx, vdoc, 0);
				fz_try(vctx) {
					fz_pixmap* pix = fz_new_pixmap_from_page(vctx, page, fz_scale(1, 1), fz_device_rgb(vctx), 0);
					fz_drop_pixmap(vctx, pix);
					ok = true;
				}
				fz_always(vctx) { fz_drop_page(vctx, page); }
				fz_catch(vctx) { ok = false; }
			}
		}
		fz_always(vctx) { fz_drop_document(vctx, vdoc); }
		fz_catch(vctx) { ok = false; }
	}
	fz_catch(vctx) {
		ok = false;
	}
	fz_drop_context(vctx);
	return ok;
}

} // namespace

bool PdfDocument::save(const wchar_t* path, bool incremental, std::string& err)
{
	return writeAndReplace(path, incremental, /*stripEncryption=*/false, err);
}

bool PdfDocument::removeProtection(const wchar_t* path, std::string& err)
{
	if (!ctx_ || !doc_) { err = "no document"; return false; }
	bool ok = writeAndReplace(path, /*incremental=*/false, /*stripEncryption=*/true, err);
	if (ok) neededPassword_ = false;
	return ok;
}

bool PdfDocument::writeAndReplace(const wchar_t* path, bool incremental, bool stripEncryption, std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }

	std::lock_guard<std::recursive_mutex> lock(mutex_);

	// Never write directly over the target: write to a sibling temp file,
	// verify the result actually opens and renders, and only then replace
	// the original. This makes a bad/interrupted write unable to destroy
	// the user's file, regardless of what causes it.
	std::wstring wtarget(path);
	std::wstring wtemp = wtarget + L".pdfviewer_tmp";
	std::string utemp = toUtf8(wtemp.c_str());

	bool wrote = false;
	fz_try(ctx_) {
		pdf_write_options opts = pdf_default_write_options;
		if (incremental) {
			opts.do_incremental = 1;
		} else {
			opts.do_garbage = 1;
			opts.do_compress = 1;
			opts.do_compress_images = 1;
			opts.do_compress_fonts = 1;
			opts.do_use_objstms = 1;
			opts.compression_effort = 100;
		}
		if (stripEncryption) opts.do_encrypt = PDF_ENCRYPT_NONE;
		pdf_save_document(ctx_, pdf, utemp.c_str(), &opts);
		wrote = true;
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_);
		wrote = false;
	}
	if (!wrote) {
		DeleteFileW(wtemp.c_str());
		return false;
	}

	if (!verifyPdfFile(utemp, pageCount_)) {
		err = "save produced an invalid file; the original was left untouched";
		DeleteFileW(wtemp.c_str());
		return false;
	}

	// Release our own read handle before replacing -- Windows won't let us
	// replace a file this same process still has open for reading.
	if (doc_) { fz_drop_document(ctx_, doc_); doc_ = nullptr; }

	if (!MoveFileExW(wtemp.c_str(), wtarget.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		err = "could not replace the original file (it may be open elsewhere)";
		DeleteFileW(wtemp.c_str());
		std::string reopenErr;
		reopenCurrentPath(reopenErr); // restore state from the untouched original
		return false;
	}

	openedPath_ = wtarget;
	std::string reopenErr;
	if (!reopenCurrentPath(reopenErr)) {
		err = "saved, but failed to reopen the result: " + reopenErr;
		return false; // the file on disk is fine; in-memory state is broken
	}

	dirty_ = false;
	return true;
}

bool PdfDocument::exportPages(const std::vector<int>& pageIndices, const wchar_t* path, std::string& err)
{
	pdf_document* selfPdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!selfPdf) { err = "not a PDF"; return false; }
	if (pageIndices.empty()) { err = "no pages selected"; return false; }

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::vector<PagePlanEntry> entries;
	entries.reserve(pageIndices.size());
	for (int p : pageIndices) entries.push_back({ -1, p, 0 });

	pdf_document* newDoc = nullptr;
	fz_try(ctx_) {
		newDoc = buildFromPlan(ctx_, selfPdf, {}, entries);
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_);
		return false;
	}

	// Same never-overwrite-directly discipline as writeAndReplace(), but this
	// document's own live handle/openedPath_ are untouched -- exportPages()
	// produces a standalone file, not a save of *this* document.
	std::wstring wtarget(path);
	std::wstring wtemp = wtarget + L".pdfviewer_tmp";
	std::string utemp = toUtf8(wtemp.c_str());
	bool wrote = false;
	fz_try(ctx_) {
		pdf_write_options opts = pdf_default_write_options;
		opts.do_garbage = 1;
		opts.do_compress = 1;
		opts.do_compress_images = 1;
		opts.do_compress_fonts = 1;
		pdf_save_document(ctx_, newDoc, utemp.c_str(), &opts);
		wrote = true;
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_);
	}
	pdf_drop_document(ctx_, newDoc);
	if (!wrote) {
		DeleteFileW(wtemp.c_str());
		return false;
	}
	if (!verifyPdfFile(utemp, static_cast<int>(pageIndices.size()))) {
		err = "export produced an invalid file";
		DeleteFileW(wtemp.c_str());
		return false;
	}
	if (!MoveFileExW(wtemp.c_str(), wtarget.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		err = "could not write the output file (it may be open elsewhere)";
		DeleteFileW(wtemp.c_str());
		return false;
	}
	return true;
}

// --- Document-wide transforms (resize/flatten/compress/redact) -------------
namespace {

// Concatenates a page's /Contents (a single stream, or an array of streams
// -- both are legal PDF) into one buffer. Caller owns the result.
fz_buffer* loadPageContentBytes(fz_context* ctx, pdf_obj* page_ref)
{
	pdf_obj* contents = pdf_dict_get(ctx, page_ref, PDF_NAME(Contents));
	fz_buffer* buf = fz_new_buffer(ctx, 1024);
	fz_try(ctx) {
		if (pdf_is_array(ctx, contents)) {
			int n = pdf_array_len(ctx, contents);
			for (int i = 0; i < n; ++i) {
				fz_buffer* part = pdf_load_stream(ctx, pdf_array_get(ctx, contents, i));
				fz_try(ctx) {
					fz_append_buffer(ctx, buf, part);
					fz_append_byte(ctx, buf, '\n');
				}
				fz_always(ctx) { fz_drop_buffer(ctx, part); }
				fz_catch(ctx) { fz_rethrow(ctx); }
			}
		} else if (contents) {
			fz_buffer* part = pdf_load_stream(ctx, contents);
			fz_try(ctx) { fz_append_buffer(ctx, buf, part); }
			fz_always(ctx) { fz_drop_buffer(ctx, part); }
			fz_catch(ctx) { fz_rethrow(ctx); }
		}
	}
	fz_catch(ctx) {
		fz_drop_buffer(ctx, buf);
		fz_rethrow(ctx);
	}
	return buf;
}

// Transforms every annotation on an already-loaded page by `m`: quad points
// (Highlight's actual visual shape) and the rect, then asks MuPDF to
// regenerate the appearance stream from the updated model data. Best-effort
// -- a single annotation failing to update doesn't abort the page.
void transformPageAnnotations(fz_context* ctx, pdf_page* pg, fz_matrix m)
{
	for (pdf_annot* a = pdf_first_annot(ctx, pg); a; a = pdf_next_annot(ctx, a)) {
		fz_try(ctx) {
			int qn = pdf_annot_quad_point_count(ctx, a);
			if (qn > 0) {
				std::vector<fz_quad> quads(qn);
				for (int i = 0; i < qn; ++i) quads[i] = fz_transform_quad(pdf_annot_quad_point(ctx, a, i), m);
				pdf_clear_annot_quad_points(ctx, a);
				for (auto& q : quads) pdf_add_annot_quad_point(ctx, a, q);
			}
			fz_rect r = fz_transform_rect(pdf_bound_annot(ctx, a), m);
			pdf_set_annot_rect(ctx, a, r);
			pdf_update_annot(ctx, a);
		}
		fz_catch(ctx) {
			// best-effort: leave this annotation as-is and continue with others
		}
	}
}

} // namespace

bool PdfDocument::resizeToA4(std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }
	std::lock_guard<std::recursive_mutex> lock(mutex_);

	constexpr float kA4W = 595.28f, kA4H = 841.89f; // ISO A4, points (1/72")
	fz_try(ctx_) {
		int n = fz_count_pages(ctx_, doc_);
		for (int i = 0; i < n; ++i) {
			pdf_page* pg = pdf_load_page(ctx_, pdf, i);
			fz_try(ctx_) {
				fz_rect bound = fz_bound_page(ctx_, reinterpret_cast<fz_page*>(pg));
				float srcW = bound.x1 - bound.x0, srcH = bound.y1 - bound.y0;
				if (srcW > 0 && srcH > 0) {
					// Landscape A4 for pages already wider than tall, so we're
					// not forcing a near-90-degree-rotated document into a
					// portrait box with huge margins.
					bool landscape = srcW > srcH;
					float targetW = landscape ? kA4H : kA4W;
					float targetH = landscape ? kA4W : kA4H;
					float scale = std::min(targetW / srcW, targetH / srcH);
					float tx = (targetW - srcW * scale) / 2.0f;
					float ty = (targetH - srcH * scale) / 2.0f;
					fz_matrix m = fz_concat(fz_translate(-bound.x0, -bound.y0), fz_scale(scale, scale));
					m = fz_concat(m, fz_translate(tx, ty));

					pdf_obj* page_ref = pdf_lookup_page_obj(ctx_, pdf, i);
					pdf_obj* origRes = pdf_dict_get_inheritable(ctx_, page_ref, PDF_NAME(Resources));
					fz_buffer* content = loadPageContentBytes(ctx_, page_ref);

					// Wrap the existing content as a self-contained Form XObject
					// rather than literally concatenating a "q ... cm" prefix into
					// the raw stream -- a `Do` invocation is atomic to the outer
					// stream, so this can't be broken by a mismatched q/Q balance
					// in the original content (a real-world risk with the naive
					// text-splice approach).
					pdf_obj* xobj = pdf_new_dict(ctx_, pdf, 4);
					pdf_dict_put(ctx_, xobj, PDF_NAME(Type), PDF_NAME(XObject));
					pdf_dict_put(ctx_, xobj, PDF_NAME(Subtype), PDF_NAME(Form));
					pdf_dict_put_rect(ctx_, xobj, PDF_NAME(BBox), bound);
					if (origRes) pdf_dict_put(ctx_, xobj, PDF_NAME(Resources), origRes);
					pdf_obj* xobjRef = pdf_add_stream(ctx_, pdf, content, xobj, 0);
					pdf_drop_obj(ctx_, xobj);
					fz_drop_buffer(ctx_, content);

					pdf_obj* newRes = pdf_new_dict(ctx_, pdf, 1);
					pdf_obj* xobjResDict = pdf_dict_put_dict(ctx_, newRes, PDF_NAME(XObject), 1);
					pdf_dict_puts(ctx_, xobjResDict, "Fx0", xobjRef);
					pdf_dict_put_drop(ctx_, page_ref, PDF_NAME(Resources), newRes);
					pdf_drop_obj(ctx_, xobjRef);

					fz_buffer* newContent = fz_new_buffer(ctx_, 128);
					fz_append_printf(ctx_, newContent, "q %g %g %g %g %g %g cm /Fx0 Do Q",
						m.a, m.b, m.c, m.d, m.e, m.f);
					pdf_obj* contentsRef = pdf_add_stream(ctx_, pdf, newContent, nullptr, 0);
					fz_drop_buffer(ctx_, newContent);
					pdf_dict_put_drop(ctx_, page_ref, PDF_NAME(Contents), contentsRef);
					pdf_dict_put_rect(ctx_, page_ref, PDF_NAME(MediaBox), fz_make_rect(0, 0, targetW, targetH));
					pdf_dict_del(ctx_, page_ref, PDF_NAME(CropBox));
					pdf_dict_del(ctx_, page_ref, PDF_NAME(BleedBox));
					pdf_dict_del(ctx_, page_ref, PDF_NAME(TrimBox));
					pdf_dict_del(ctx_, page_ref, PDF_NAME(ArtBox));

					transformPageAnnotations(ctx_, pg, m);
				}
			}
			fz_always(ctx_) { fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
			fz_catch(ctx_) { fz_rethrow(ctx_); }
		}
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_);
		return false;
	}
	loadInfo();
	dirty_ = true;
	return true;
}

bool PdfDocument::flattenToImages(std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }
	std::lock_guard<std::recursive_mutex> lock(mutex_);

	constexpr float kDpi = 200.0f;
	constexpr float kScale = kDpi / 72.0f;
	fz_try(ctx_) {
		int n = fz_count_pages(ctx_, doc_);
		for (int i = 0; i < n; ++i) {
			fz_page* page = fz_load_page(ctx_, doc_, i);
			fz_pixmap* pix = nullptr;
			fz_try(ctx_) {
				fz_rect bound = fz_bound_page(ctx_, page);
				float w = bound.x1 - bound.x0, h = bound.y1 - bound.y0;
				// Same ctm shape as renderPage(): fz_new_pixmap_from_page derives
				// the pixmap's own origin/extent from the page bbox under this
				// matrix, so no manual translate-to-origin is needed here.
				fz_matrix ctm = fz_scale(kScale, kScale);
				pix = fz_new_pixmap_from_page(ctx_, page, ctm, fz_device_rgb(ctx_), 0);

				fz_image* img = fz_new_image_from_pixmap(ctx_, pix, nullptr);
				pdf_obj* imgRef = nullptr;
				fz_try(ctx_) {
					imgRef = pdf_add_image(ctx_, pdf, img);
				}
				fz_always(ctx_) { fz_drop_image(ctx_, img); }
				fz_catch(ctx_) { fz_rethrow(ctx_); }

				pdf_obj* page_ref = pdf_lookup_page_obj(ctx_, pdf, i);
				pdf_obj* newRes = pdf_new_dict(ctx_, pdf, 1);
				pdf_obj* xobjResDict = pdf_dict_put_dict(ctx_, newRes, PDF_NAME(XObject), 1);
				pdf_dict_puts(ctx_, xobjResDict, "Im0", imgRef);
				pdf_dict_put_drop(ctx_, page_ref, PDF_NAME(Resources), newRes);
				pdf_drop_obj(ctx_, imgRef);

				// Image XObjects always map to the unit square -- scale it up to
				// the page's own width/height (in points) and place it at the
				// page's origin to cover the visible area exactly.
				fz_buffer* content = fz_new_buffer(ctx_, 128);
				fz_append_printf(ctx_, content, "q %g 0 0 %g %g %g cm /Im0 Do Q", w, h, bound.x0, bound.y0);
				pdf_obj* contentsRef = pdf_add_stream(ctx_, pdf, content, nullptr, 0);
				fz_drop_buffer(ctx_, content);
				pdf_dict_put_drop(ctx_, page_ref, PDF_NAME(Contents), contentsRef);
				pdf_dict_del(ctx_, page_ref, PDF_NAME(Annots));
			}
			fz_always(ctx_) {
				if (pix) fz_drop_pixmap(ctx_, pix);
				fz_drop_page(ctx_, page);
			}
			fz_catch(ctx_) { fz_rethrow(ctx_); }
		}
		pdf_obj* root = pdf_dict_get(ctx_, pdf_trailer(ctx_, pdf), PDF_NAME(Root));
		if (root) pdf_dict_del(ctx_, root, PDF_NAME(AcroForm));
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_);
		return false;
	}
	loadInfo();
	dirty_ = true;
	return true;
}

bool PdfDocument::compress(std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }
	std::lock_guard<std::recursive_mutex> lock(mutex_);

	// Use MuPDF's own image rewriter (the exact engine behind `mutool clean
	// --image-subsample-dpi ... --image-recompress-method jpeg`) rather than
	// a hand-rolled per-XObject loop. It computes each image's TRUE display
	// DPI from its placement matrix in the content streams (a bare
	// pixels-vs-page-width guess badly under/over-estimates for images not
	// spanning the full page), classifies color/gray/bitonal separately
	// (1-bit scanned pages must NOT become JPEG -- that bloats them; they
	// stay CCITT-fax), and only keeps a re-encoded image when it's actually
	// smaller (WHEN_SMALLER) so this can never grow a file. This is what
	// makes a 30MB image-heavy scan drop by an order of magnitude, which the
	// old loop (which skipped anything under 300 DPI outright) could not.
	//
	// Tunables: color/gray images above 150 DPI are downsampled to 150 DPI
	// and (re)compressed as quality-75 JPEG; that's the standard "good
	// balance" preset (comparable to the online compressors' default
	// "recommended" level) -- visibly clean for scanned documents while
	// giving large reductions. 150 DPI is the threshold most tools settle on
	// for screen-and-normal-print quality.
	char quality[] = "75";
	pdf_image_rewriter_options opt = {};
	opt.color_lossy_image_subsample_method = FZ_SUBSAMPLE_BICUBIC;
	opt.color_lossless_image_subsample_method = FZ_SUBSAMPLE_BICUBIC;
	opt.gray_lossy_image_subsample_method = FZ_SUBSAMPLE_BICUBIC;
	opt.gray_lossless_image_subsample_method = FZ_SUBSAMPLE_BICUBIC;
	opt.bitonal_image_subsample_method = FZ_SUBSAMPLE_AVERAGE;

	constexpr int kColorThreshold = 150, kColorTarget = 150;
	opt.color_lossy_image_subsample_threshold = kColorThreshold;
	opt.color_lossy_image_subsample_to = kColorTarget;
	opt.color_lossless_image_subsample_threshold = kColorThreshold;
	opt.color_lossless_image_subsample_to = kColorTarget;
	opt.gray_lossy_image_subsample_threshold = kColorThreshold;
	opt.gray_lossy_image_subsample_to = kColorTarget;
	opt.gray_lossless_image_subsample_threshold = kColorThreshold;
	opt.gray_lossless_image_subsample_to = kColorTarget;
	// Bitonal (1-bit) images are already compact via fax coding; only touch
	// genuinely oversized ones, and never turn them lossy.
	opt.bitonal_image_subsample_threshold = 400;
	opt.bitonal_image_subsample_to = 300;

	opt.color_lossy_image_recompress_method = FZ_RECOMPRESS_JPEG;
	opt.color_lossless_image_recompress_method = FZ_RECOMPRESS_JPEG;
	opt.gray_lossy_image_recompress_method = FZ_RECOMPRESS_JPEG;
	opt.gray_lossless_image_recompress_method = FZ_RECOMPRESS_JPEG;
	opt.bitonal_image_recompress_method = FZ_RECOMPRESS_FAX;
	opt.color_lossy_image_recompress_quality = quality;
	opt.color_lossless_image_recompress_quality = quality;
	opt.gray_lossy_image_recompress_quality = quality;
	opt.gray_lossless_image_recompress_quality = quality;
	opt.bitonal_image_recompress_quality = quality; // ignored by fax, harmless
	opt.recompress_when = FZ_RECOMPRESS_WHEN_SMALLER;

	fz_try(ctx_) {
		pdf_rewrite_images(ctx_, pdf, &opt);
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_);
		return false;
	}
	dirty_ = true;
	return true;
}

int PdfDocument::pendingRedactionCount()
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) return 0;
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	int total = 0;
	fz_try(ctx_) {
		int n = fz_count_pages(ctx_, doc_);
		for (int i = 0; i < n; ++i) {
			pdf_page* pg = pdf_load_page(ctx_, pdf, i);
			fz_try(ctx_) {
				for (pdf_annot* a = pdf_first_annot(ctx_, pg); a; a = pdf_next_annot(ctx_, a))
					if (pdf_annot_type(ctx_, a) == PDF_ANNOT_REDACT) ++total;
			}
			fz_always(ctx_) { fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
			fz_catch(ctx_) { fz_rethrow(ctx_); }
		}
	}
	fz_catch(ctx_) {
		// return whatever was counted before the failure
	}
	return total;
}

bool PdfDocument::applyRedactions(std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	fz_try(ctx_) {
		pdf_redact_options opts = {};
		opts.black_boxes = 1;
		opts.image_method = PDF_REDACT_IMAGE_PIXELS;
		opts.line_art = PDF_REDACT_LINE_ART_REMOVE_IF_TOUCHED;
		opts.text = PDF_REDACT_TEXT_REMOVE;
		int n = fz_count_pages(ctx_, doc_);
		for (int i = 0; i < n; ++i) {
			pdf_page* pg = pdf_load_page(ctx_, pdf, i);
			fz_try(ctx_) { pdf_redact_page(ctx_, pdf, pg, &opts); }
			fz_always(ctx_) { fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
			fz_catch(ctx_) { fz_rethrow(ctx_); }
		}
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_);
		return false;
	}
	loadInfo();
	dirty_ = true;
	return true;
}

bool PdfDocument::clearPendingRedactions(std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	bool any = false;
	fz_try(ctx_) {
		int n = fz_count_pages(ctx_, doc_);
		for (int i = 0; i < n; ++i) {
			pdf_page* pg = pdf_load_page(ctx_, pdf, i);
			fz_try(ctx_) {
				// Restart the walk after each delete rather than trying to
				// safely advance past a just-freed node -- redaction counts
				// are always small, so the O(n^2) worst case doesn't matter.
				bool foundOne = true;
				while (foundOne) {
					foundOne = false;
					for (pdf_annot* a = pdf_first_annot(ctx_, pg); a; a = pdf_next_annot(ctx_, a)) {
						if (pdf_annot_type(ctx_, a) == PDF_ANNOT_REDACT) {
							pdf_delete_annot(ctx_, pg, a);
							foundOne = true;
							any = true;
							break;
						}
					}
				}
			}
			fz_always(ctx_) { fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
			fz_catch(ctx_) { fz_rethrow(ctx_); }
		}
	}
	fz_catch(ctx_) {
		err = fz_caught_message(ctx_);
		return false;
	}
	if (any) dirty_ = true;
	return true;
}
