#pragma once
#include <windows.h>
#include <string>
#include <vector>

class PdfDocument;

// Page range selection, mirroring a browser print dialog's radio group.
enum class PrintRangeMode { All, Current, Custom };

// Everything the print side panel lets the user configure. Both the live
// preview (CanvasView) and the actual print job read from the same struct,
// so what you see in the preview is what prints.
struct PrintSettings {
	std::wstring printerName;      // empty => system default
	int copies = 1;
	PrintRangeMode rangeMode = PrintRangeMode::All;
	std::wstring customRange;      // e.g. "1-3,5" -- only used when rangeMode == Custom
	bool landscape = false;
	bool grayscale = false;
};

// Installed printer names (EnumPrintersW), for the panel's printer combo box.
std::vector<std::wstring> ListPrinterNames();

// The system default printer's name, or empty if none.
std::wstring DefaultPrinterName();

// Resolves `settings`'s range selection (All/Current/Custom) against a
// `pageCount`-page document (0-based indices, in print order). `currentPage`
// is used for PrintRangeMode::Current. Invalid/out-of-range custom entries
// are skipped; returns an empty vector if nothing valid remains.
std::vector<int> ResolvePrintPages(const PrintSettings& settings, int pageCount, int currentPage);

// Renders `pageIndex` into a `previewWpx` x `previewHpx` (device pixels)
// bitmap depicting the sheet it'll print on: a white rect fit-centered in a
// gray surround, with the page content fit-centered into that rect (which
// doubles as the "sheet"). When `landscape` is true the fit uses the page's
// own aspect ratio swapped (not the preview box's aspect -- the preview box
// itself is usually already wider than tall here, so rotating ITS aspect
// would produce a portrait-shaped sheet, backwards), and the page content is
// itself rotated 90 degrees to fill it -- same as what the real print job
// does (see printOnePage in print_document.cpp) for a portrait page forced
// onto landscape paper. grayscale desaturates the result. Returns an owned
// HBITMAP (32bpp BGRA DIB section, caller deletes) or nullptr.
HBITMAP RenderPrintPreviewPage(PdfDocument& doc, int pageIndex, int previewWpx, int previewHpx,
	bool grayscale, bool landscape);

// Runs the actual print job (StartDoc/StartPage/.../EndDoc) against
// `settings`, printing exactly `pages` (0-based, in order) -- the caller
// resolves the range via ResolvePrintPages() itself, since only it knows
// the current page for PrintRangeMode::Current. Returns false and fills
// `err` on failure; shows no dialog of its own (the print panel is the UI,
// this just executes).
bool ExecutePrintJob(HWND owner, PdfDocument& doc, const PrintSettings& settings,
	const std::vector<int>& pages, std::string& err);
