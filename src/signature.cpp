#include "signature.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iterator>

#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

namespace {

// Candidate handwriting faces, best-looking first. A mix of Windows-shipped
// (Segoe Script/Print, Ink Free, Gabriola, Lucida Handwriting, Mistral,
// Monotype Corsiva) and Office-shipped (Bradley Hand, Edwardian/Freestyle/
// French/Palace/Kunstler Script, Viner Hand, Vladimir Script) families --
// SignatureFontFaces() filters this down to what's actually installed, so the
// list adapts per machine instead of silently substituting Arial for a face
// that isn't there.
const wchar_t* const kCandidateFaces[] = {
	L"Segoe Script",
	L"Lucida Handwriting",
	L"Ink Free",
	L"Bradley Hand ITC",
	L"Edwardian Script ITC",
	L"Freestyle Script",
	L"Mistral",
	L"Monotype Corsiva",
	L"French Script MT",
	L"Palace Script MT",
	L"Kunstler Script",
	L"Vladimir Script",
	L"Viner Hand ITC",
	L"Segoe Print",
	L"Gabriola",
};

// Blank margin (at render resolution) kept around the inked pixels after the
// tight crop, so the stamped image never looks clipped at its own edges.
constexpr int kCropPadPx = 4;

// A glyph's em box is much taller than its inked extent, and script faces
// swing well outside it with ascender/descender flourishes. Render into a
// canvas generously larger than the nominal em size in every direction and
// let the tight crop find the real bounds, rather than guessing at metrics.
constexpr float kTypedCanvasSlackY = 2.2f;
constexpr float kTypedCanvasSlackX = 1.4f;

std::wstring TrimWs(const std::wstring& s)
{
	size_t b = s.find_first_not_of(L" \t\r\n");
	if (b == std::wstring::npos) return std::wstring();
	size_t e = s.find_last_not_of(L" \t\r\n");
	return s.substr(b, e - b + 1);
}

// Tight bounding box of every pixel with any alpha, in a 32bpp PARGB bitmap.
// Returns false if the bitmap is entirely transparent (nothing was drawn).
bool InkBounds(Gdiplus::Bitmap& bmp, RECT& out)
{
	Gdiplus::Rect all(0, 0, static_cast<INT>(bmp.GetWidth()), static_cast<INT>(bmp.GetHeight()));
	Gdiplus::BitmapData data = {};
	if (bmp.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok)
		return false;
	int minX = all.Width, minY = all.Height, maxX = -1, maxY = -1;
	for (int y = 0; y < all.Height; ++y) {
		const BYTE* row = static_cast<const BYTE*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride;
		for (int x = 0; x < all.Width; ++x) {
			if (row[x * 4 + 3] == 0) continue;
			if (x < minX) minX = x;
			if (x > maxX) maxX = x;
			if (y < minY) minY = y;
			if (y > maxY) maxY = y;
		}
	}
	bmp.UnlockBits(&data);
	if (maxX < 0) return false;
	out = { minX, minY, maxX + 1, maxY + 1 };
	return true;
}

// Copies the `crop` sub-rectangle out of `bmp` as premultiplied top-down BGRA
// -- exactly what PdfDocument::addImageStamp expects (and the same byte order
// GDI+ already stores PixelFormat32bppPARGB in, so this is a straight copy).
bool CopyCropped(Gdiplus::Bitmap& bmp, const RECT& crop, std::vector<BYTE>& out, int& w, int& h)
{
	w = crop.right - crop.left;
	h = crop.bottom - crop.top;
	if (w <= 0 || h <= 0) return false;
	Gdiplus::Rect r(crop.left, crop.top, w, h);
	Gdiplus::BitmapData data = {};
	if (bmp.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok)
		return false;
	out.resize(static_cast<size_t>(w) * h * 4);
	for (int y = 0; y < h; ++y) {
		const BYTE* src = static_cast<const BYTE*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride;
		memcpy(out.data() + static_cast<size_t>(y) * w * 4, src, static_cast<size_t>(w) * 4);
	}
	bmp.UnlockBits(&data);
	return true;
}

Gdiplus::Color ToGdiColor(COLORREF c)
{
	return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}

// --- Registry persistence -------------------------------------------------
// One REG_SZ value per saved signature ("Sig0" = most recent). Serialized as
// pipe-separated fields with the free-form part (typed text / stroke data)
// last, so it can contain anything without needing an escape scheme.
//
//   Typed: T2|RRGGBB|<date>|<face>|<text>
//   Drawn: D2|RRGGBB|<date>|<face>|<padAspect>|x,y x,y;x,y x,y   (';' = stroke)
//
// <date> is empty when there is none, else "yyyy-MM-dd,format,position" (',',
// not '|', so it stays a single field). The original date-less "T|"/"D|" forms
// are still READ, so signatures saved before dates existed keep working; only
// the new forms are ever written.
constexpr wchar_t kSigRegKey[] = L"Software\\PDFast\\Signatures";

std::wstring SerializeDate(const Signature& s)
{
	if (!s.hasValidDate()) return std::wstring();
	wchar_t buf[64];
	swprintf(buf, 64, L"%04d-%02d-%02d,%d,%d", s.dateYear, s.dateMonth, s.dateDay,
		static_cast<int>(s.dateFormat), static_cast<int>(s.datePos));
	return buf;
}

void DeserializeDate(const std::wstring& spec, Signature& s)
{
	s.hasDate = false;
	if (spec.empty()) return;
	int y = 0, m = 0, d = 0, fmt = 0, pos = 0;
	if (swscanf_s(spec.c_str(), L"%d-%d-%d,%d,%d", &y, &m, &d, &fmt, &pos) != 5) return;
	s.dateYear = y; s.dateMonth = m; s.dateDay = d;
	s.dateFormat = (fmt >= 0 && fmt <= 3) ? static_cast<SigDateFormat>(fmt) : SigDateFormat::DMY;
	s.datePos = pos == 1 ? SigDatePos::Right : SigDatePos::Below;
	s.hasDate = true;
	if (!s.hasValidDate()) s.hasDate = false;
}

std::wstring Serialize(const Signature& s)
{
	wchar_t head[64];
	if (s.kind == Signature::Kind::Typed) {
		swprintf(head, 64, L"T2|%02X%02X%02X|", GetRValue(s.color), GetGValue(s.color), GetBValue(s.color));
		return std::wstring(head) + SerializeDate(s) + L"|" + s.font + L"|" + s.text;
	}
	swprintf(head, 64, L"D2|%02X%02X%02X|", GetRValue(s.color), GetGValue(s.color), GetBValue(s.color));
	wchar_t tail[32];
	swprintf(tail, 32, L"|%.4f|", s.padAspect);
	std::wstring body;
	for (const auto& stroke : s.strokes) {
		if (stroke.empty()) continue;
		if (!body.empty()) body += L';';
		for (size_t i = 0; i < stroke.size(); ++i) {
			wchar_t pt[40];
			swprintf(pt, 40, i ? L" %.4f,%.4f" : L"%.4f,%.4f", stroke[i].x, stroke[i].y);
			body += pt;
		}
	}
	return std::wstring(head) + SerializeDate(s) + L"|" + s.font + tail + body;
}

// Splits off the first `n` '|'-delimited fields, leaving the remainder (which
// may itself contain '|') as the final piece.
bool SplitFields(const std::wstring& in, int n, std::vector<std::wstring>& out)
{
	out.clear();
	size_t pos = 0;
	for (int i = 0; i < n; ++i) {
		size_t bar = in.find(L'|', pos);
		if (bar == std::wstring::npos) return false;
		out.push_back(in.substr(pos, bar - pos));
		pos = bar + 1;
	}
	out.push_back(in.substr(pos));
	return true;
}

COLORREF ParseHexColor(const std::wstring& hex)
{
	if (hex.size() < 6) return RGB(0, 0, 0);
	unsigned r = 0, g = 0, b = 0;
	if (swscanf_s(hex.c_str(), L"%2x%2x%2x", &r, &g, &b) != 3) return RGB(0, 0, 0);
	return RGB(r, g, b);
}

bool Deserialize(const std::wstring& in, Signature& out)
{
	if (in.size() < 3) return false;
	std::vector<std::wstring> f;
	// "T2"/"D2" carry a date field and (for drawn) a font; bare "T"/"D" are
	// the original date-less forms, still read so older saves survive.
	const bool v2 = in.size() > 1 && in[1] == L'2';
	if (in[0] == L'T') {
		if (!SplitFields(in, v2 ? 4 : 3, f)) return false;
		out.kind = Signature::Kind::Typed;
		out.color = ParseHexColor(f[1]);
		if (v2) {
			DeserializeDate(f[2], out);
			out.font = f[3];
			out.text = f[4];
		} else {
			out.font = f[2];
			out.text = f[3];
		}
		return !out.empty();
	}
	if (in[0] != L'D') return false;
	if (!SplitFields(in, v2 ? 5 : 3, f)) return false;
	out.kind = Signature::Kind::Drawn;
	out.color = ParseHexColor(f[1]);
	size_t aspectField = 2, bodyField = 3;
	if (v2) {
		DeserializeDate(f[2], out);
		out.font = f[3];
		aspectField = 4;
		bodyField = 5;
	}
	out.padAspect = static_cast<float>(_wtof(f[aspectField].c_str()));
	if (!(out.padAspect > 0.01f) || out.padAspect > 100.0f) out.padAspect = 3.0f;
	out.strokes.clear();
	const std::wstring& body = f[bodyField];
	size_t pos = 0;
	while (pos <= body.size()) {
		size_t semi = body.find(L';', pos);
		std::wstring seg = body.substr(pos, semi == std::wstring::npos ? std::wstring::npos : semi - pos);
		std::vector<SigPoint> stroke;
		size_t p = 0;
		while (p < seg.size()) {
			size_t sp = seg.find(L' ', p);
			std::wstring tok = seg.substr(p, sp == std::wstring::npos ? std::wstring::npos : sp - p);
			float x = 0, y = 0;
			if (swscanf_s(tok.c_str(), L"%f,%f", &x, &y) == 2) stroke.push_back({ x, y });
			if (sp == std::wstring::npos) break;
			p = sp + 1;
		}
		if (!stroke.empty()) out.strokes.push_back(std::move(stroke));
		if (semi == std::wstring::npos) break;
		pos = semi + 1;
	}
	return !out.empty();
}

} // namespace

