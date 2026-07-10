# Builds MuPDF as a static library from the vendored source tree in
# third_party/mupdf, using only the pieces the app actually needs:
# PDF + CBZ + image documents, with JBIG2/JPEG2000/JPEG/ICC/JS support.
#
# XPS/SVG/HTML/EPUB/MOBI/FB2/TXT/Office document support, OCR, DOCX/ODT
# export, Brotli and barcode support are switched off via MuPDF's own
# include/mupdf/fitz/config.h feature flags -- none of those are needed
# for a PDF viewer/editor, and cutting them avoids pulling in Tesseract,
# Leptonica, cURL, Gumbo, cmark-gfm, ZXing and Brotli as dependencies.

set(MUPDF_DIR ${CMAKE_SOURCE_DIR}/third_party/mupdf)
include(${CMAKE_SOURCE_DIR}/cmake/MuPDFSourceList.cmake)

mupdf_extract_src(FREETYPE_SRC FREETYPE_SOURCES)
mupdf_extract_src(HARFBUZZ_SRC HARFBUZZ_SOURCES)
mupdf_extract_src(ZLIB_SRC ZLIB_SOURCES)
mupdf_extract_src(JBIG2DEC_SRC JBIG2DEC_SOURCES)
mupdf_extract_src(OPENJPEG_SRC OPENJPEG_SOURCES)
mupdf_extract_src(LIBJPEG_SRC LIBJPEG_SOURCES)
mupdf_extract_src(LCMS2_SRC LCMS2_SOURCES)
mupdf_extract_src(MUJS_SRC MUJS_SOURCES)

# Applies everywhere: mupdf's config.h reads these to decide which
# document handlers / fonts / color-management code to compile in.
add_compile_definitions(
	FZ_ENABLE_XPS=0
	FZ_ENABLE_SVG=0
	FZ_ENABLE_HTML=0
	FZ_ENABLE_EPUB=0
	FZ_ENABLE_MOBI=0
	FZ_ENABLE_FB2=0
	FZ_ENABLE_TXT=0
	FZ_ENABLE_OFFICE=0
	FZ_ENABLE_MD=0
	FZ_ENABLE_OCR_OUTPUT=0
	FZ_ENABLE_DOCX_OUTPUT=0
	FZ_ENABLE_ODT_OUTPUT=0
	FZ_ENABLE_BROTLI=0
	FZ_ENABLE_BARCODE=0
	OPJ_STATIC
	HAVE_LCMS2MT=1
	LCMS2MT_PREFIX=lcms2mt_
	_CRT_SECURE_NO_WARNINGS
	_CRT_NONSTDC_NO_DEPRECATE
)

# Which built-in fallback fonts to embed. These are only used when a PDF
# does NOT embed its own font (most do). More coverage = bigger .exe:
#   base14 : Latin standard fonts only (~0.6 MB) -- Helvetica/Times/etc.
#   cjk    : base14 + Chinese/Japanese/Korean fallback (~+29 MB)
#   full   : base14 + CJK + Noto (Arabic/Hebrew/Thai/emoji/...) (~+43 MB)
# MuPDF's TOFU_* defines switch fonts OFF; we translate the option here.
# (SIL fonts are always dropped -- they only matter for EPUB/HTML, which
# this build disables; MuPDF's config.h auto-defines TOFU_SIL for us.)
set(PDFVIEWER_FONTS "base14" CACHE STRING "Embedded fallback fonts: base14 | cjk | full")
if(PDFVIEWER_FONTS STREQUAL "base14")
	add_compile_definitions(TOFU TOFU_CJK)
elseif(PDFVIEWER_FONTS STREQUAL "cjk")
	add_compile_definitions(TOFU) # drop Noto, keep CJK
elseif(PDFVIEWER_FONTS STREQUAL "full")
	# embed everything MuPDF ships (no TOFU_* defines)
else()
	message(FATAL_ERROR "PDFVIEWER_FONTS must be base14, cjk, or full")
endif()

# --- freetype ---
add_library(mupdf_freetype STATIC ${FREETYPE_SOURCES})
target_include_directories(mupdf_freetype PRIVATE
	${MUPDF_DIR}/thirdparty/freetype/include
	${MUPDF_DIR}/scripts/freetype)
target_compile_definitions(mupdf_freetype PRIVATE
	FT_CONFIG_MODULES_H=\"slimftmodules.h\"
	FT_CONFIG_OPTIONS_H=\"slimftoptions.h\"
	FT2_BUILD_LIBRARY)

# --- zlib ---
add_library(mupdf_zlib STATIC ${ZLIB_SOURCES})
target_include_directories(mupdf_zlib PUBLIC ${MUPDF_DIR}/thirdparty/zlib)
# HAVE_UNISTD_H is deliberately omitted: that header doesn't exist on
# MSVC (it's only correct for mupdf's Unix/mingw Makefile builds).
target_compile_definitions(mupdf_zlib PRIVATE HAVE_STDARG_H)

