#include "print_document.h"
#include "pdf_document.h"

#include <commdlg.h>
#include <algorithm>

namespace {

// Render one page onto the printer DC, scaled to fill the printable area
// while preserving aspect ratio and centering.
void printOnePage(HDC hdc, PdfDocument& doc, int pageIndex)
{
	PageSizePt s = doc.pageSize(pageIndex);
	if (s.w <= 0 || s.h <= 0) return;

	// Printable area in device pixels, and the printer's resolution.
	int paperW = GetDeviceCaps(hdc, HORZRES);
	int paperH = GetDeviceCaps(hdc, VERTRES);
	int dpiX = GetDeviceCaps(hdc, LOGPIXELSX);
	int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
	if (paperW <= 0 || paperH <= 0) return;

	// Fit the page (in inches) into the printable area, preserving aspect.
	double pageInW = s.w / 72.0, pageInH = s.h / 72.0;
	double fitW = static_cast<double>(paperW) / (pageInW * dpiX);
	double fitH = static_cast<double>(paperH) / (pageInH * dpiY);
	double fit = std::min(fitW, fitH);
	int destW = static_cast<int>(pageInW * dpiX * fit);
	int destH = static_cast<int>(pageInH * dpiY * fit);
	int destX = (paperW - destW) / 2;
	int destY = (paperH - destH) / 2;

	// Render at up to 200 DPI to keep bitmap memory sane; the printer driver
	// scales it up to full device resolution during StretchBlt.
	double renderDpi = std::min<double>(std::max(dpiX, dpiY), 200.0);
	float scale = static_cast<float>(renderDpi / 72.0);
	PageBitmap bmp = doc.renderPage(pageIndex, scale);
	if (!bmp.hbmp) return;

	HDC mem = CreateCompatibleDC(hdc);
	HBITMAP old = static_cast<HBITMAP>(SelectObject(mem, bmp.hbmp));
	SetStretchBltMode(hdc, HALFTONE);
	SetBrushOrgEx(hdc, 0, 0, nullptr);
	StretchBlt(hdc, destX, destY, destW, destH,
		mem, 0, 0, bmp.width, bmp.height, SRCCOPY);
	SelectObject(mem, old);
	DeleteDC(mem);
}

} // namespace

bool PrintDocument(HWND owner, PdfDocument& doc, int currentPage)
{
	if (!doc.isOpen() || doc.pageCount() <= 0) return false;

	PRINTDLGW pd = {};
	pd.lStructSize = sizeof(pd);
	pd.hwndOwner = owner;
	pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_COLLATE;
	pd.nFromPage = 1;
	pd.nToPage = static_cast<WORD>(doc.pageCount());
	pd.nMinPage = 1;
	pd.nMaxPage = static_cast<WORD>(doc.pageCount());

	if (!PrintDlgW(&pd)) return false; // cancelled or error
	HDC hdc = pd.hDC;
	if (!hdc) return false;

	int first = 0, last = doc.pageCount() - 1;
	if (pd.Flags & PD_PAGENUMS) {
		first = std::clamp<int>(pd.nFromPage - 1, 0, doc.pageCount() - 1);
		last = std::clamp<int>(pd.nToPage - 1, 0, doc.pageCount() - 1);
		if (first > last) std::swap(first, last);
	}

	DOCINFOW di = {};
	di.cbSize = sizeof(di);
	di.lpszDocName = L"PDF Document";

	bool ok = true;
	if (StartDocW(hdc, &di) > 0) {
		int copies = std::max<int>(1, pd.nCopies);
		for (int c = 0; c < copies && ok; ++c) {
			for (int p = first; p <= last; ++p) {
				if (StartPage(hdc) <= 0) { ok = false; break; }
				printOnePage(hdc, doc, p);
				if (EndPage(hdc) <= 0) { ok = false; break; }
			}
		}
		EndDoc(hdc);
	} else {
		ok = false;
	}

	DeleteDC(hdc);
	if (pd.hDevMode) GlobalFree(pd.hDevMode);
	if (pd.hDevNames) GlobalFree(pd.hDevNames);
	return ok;
}
