// Word (.docx) to PDF via Microsoft Word COM automation -- there's no small,
// fast way to render OOXML layout ourselves (it's a full document-layout
// engine), so this hands the work to a real copy of Word instead of
// hand-rolling a parser. Requires Word to be installed on the machine;
// see ConvertDocxToPdf's comment for the failure behavior when it isn't.
#pragma once

#include <string>

// Converts `docxPath` to a standalone PDF at `pdfPath` by driving a hidden,
// out-of-process instance of Word (Documents.Open -> SaveAs2 wdFormatPDF ->
// Close -> Quit). Late-bound IDispatch calls only (no #import/type library),
// so this builds without Word or the Office SDK installed on the build
// machine -- it only needs Word installed on the machine that RUNS the
// conversion. Returns false and fills `err` with a user-facing message if
// Word isn't installed, the document can't be opened (e.g. corrupt or itself
// password-protected), or the save fails. Never partially succeeds: on
// failure, no output file is left behind by this function (though Word
// itself manages its own document lifecycle, so a manual retry is safe).
bool ConvertDocxToPdf(const wchar_t* docxPath, const wchar_t* pdfPath, std::string& err);
