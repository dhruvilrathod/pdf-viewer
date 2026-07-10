#pragma once
#include <windows.h>

// Creates the main viewer window and runs the message loop.
// optionalPath: a document to open on startup (from the command line), or null.
int RunViewer(HINSTANCE hInstance, const wchar_t* optionalPath, int nCmdShow);
