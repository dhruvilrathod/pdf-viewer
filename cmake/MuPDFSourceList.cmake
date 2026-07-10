# Extracts thirdparty source-file lists straight out of MuPDF's own
# `Makelists` file, so we don't hand-maintain hundreds of filenames and
# stay in sync if the vendored MuPDF checkout is ever updated.
#
# Usage: mupdf_extract_src(FREETYPE_SRC OUT_VAR)
function(mupdf_extract_src varname outlist)
  file(STRINGS "${MUPDF_DIR}/Makelists" lines REGEX "^${varname}[ \t]*\\+=")
  set(result "")
  foreach(line ${lines})
    string(REGEX REPLACE "^${varname}[ \t]*\\+=[ \t]*" "" relpath "${line}")
    string(STRIP "${relpath}" relpath)
    list(APPEND result "${MUPDF_DIR}/${relpath}")
  endforeach()
  set(${outlist} "${result}" PARENT_SCOPE)
endfunction()
