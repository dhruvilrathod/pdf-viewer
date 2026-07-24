#include "print_document.h"
#include "pdf_document.h"

#include <winspool.h>
#include <algorithm>
#include <cwctype>

// Only needed to rotate rendered page content 90 degrees for landscape
// printing/preview (see blitPageContent below) -- plain GDI's StretchBlt
// can't rotate. Same system component the rest of the app already links
// against for its vector toolbar icons.
#include <gdiplus.h>

#pragma comment(lib, "winspool.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {

// Converts a 32bpp BGRA DIB's pixels to grayscale in place, luma-weighted
// (matches how print drivers/most viewers desaturate for "print in
// grayscale" -- not a flat average, or colored text reads at a visibly
// different darkness than its true luminance).
void desaturateDIB(void* bits, int w, int h)
{
	auto* p = static_cast<unsigned char*>(bits);
	for (int i = 0; i < w * h; ++i) {
		unsigned char b = p[0], g = p[1], r = p[2];
		unsigned char lum = static_cast<unsigned char>((r * 54 + g * 183 + b * 19) >> 8); // 0.2126/0.7152/0.0722 approx, integer
		p[0] = p[1] = p[2] = lum;
		p += 4;
	}
}

// Fits a `srcW`x`srcH` rectangle into `dstW`x`dstH`, preserving aspect ratio
// and centering -- shared by the preview bitmap and the real printer DC so
// they compute the identical placement.
void fitCentered(int srcW, int srcH, int dstW, int dstH, int& outX, int& outY, int& outW, int& outH)
{
	if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) { outX = outY = outW = outH = 0; return; }
	double fit = std::min(static_cast<double>(dstW) / srcW, static_cast<double>(dstH) / srcH);
	outW = std::max(1, static_cast<int>(srcW * fit));
	outH = std::max(1, static_cast<int>(srcH * fit));
	outX = (dstW - outW) / 2;
	outY = (dstH - outH) / 2;
}

// Draws a rendered page bitmap (always rendered upright -- PdfDocument has
// no rotate-on-render option) into its fitted destination rect, rotating 90
// degrees clockwise first when `landscape` is set. Forcing a portrait PDF
// page onto a landscape sheet is supposed to rotate the content to fill the
// wide sheet, not just shrink the unrotated page into the middle of it
// (which is what fitting srcW x srcH straight into a wider-than-tall rect
// does, and is the "landscape isn't really landscape" bug). GDI's
// StretchBlt can't rotate, so the landscape case goes through GDI+;
// portrait keeps the plain StretchBlt path since it never needs rotation.
void blitPageContent(HDC hdc, HBITMAP hbmp, int bmpW, int bmpH,
	int destX, int destY, int destW, int destH, bool landscape)
{
	if (!landscape) {
		HDC mem = CreateCompatibleDC(hdc);
		HBITMAP old = static_cast<HBITMAP>(SelectObject(mem, hbmp));
		SetStretchBltMode(hdc, HALFTONE);
		SetBrushOrgEx(hdc, 0, 0, nullptr);
		StretchBlt(hdc, destX, destY, destW, destH, mem, 0, 0, bmpW, bmpH, SRCCOPY);
		SelectObject(mem, old);
		DeleteDC(mem);
		return;
	}
	Gdiplus::Bitmap bitmap(hbmp, nullptr);
	Gdiplus::Graphics g(hdc);
	g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
	g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
	// Rotate about the destination rect's center. The bitmap is drawn at its
	// PRE-rotation footprint (destH wide x destW tall -- width/height
	// swapped relative to the final destW x destH box), so after the 90
	// degree turn it lands exactly on destW x destH.
	g.TranslateTransform(static_cast<Gdiplus::REAL>(destX + destW / 2.0), static_cast<Gdiplus::REAL>(destY + destH / 2.0));
	g.RotateTransform(90.0f);
	g.DrawImage(&bitmap, static_cast<Gdiplus::REAL>(-destH / 2.0), static_cast<Gdiplus::REAL>(-destW / 2.0),
		static_cast<Gdiplus::REAL>(destH), static_cast<Gdiplus::REAL>(destW));
}

