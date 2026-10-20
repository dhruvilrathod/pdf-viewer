// Thin wrapper over the OCR engine that ships with Windows 10/11
// (Windows.Media.Ocr). Zero extra binary size, no models to bundle, fully
// offline. Kept in its own translation unit so the C++/WinRT headers don't
// leak into the rest of the app.
#pragma once

#include <string>
#include <vector>

struct OcrWord {
	std::wstring text;
	float x = 0, y = 0, w = 0, h = 0; // pixels in the recognized image, top-left origin
};

struct OcrLine {
	std::vector<OcrWord> words;
};

// Recognizes text in an 8-bit grayscale image (`stride` bytes per row).
// Safe to call from any thread; initializes COM for the calling thread
// itself. Returns false and fills `err` if no OCR language is installed or
// recognition fails. An image with no text is success with empty `lines`.
bool OcrRecognizeGray8(const unsigned char* pixels, int width, int height, int stride,
	std::vector<OcrLine>& lines, std::string& err);

// True if Windows has at least one OCR-capable language installed. Safe on
// the UI thread; the answer is cached after the first call.
bool OcrIsAvailable();

// Largest width/height (pixels) the engine accepts; 0 if unavailable.
// Worker threads only (it joins that thread to the multithreaded apartment).
int OcrMaxImageDimension();
