#include "ocr.h"

#include <windows.h>
#include <combaseapi.h>

#include <atomic>
#include <mutex>
#include <thread>

#define WINRT_LEAN_AND_MEAN
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "ole32")

namespace {

using namespace winrt;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Media::Ocr;

// A fresh engine per call, released before the caller returns. It must NOT be
// cached in a thread_local: that destructor runs at thread exit, after Windows
// has torn the thread's apartment down and unloaded the OCR DLL, and releasing
// the engine then crashes inside the unloaded module.
OcrEngine CreateEngine()
{
	OcrEngine engine{ nullptr };
	try {
		engine = OcrEngine::TryCreateFromUserProfileLanguages();
		if (!engine) {
			// No profile language has an OCR pack; English is the most
			// common one to be present anyway.
			engine = OcrEngine::TryCreateFromLanguage(Windows::Globalization::Language(L"en-US"));
		}
	} catch (...) {
		engine = nullptr;
	}
	return engine;
}

// Keeps COM's multithreaded apartment alive for the whole process. Without it
// the apartment dies with whichever worker thread created the OCR objects,
// Windows unloads Windows.Media.Ocr.dll, and the factory C++/WinRT cached
// inside this exe is left pointing into unmapped memory (crash on the next
// call from any other thread). Threads that never initialized COM join this
// implicit MTA on their own, so nothing is initialized per thread and the UI
// thread's single-threaded apartment is never touched.
void EnsureApartment()
{
	static std::once_flag once;
	std::call_once(once, [] {
		CO_MTA_USAGE_COOKIE cookie = nullptr;
		CoIncrementMTAUsage(&cookie);
	});
}

std::string Narrow(const hstring& s)
{
	std::string out;
	for (wchar_t c : s) out.push_back(c < 128 ? static_cast<char>(c) : '?');
	return out;
}

} // namespace

bool OcrIsAvailable()
{
	// Probed on a scratch thread: the WinRT call would otherwise put the
	// calling (UI) thread into the multithreaded apartment, which must stay
	// free for the shell dialogs' single-threaded COM.
	static std::atomic<int> cached{ -1 };
	int c = cached.load();
	if (c >= 0) return c == 1;
	bool ok = false;
	std::thread t([&ok] {
		EnsureApartment();
		ok = static_cast<bool>(CreateEngine());
	});
	t.join();
	cached = ok ? 1 : 0;
	return ok;
}

int OcrMaxImageDimension()
{
	EnsureApartment();
	try { return static_cast<int>(OcrEngine::MaxImageDimension()); } catch (...) { return 0; }
}

bool OcrRecognizeGray8(const unsigned char* pixels, int width, int height, int stride,
	std::vector<OcrLine>& lines, std::string& err)
{
	lines.clear();
	EnsureApartment();
	try {
		OcrEngine engine = CreateEngine();
		if (!engine) {
			err = "No Windows OCR language is installed (Settings > Time & language > Language & region).";
			return false;
		}
		if (width <= 0 || height <= 0 || !pixels) { err = "empty image"; return false; }
		if (static_cast<uint32_t>(width) > OcrEngine::MaxImageDimension() ||
			static_cast<uint32_t>(height) > OcrEngine::MaxImageDimension()) {
			err = "image too large for the OCR engine";
			return false;
		}

		// Copy row by row so a padded stride can't skew the image.
		Windows::Storage::Streams::DataWriter writer;
		for (int y = 0; y < height; ++y) {
			const unsigned char* row = pixels + static_cast<size_t>(y) * stride;
			writer.WriteBytes(array_view<const uint8_t>(row, row + width));
		}
		SoftwareBitmap bitmap = SoftwareBitmap::CreateCopyFromBuffer(
			writer.DetachBuffer(), BitmapPixelFormat::Gray8, width, height);

		OcrResult result = engine.RecognizeAsync(bitmap).get();
		for (auto const& l : result.Lines()) {
			::OcrLine line;
			for (auto const& w : l.Words()) {
				auto r = w.BoundingRect();
				::OcrWord word;
				word.text = w.Text().c_str();
				word.x = r.X; word.y = r.Y; word.w = r.Width; word.h = r.Height;
				if (!word.text.empty()) line.words.push_back(std::move(word));
			}
			if (!line.words.empty()) lines.push_back(std::move(line));
		}
		return true;
	} catch (hresult_error const& e) {
		err = "OCR failed: " + Narrow(e.message());
		return false;
	} catch (...) {
		err = "OCR failed";
		return false;
	}
}
