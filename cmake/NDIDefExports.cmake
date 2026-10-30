# Export-list parser for the NDI stub builds (spec decision 9; Windows port
# CI mode). third_party/ndi/Processing.NDI.Lib.x64.def is the single source of
# the Standard SDK's royalty-free API surface; both stubs are generated from it:
#   Windows - a stub DLL whose import library the plugin links (CMakeLists.txt)
#   Linux   - a stub libndi.so.6 (SONAME libndi.so.6) the plugin links
# Neither contains NDI code. tests/test_ndi_def_exports.cmake covers this.

# ndi_def_exports(<def-file> <out-var>)
# Sets <out-var> to the export names in file order: every bare identifier line
# after the EXPORTS directive. Comments (;), blank lines and anything before
# EXPORTS (LIBRARY, ...) are skipped; CRLF and tab indents are tolerated.
function(ndi_def_exports def_file out_var)
    file(STRINGS "${def_file}" _lines)
    set(_names "")
    set(_in_exports FALSE)
    foreach(_line IN LISTS _lines)
        string(STRIP "${_line}" _line)
        if(_line STREQUAL "EXPORTS")
            set(_in_exports TRUE)
        elseif(_in_exports AND _line MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
            list(APPEND _names "${_line}")
        endif()
    endforeach()
    set(${out_var} "${_names}" PARENT_SCOPE)
endfunction()