std::wstring Signature::label() const
{
	if (kind == Kind::Typed) return TrimWs(text);
	return L"Drawn signature";
}

const std::vector<std::wstring>& SignatureFontFaces()
{
	static const std::vector<std::wstring> faces = [] {
		std::vector<std::wstring> out;
		for (const wchar_t* face : kCandidateFaces) {
			Gdiplus::FontFamily fam(face);
			if (fam.IsAvailable()) out.push_back(face);
		}
		// Every supported Windows version ships several of the above, so this
		// is a belt-and-braces fallback rather than an expected path -- but an
		// empty list would leave the panel with nothing to click.
		if (out.empty()) out.push_back(L"Segoe UI");
		return out;
	}();
	return faces;
}

namespace {

// The date's em size and the gap to the signature, both as a fraction of the
// render height, so the pair keeps its proportions at any resolution.
constexpr float kDateEmFrac = 0.32f;
constexpr float kDateGapFrac = 0.10f;

// Renders `text` in `face` to a tightly-cropped premultiplied BGRA buffer.
// Shared by the typed signature itself and the date line beneath it.
bool RenderTextRun(const std::wstring& text, const std::wstring& faceIn, float emPx,
	COLORREF color, std::vector<BYTE>& bgra, int& outW, int& outH)
{
	bgra.clear();
	outW = outH = 0;
	if (text.empty() || emPx < 4.0f) return false;
	// Resolve the face BEFORE constructing the family: Gdiplus::FontFamily
	// is non-assignable, so a "construct then swap on miss" shape doesn't
	// compile. A saved signature can name a font that's since been
	// uninstalled, hence the availability check at all.
	std::wstring face = faceIn;
	if (face.empty() || !Gdiplus::FontFamily(face.c_str()).IsAvailable())
		face = SignatureFontFaces().front();
	Gdiplus::FontFamily fam(face.c_str());
	Gdiplus::Font font(&fam, emPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
	if (font.GetLastStatus() != Gdiplus::Ok) return false;

	// Measure first (against a throwaway 1x1 surface) so the real canvas is
	// sized to the string rather than an arbitrary guess.
	Gdiplus::RectF measured;
	{
		Gdiplus::Bitmap probe(1, 1, PixelFormat32bppPARGB);
		Gdiplus::Graphics g(&probe);
		g.MeasureString(text.c_str(), -1, &font, Gdiplus::PointF(0, 0),
			Gdiplus::StringFormat::GenericTypographic(), &measured);
	}
	int canvasW = static_cast<int>(std::ceil(measured.Width * kTypedCanvasSlackX)) +
		static_cast<int>(emPx);
	int canvasH = static_cast<int>(std::ceil(emPx * kTypedCanvasSlackY));
	canvasW = std::clamp(canvasW, 16, 8000);
	canvasH = std::clamp(canvasH, 16, 8000);

	Gdiplus::Bitmap bmp(canvasW, canvasH, PixelFormat32bppPARGB);
	{
		Gdiplus::Graphics g(&bmp);
		g.Clear(Gdiplus::Color(0, 0, 0, 0));
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		// ClearType subpixel AA bakes the assumed background color into the
		// glyph edges, which turns into colored fringing on a transparent
		// bitmap composited over an arbitrary page. Grayscale AA is the
		// correct mode for an alpha-channel render.
		g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
		Gdiplus::SolidBrush brush(ToGdiColor(color));
		// Draw centered in the slack canvas so flourishes that overshoot the
		// em box in any direction still land inside it.
		g.DrawString(text.c_str(), -1, &font,
			Gdiplus::PointF((canvasW - measured.Width) / 2.0f, (canvasH - emPx) / 2.0f),
			Gdiplus::StringFormat::GenericTypographic(), &brush);
	}
	RECT ink;
	if (!InkBounds(bmp, ink)) return false;
	RECT crop = { std::max<LONG>(0, ink.left - kCropPadPx), std::max<LONG>(0, ink.top - kCropPadPx),
		std::min<LONG>(canvasW, ink.right + kCropPadPx), std::min<LONG>(canvasH, ink.bottom + kCropPadPx) };
	return CopyCropped(bmp, crop, bgra, outW, outH);
}

// Blits `src` (w*h premultiplied BGRA) into `dst` (dw*dh, same layout) at
// (x,y). The two never overlap in the compositions below, so this is a plain
// row copy -- no alpha blending needed.
void BlitInto(std::vector<BYTE>& dst, int dw, int dh,
	const std::vector<BYTE>& src, int w, int h, int x, int y)
{
	for (int row = 0; row < h; ++row) {
		int dy = y + row;
		if (dy < 0 || dy >= dh) continue;
		for (int col = 0; col < w; ++col) {
			int dx = x + col;
			if (dx < 0 || dx >= dw) continue;
			memcpy(&dst[(static_cast<size_t>(dy) * dw + dx) * 4],
				&src[(static_cast<size_t>(row) * w + col) * 4], 4);
		}
	}
}

// Renders just the signature mark (no date). The date is composed on top of
// this by RenderSignature().
bool RenderSignatureMark(const Signature& sig, int heightPx,
	std::vector<BYTE>& bgra, int& outW, int& outH)
{
	bgra.clear();
	outW = outH = 0;
	if (sig.empty() || heightPx < 8) return false;

	if (sig.kind == Signature::Kind::Typed) {
		return RenderTextRun(TrimWs(sig.text), sig.font, static_cast<float>(heightPx),
			sig.color, bgra, outW, outH);
	}

	// Drawn: replay the normalized strokes into a canvas of the pad's aspect.
	float aspect = sig.padAspect > 0.01f ? sig.padAspect : 3.0f;
	int canvasH = heightPx;
	int canvasW = std::clamp(static_cast<int>(std::lround(heightPx * aspect)), 16, 8000);
	float penW = std::max(1.5f, heightPx * 0.035f);
	int margin = static_cast<int>(std::ceil(penW)) + kCropPadPx;
	Gdiplus::Bitmap bmp(canvasW + margin * 2, canvasH + margin * 2, PixelFormat32bppPARGB);
	{
		Gdiplus::Graphics g(&bmp);
		g.Clear(Gdiplus::Color(0, 0, 0, 0));
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		Gdiplus::Pen pen(ToGdiColor(sig.color), penW);
		pen.SetStartCap(Gdiplus::LineCapRound);
		pen.SetEndCap(Gdiplus::LineCapRound);
		pen.SetLineJoin(Gdiplus::LineJoinRound);
		Gdiplus::SolidBrush dot(ToGdiColor(sig.color));
		for (const auto& stroke : sig.strokes) {
			if (stroke.empty()) continue;
			std::vector<Gdiplus::PointF> pts;
			pts.reserve(stroke.size());
			for (const auto& p : stroke)
				pts.push_back(Gdiplus::PointF(margin + p.x * canvasW, margin + p.y * canvasH));
			if (pts.size() == 1) {
				// A single tap still deserves a visible mark (the dot on an
				// "i", a full stop) -- DrawLines/DrawCurve draw nothing here.
				g.FillEllipse(&dot, pts[0].X - penW / 2, pts[0].Y - penW / 2, penW, penW);
			} else if (pts.size() == 2) {
				g.DrawLines(&pen, pts.data(), 2);
			} else {
				// Mouse sampling is chunky at speed; a low-tension cardinal
				// spline smooths that into something that reads as handwriting
				// without the overshoot a higher tension would introduce.
				g.DrawCurve(&pen, pts.data(), static_cast<INT>(pts.size()), 0.35f);
			}
		}
	}
	RECT ink;
	if (!InkBounds(bmp, ink)) return false;
	int bw = static_cast<int>(bmp.GetWidth()), bh = static_cast<int>(bmp.GetHeight());
	RECT crop = { std::max<LONG>(0, ink.left - kCropPadPx), std::max<LONG>(0, ink.top - kCropPadPx),
		std::min<LONG>(bw, ink.right + kCropPadPx), std::min<LONG>(bh, ink.bottom + kCropPadPx) };
	return CopyCropped(bmp, crop, bgra, outW, outH);
}

} // namespace

std::wstring FormatSignatureDate(const Signature& s)
{
	if (!s.hasValidDate()) return std::wstring();
	static const wchar_t* const kPictures[] = {
		L"dd/MM/yyyy",   // DMY
		L"MM/dd/yyyy",   // MDY
		L"d MMMM yyyy",  // DMonthY
		L"yyyy-MM-dd",   // ISO
	};
	size_t i = static_cast<size_t>(s.dateFormat);
	if (i >= std::size(kPictures)) i = 0;
	SYSTEMTIME st = {};
	st.wYear = static_cast<WORD>(s.dateYear);
	st.wMonth = static_cast<WORD>(s.dateMonth);
	st.wDay = static_cast<WORD>(s.dateDay);
	wchar_t buf[128] = {};
	// LOCALE_NAME_INVARIANT, not the user's locale: the picture string already
	// fixes the field order, and this keeps month names stable so a saved
	// signature renders the same on any machine.
	if (GetDateFormatEx(LOCALE_NAME_INVARIANT, 0, &st, kPictures[i], buf,
			static_cast<int>(std::size(buf)), nullptr) == 0)
		return std::wstring();
	return buf;
}

const std::vector<std::wstring>& SignatureDateFormatLabels()
{
	// Built from a sample date run through the real formatter, so the chooser
	// can never drift out of sync with what actually gets rendered.
	static const std::vector<std::wstring> labels = [] {
		Signature sample;
		sample.hasDate = true;
		sample.dateYear = 2026; sample.dateMonth = 9; sample.dateDay = 1;
		std::vector<std::wstring> out;
		for (int i = 0; i < 4; ++i) {
			sample.dateFormat = static_cast<SigDateFormat>(i);
			out.push_back(FormatSignatureDate(sample));
		}
		return out;
	}();
	return labels;
}

bool RenderSignature(const Signature& sig, int heightPx,
	std::vector<BYTE>& bgra, int& outW, int& outH)
{
	std::vector<BYTE> mark;
	int mw = 0, mh = 0;
	if (!RenderSignatureMark(sig, heightPx, mark, mw, mh)) return false;

	const std::wstring dateText = FormatSignatureDate(sig);
	if (dateText.empty()) {
		bgra = std::move(mark);
		outW = mw; outH = mh;
		return true;
	}

	std::vector<BYTE> date;
	int dw = 0, dh = 0;
	// The date is drawn in the same handwriting face and ink as the
	// signature, so the pair reads as one hand-written act rather than a
	// signature with a typeset label stuck to it.
	if (!RenderTextRun(dateText, sig.font, heightPx * kDateEmFrac, sig.color, date, dw, dh)) {
		// A date that won't render must not lose the signature with it.
		bgra = std::move(mark);
		outW = mw; outH = mh;
		return true;
	}

	int gap = std::max(2, static_cast<int>(heightPx * kDateGapFrac));
	int cw = 0, ch = 0, mx = 0, my = 0, dx = 0, dy = 0;
	if (sig.datePos == SigDatePos::Right) {
		// Side by side, baselines roughly aligned by bottom-aligning the two
		// ink boxes -- the date is much shorter, and hanging it off the top
		// would look detached.
		cw = mw + gap + dw;
		ch = std::max(mh, dh);
		my = ch - mh;
		dx = mw + gap;
		dy = ch - dh;
	} else {
		// Stacked, date left-aligned under the signature: how a signature
		// block sits on a paper form.
		cw = std::max(mw, dw);
		ch = mh + gap + dh;
		dy = mh + gap;
	}
	if (cw <= 0 || ch <= 0) return false;

	bgra.assign(static_cast<size_t>(cw) * ch * 4, 0); // fully transparent
	BlitInto(bgra, cw, ch, mark, mw, mh, mx, my);
	BlitInto(bgra, cw, ch, date, dw, dh, dx, dy);
	outW = cw;
	outH = ch;
	return true;
}

std::vector<Signature> LoadSignatures()
{
	std::vector<Signature> out;
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kSigRegKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
		return out;
	for (size_t i = 0; i < kMaxSavedSignatures; ++i) {
		wchar_t name[16];
		swprintf(name, 16, L"Sig%zu", i);
		DWORD type = 0, bytes = 0;
		if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS) continue;
		if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes > 256 * 1024) continue;
		std::wstring buf(bytes / sizeof(wchar_t), L'\0');
		if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &bytes) != ERROR_SUCCESS)
			continue;
		buf.resize(wcsnlen(buf.c_str(), buf.size()));
		Signature s;
		if (Deserialize(buf, s)) out.push_back(std::move(s));
	}
	RegCloseKey(key);
	return out;
}

void SaveSignatures(const std::vector<Signature>& sigs)
{
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kSigRegKey, 0, nullptr, 0,
			KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
		return;
	for (size_t i = 0; i < kMaxSavedSignatures; ++i) {
		wchar_t name[16];
		swprintf(name, 16, L"Sig%zu", i);
		if (i < sigs.size()) {
			std::wstring v = Serialize(sigs[i]);
			RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
				static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
		} else {
			// Trailing slots must be cleared, not just skipped -- otherwise
			// deleting a signature leaves the old tail value behind and it
			// reappears on the next load.
			RegDeleteValueW(key, name);
		}
	}
	RegCloseKey(key);
}

void RememberSignature(std::vector<Signature>& sigs, const Signature& sig)
{
	if (sig.empty()) return;
	const std::wstring key = Serialize(sig);
	sigs.erase(std::remove_if(sigs.begin(), sigs.end(),
		[&](const Signature& s) { return Serialize(s) == key; }), sigs.end());
	sigs.insert(sigs.begin(), sig);
	if (sigs.size() > kMaxSavedSignatures) sigs.resize(kMaxSavedSignatures);
	SaveSignatures(sigs);
}
