# Produces the list of generated embedded-font .c files for the selected
# PDFVIEWER_FONTS level, generating any that don't already ship in the
# MuPDF checkout's generated/ directory.
#
# Font binary -> generated file mapping mirrors MuPDF's own Makefile:
#   resources/fonts/X/foo.cff  ->  generated/resources/fonts/X/foo.cff.c
# and the per-level selection mirrors the Makefile's FONT_BIN TOFU logic.

function(mupdf_embed_fonts out_sources)
	# Base14 (always needed -- without these, PDF rendering is unusable).
	file(GLOB _urw ${MUPDF_DIR}/resources/fonts/urw/*.cff)
	set(_bins ${_urw})

	if(PDFVIEWER_FONTS STREQUAL "cjk" OR PDFVIEWER_FONTS STREQUAL "full")
		file(GLOB _han ${MUPDF_DIR}/resources/fonts/han/*.ttc)
		list(APPEND _bins
			${_han}
			${MUPDF_DIR}/resources/fonts/droid/DroidSansFallbackFull.ttf
			${MUPDF_DIR}/resources/fonts/droid/DroidSansFallback.ttf)
	endif()

	if(PDFVIEWER_FONTS STREQUAL "full")
		file(GLOB _noto_otf ${MUPDF_DIR}/resources/fonts/noto/*.otf)
		file(GLOB _noto_ttf ${MUPDF_DIR}/resources/fonts/noto/*.ttf)
		list(APPEND _bins ${_noto_otf} ${_noto_ttf})
	endif()

	# hexdump.sh needs a POSIX bash + od + sed. Git for Windows provides
	# these; it's already a build-time dependency (used to fetch MuPDF).
	find_program(BASH_EXE NAMES bash
		HINTS
			"$ENV{ProgramFiles}/Git/bin"
			"$ENV{LOCALAPPDATA}/Programs/Git/bin"
			"C:/Program Files/Git/bin")

	set(_generated "")
	foreach(_bin ${_bins})
		file(RELATIVE_PATH _rel ${MUPDF_DIR} ${_bin})
		set(_gen ${MUPDF_DIR}/generated/${_rel}.c)
		if(NOT EXISTS ${_gen})
			if(NOT BASH_EXE)
				message(FATAL_ERROR
					"Need to generate embedded font ${_rel} but bash was not found. "
					"Install Git for Windows, or set PDFVIEWER_FONTS=base14.")
			endif()
			get_filename_component(_gendir ${_gen} DIRECTORY)
			file(MAKE_DIRECTORY ${_gendir})
			message(STATUS "Generating embedded font: ${_rel}.c")
			execute_process(
				COMMAND ${BASH_EXE} ${MUPDF_DIR}/scripts/hexdump.sh ${_bin}
				OUTPUT_FILE ${_gen}
				WORKING_DIRECTORY ${MUPDF_DIR}
				RESULT_VARIABLE _rc)
			if(NOT _rc EQUAL 0)
				message(FATAL_ERROR "hexdump.sh failed for ${_rel} (code ${_rc})")
			endif()
		endif()
		list(APPEND _generated ${_gen})
	endforeach()

	set(${out_sources} ${_generated} PARENT_SCOPE)
endfunction()
