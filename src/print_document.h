#pragma once
#include <windows.h>

class PdfDocument;

// Shows the standard Windows print dialog and prints the selected page range.
// currentPage is used when the user picks "Current Page". Returns false if the
// user cancelled or an error occurred. The main viewer window serves as the
// preview, so no separate preview UI is provided.
bool PrintDocument(HWND owner, PdfDocument& doc, int currentPage);
