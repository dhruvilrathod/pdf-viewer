#include "pdf_document.h"
#include "word_convert.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
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

	// PDF/PostScript font names are typically spaceless CamelCase
	// ("CenturyGothic", "MyriadPro"), but Windows family names have spaces
	// ("Century Gothic"). Insert a space at each lowercase->uppercase boundary
	// so CreateFontW can find the installed family instead of falling back.
	std::string spaced;
	spaced.reserve(n.size() + 4);
	for (size_t i = 0; i < n.size(); ++i) {
		if (i > 0 && std::isupper(static_cast<unsigned char>(n[i])) &&
			std::islower(static_cast<unsigned char>(n[i - 1])))
			spaced.push_back(' ');
		spaced.push_back(n[i]);
	}
	n = spaced;

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
// If targetW/targetH are given (and differ from the pixmap's own size), the
// source is box-filtered down to that size instead of copied 1:1 -- each
// destination pixel averages the rectangle of source pixels that map onto
// it, so partially-covered/anti-aliased glyph edges from a higher-resolution
// source blend into smoother, denser-looking output instead of each output
// pixel only sampling a single (possibly barely-covered) source pixel.
HBITMAP pixmapToDIB(fz_context* ctx, fz_pixmap* pix, int& outW, int& outH, int targetW = 0, int targetH = 0)
{
	int w = fz_pixmap_width(ctx, pix);
	int h = fz_pixmap_height(ctx, pix);
	int n = fz_pixmap_components(ctx, pix);
	ptrdiff_t stride = fz_pixmap_stride(ctx, pix);
	const unsigned char* src = fz_pixmap_samples(ctx, pix);
	if (w <= 0 || h <= 0 || n < 3) return nullptr;

	int dstW = (targetW > 0) ? targetW : w;
	int dstH = (targetH > 0) ? targetH : h;

	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = dstW;
	bmi.bmiHeader.biHeight = -dstH; // negative => top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	void* bits = nullptr;
	HBITMAP hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!hbmp || !bits) { if (hbmp) DeleteObject(hbmp); return nullptr; }

	auto* dst = static_cast<unsigned char*>(bits);
	if (dstW == w && dstH == h) {
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
	} else {
		for (int dy = 0; dy < dstH; ++dy) {
			int sy0 = static_cast<int>(static_cast<int64_t>(dy) * h / dstH);
			int sy1 = static_cast<int>(static_cast<int64_t>(dy + 1) * h / dstH);
			if (sy1 <= sy0) sy1 = sy0 + 1;
			if (sy1 > h) sy1 = h;
			unsigned char* drow = dst + static_cast<ptrdiff_t>(dy) * dstW * 4;
			for (int dx = 0; dx < dstW; ++dx) {
				int sx0 = static_cast<int>(static_cast<int64_t>(dx) * w / dstW);
				int sx1 = static_cast<int>(static_cast<int64_t>(dx + 1) * w / dstW);
				if (sx1 <= sx0) sx1 = sx0 + 1;
				if (sx1 > w) sx1 = w;
				unsigned int sr = 0, sg = 0, sb = 0, count = 0;
				for (int sy = sy0; sy < sy1; ++sy) {
					const unsigned char* srow = src + static_cast<ptrdiff_t>(sy) * stride;
					for (int sx = sx0; sx < sx1; ++sx) {
						const unsigned char* sp = srow + static_cast<ptrdiff_t>(sx) * n;
						sr += sp[0]; sg += sp[1]; sb += sp[2];
						++count;
					}
				}
				if (count == 0) count = 1;
				unsigned char* dp = drow + static_cast<ptrdiff_t>(dx) * 4;
				dp[0] = static_cast<unsigned char>(sb / count); // B
				dp[1] = static_cast<unsigned char>(sg / count); // G
				dp[2] = static_cast<unsigned char>(sr / count); // R
				dp[3] = 255;                                    // A (opaque)
			}
		}
	}
	outW = dstW; outH = dstH;
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

	// Read the whole file into an in-memory buffer and open from that,
	// rather than fz_open_document(path) -- the latter keeps a FILE* open
	// via _wfopen for the document's entire lifetime, and the CRT's default
	// share mode doesn't include FILE_SHARE_DELETE, so Explorer can't
	// rename/move/cut-paste the file while it's open in a tab. Reading it
	// once up front means no handle lingers at all once open() returns.
	fz_try(ctx_) {
		fz_buffer* buf = fz_read_file(ctx_, upath.c_str());
		fz_try(ctx_) {
			doc_ = fz_open_document_with_buffer(ctx_, "application/pdf", buf);
		}
		fz_always(ctx_) { fz_drop_buffer(ctx_, buf); }
		fz_catch(ctx_) { fz_rethrow(ctx_); }
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

bool PdfDocument::reopenCurrentPath(std::string& err, const char* authPassword)
{
	if (!ctx_ || openedPath_.empty()) { err = "no path to reopen"; return false; }
	std::string upath = toUtf8(openedPath_.c_str());
	if (doc_) { fz_drop_document(ctx_, doc_); doc_ = nullptr; }
	// Same in-memory-buffer approach as open() -- see its comment. Keeps this
	// reopen-after-save step from re-pinning a share-mode-limited handle to
	// the file, which would undo the whole point of fixing open().
	fz_try(ctx_) {
		fz_buffer* buf = fz_read_file(ctx_, upath.c_str());
		fz_try(ctx_) {
			doc_ = fz_open_document_with_buffer(ctx_, "application/pdf", buf);
		}
		fz_always(ctx_) { fz_drop_buffer(ctx_, buf); }
		fz_catch(ctx_) { fz_rethrow(ctx_); }
	}
	fz_catch(ctx_) {
		doc_ = nullptr;
		err = fz_caught_message(ctx_);
		authed_ = false;
		return false;
	}
	if (authPassword && fz_needs_password(ctx_, doc_))
		fz_authenticate_password(ctx_, doc_, authPassword);
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
	fz_device* dev = nullptr;
	fz_try(ctx_) {
		page = fz_load_page(ctx_, doc_, index);

		// Target (final) pixel box -- exactly what a direct render at `scale`
		// produces (fz_new_pixmap_from_page uses this same round_rect, see
		// util.c) and the size the on-screen layout (pagePixelSize) expects.
		fz_irect targetBox = fz_round_rect(fz_transform_rect(fz_bound_page(ctx_, page), fz_scale(scale, scale)));
		int targetW = std::max(1, targetBox.x1 - targetBox.x0);
		int targetH = std::max(1, targetBox.y1 - targetBox.y0);

		if (scale < 1.5f) {
			// Below 150% zoom, rasterizing straight at `scale` leaves thin
			// glyph stems only partially covering a single output pixel --
			// grainy/washed-out next to Chrome, especially on bold text.
			// Supersample at 2x, then box-filter back down to the target size
			// (pixmapToDIB) for extra effective anti-aliasing.
			//
			// The supersampled pixmap MUST be EXACTLY 2x the target box in each
			// axis, so every output pixel averages a uniform 2x2 source block.
			// Letting MuPDF round a `scale*2` matrix into its own bbox gives a
			// non-integer size ratio, so different glyph stems land on 2- vs
			// 3-pixel averaging boxes -- which shows up as visibly uneven
			// per-letter boldness (a real bug this exact-2x construction fixes).
			// Otherwise this mirrors fz_new_pixmap_from_page (util.c) exactly:
			// the ctm goes to the draw device, the page runs with fz_identity,
			// opaque pixmap => white clear.
			fz_irect ssBox = { targetBox.x0 * 2, targetBox.y0 * 2, targetBox.x1 * 2, targetBox.y1 * 2 };
			fz_matrix ctm = fz_scale(scale * 2.0f, scale * 2.0f);
			pix = fz_new_pixmap_with_bbox(ctx_, fz_device_rgb(ctx_), ssBox, nullptr, 0);
			fz_clear_pixmap_with_value(ctx_, pix, 0xFF);
			dev = fz_new_draw_device(ctx_, ctm, pix);
			fz_run_page(ctx_, page, dev, fz_identity, nullptr);
			fz_close_device(ctx_, dev);
			int w = 0, h = 0;
			HBITMAP hbmp = pixmapToDIB(ctx_, pix, w, h, targetW, targetH);
			if (hbmp) { result.hbmp = hbmp; result.width = w; result.height = h; }
		} else {
			// At/above 150% zoom a glyph stem already spans several device
			// pixels, so supersampling wouldn't help -- render straight.
			fz_matrix ctm = fz_scale(scale, scale);
			pix = fz_new_pixmap_from_page(ctx_, page, ctm, fz_device_rgb(ctx_), 0);
			int w = 0, h = 0;
			HBITMAP hbmp = pixmapToDIB(ctx_, pix, w, h);
			if (hbmp) { result.hbmp = hbmp; result.width = w; result.height = h; }
		}
	}
	fz_always(ctx_) {
		if (dev) fz_drop_device(ctx_, dev);
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
	fullRewriteProbed_ = false;
	fullRewriteRisky_ = false;
	widgetBaseFontSize_.clear();
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

std::vector<LinkInfo> PdfDocument::pageLinks(int page)
{
	std::vector<LinkInfo> out;
	if (!ctx_ || !doc_ || !authed_) return out;
	if (page < 0 || page >= pageCount_) return out;

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	fz_page* pg = nullptr;
	fz_link* links = nullptr;
	fz_try(ctx_) {
		pg = fz_load_page(ctx_, doc_, page);
		links = fz_load_links(ctx_, pg);
		for (fz_link* l = links; l; l = l->next) {
			if (!l->uri || !*l->uri) continue;
			LinkInfo info;
			info.rect = { l->rect.x0, l->rect.y0, l->rect.x1, l->rect.y1 };
			info.uri = l->uri;
			info.external = fz_is_external_link(ctx_, l->uri) != 0;
			if (!info.external) {
				fz_location loc = fz_resolve_link(ctx_, doc_, l->uri, nullptr, nullptr);
				info.targetPage = fz_page_number_from_location(ctx_, doc_, loc);
			}
			out.push_back(std::move(info));
		}
	}
	fz_always(ctx_) {
		if (links) fz_drop_link(ctx_, links);
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

std::wstring toWide(const std::string& utf8)
{
	if (utf8.empty()) return {};
	int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	if (wn <= 0) return {};
	std::wstring w(static_cast<size_t>(wn - 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), wn);
	return w;
}

// Rough GDI-measured width, in PDF points, of `text` set at `sizePt`. Uses
// Arial as a metrically-close stand-in for Helvetica (MuPDF's "Helv" base-14
// alias, what the overwhelming majority of real-world form fields use) --
// this is a shrink-to-fit decision, not meant to be pixel-exact.
float measureTextWidthPt(const std::wstring& text, float sizePt)
{
	if (text.empty()) return 0.0f;
	constexpr int kRefDpi = 96;
	HDC dc = GetDC(nullptr);
	LOGFONTW lf = {};
	lf.lfHeight = -MulDiv(std::max(1, static_cast<int>(std::lround(sizePt))), kRefDpi, 72);
	lf.lfWeight = FW_NORMAL;
	lf.lfCharSet = DEFAULT_CHARSET;
	wcscpy_s(lf.lfFaceName, L"Arial");
	HFONT f = CreateFontIndirectW(&lf);
	HFONT old = static_cast<HFONT>(SelectObject(dc, f));
	SIZE sz{};
	GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
	SelectObject(dc, old);
	DeleteObject(f);
	ReleaseDC(nullptr, dc);
	return sz.cx * 72.0f / kRefDpi;
}

} // namespace

void PdfDocument::autoFitTextWidgetFontSize(pdf_annot* w, int page, int widgetIndex, const std::string& utf8)
{
	// MuPDF's own appearance generator already does a correct shrink-to-fit
	// for single-line fields whose DA font size is 0 ("auto", per spec) --
	// see write_variable_text() in pdf-appearance.c, which measures the text
	// with the real embedded font and solves for the size that exactly fills
	// the box width. Multiline/comb fields are a different story: multiline
	// auto-size always falls back to a fixed 12pt there (no fit computation
	// at all) and comb fields are fixed-width cells by design -- neither is
	// this bug (a FIXED nonzero DA size silently clipping the box's own
	// render-time clip rect), so leave both alone rather than fighting
	// MuPDF's own behavior for them.
	int ff = pdf_annot_field_flags(ctx_, w);
	if (ff & (PDF_TX_FIELD_IS_MULTILINE | PDF_TX_FIELD_IS_COMB)) return;

	const char* font = nullptr;
	float size = 0.0f, color[4] = {};
	int n = 0;
	pdf_annot_default_appearance(ctx_, w, &font, &size, &n, color);
	if (size <= 0) return; // already auto -- MuPDF fits this on its own

	// Fit FROM the field's size the first time this session touches it, not
	// from whatever it may already have been shrunk to by an earlier edit --
	// otherwise typing a long value then deleting it back down to something
	// short would leave the font permanently small.
	long long key = (static_cast<long long>(page) << 32) | static_cast<unsigned>(widgetIndex);
	auto it = widgetBaseFontSize_.find(key);
	float base = it != widgetBaseFontSize_.end() ? it->second : size;
	if (it == widgetBaseFontSize_.end()) widgetBaseFontSize_[key] = base;

	fz_rect r = pdf_bound_widget(ctx_, w);
	constexpr float kPad = 4.0f; // rough border/padding allowance
	float avail = std::max(1.0f, (r.x1 - r.x0) - kPad);

	std::wstring wtext = toWide(utf8);
	constexpr float kFloor = 4.0f;
	float fitted = base;
	while (fitted > kFloor && measureTextWidthPt(wtext, fitted) > avail)
		fitted -= 0.5f;

	if (fitted != size)
		pdf_set_annot_default_appearance(ctx_, w, font, fitted, n, color);
}

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
		if (w) {
			autoFitTextWidgetFontSize(w, page, widgetIndex, utf8);
			pdf_set_text_field_value(ctx_, w, utf8.c_str());
			pdf_update_annot(ctx_, w);
			ok = true;
		}
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
// the user's original file. `password`, if non-null, authenticates the
// freshly-opened document first -- needed when the write just encrypted the
// output (fz_count_pages/fz_load_page on an unauthenticated encrypted PDF
// otherwise fail, which would misreport a perfectly good encrypted write as
// a broken one).
bool verifyPdfFile(const std::string& utf8path, int expectedPageCount, const char* password = nullptr)
{
	fz_context* vctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
	if (!vctx) return false;
	installSystemFontHook(vctx);
	bool ok = false;
	fz_try(vctx) {
		fz_register_document_handlers(vctx);
		fz_document* vdoc = fz_open_document(vctx, utf8path.c_str());
		fz_try(vctx) {
			if (password && fz_needs_password(vctx, vdoc))
				fz_authenticate_password(vctx, vdoc, password);
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

// Some third-party PDF generators write an xref whose subsections leave a
// hole for one or more object numbers -- the number is never declared
// in-use OR free, in any generation of an incremental chain. A plain
// open/render never needs to resolve that number, so the file "opens fine",
// but MuPDF's full/garbage-collecting write path (do_garbage=1, used for
// every normal Save in this app -- see writeAndReplace()) walks every
// declared object number while marking/renumbering and hard-throws "cannot
// find object in xref (N 0 R)" on the hole. That renumbering mutates the
// document's xref table in place as it goes, so if this were probed against
// the LIVE, already-edited doc_/ctx_, a failed attempt could leave it
// half-renumbered and break a subsequent save attempt too (confirmed
// empirically). This runs the same probe against a disposable, freshly
// re-read copy of the file in its own throwaway fz_context instead --
// whatever happens to it can't affect the live document -- purely to answer
// "would a full rewrite of this file's structure choke on a missing
// object," independent of anything this session has edited.
bool fullRewriteChokesOnMissingObject(const std::wstring& wpath)
{
	std::string upath = toUtf8(wpath.c_str());
	std::wstring wprobe = wpath + L".pdfviewer_probe_tmp";
	std::string uprobe = toUtf8(wprobe.c_str());

	fz_context* pctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
	if (!pctx) return false;
	installSystemFontHook(pctx);
	bool chokes = false;
	fz_try(pctx) {
		fz_register_document_handlers(pctx);
		fz_document* pdoc = fz_open_document(pctx, upath.c_str());
		fz_try(pctx) {
			if (!fz_needs_password(pctx, pdoc)) {
				pdf_document* ppdf = pdf_document_from_fz_document(pctx, pdoc);
				if (ppdf) {
					fz_try(pctx) {
						pdf_write_options opts = pdf_default_write_options;
						opts.do_garbage = 1;
						opts.do_compress = 1;
						opts.do_compress_images = 1;
						opts.do_compress_fonts = 1;
						opts.do_use_objstms = 1;
						opts.compression_effort = 100;
						pdf_save_document(pctx, ppdf, uprobe.c_str(), &opts);
					}
					fz_catch(pctx) {
						const char* msg = fz_caught_message(pctx);
						chokes = msg && std::strstr(msg, "cannot find object in xref") != nullptr;
					}
				}
			}
		}
		fz_always(pctx) { fz_drop_document(pctx, pdoc); }
		fz_catch(pctx) { /* open/needs-password failure -- not this bug, leave chokes=false */ }
	}
	fz_catch(pctx) {
		/* leave chokes=false */
	}
	fz_drop_context(pctx);
	DeleteFileW(wprobe.c_str());
	return chokes;
}

} // namespace

bool PdfDocument::probeFullRewriteRisky()
{
	if (!fullRewriteProbed_) {
		fullRewriteRisky_ = !openedPath_.empty() && fullRewriteChokesOnMissingObject(openedPath_);
		fullRewriteProbed_ = true;
	}
	return fullRewriteRisky_;
}

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

bool PdfDocument::setPassword(const wchar_t* path, const char* password, std::string& err)
{
	if (!ctx_ || !doc_) { err = "no document"; return false; }
	if (!password || !password[0]) { err = "password is empty"; return false; }
	// writeAndReplace() passes `password` through to its internal
	// reopenCurrentPath() so the live document gets authenticated as part of
	// the reopen -- see reopenCurrentPath()'s comment for why that has to
	// happen before loadInfo() runs, not after.
	bool ok = writeAndReplace(path, /*incremental=*/false, /*stripEncryption=*/false, err, password);
	if (ok) neededPassword_ = true; // reopening this file will now require a password
	return ok;
}

bool PdfDocument::writeAndReplace(const wchar_t* path, bool incremental, bool stripEncryption, std::string& err,
	const char* newPassword)
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

	// Some third-party PDF generators write an xref whose subsections leave
	// a hole for one or more object numbers -- never declared in-use OR
	// free, in any generation of the incremental chain. A plain render never
	// needs to resolve that number, but a full/garbage-collecting rewrite
	// (do_garbage=1, this app's normal Save) walks every declared object
	// number while marking/renumbering and hard-throws "cannot find object
	// in xref (N 0 R)" on it. That renumbering mutates the xref table in
	// place as it walks, so if the full write is even ATTEMPTED against the
	// live doc_ and then fails partway through, doc_ is left half-renumbered
	// and a subsequent save attempt (even a plain incremental one) fails too
	// -- confirmed empirically, so the full path must never be attempted
	// live once it's known to be broken for this file. probeFullRewriteRisky()
	// answers that question safely on a disposable copy first. When it's
	// risky, skip straight to an INCREMENTAL write seeded with a copy of the
	// original bytes (do_garbage/compress require do_incremental=0, so this
	// sidesteps the hole entirely: no mark/sweep pass, only the objects
	// actually touched this session get serialized) -- this preserves every
	// pending edit, unlike pdf_repair_xref() (which would also clear the
	// hole, but calls pdf_forget_xref() internally and unconditionally
	// discards the in-memory incremental section, i.e. every unsaved edit).
	// Only applies to the plain edit-save case -- changing encryption always
	// requires a full rewrite by construction (see the checks inside
	// pdf_save_document itself), so this fallback doesn't apply there.
	bool useIncremental = incremental;
	if (!useIncremental && !stripEncryption && !newPassword && probeFullRewriteRisky()) {
		useIncremental = true;
	}

	bool wrote = false;
	if (useIncremental && !incremental) {
		// Forced into incremental by the probe above (the caller asked for a
		// normal full save): seed the temp file with the CURRENTLY OPEN
		// file's bytes first, since an incremental write only appends a new
		// xref generation after whatever's already at the output path --
		// and that must be openedPath_ (what doc_ actually is), not
		// `wtarget`. They're the same file for a plain in-place Save, but
		// differ for Save As, where wtarget is a brand-new path that has no
		// bytes yet.
		if (openedPath_.empty() || !CopyFileW(openedPath_.c_str(), wtemp.c_str(), FALSE)) {
			err = "could not prepare incremental fallback save";
			DeleteFileW(wtemp.c_str());
			return false;
		}
	}
	fz_try(ctx_) {
		pdf_write_options opts = pdf_default_write_options;
		if (useIncremental) {
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
		if (newPassword) {
			opts.do_encrypt = PDF_ENCRYPT_AES_256;
			opts.permissions = ~0; // full access once opened with this password
			strncpy_s(opts.opwd_utf8, sizeof(opts.opwd_utf8), newPassword, _TRUNCATE);
			strncpy_s(opts.upwd_utf8, sizeof(opts.upwd_utf8), newPassword, _TRUNCATE);
		}
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

	if (!verifyPdfFile(utemp, pageCount_, newPassword)) {
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
	if (!reopenCurrentPath(reopenErr, newPassword)) {
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

// --- Convert-to-PDF (images / text / markdown) ------------------------------
namespace {

enum class ConvertKind { Image, Text, Markdown, Docx, Pdf, Unsupported };

ConvertKind classifyForConvert(const std::wstring& path)
{
	size_t dot = path.find_last_of(L'.');
	if (dot == std::wstring::npos) return ConvertKind::Unsupported;
	std::wstring ext = path.substr(dot + 1);
	for (auto& c : ext) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
	static const wchar_t* kImageExts[] = { L"jpg", L"jpeg", L"png", L"bmp", L"gif", L"tif", L"tiff" };
	for (auto* e : kImageExts) if (ext == e) return ConvertKind::Image;
	if (ext == L"txt") return ConvertKind::Text;
	if (ext == L"md" || ext == L"markdown") return ConvertKind::Markdown;
	if (ext == L"docx") return ConvertKind::Docx;
	if (ext == L"pdf") return ConvertKind::Pdf; // lets a plain PDF's pages be folded into the combined output too
	return ConvertKind::Unsupported;
}

// Grafts every page of `src` (a plain PDF, or the one Word just produced
// from a .docx) onto the end of `dst`, same graftPageWithAnnots() used by
// Merge/Organize. `src` is opened and dropped here -- caller doesn't keep it.
bool graftAllPagesFrom(fz_context* ctx, pdf_document* dst, const char* srcUtf8Path)
{
	fz_document* srcDoc = nullptr;
	bool ok = false;
	fz_try(ctx) {
		srcDoc = fz_open_document(ctx, srcUtf8Path);
		if (fz_needs_password(ctx, srcDoc)) fz_throw(ctx, FZ_ERROR_GENERIC, "password-protected");
		pdf_document* srcPdf = pdf_document_from_fz_document(ctx, srcDoc);
		if (!srcPdf) fz_throw(ctx, FZ_ERROR_GENERIC, "not a PDF");
		int n = fz_count_pages(ctx, srcDoc);
		pdf_graft_map* map = pdf_new_graft_map(ctx, dst);
		fz_try(ctx) {
			for (int i = 0; i < n; ++i) graftPageWithAnnots(ctx, map, dst, -1, srcPdf, i, 0);
		}
		fz_always(ctx) { pdf_drop_graft_map(ctx, map); }
		fz_catch(ctx) { fz_rethrow(ctx); }
		ok = true;
	}
	fz_always(ctx) { if (srcDoc) fz_drop_document(ctx, srcDoc); }
	fz_catch(ctx) { ok = false; }
	return ok;
}

// Reads a text file into UTF-16, handling the common Windows text encodings:
// UTF-16LE (Notepad's default, BOM FF FE), UTF-8 with or without a BOM, and
// legacy ANSI (system codepage) as a last-resort fallback for older files.
std::wstring readTextFileWide(const wchar_t* path)
{
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return {};
	LARGE_INTEGER sz{};
	GetFileSizeEx(h, &sz);
	std::vector<char> buf(static_cast<size_t>(sz.QuadPart));
	DWORD readN = 0;
	if (!buf.empty()) ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &readN, nullptr);
	CloseHandle(h);
	buf.resize(readN);

	if (buf.size() >= 2 && static_cast<unsigned char>(buf[0]) == 0xFF && static_cast<unsigned char>(buf[1]) == 0xFE) {
		size_t n = (buf.size() - 2) / 2;
		std::wstring w(n, L'\0');
		std::memcpy(w.data(), buf.data() + 2, n * 2);
		return w;
	}
	size_t bomOffset = (buf.size() >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
		static_cast<unsigned char>(buf[1]) == 0xBB && static_cast<unsigned char>(buf[2]) == 0xBF) ? 3 : 0;
	const char* textStart = buf.data() + bomOffset;
	int textLen = static_cast<int>(buf.size() - bomOffset);
	int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, textStart, textLen, nullptr, 0);
	if (wn > 0) {
		std::wstring w(static_cast<size_t>(wn), L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, textStart, textLen, w.data(), wn);
		return w;
	}
	wn = MultiByteToWideChar(CP_ACP, 0, textStart, textLen, nullptr, 0); // not valid UTF-8 -- assume legacy ANSI
	if (wn <= 0) return {};
	std::wstring w(static_cast<size_t>(wn), L'\0');
	MultiByteToWideChar(CP_ACP, 0, textStart, textLen, w.data(), wn);
	return w;
}

// Splits on '\n' (CRLF-safe: a trailing '\r' is dropped), tabs expanded to 4
// spaces since the raw content stream below has no tab-stop math.
std::vector<std::wstring> splitLinesExpandTabs(const std::wstring& text)
{
	std::vector<std::wstring> lines;
	std::wstring cur;
	for (wchar_t c : text) {
		if (c == L'\n') { lines.push_back(cur); cur.clear(); }
		else if (c == L'\r') { /* CRLF: the '\n' above already ends the line */ }
		else if (c == L'\t') cur += L"    ";
		else cur += c;
	}
	lines.push_back(cur);
	return lines;
}

// Converts a wide line to Windows-1252 bytes for a PDF simple-font content
// stream and escapes '(' ')' '\' for a PDF literal string. Characters
// outside Windows-1252 become '?' -- see ConvertFilesToPdf's header comment
// for why there's no embedded Unicode font to avoid this.
std::string toPdfLiteralWinAnsi(const std::wstring& line)
{
	std::string bytes;
	if (!line.empty()) {
		int n = WideCharToMultiByte(1252, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
		if (n > 0) {
			bytes.resize(static_cast<size_t>(n));
			WideCharToMultiByte(1252, 0, line.data(), static_cast<int>(line.size()), bytes.data(), n, "?", nullptr);
		}
	}
	std::string escaped;
	escaped.reserve(bytes.size() + 8);
	for (char c : bytes) {
		if (c == '(' || c == ')' || c == '\\') escaped += '\\';
		escaped += c;
	}
	return escaped;
}

// Greedy word-wrap for a monospace font -- `maxChars` is exact since every
// glyph has the same advance width, no per-character measurement needed.
std::vector<std::wstring> wrapMonospace(const std::wstring& line, int maxChars)
{
	std::vector<std::wstring> out;
	if (line.empty()) { out.push_back(L""); return out; }
	if (maxChars <= 0) { out.push_back(line); return out; }
	size_t pos = 0;
	while (pos < line.size()) {
		size_t remaining = line.size() - pos;
		if (remaining <= static_cast<size_t>(maxChars)) { out.push_back(line.substr(pos)); break; }
		size_t breakAt = pos + maxChars;
		size_t lastSpace = line.find_last_of(L' ', breakAt);
		if (lastSpace != std::wstring::npos && lastSpace > pos) {
			out.push_back(line.substr(pos, lastSpace - pos));
			pos = lastSpace + 1;
		} else {
			out.push_back(line.substr(pos, static_cast<size_t>(maxChars))); // one long "word" -- hard break
			pos += static_cast<size_t>(maxChars);
		}
	}
	return out;
}

// Accumulates wrapped lines onto A4 pages, paginating automatically. Each
// finished page gets its own /Contents stream (re-issuing BT/Tf per line
// rather than tracking a running text-matrix offset -- simpler and safer to
// get right than cumulative Td math, and the byte overhead is irrelevant at
// the sizes a text/markdown file produces).
struct ConvertPageBuilder {
	fz_context* ctx;
	pdf_document* pdf;
	pdf_obj* fontRegular;
	pdf_obj* fontBold;
	float pageW, pageH, marginX, marginTop, marginBottom;
	float y = 0;
	fz_buffer* content = nullptr;
	bool pageOpen = false;

	void finishPage()
	{
		if (!pageOpen) return;
		pdf_obj* res = pdf_new_dict(ctx, pdf, 1);
		pdf_obj* fontDict = pdf_dict_put_dict(ctx, res, PDF_NAME(Font), 2);
		pdf_dict_puts(ctx, fontDict, "F0", fontRegular);
		pdf_dict_puts(ctx, fontDict, "F1", fontBold);
		pdf_obj* page = pdf_add_page(ctx, pdf, fz_make_rect(0, 0, pageW, pageH), 0, res, content);
		pdf_drop_obj(ctx, res);
		pdf_insert_page(ctx, pdf, -1, page);
		pdf_drop_obj(ctx, page);
		fz_drop_buffer(ctx, content);
		content = nullptr;
		pageOpen = false;
	}

	void newPage()
	{
		finishPage();
		content = fz_new_buffer(ctx, 4096);
		y = pageH - marginTop;
		pageOpen = true;
	}

	void emitLine(const std::wstring& text, bool bold, float size, float extraIndent)
	{
		float lineHeight = size * 1.35f;
		if (!pageOpen || y - lineHeight < marginBottom) newPage();
		std::string lit = toPdfLiteralWinAnsi(text);
		fz_append_printf(ctx, content, "BT /%s %g Tf %g %g Td (%s) Tj ET\n",
			bold ? "F1" : "F0", size, marginX + extraIndent, y - size, lit.c_str());
		y -= lineHeight;
	}
};

} // namespace

bool PdfDocument::ConvertFilesToPdf(const std::vector<std::wstring>& paths, const wchar_t* outPath,
	std::string& err, std::vector<std::wstring>* skipped)
{
	if (paths.empty()) { err = "no files selected"; return false; }

	fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
	if (!ctx) { err = "MuPDF context not initialized"; return false; }
	installSystemFontHook(ctx);
	fz_try(ctx) { fz_register_document_handlers(ctx); }
	fz_catch(ctx) { /* best-effort, mirrors PdfDocument's own constructor */ }

	pdf_document* pdf = nullptr;
	fz_try(ctx) { pdf = pdf_create_document(ctx); }
	fz_catch(ctx) {
		err = fz_caught_message(ctx);
		fz_drop_context(ctx);
		return false;
	}

	constexpr float kPageW = 595.0f, kPageH = 842.0f; // A4, matches resizeToA4()'s own target size
	constexpr float kMargin = 54.0f; // 0.75in
	constexpr float kBodySize = 11.0f;

	fz_font* fRegular = nullptr;
	fz_font* fBold = nullptr;
	pdf_obj* fontRegular = nullptr;
	pdf_obj* fontBold = nullptr;
	fz_try(ctx) {
		fRegular = fz_new_base14_font(ctx, "Courier");
		fontRegular = pdf_add_simple_font(ctx, pdf, fRegular, PDF_SIMPLE_ENCODING_LATIN);
		fBold = fz_new_base14_font(ctx, "Courier-Bold");
		fontBold = pdf_add_simple_font(ctx, pdf, fBold, PDF_SIMPLE_ENCODING_LATIN);
	}
	fz_always(ctx) {
		if (fRegular) fz_drop_font(ctx, fRegular);
		if (fBold) fz_drop_font(ctx, fBold);
	}
	fz_catch(ctx) {
		err = fz_caught_message(ctx);
		pdf_drop_document(ctx, pdf);
		fz_drop_context(ctx);
		return false;
	}

	ConvertPageBuilder pb{ ctx, pdf, fontRegular, fontBold, kPageW, kPageH, kMargin, kMargin, kMargin };
	bool anyOk = false;

	for (const auto& path : paths) {
		ConvertKind kind = classifyForConvert(path);
		std::string upath = toUtf8(path.c_str());
		if (kind == ConvertKind::Image) {
			fz_image* img = nullptr;
			fz_try(ctx) {
				img = fz_new_image_from_file(ctx, upath.c_str());
				int xres = 0, yres = 0;
				fz_image_resolution(img, &xres, &yres);
				if (xres <= 0) xres = 96;
				if (yres <= 0) yres = 96;
				float w = img->w * 72.0f / static_cast<float>(xres);
				float h = img->h * 72.0f / static_cast<float>(yres);
				pdf_obj* imgRef = pdf_add_image(ctx, pdf, img);
				pdf_obj* res = pdf_new_dict(ctx, pdf, 1);
				pdf_obj* xobj = pdf_dict_put_dict(ctx, res, PDF_NAME(XObject), 1);
				pdf_dict_puts(ctx, xobj, "Im0", imgRef);
				pdf_drop_obj(ctx, imgRef);
				fz_buffer* content = fz_new_buffer(ctx, 128);
				fz_append_printf(ctx, content, "q %g 0 0 %g 0 0 cm /Im0 Do Q", w, h);
				pdf_obj* page = pdf_add_page(ctx, pdf, fz_make_rect(0, 0, w, h), 0, res, content);
				fz_drop_buffer(ctx, content);
				pdf_drop_obj(ctx, res);
				pdf_insert_page(ctx, pdf, -1, page);
				pdf_drop_obj(ctx, page);
				anyOk = true;
			}
			fz_always(ctx) { if (img) fz_drop_image(ctx, img); }
			fz_catch(ctx) { if (skipped) skipped->push_back(path); }
		} else if (kind == ConvertKind::Text || kind == ConvertKind::Markdown) {
			bool markdown = (kind == ConvertKind::Markdown);
			std::wstring text = readTextFileWide(path.c_str());
			fz_try(ctx) {
				pb.newPage();
				for (auto& raw : splitLinesExpandTabs(text)) {
					bool bold = false; float size = kBodySize; float indent = 0; std::wstring line = raw;
					if (markdown) {
						if (line.rfind(L"### ", 0) == 0) { bold = true; size = 13; line = line.substr(4); }
						else if (line.rfind(L"## ", 0) == 0) { bold = true; size = 14; line = line.substr(3); }
						else if (line.rfind(L"# ", 0) == 0) { bold = true; size = 16; line = line.substr(2); }
						else if (line.rfind(L"- ", 0) == 0 || line.rfind(L"* ", 0) == 0) {
							line = L"- " + line.substr(2); // plain hyphen: base-14 Courier's bullet glyph at cp1252 0x95 renders as .notdef
							indent = 12;
						}
					}
					int lineMaxChars = std::max(1, static_cast<int>((kPageW - 2 * kMargin - indent) / (size * 0.6f)));
					for (auto& wrapped : wrapMonospace(line, lineMaxChars))
						pb.emitLine(wrapped, bold, size, indent);
				}
				anyOk = true;
			}
			fz_catch(ctx) { if (skipped) skipped->push_back(path); }
		} else if (kind == ConvertKind::Pdf) {
			pb.finishPage(); // keep page order = file order: close any open text page first
			if (graftAllPagesFrom(ctx, pdf, upath.c_str())) anyOk = true;
			else if (skipped) skipped->push_back(path);
		} else if (kind == ConvertKind::Docx) {
			pb.finishPage();
			wchar_t tempPdf[MAX_PATH];
			wchar_t tempDir[MAX_PATH];
			GetTempPathW(MAX_PATH, tempDir);
			GetTempFileNameW(tempDir, L"pfd", 0, tempPdf); // creates a 0-byte placeholder; Word overwrites it
			std::string wordErr;
			bool converted = ConvertDocxToPdf(path.c_str(), tempPdf, wordErr);
			if (converted) {
				std::string tempUtf8 = toUtf8(tempPdf);
				if (graftAllPagesFrom(ctx, pdf, tempUtf8.c_str())) anyOk = true;
				else if (skipped) skipped->push_back(path);
			} else if (skipped) {
				skipped->push_back(path);
			}
			DeleteFileW(tempPdf);
		} else {
			if (skipped) skipped->push_back(path);
		}
	}
	pb.finishPage();

	if (!anyOk) {
		err = "no files could be converted";
		pdf_drop_document(ctx, pdf);
		fz_drop_context(ctx);
		return false;
	}

	// Same write-to-temp-verify-move discipline as exportPages() -- this
	// isn't a live document, so no in-memory state to worry about either way.
	std::wstring wtarget(outPath);
	std::wstring wtemp = wtarget + L".pdfviewer_tmp";
	std::string utemp = toUtf8(wtemp.c_str());
	int pageCount = 0;
	fz_try(ctx) { pageCount = pdf_count_pages(ctx, pdf); }
	fz_catch(ctx) { pageCount = 0; }

	bool wrote = false;
	fz_try(ctx) {
		pdf_write_options opts = pdf_default_write_options;
		opts.do_garbage = 1;
		opts.do_compress = 1;
		opts.do_compress_images = 1;
		opts.do_compress_fonts = 1;
		pdf_save_document(ctx, pdf, utemp.c_str(), &opts);
		wrote = true;
	}
	fz_catch(ctx) { err = fz_caught_message(ctx); }
	pdf_drop_document(ctx, pdf);
	if (!wrote) {
		DeleteFileW(wtemp.c_str());
		fz_drop_context(ctx);
		return false;
	}
	if (!verifyPdfFile(utemp, pageCount)) {
		err = "conversion produced an invalid file";
		DeleteFileW(wtemp.c_str());
		fz_drop_context(ctx);
		return false;
	}
	fz_drop_context(ctx);
	if (!MoveFileExW(wtemp.c_str(), wtarget.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		err = "could not write the output file (it may be open elsewhere)";
		DeleteFileW(wtemp.c_str());
		return false;
	}
	return true;
}

bool PdfDocument::ZipFiles(const std::vector<std::wstring>& paths, const wchar_t* outPath,
	std::string& err, std::vector<std::wstring>* skipped)
{
	if (paths.empty()) { err = "no files selected"; return false; }

	fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
	if (!ctx) { err = "MuPDF context not initialized"; return false; }

	// Same write-to-temp-then-rename discipline as ConvertFilesToPdf() --
	// a half-written zip never becomes the visible output file.
	std::wstring wtarget(outPath);
	std::wstring wtemp = wtarget + L".pdfviewer_tmp";
	std::string utemp = toUtf8(wtemp.c_str());

	fz_zip_writer* zip = nullptr;
	fz_try(ctx) { zip = fz_new_zip_writer(ctx, utemp.c_str()); }
	fz_catch(ctx) {
		err = fz_caught_message(ctx);
		fz_drop_context(ctx);
		return false;
	}

	// Zip entry names are just the source files' own base names, deduped
	// with a " (2)", " (3)", ... suffix on a collision (case-insensitive,
	// matching Windows' own filename semantics) -- e.g. two tabs open from
	// different folders that happen to share a file name.
	std::unordered_map<std::wstring, int> nameCounts;
	auto entryNameFor = [&](const std::wstring& path) {
		size_t slash = path.find_last_of(L"/\\");
		std::wstring base = slash == std::wstring::npos ? path : path.substr(slash + 1);
		std::wstring key = base;
		std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return std::towlower(c); });
		int n = ++nameCounts[key];
		if (n == 1) return base;
		size_t dot = base.find_last_of(L'.');
		std::wstring stem = dot == std::wstring::npos ? base : base.substr(0, dot);
		std::wstring ext = dot == std::wstring::npos ? std::wstring() : base.substr(dot);
		wchar_t suffix[16];
		swprintf(suffix, 16, L" (%d)", n);
		return stem + suffix + ext;
	};

	bool anyOk = false;
	for (const auto& path : paths) {
		std::string upath = toUtf8(path.c_str());
		std::string entryName = toUtf8(entryNameFor(path).c_str());
		fz_buffer* buf = nullptr;
		fz_try(ctx) {
			buf = fz_read_file(ctx, upath.c_str());
			fz_write_zip_entry(ctx, zip, entryName.c_str(), buf, /*compress=*/1);
			anyOk = true;
		}
		fz_always(ctx) { if (buf) fz_drop_buffer(ctx, buf); }
		fz_catch(ctx) { if (skipped) skipped->push_back(path); }
	}

	if (!anyOk) {
		err = "no files could be added";
		fz_drop_zip_writer(ctx, zip);
		fz_drop_context(ctx);
		DeleteFileW(wtemp.c_str());
		return false;
	}

	bool wrote = false;
	fz_try(ctx) { fz_close_zip_writer(ctx, zip); wrote = true; }
	fz_always(ctx) { fz_drop_zip_writer(ctx, zip); }
	fz_catch(ctx) { err = fz_caught_message(ctx); }
	fz_drop_context(ctx);
	if (!wrote) {
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

bool PdfDocument::flattenAnnotationsToContent(std::string& err)
{
	pdf_document* pdf = ctx_ && doc_ ? pdf_document_from_fz_document(ctx_, doc_) : nullptr;
	if (!pdf) { err = "not a PDF"; return false; }
	std::lock_guard<std::recursive_mutex> lock(mutex_);

	fz_try(ctx_) {
		int n = fz_count_pages(ctx_, doc_);
		for (int i = 0; i < n; ++i) {
			pdf_page* pg = pdf_load_page(ctx_, pdf, i);
			fz_try(ctx_) {
				pdf_obj* page_ref = pdf_lookup_page_obj(ctx_, pdf, i);
				fz_buffer* extra = fz_new_buffer(ctx_, 256);
				fz_try(ctx_) {
					pdf_obj* xobjDict = nullptr;
					int counter = 0;
					// pdf_annot_transform() is the exact placement matrix MuPDF's
					// own renderer uses to draw an annotation's appearance stream
					// (see pdf_process_annot in pdf-interpret.c: "q <matrix> cm
					// /AP Do Q") -- reusing it here means a baked annotation lands
					// pixel-for-pixel where it used to render live.
					auto bake = [&](pdf_annot* a) {
						enum pdf_annot_type type = pdf_annot_type(ctx_, a);
						if (type == PDF_ANNOT_LINK || type == PDF_ANNOT_POPUP) return;
						pdf_obj* ap = pdf_annot_ap(ctx_, a);
						if (!ap) return; // no visible appearance (e.g. a pending redaction mark, or an unfilled field) -- nothing to bake
						if (!xobjDict) {
							pdf_obj* res = pdf_dict_get(ctx_, page_ref, PDF_NAME(Resources));
							if (!res) res = pdf_dict_put_dict(ctx_, page_ref, PDF_NAME(Resources), 2);
							xobjDict = pdf_dict_get(ctx_, res, PDF_NAME(XObject));
							if (!xobjDict) xobjDict = pdf_dict_put_dict(ctx_, res, PDF_NAME(XObject), 4);
						}
						fz_matrix m = pdf_annot_transform(ctx_, a);
						char name[32];
						snprintf(name, sizeof(name), "FlatAnnot%d", counter++);
						pdf_dict_puts(ctx_, xobjDict, name, ap);
						fz_append_printf(ctx_, extra, "q %g %g %g %g %g %g cm /%s Do Q\n",
							m.a, m.b, m.c, m.d, m.e, m.f, name);
					};
					// page->annots (markup: FreeText/Highlight/Ink/...) and
					// page->widgets (AcroForm fields: text/checkbox/radio/...)
					// are two SEPARATE linked lists in MuPDF -- pdf_first_annot
					// only walks the former, so form fields need their own walk
					// via pdf_first_widget or their filled-in values silently
					// never get baked in (confirmed bug: text box annotations
					// flattened fine, but fillable form field text didn't).
					for (pdf_annot* a = pdf_first_annot(ctx_, pg); a; a = pdf_next_annot(ctx_, a)) bake(a);
					for (pdf_annot* w = pdf_first_widget(ctx_, pg); w; w = pdf_next_widget(ctx_, w)) bake(w);
					if (counter > 0) {
						// Append a new content stream rather than replacing the
						// existing one(s) -- the page's original text/vector
						// content is untouched and stays selectable/searchable.
						pdf_obj* contentsRef = pdf_add_stream(ctx_, pdf, extra, nullptr, 0);
						pdf_obj* oldContents = pdf_dict_get(ctx_, page_ref, PDF_NAME(Contents));
						pdf_obj* newContents;
						if (pdf_is_array(ctx_, oldContents)) {
							newContents = pdf_copy_array(ctx_, oldContents);
							pdf_array_push(ctx_, newContents, contentsRef);
						} else {
							newContents = pdf_new_array(ctx_, pdf, 2);
							if (oldContents) pdf_array_push(ctx_, newContents, oldContents);
							pdf_array_push(ctx_, newContents, contentsRef);
						}
						pdf_drop_obj(ctx_, contentsRef);
						pdf_dict_put_drop(ctx_, page_ref, PDF_NAME(Contents), newContents);

						// Now that every appearance is baked into page content,
						// the live annotation objects themselves are just dead
						// weight -- delete them. Restart the walk after each
						// delete rather than trying to safely advance past a
						// just-freed node (same pattern as
						// clearPendingRedactions); per-page annotation counts
						// are always small so the O(n^2) worst case is fine.
						auto shouldDelete = [&](pdf_annot* a) {
							enum pdf_annot_type type = pdf_annot_type(ctx_, a);
							if (type == PDF_ANNOT_LINK || type == PDF_ANNOT_POPUP) return false;
							return pdf_annot_ap(ctx_, a) != nullptr;
						};
						bool foundOne = true;
						while (foundOne) {
							foundOne = false;
							for (pdf_annot* a = pdf_first_annot(ctx_, pg); a; a = pdf_next_annot(ctx_, a)) {
								if (!shouldDelete(a)) continue;
								pdf_delete_annot(ctx_, pg, a);
								foundOne = true;
								break;
							}
						}
						foundOne = true;
						while (foundOne) {
							foundOne = false;
							for (pdf_annot* w = pdf_first_widget(ctx_, pg); w; w = pdf_next_widget(ctx_, w)) {
								if (!shouldDelete(w)) continue;
								pdf_delete_annot(ctx_, pg, w);
								foundOne = true;
								break;
							}
						}
					}
				}
				fz_always(ctx_) { fz_drop_buffer(ctx_, extra); }
				fz_catch(ctx_) { fz_rethrow(ctx_); }
			}
			fz_always(ctx_) { fz_drop_page(ctx_, reinterpret_cast<fz_page*>(pg)); }
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
