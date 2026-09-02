// Digital-signature model for the Sign tool: a signature is either TYPED
// (a name rendered in an installed handwriting font) or DRAWN (freehand
// strokes captured from the signature pad), both rasterized on demand into a
// transparent bitmap that PdfDocument::addImageStamp() embeds as a Stamp
// annotation.
//
// Deliberately NOT a cryptographic/PKI signature -- no certificate, no
// audit trail, no /Sig field. It's the "write your name on the page" kind,
// which is what the app's document-locking flow (Finish & Lock, backed by
// PdfDocument::flattenAnnotationsToContent) actually enforces.
//
// Strokes are stored resolution-independently (normalized to the capture
// pad's own box) rather than as a fixed bitmap, so the same saved signature
// re-renders crisply at whatever size it's later stamped at.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

// A point inside the capture pad, normalized to 0..1 on both axes (so the
// pad can be any size/DPI and a saved signature still replays correctly).
struct SigPoint { float x = 0.0f; float y = 0.0f; };

// How an accompanying date is written out. Fixed picture strings rather than
// the machine's locale default, so a saved signature renders identically
// everywhere -- and so day/month order is never ambiguous on a signed form.
enum class SigDateFormat { DMY, MDY, DMonthY, ISO };
// Where the date sits relative to the signature in the composed stamp.
enum class SigDatePos { Below, Right };

struct Signature {
	enum class Kind { Typed, Drawn };
	Kind kind = Kind::Typed;

	// Typed
	std::wstring text;  // what to render
	// Handwriting font face (one of SignatureFontFaces()). Set for BOTH kinds:
	// a drawn signature still uses it to render its date, so the date looks
	// hand-written rather than typeset next to it.
	std::wstring font;

	// Drawn
	std::vector<std::vector<SigPoint>> strokes;
	float padAspect = 3.0f; // width/height of the pad the strokes were drawn in

	COLORREF color = RGB(0, 0, 0);

	// Optional date, composed into the SAME stamp as the signature so the two
	// place as one unit. Always an explicit date the user picked -- never
	// "today" resolved at stamping time, which would silently change what a
	// saved signature means from one day to the next.
	bool hasDate = false;
	int dateYear = 0, dateMonth = 0, dateDay = 0;
	SigDateFormat dateFormat = SigDateFormat::DMY;
	SigDatePos datePos = SigDatePos::Below;

	// On-page height in points that this signature was last placed/resized at,
	// so re-using it puts down the same size rather than a generic default.
	// 0 means "never sized" -- use DefaultSignatureHeightPt(). Deliberately a
	// HEIGHT, not a width: a signature's height is what reads as its size on a
	// page, and holding width fixed instead makes a short name enormous and a
	// long one tiny (they differ several-fold in aspect ratio).
	float heightPt = 0.0f;

	bool hasValidDate() const
	{
		return hasDate && dateYear >= 1 && dateYear <= 9999 &&
			dateMonth >= 1 && dateMonth <= 12 && dateDay >= 1 && dateDay <= 31;
	}

	bool empty() const
	{
		if (kind == Kind::Typed) return text.find_first_not_of(L" \t") == std::wstring::npos;
		for (const auto& s : strokes) if (!s.empty()) return false;
		return true;
	}
	// A short human label for the gallery / status text.
	std::wstring label() const;
};

// Height in points to place `sig` at when it has no remembered size. Sized so
// a signature lands about as tall as a hand-written one on a real form (~1cm);
// a date below needs proportionally more room for the extra line.
float DefaultSignatureHeightPt(const Signature& sig);
// Widest a freshly-dropped signature is allowed to be (~2.6 inches), so a long
// name can't span the page; height scales down to respect it. Handwriting a
// long name 1cm tall really is very wide, but a signature on a form is not --
// so long names end up shorter than the nominal height, which is what a person
// would actually do when signing into a fixed-width space.
constexpr float kSignatureMaxDropWidthPt = 190.0f;

// The signature's date rendered per its dateFormat; empty if it has none.
std::wstring FormatSignatureDate(const Signature& s);
// Human-readable names for the SigDateFormat choices, in enum order, each
// showing that format applied to a sample date -- so the chooser previews the
// actual result instead of naming an abstract pattern.
const std::vector<std::wstring>& SignatureDateFormatLabels();

// Handwriting-style faces offered for a typed signature, filtered down to the
// ones actually installed on this machine (the list spans Windows-shipped and
// Office-shipped families, so which ones survive varies per machine). Never
// empty -- falls back to the default UI font if somehow none are present.
const std::vector<std::wstring>& SignatureFontFaces();

// Rasterizes `sig` into a premultiplied top-down BGRA buffer (the layout GDI+
// PixelFormat32bppPARGB and PdfDocument::addImageStamp both use), tightly
// cropped to the inked pixels. `heightPx` sets the rendering resolution, not
// the final size -- the cropped result is usually shorter, and its own w/h
// define the signature's true aspect ratio. Returns false for an empty
// signature or if nothing visible was drawn.
bool RenderSignature(const Signature& sig, int heightPx,
	std::vector<BYTE>& bgra, int& outW, int& outH);

// Persisted gallery under HKCU\Software\PDFast\Signatures (values Sig0..SigN).
// Newest first; Load silently skips anything unparseable so a hand-edited or
// future-format value can never break the panel.
constexpr size_t kMaxSavedSignatures = 5;
std::vector<Signature> LoadSignatures();
void SaveSignatures(const std::vector<Signature>& sigs);
// Puts `sig` at the front of the saved list, dropping an existing identical
// entry (so re-using a saved signature just promotes it instead of
// duplicating it) and trimming to kMaxSavedSignatures.
void RememberSignature(std::vector<Signature>& sigs, const Signature& sig);
