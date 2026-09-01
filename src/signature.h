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

struct Signature {
	enum class Kind { Typed, Drawn };
	Kind kind = Kind::Typed;

	// Typed
	std::wstring text;  // what to render
	std::wstring font;  // handwriting font face (one of SignatureFontFaces())

	// Drawn
	std::vector<std::vector<SigPoint>> strokes;
	float padAspect = 3.0f; // width/height of the pad the strokes were drawn in

	COLORREF color = RGB(0, 0, 0);

	bool empty() const
	{
		if (kind == Kind::Typed) return text.find_first_not_of(L" \t") == std::wstring::npos;
		for (const auto& s : strokes) if (!s.empty()) return false;
		return true;
	}
	// A short human label for the gallery / status text.
	std::wstring label() const;
};

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