// Render one page onto the printer DC, scaled to fill the printable area
// while preserving aspect ratio and centering (rotated into place first for
// landscape -- see blitPageContent).
void printOnePage(HDC hdc, PdfDocument& doc, int pageIndex, bool grayscale, bool landscape)
{
	PageSizePt s = doc.pageSize(pageIndex);
	if (s.w <= 0 || s.h <= 0) return;

	int paperW = GetDeviceCaps(hdc, HORZRES);
	int paperH = GetDeviceCaps(hdc, VERTRES);
	int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
	int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
	if (paperW <= 0 || paperH <= 0) return;

	double pageInW = s.w / 72.0, pageInH = s.h / 72.0;
	int srcW = static_cast<int>(pageInW * dpiX);
	int srcH = static_cast<int>(pageInH * dpiY);
	int destX, destY, destW, destH;
	// Landscape: fit the page's ROTATED footprint (width/height swapped) into
	// the sheet, since the content will be drawn rotated 90 degrees below.
	if (landscape) fitCentered(srcH, srcW, paperW, paperH, destX, destY, destW, destH);
	else fitCentered(srcW, srcH, paperW, paperH, destX, destY, destW, destH);

	// Render at up to 200 DPI to keep bitmap memory sane; the printer driver
	// scales it up to full device resolution during StretchBlt.
	double renderDpi = std::min<double>(std::max(dpiX, dpiY), 200.0);
	float scale = static_cast<float>(renderDpi / 72.0);
	PageBitmap bmp = doc.renderPage(pageIndex, scale);
	if (!bmp.hbmp) return;

	if (grayscale) {
		DIBSECTION ds{};
		if (GetObjectW(bmp.hbmp, sizeof(ds), &ds) && ds.dsBm.bmBits)
			desaturateDIB(ds.dsBm.bmBits, ds.dsBm.bmWidth, ds.dsBm.bmHeight);
	}

	blitPageContent(hdc, bmp.hbmp, bmp.width, bmp.height, destX, destY, destW, destH, landscape);
}

} // namespace

std::vector<std::wstring> ListPrinterNames()
{
	std::vector<std::wstring> out;
	DWORD needed = 0, returned = 0;
	EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, nullptr, 0, &needed, &returned);
	if (needed == 0) return out;
	std::vector<unsigned char> buf(needed);
	if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 4, buf.data(), needed, &needed, &returned))
		return out;
	auto* info = reinterpret_cast<PRINTER_INFO_4W*>(buf.data());
	for (DWORD i = 0; i < returned; ++i)
		if (info[i].pPrinterName) out.push_back(info[i].pPrinterName);
	return out;
}

std::wstring DefaultPrinterName()
{
	wchar_t buf[512];
	DWORD n = 512;
	if (GetDefaultPrinterW(buf, &n)) return buf;
	return L"";
}

std::vector<int> ResolvePrintPages(const PrintSettings& settings, int pageCount, int currentPage)
{
	std::vector<int> out;
	if (pageCount <= 0) return out;
	if (settings.rangeMode == PrintRangeMode::Current) {
		if (currentPage >= 0 && currentPage < pageCount) out.push_back(currentPage);
		return out;
	}
	if (settings.rangeMode == PrintRangeMode::All) {
		out.reserve(pageCount);
		for (int i = 0; i < pageCount; ++i) out.push_back(i);
		return out;
	}
	// Custom: comma-separated list of "N" or "N-M" (1-based, inclusive).
	std::wstring s = settings.customRange;
	size_t pos = 0;
	while (pos < s.size()) {
		size_t comma = s.find(L',', pos);
		std::wstring token = s.substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
		pos = (comma == std::wstring::npos) ? s.size() : comma + 1;
		size_t a = 0, b = token.size();
		while (a < b && iswspace(token[a])) ++a;
		while (b > a && iswspace(token[b - 1])) --b;
		token = token.substr(a, b - a);
		if (token.empty()) continue;
		size_t dash = token.find(L'-');
		int lo, hi;
		if (dash == std::wstring::npos) {
			lo = hi = _wtoi(token.c_str());
		} else {
			lo = _wtoi(token.substr(0, dash).c_str());
			hi = _wtoi(token.substr(dash + 1).c_str());
		}
		if (lo <= 0 || hi <= 0) continue;
		if (lo > hi) std::swap(lo, hi);
		for (int p = lo; p <= hi; ++p) {
			if (p >= 1 && p <= pageCount) out.push_back(p - 1);
		}
	}
	return out;
}

