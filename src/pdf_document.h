// RAII wrapper around a MuPDF context + document, plus helpers to render
// a page into a Windows 32-bit top-down BGRA DIB section (ready to BitBlt).
#pragma once

#include <windows.h>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
}

struct PageSizePt { float w = 0.0f; float h = 0.0f; }; // page size in points (1/72")

// An axis-aligned rectangle in a page's coordinate space (points).
struct PageRectPt { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

// A point in a page's coordinate space (points).
struct PagePointF { float x = 0, y = 0; };

// Interactive form-field kinds we handle.
enum class WidgetKind { None, Text, Checkbox, Radio, Combo, ListBox, Button, Signature };

// Information about a form widget found under a point.
struct WidgetInfo {
	int index = -1;                    // widget index on the page, -1 if none
	WidgetKind kind = WidgetKind::None;
	PageRectPt rect;                   // page-space bounds
	std::string value;                 // current value (UTF-8)
	std::vector<std::string> options;  // choices for combo/list boxes
	bool multiline = false;            // text fields only
};

// One character of a page's extracted text, in reading order.
struct PageChar {
	PageRectPt quad;
	int unicode = 0;
	bool lineBreakAfter = false;  // last char of its line
	bool paragraphBreakAfter = false; // last char of its block
};

// An existing markup annotation (free text, highlight, ink, ...) found under
// a point, for click-to-edit / delete support.
enum class AnnotKind { None, FreeText, Highlight, Ink, Other };
struct AnnotInfo {
	int index = -1;    // stable index into this page's pdf_first_annot/next_annot enumeration
	AnnotKind kind = AnnotKind::None;
	PageRectPt rect;
	std::string contents;    // UTF-8, FreeText only
	unsigned long color = 0; // 0xRRGGBB
	float fontSize = 12.0f;  // FreeText only
	std::string font;        // FreeText only, e.g. "Helv"
};

// A rendered page bitmap. Owns the HBITMAP; deletes it on destruction.
struct PageBitmap {
	HBITMAP hbmp = nullptr;
	int width = 0;   // device pixels
	int height = 0;
	PageBitmap() = default;
	~PageBitmap() { if (hbmp) DeleteObject(hbmp); }
	PageBitmap(const PageBitmap&) = delete;
	PageBitmap& operator=(const PageBitmap&) = delete;
	PageBitmap(PageBitmap&& o) noexcept { *this = std::move(o); }
	PageBitmap& operator=(PageBitmap&& o) noexcept {
		if (this != &o) {
			if (hbmp) DeleteObject(hbmp);
			hbmp = o.hbmp; width = o.width; height = o.height;
			o.hbmp = nullptr; o.width = o.height = 0;
		}
		return *this;
	}
};

class PdfDocument {
public:
	PdfDocument();
	~PdfDocument();

	// Opens the document. Returns false and fills `error` on failure.
	// If the file needs a password, sets needsPassword=true and returns false;
	// call authenticate() then retry loadInfo via reopen (handled by open()).
	bool open(const wchar_t* path, std::string& error, bool& needsPassword);
	// Supply a password for an encrypted document opened but not yet authed.
	bool authenticate(const char* password);
	// True if this document needed a password to open (sticky from open()).
	bool neededPassword() const { return neededPassword_; }
	// True if the PDF currently has any encryption/permission restrictions --
	// requires a password to open, and/or an owner password restricts
	// copy/print/etc even though it opened without one. False for non-PDF docs.
	bool isEncrypted() const;
	// Rewrites the PDF with all encryption removed: no password required
	// afterward and full permissions restored. Same atomic write-verify-
	// replace-reopen safety as save(). There's no way to keep an open-
	// password requirement while only clearing owner restrictions without
	// knowing the original owner password, so this one operation backs both
	// "remove password" and "remove restrictions" in the UI.
	bool removeProtection(const wchar_t* path, std::string& err);
	// Finalize: read page count and sizes. Call after a successful open/auth.
	void loadInfo();
	void close();

	bool isOpen() const { return doc_ != nullptr && authed_; }
	int pageCount() const { return pageCount_; }
	PageSizePt pageSize(int index) const;
	// Page bounding box origin/extent in page space; needed to map search
	// hit coordinates onto rendered pixels the same way renderPage() does.
	PageRectPt pageBound(int index) const;

