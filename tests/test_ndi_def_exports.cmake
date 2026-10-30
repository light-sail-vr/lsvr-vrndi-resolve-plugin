# Tests the NDI export-list parser (cmake/NDIDefExports.cmake) that both
# stub builds are generated from: the Windows stub import library and the
# Linux stub libndi.so.6 (spec decision 9). Run by ctest on both platforms:
#   cmake -DSOURCE_DIR=<repo> -DWORK_DIR=<scratch dir> -P tests/test_ndi_def_exports.cmake

include("${SOURCE_DIR}/cmake/NDIDefExports.cmake")

set(_failures 0)
macro(expect_equal actual expected name)
    if("${actual}" STREQUAL "${expected}")
        message(STATUS "ok   ${name}")
    else()
        message(SEND_ERROR "FAIL ${name}\n  expected: ${expected}\n  actual:   ${actual}")
        math(EXPR _failures "${_failures} + 1")
    endif()
endmacro()

# --- The checked-in list ------------------------------------------------------
# Expected values come from the file itself, read by eye: 120 indented names,
# first NDIlib_destroy, last NDIlib_version.
ndi_def_exports("${SOURCE_DIR}/third_party/ndi/Processing.NDI.Lib.x64.def" _real)
list(LENGTH _real _count)
expect_equal("${_count}" "120" "checked-in .def yields every export")
list(GET _real 0 _first)
list(GET _real -1 _last)
expect_equal("${_first}" "NDIlib_destroy" "first export in file order")
expect_equal("${_last}" "NDIlib_version" "last export in file order")
list(FIND _real "NDIlib_initialize" _idx)
if(_idx LESS 0)
    message(SEND_ERROR "FAIL NDIlib_initialize missing")
    math(EXPR _failures "${_failures} + 1")
endif()
list(FIND _real "LIBRARY" _lib_idx)
list(FIND _real "EXPORTS" _exp_idx)
expect_equal("${_lib_idx}/${_exp_idx}" "-1/-1" "directives are not exports")

# --- Grammar edge cases ---------------------------------------------------------
# CRLF endings (a Windows checkout with autocrlf), tab indents, comments and
# blank lines anywhere, names before EXPORTS ignored.
file(MAKE_DIRECTORY "${WORK_DIR}")
set(_fixture "${WORK_DIR}/fixture.def")
file(WRITE "${_fixture}"
    "; leading comment\r\n"
    "LIBRARY \"libfoo.dll\"\r\n"
    "NotAnExport\r\n"
    "EXPORTS\r\n"
    "\tfoo_one\r\n"
    "\r\n"
    "; a comment between names\r\n"
    "    foo_two\r\n"
    "foo_3_three\r\n")
ndi_def_exports("${_fixture}" _fx)
expect_equal("${_fx}" "foo_one;foo_two;foo_3_three" "CRLF, tabs, comments, blank lines")

# A file without an EXPORTS section has no exports.
file(WRITE "${_fixture}" "LIBRARY \"libfoo.dll\"\nfoo_one\n")
ndi_def_exports("${_fixture}" _none)
expect_equal("${_none}" "" "no EXPORTS section -> empty list")

if(_failures GREATER 0)
    message(FATAL_ERROR "${_failures} failure(s)")
endif()
message(STATUS "all NDI export-list parser tests passed")