HBITMAP RenderPrintPreviewPage(PdfDocument& doc, int pageIndex, int previewWpx, int previewHpx,
	bool grayscale, bool landscape)
{
	if (previewWpx <= 0 || previewHpx <= 0) return nullptr;
	PageSizePt s = doc.pageSize(pageIndex);
	if (s.w <= 0 || s.h <= 0) return nullptr;

	// Landscape: fit the page's ROTATED footprint (width/height swapped) into
	// the preview box -- the content is drawn rotated 90 degrees below (see
	// blitPageContent), so this rect comes out the correct landscape-paper
	// shape and doubles as the "sheet" rect directly, same as portrait
	// already did. (An earlier version of this code computed a separate
	// sheet rect by rotating the PREVIEW BOX's own aspect ratio -- that's
	// backwards whenever the box itself is already wider than tall, which is
	// the normal case here since the preview lives on the main canvas, not a
	// narrow dialog: rotating an already-wide box's aspect produces a TALL
	// sheet instead of a wide one.)
	int destX, destY, destW, destH;
	if (landscape) fitCentered(static_cast<int>(s.h), static_cast<int>(s.w), previewWpx, previewHpx, destX, destY, destW, destH);
	else fitCentered(static_cast<int>(s.w), static_cast<int>(s.h), previewWpx, previewHpx, destX, destY, destW, destH);

	float scale = landscape ? static_cast<float>(destH) / s.w : static_cast<float>(destW) / s.w;
	PageBitmap bmp = doc.renderPage(pageIndex, scale);
	if (!bmp.hbmp) return nullptr;

	BITMAPINFO bmi = {};
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = previewWpx;
	bmi.bmiHeader.biHeight = -previewHpx; // top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HBITMAP page = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!page || !bits) { if (page) DeleteObject(page); return nullptr; }

	HDC dc = CreateCompatibleDC(nullptr);
	HBITMAP oldTarget = static_cast<HBITMAP>(SelectObject(dc, page));
	RECT full = { 0, 0, previewWpx, previewHpx };
	HBRUSH gray = CreateSolidBrush(RGB(160, 160, 160)); // matches the app's page-background gutter
	FillRect(dc, &full, gray);
	DeleteObject(gray);
	// A crisp 1px border around the page edge -- without this, a white page
	// on the (fairly light) gray gutter above has almost no contrast at a
	// glance, which is exactly what made the preview look like "just a
	// blank white page" with no visible frame around it. No separate white
	// fill is needed first: the rotated (landscape) or plain (portrait) page
	// bitmap drawn below is itself opaque and exactly fills destX/Y/W/H.
	{
		HPEN pen = CreatePen(PS_SOLID, 1, RGB(90, 90, 90));
		HGDIOBJ oldPen = SelectObject(dc, pen);
		HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
		Rectangle(dc, destX, destY, destX + destW - 1, destY + destH - 1);
		SelectObject(dc, oldBrush);
		SelectObject(dc, oldPen);
		DeleteObject(pen);
	}

	blitPageContent(dc, bmp.hbmp, bmp.width, bmp.height, destX, destY, destW, destH, landscape);
	SelectObject(dc, oldTarget);
	DeleteDC(dc);

	if (grayscale) desaturateDIB(bits, previewWpx, previewHpx);
	return page;
}

bool ExecutePrintJob(HWND owner, PdfDocument& doc, const PrintSettings& settings,
	const std::vector<int>& pages, std::string& err)
{
	if (!doc.isOpen() || doc.pageCount() <= 0) { err = "no document open"; return false; }
	if (pages.empty()) { err = "no pages in range"; return false; }

	std::wstring printer = settings.printerName.empty() ? DefaultPrinterName() : settings.printerName;
	if (printer.empty()) { err = "no printer available"; return false; }

	// DEVMODE drives paper orientation (and is where a driver that DOES honor
	// dmColor could pick up grayscale -- but printOnePage's own desaturation
	// is what actually guarantees it, since not all drivers honor dmColor for
	// a StretchBlt'd bitmap).
	HANDLE hPrinter = nullptr;
	if (!OpenPrinterW(const_cast<LPWSTR>(printer.c_str()), &hPrinter, nullptr)) {
		err = "could not open printer";
		return false;
	}
	LONG dmSize = DocumentPropertiesW(owner, hPrinter, const_cast<LPWSTR>(printer.c_str()), nullptr, nullptr, 0);
	std::vector<unsigned char> dmBuf;
	DEVMODEW* dm = nullptr;
	if (dmSize > 0) {
		dmBuf.resize(dmSize);
		dm = reinterpret_cast<DEVMODEW*>(dmBuf.data());
		if (DocumentPropertiesW(owner, hPrinter, const_cast<LPWSTR>(printer.c_str()), dm, nullptr, DM_OUT_BUFFER) < 0)
			dm = nullptr;
	}
	if (dm) {
		dm->dmFields |= DM_ORIENTATION;
		dm->dmOrientation = settings.landscape ? DMORIENT_LANDSCAPE : DMORIENT_PORTRAIT;
		DocumentPropertiesW(owner, hPrinter, const_cast<LPWSTR>(printer.c_str()), dm, dm, DM_IN_BUFFER | DM_OUT_BUFFER);
	}
	ClosePrinter(hPrinter);

	HDC hdc = CreateDCW(nullptr, printer.c_str(), nullptr, dm);
	if (!hdc) { err = "could not create printer device context"; return false; }

	DOCINFOW di = {};
	di.cbSize = sizeof(di);
	di.lpszDocName = L"PDF Document";

	bool ok = true;
	if (StartDocW(hdc, &di) > 0) {
		int copies = std::max(1, settings.copies);
		for (int c = 0; c < copies && ok; ++c) {
			for (int p : pages) {
				if (StartPage(hdc) <= 0) { ok = false; break; }
				printOnePage(hdc, doc, p, settings.grayscale, settings.landscape);
				if (EndPage(hdc) <= 0) { ok = false; break; }
			}
		}
		EndDoc(hdc);
	} else {
		ok = false;
	}
	if (!ok) err = "printing failed";
	DeleteDC(hdc);
	return ok;
}