	// Finds `needle` (UTF-8) on page `index`; returns each match's bounding
	// rectangle in page-space points. Empty needle or no matches => empty.
	std::vector<PageRectPt> searchPage(int index, const char* needle, int maxHits = 500);

	// --- Editing (PDF only) ---------------------------------------------
	bool isPdf() const { return isPdf_; }
	bool dirty() const { return dirty_; }
	void markClean() { dirty_ = false; }

	// Character quads (page space) that fall within `rect` on `page`; used to
	// turn a drag rectangle into text-hugging highlight quads.
	std::vector<PageRectPt> textQuadsInRect(int page, PageRectPt rect);

	// All characters of a page in reading order, for text selection + copy.
	std::vector<PageChar> pageChars(int page);

	// Existing markup annotations (not form widgets) on a page / under a
	// point, for click-to-edit and delete support.
	std::vector<AnnotInfo> pageAnnots(int page);
	AnnotInfo annotAt(int page, PagePointF pt);
	bool setFreeTextAnnot(int page, int annotIndex, const std::string& utf8,
		float fontSize, unsigned long color, PageRectPt rect, std::string& err);
	bool deleteAnnot(int page, int annotIndex, std::string& err);

	// Annotation creation. Each returns false + fills `err` on failure and
	// marks the document dirty on success. color is 0xRRGGBB (COLORREF-style).
	bool addHighlight(int page, const std::vector<PageRectPt>& quads,
		unsigned long color, float opacity, std::string& err);
	bool addInk(int page, const std::vector<std::vector<PagePointF>>& strokes,
		unsigned long color, float widthPt, std::string& err);
	bool addTextBox(int page, PageRectPt rect, const std::string& utf8,
		const std::string& font, float sizePt, unsigned long color, std::string& err);
	// Marks a region for redaction (shown as a solid black box). Nothing is
	// actually removed from the page until applyRedactions().
	bool addRedaction(int page, PageRectPt rect, std::string& err);

	// Forms.
	// All widgets on `page`, for drawing field highlights (like Edge/Acrobat
	// tint fillable fields even before the user interacts with them).
	std::vector<WidgetInfo> pageWidgets(int page);
	WidgetInfo widgetAt(int page, PagePointF pt);
	bool setTextWidget(int page, int widgetIndex, const std::string& utf8, std::string& err);
	bool toggleWidget(int page, int widgetIndex, std::string& err);
	bool setChoiceWidget(int page, int widgetIndex, const std::string& value, std::string& err);

	// --- Page assembly (merge/split/organize) -----------------------------
	// One entry in an ordered page-plan used to rebuild/export a document
	// from pages drawn from this document and/or externally opened files
	// (see openExternalFile()). externalFileIndex == -1 means "a page of
	// this document"; otherwise an index returned by openExternalFile().
	struct PagePlanEntry {
		int externalFileIndex = -1;
		int pageIndex = 0;
		int extraRotation = 0; // 0/90/180/270, added to the source page's own rotation
	};

	// Opens `path` read-only, purely to read its page count/thumbnails and
	// later graft its pages into this document -- used by Merge and
	// Organize's "Insert Pages". Uses THIS document's own fz_context, so its
	// pages can be grafted directly with no cross-context issues. Returns an
	// index to pass as PagePlanEntry::externalFileIndex, or -1 on failure
	// (including password-protected files -- not supported here).
	int openExternalFile(const wchar_t* path, std::string& err);
	int externalPageCount(int fileIndex) const;
	PageSizePt externalPageSize(int fileIndex, int pageIndex) const;
	PageBitmap renderExternalPage(int fileIndex, int pageIndex, float scale);
	// Drops everything opened via openExternalFile(). Safe to call any time;
	// called automatically by rebuildFromPages() and should also be called
	// by the UI when an Organize/Merge session is cancelled.
	void closeExternalFiles();

	// Rebuilds THIS document in place from an ordered page plan (pages may
	// come from this document and/or files opened via openExternalFile()).
	// Marks dirty; does not touch disk. Backs Organize's "Done" and Merge.
	// Known limitation: AcroForm interactive fields lose live fillability
	// (appearance/value survive; the field tree isn't reconstructed).
	bool rebuildFromPages(const std::vector<PagePlanEntry>& entries, std::string& err);

