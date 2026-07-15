cmake_minimum_required(VERSION 3.20)
project(pdfviewer LANGUAGES C CXX)

if (NOT MSVC)
	message(FATAL_ERROR "This project only supports MSVC on Windows.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Statically link the CRT (/MT or /MTd) into every target so the final
# .exe has no dependency on the VC++ redistributable at runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
cmake_policy(SET CMP0091 NEW)

include(cmake/MuPDF.cmake)

# WebView2 SDK, vendored from the microsoft.web.webview2 NuGet package
# (headers + the statically-linkable loader lib, no separate DLL to ship --
# keeps the single-exe property). Only the SDK is vendored; the much larger
# WebView2 Runtime itself is NOT bundled -- it ships with Edge and is
# expected to already be present system-wide (see webview_convert.h).
set(WEBVIEW2_DIR ${CMAKE_SOURCE_DIR}/third_party/webview2)

# Console proof-of-concept (kept for quick render sanity checks).
add_executable(pdfviewer_poc src/poc_main.cpp)
target_link_libraries(pdfviewer_poc PRIVATE mupdf)

# Console diagnostic tool for the edit+save code path (no GUI/window
# involved) -- lets us isolate save-corruption bugs from Win32 input issues.
add_executable(pdfviewer_savetest src/savetest_main.cpp src/pdf_document.cpp src/word_convert.cpp src/webview_convert.cpp)
# gdi32/user32: pdf_document.cpp now loads installed Windows fonts (CreateFontW/
# GetFontData) so MuPDF can substitute real fonts for non-embedded references.
# ole32/oleaut32: word_convert.cpp's Word COM automation (Convert to PDF's
# .docx support). WebView2LoaderStatic: webview_convert.cpp (Convert to PDF's
# web-page support).
target_include_directories(pdfviewer_savetest PRIVATE ${WEBVIEW2_DIR}/include)
target_link_libraries(pdfviewer_savetest PRIVATE mupdf gdi32 user32 ole32 oleaut32
	${WEBVIEW2_DIR}/lib/x64/WebView2LoaderStatic.lib)
target_compile_definitions(pdfviewer_savetest PRIVATE NOMINMAX UNICODE _UNICODE)

# Console harness for the auto-updater's network + parse path (no GUI). Point
# it at any public GitHub repo with releases to validate the update check.
add_executable(pdfviewer_updatetest src/updatetest_main.cpp src/updater.cpp)
target_link_libraries(pdfviewer_updatetest PRIVATE winhttp shell32 comctl32 user32)
target_compile_definitions(pdfviewer_updatetest PRIVATE NOMINMAX UNICODE _UNICODE)

# The real GUI application: single self-contained pdfviewer.exe
add_executable(pdfviewer WIN32
	src/main.cpp
	src/viewer_window.cpp
	src/pdf_document.cpp
	src/print_document.cpp
	src/updater.cpp
	src/word_convert.cpp
	src/webview_convert.cpp
	resources/app.rc
)
# winhttp: GitHub-releases update check + download (stock Windows system DLL,
# keeps the single-exe/no-redist property). See src/updater.cpp. oleaut32:
# word_convert.cpp's Word COM automation (Convert to PDF's .docx support).
# WebView2LoaderStatic: webview_convert.cpp (Convert to PDF's web-page support).
target_include_directories(pdfviewer PRIVATE ${WEBVIEW2_DIR}/include)
target_link_libraries(pdfviewer PRIVATE mupdf comctl32 shell32 gdi32 user32 comdlg32 msimg32 gdiplus ole32 oleaut32 uxtheme dwmapi winhttp
	${WEBVIEW2_DIR}/lib/x64/WebView2LoaderStatic.lib)
target_compile_definitions(pdfviewer PRIVATE NOMINMAX UNICODE _UNICODE)
# App/taskbar/Explorer icon, if resources/app.ico has been generated (see
# icon.png at the repo root -- not committed as .ico directly since it's
# generated from that source image).
if (EXISTS "${CMAKE_SOURCE_DIR}/resources/app.ico")
	target_compile_definitions(pdfviewer PRIVATE PDFVIEWER_HAS_ICON)
endif()
# Treat our source as UTF-8 so non-ASCII string literals (→, etc.) encode right.
target_compile_options(pdfviewer PRIVATE /utf-8)
# Ship the binary as PDFast.exe (the product name) while keeping the internal
# CMake target name `pdfviewer` stable. The self-updater finds its own path via
# GetModuleFileNameW, so the renamed output updates itself fine.
set_target_properties(pdfviewer PROPERTIES OUTPUT_NAME "PDFast")
# app.rc embeds our own manifest (id 1); stop the linker generating a second.
target_link_options(pdfviewer PRIVATE /MANIFEST:NO)
# Ensure the resource compiler can find app.manifest referenced by app.rc.
set_source_files_properties(resources/app.rc PROPERTIES
	COMPILE_FLAGS "/I${CMAKE_SOURCE_DIR}/resources")
