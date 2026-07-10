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

# Console proof-of-concept (kept for quick render sanity checks).
add_executable(pdfviewer_poc src/poc_main.cpp)
target_link_libraries(pdfviewer_poc PRIVATE mupdf)

# Console diagnostic tool for the edit+save code path (no GUI/window
# involved) -- lets us isolate save-corruption bugs from Win32 input issues.
add_executable(pdfviewer_savetest src/savetest_main.cpp src/pdf_document.cpp)
target_link_libraries(pdfviewer_savetest PRIVATE mupdf)
target_compile_definitions(pdfviewer_savetest PRIVATE NOMINMAX UNICODE _UNICODE)

# The real GUI application: single self-contained pdfviewer.exe
add_executable(pdfviewer WIN32
	src/main.cpp
	src/viewer_window.cpp
	src/pdf_document.cpp
	src/print_document.cpp
	resources/app.rc
)
target_link_libraries(pdfviewer PRIVATE mupdf comctl32 shell32 gdi32 user32 comdlg32 msimg32 gdiplus ole32 uxtheme)
target_compile_definitions(pdfviewer PRIVATE NOMINMAX UNICODE _UNICODE)
# App/taskbar/Explorer icon, if resources/app.ico has been generated (see
# icon.png at the repo root -- not committed as .ico directly since it's
# generated from that source image).
if (EXISTS "${CMAKE_SOURCE_DIR}/resources/app.ico")
	target_compile_definitions(pdfviewer PRIVATE PDFVIEWER_HAS_ICON)
endif()
# Treat our source as UTF-8 so non-ASCII string literals (→, etc.) encode right.
target_compile_options(pdfviewer PRIVATE /utf-8)
# app.rc embeds our own manifest (id 1); stop the linker generating a second.
target_link_options(pdfviewer PRIVATE /MANIFEST:NO)
# Ensure the resource compiler can find app.manifest referenced by app.rc.
set_source_files_properties(resources/app.rc PROPERTIES
	COMPILE_FLAGS "/I${CMAKE_SOURCE_DIR}/resources")