	// Builds a brand-new standalone PDF from a subset of THIS document's
	// pages and writes it directly to `path`; does not affect this
	// document's in-memory state. Backs Split.
	bool exportPages(const std::vector<int>& pageIndices, const wchar_t* path, std::string& err);

	// Rewraps every page's content in a scale/translate matrix and rewrites
	// its MediaBox to A4 (landscape if the source page is wider than tall),
	// fitting the original content fully inside, centered. Best-effort: also
	// transforms every annotation's rect/quad points by the same matrix so
	// highlights/widgets stay aligned; annotations with unusual custom
	// appearance streams may not re-scale perfectly.
	bool resizeToA4(std::string& err);

	// Renders every page to a raster image (fixed 200 DPI) and replaces its
	// content with just that image, dropping all annotations/form widgets
	// and the document's /AcroForm -- text is no longer selectable/
	// searchable and forms are no longer fillable.
	bool flattenToImages(std::string& err);

	// Recompresses images stored at a resolution well beyond their on-page
	// display size (approximated as image-pixel-width vs. host-page-width;
	// a given image is only ever assessed once even if it's used on several
	// pages) down to ~200 DPI as quality-90 JPEG. Text/vector content is
	// never touched. Combined with the stronger write options save() already
	// applies on a full rewrite, this is the "high compression, no visible
	// quality loss" lever from the UI.
	bool compress(std::string& err);

	// Count of pending (not-yet-applied) redaction marks across all pages,
	// for the redact bar's live counter.
	int pendingRedactionCount();
	// Removes every pending redaction mark across all pages without
	// redacting anything. Backs the redact bar's "Clear All".
	bool clearPendingRedactions(std::string& err);
	// Permanently strips text/vector/image content under every pending
	// redaction mark (pdf_redact_page, black boxes drawn in their place) and
	// removes the marks themselves. Irreversible once applied, even before
	// the next save.
	bool applyRedactions(std::string& err);

	// Saves the document. `incremental` appends changes to the existing file
	// (fast, only valid when saving over the file it was opened from).
	bool save(const wchar_t* path, bool incremental, std::string& err);

	// Render page `index` at `scale` device-pixels-per-point into a BGRA DIB.
	// Thread-safe against other renders on this document (serialized).
	PageBitmap renderPage(int index, float scale);
	// Same, but with annotation `annotIndex` temporarily hidden for this one
	// render (flag is restored immediately after) -- used to build the
	// "transparent" background behind an in-progress edit of that annotation
	// without showing its own not-yet-committed old appearance underneath.
	PageBitmap renderPageWithoutAnnot(int index, float scale, int annotIndex);

	fz_context* ctx() const { return ctx_; }
	fz_document* doc() const { return doc_; }

private:
	fz_context* ctx_ = nullptr;
	fz_document* doc_ = nullptr;
	bool authed_ = false;
	bool isPdf_ = false;
	bool dirty_ = false;
	bool neededPassword_ = false;
	int pageCount_ = 0;
	std::vector<PageSizePt> sizes_;
	std::vector<PageRectPt> bounds_;
	std::wstring openedPath_; // path this document is currently reading from
	// Recursive: save() holds this while it calls reopenCurrentPath(), which
	// calls loadInfo(), which also locks -- all on the same (UI) thread.
	mutable std::recursive_mutex mutex_;

	// Extra files opened via openExternalFile() for an in-progress Merge/
	// Organize session; see closeExternalFiles().
	struct ExternalFile {
		fz_document* doc = nullptr;
		std::wstring path;
	};
	std::vector<ExternalFile> externalFiles_;

	// Re-opens openedPath_ into doc_/ctx_, restoring authed_/pageCount_/etc.
	// Used after replacing the on-disk file out from under a live document.
	bool reopenCurrentPath(std::string& err);
	// Shared atomic write-verify-replace-reopen path behind save() and
	// removeProtection() (see save()'s comment for why it's never a direct
	// overwrite). stripEncryption forces the output to PDF_ENCRYPT_NONE
	// regardless of what the source was encrypted with.
	bool writeAndReplace(const wchar_t* path, bool incremental, bool stripEncryption, std::string& err);
};