# --- harfbuzz (text shaping; needs freetype headers for hb-ft.cc) ---
add_library(mupdf_harfbuzz STATIC ${HARFBUZZ_SOURCES})
target_include_directories(mupdf_harfbuzz PRIVATE
	${MUPDF_DIR}/thirdparty/harfbuzz/src
	${MUPDF_DIR}/include/mupdf
	${MUPDF_DIR}/include
	${MUPDF_DIR}/thirdparty/freetype/include
	${MUPDF_DIR}/scripts/freetype)
target_compile_definitions(mupdf_harfbuzz PRIVATE
	HAVE_FALLBACK=1 HAVE_FREETYPE HAVE_OT HAVE_ROUND HAVE_UCDN HB_NO_MT
	HB_NO_PRAGMA_GCC_DIAGNOSTIC
	hb_malloc_impl=fz_hb_malloc hb_calloc_impl=fz_hb_calloc
	hb_free_impl=fz_hb_free hb_realloc_impl=fz_hb_realloc)
set_target_properties(mupdf_harfbuzz PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# --- jbig2dec ---
add_library(mupdf_jbig2dec STATIC ${JBIG2DEC_SOURCES})
target_include_directories(mupdf_jbig2dec PRIVATE
	${MUPDF_DIR}/thirdparty/jbig2dec
	${MUPDF_DIR}/include)
target_compile_definitions(mupdf_jbig2dec PRIVATE
	HAVE_STDINT_H
	JBIG_EXTERNAL_MEMENTO_H=\"mupdf/memento.h\")

# --- openjpeg (JPEG2000) ---
add_library(mupdf_openjpeg STATIC ${OPENJPEG_SOURCES})
target_include_directories(mupdf_openjpeg PRIVATE ${MUPDF_DIR}/thirdparty/openjpeg/src/lib/openjp2)
target_compile_definitions(mupdf_openjpeg PRIVATE
	OPJ_HAVE_INTTYPES_H OPJ_HAVE_STDINT_H MUTEX_pthread=0)

# --- libjpeg (mupdf's fork) ---
add_library(mupdf_libjpeg STATIC ${LIBJPEG_SOURCES})
target_include_directories(mupdf_libjpeg PRIVATE
	${MUPDF_DIR}/thirdparty/libjpeg
	${MUPDF_DIR}/scripts/libjpeg)

# --- lcms2 (multi-thread-safe fork, renamed symbols) ---
add_library(mupdf_lcms2 STATIC ${LCMS2_SOURCES})
target_include_directories(mupdf_lcms2 PRIVATE ${MUPDF_DIR}/thirdparty/lcms2/include)

# --- mujs (PDF form JavaScript) ---
add_library(mupdf_mujs STATIC ${MUJS_SOURCES})
target_include_directories(mupdf_mujs PRIVATE ${MUPDF_DIR}/thirdparty/mujs)

# --- embedded fallback fonts (generated C hexdumps) ---
# MuPDF ships pre-generated C files for the Base14 fonts under generated/.
# The CJK / Noto ones are normally produced at build time; generate any
# that are missing for the selected coverage level via scripts/hexdump.sh
# (needs the bash from Git for Windows, already a build-time dependency).
include(${CMAKE_SOURCE_DIR}/cmake/EmbedFonts.cmake)
mupdf_embed_fonts(FONT_SOURCES)

# --- mupdf core: fitz + pdf + cbz ---
file(GLOB FITZ_SOURCES ${MUPDF_DIR}/source/fitz/*.c)
file(GLOB PDF_SOURCES ${MUPDF_DIR}/source/pdf/*.c)
file(GLOB CBZ_SOURCES ${MUPDF_DIR}/source/cbz/*.c)

add_library(mupdf STATIC
	${FITZ_SOURCES}
	${PDF_SOURCES}
	${CBZ_SOURCES}
	${FONT_SOURCES}
	${MUPDF_DIR}/source/helpers/mu-threads/mu-threads.c
)
target_include_directories(mupdf PUBLIC ${MUPDF_DIR}/include)
target_include_directories(mupdf PRIVATE
	${MUPDF_DIR}/thirdparty/freetype/include
	${MUPDF_DIR}/thirdparty/harfbuzz/src
	${MUPDF_DIR}/thirdparty/jbig2dec
	${MUPDF_DIR}/thirdparty/openjpeg/src/lib/openjp2
	${MUPDF_DIR}/thirdparty/libjpeg
	${MUPDF_DIR}/scripts/libjpeg
	${MUPDF_DIR}/thirdparty/lcms2/include
	${MUPDF_DIR}/thirdparty/mujs
	${MUPDF_DIR}/thirdparty/zlib
)
target_link_libraries(mupdf PUBLIC
	mupdf_freetype
	mupdf_harfbuzz
	mupdf_zlib
	mupdf_jbig2dec
	mupdf_openjpeg
	mupdf_libjpeg
	mupdf_lcms2
	mupdf_mujs
)
