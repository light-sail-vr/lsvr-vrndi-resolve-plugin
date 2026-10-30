# Windows + Linux build harness for the NDI Output OFX plugin.
#   Windows: CMake + VS2022 + CUDA Toolkit 12.9 (docs/windows-port-spec.md
#            decision 8)
#   Linux:   CMake + gcc-toolset + CUDA Toolkit 12.9 inside a Rocky Linux 8
#            container (docs/linux-port-spec.md decisions 4-9, 12-14)
# The Makefile remains the macOS harness; BUILD.md is the contract between
# them.
#
# The CUDA module (ticket #22) implements the same GPU-module contract the
# Metal module does; -DNDI_ENABLE_CUDA=OFF builds the CPU-only plugin (no
# GPU-native path, no CUDA toolkit needed).
#
# Configure + build on Windows (see BUILD.md, Windows section). The -T option
# points the VS generator's CUDA toolset straight at the toolkit — needed
# whenever the CUDA VS integration was never copied into Visual Studio (an
# admin step the toolkit installer performs but a bare nvcc install does not):
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
#         -T "cuda=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9" ^
#         -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure
#   cmake --install build --config Release --prefix stage
#
# Linux (BUILD.md, Linux section; inside the Rocky 8 container):
#   source /opt/rh/gcc-toolset-13/enable
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --parallel
#   ctest --test-dir build --output-on-failure
#   cmake --install build --prefix stage

# 3.22 for VS_SETTINGS on the vendored BRAW IDL (ticket #26) — on older
# CMake the property is silently ignored and the MIDL header lands where no
# target can include it. Linux needs 3.24 (checked below).
cmake_minimum_required(VERSION 3.22)

# zlib must link STATICALLY. The plugin is a DLL living in a folder Windows
# never searches when resolving its load-time imports, so any dynamic
# dependency staged beside it (vcpkg's default z.dll) makes the whole plugin
# fail to load inside Resolve - the same loader trap the delay-loaded NDI DLL
# exists to dodge. zlib can simply be static; the NDI runtime cannot (the SDK
# license requires shipping the vendor DLL). static-md keeps the CRT dynamic,
# matching the /MD default of the rest of the build. Must be set before
# project() for the vcpkg toolchain to see it. (Only the vcpkg toolchain reads
# it; Linux builds pass no toolchain file and use the distro's libz.a.)
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg triplet")
endif()

# C is enabled for the generated NDI stub (ndi_stub.c, CI mode below).
project(NDIOutput LANGUAGES C CXX)

if(WIN32)
    set(NDI_PLATFORM_LINUX FALSE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(NDI_PLATFORM_LINUX TRUE)
    if(CMAKE_VERSION VERSION_LESS 3.24)
        message(FATAL_ERROR
            "The Linux build needs CMake >= 3.24 (ZLIB_USE_STATIC_LIBS); "
            "Rocky 8's AppStream cmake is 3.26 (scripts/linux_build_deps.sh).")
    endif()
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
else()
    message(FATAL_ERROR
        "CMake drives the Windows and Linux builds only; use the Makefile on "
        "macOS (see BUILD.md).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- CUDA (ticket #22; spec decisions 4, 5, 8; Linux decisions 12-14) ------
# Architectures (spec decision 8): Pascal + Turing + Ampere as real targets
# plus Ampere PTX so newer GPUs (Ada, Hopper, ...) JIT forward. Must be set
# before enable_language(CUDA).
option(NDI_ENABLE_CUDA "Build the CUDA GPU-native pipeline" ON)
if(NDI_ENABLE_CUDA)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES "60-real;75-real;86-real;86-virtual")
    endif()
    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

# --- Version guard -----------------------------------------------------------
# VERSION and the #defines in src/NDIOutputPlugin.cpp are synced by
# scripts/set_version.sh / increment_version.sh; the source is authoritative
# for the compiled plugin (defining kPluginVersionString here as well was the
# old scaffold's redefinition collision). Fail configure on drift instead.
file(READ "${CMAKE_SOURCE_DIR}/VERSION" NDI_VERSION_FILE)
string(STRIP "${NDI_VERSION_FILE}" NDI_VERSION_FILE)
file(STRINGS "${CMAKE_SOURCE_DIR}/src/NDIOutputPlugin.cpp" NDI_VERSION_LINE
     REGEX "#define kPluginVersionString")
string(REGEX MATCH "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _unused "${NDI_VERSION_LINE}")
set(NDI_VERSION_SOURCE "${CMAKE_MATCH_1}")
if(NOT NDI_VERSION_FILE STREQUAL NDI_VERSION_SOURCE)
    message(FATAL_ERROR
        "VERSION file says ${NDI_VERSION_FILE} but src/NDIOutputPlugin.cpp "
        "defines kPluginVersionString ${NDI_VERSION_SOURCE}. Run "
        "scripts/set_version.sh to re-sync (never edit either by hand).")
endif()

# --- Dependencies ------------------------------------------------------------
# zlib. Windows: via vcpkg (spec decision 8) - pass the vcpkg toolchain file;
# vcpkg.json pins the dependency list. Linux: the distro's static libz.a
# (zlib-static), so the plugin never binds the host's libz - Resolve ships its
# own under /opt/resolve/libs (Linux decision 6).
if(NDI_PLATFORM_LINUX)
    set(ZLIB_USE_STATIC_LIBS ON)
endif()
find_package(ZLIB REQUIRED)

# Standard (royalty-free) NDI SDK — never the Advanced SDK, which is a
# commercial license. Use the real installation when present (required for any
# binary that will actually stream; release builds must use an SDK <= 30 days
# old). Without it — hosted CI, where no SDK is installed — fall back to the
# vendored MIT-licensed headers plus a stub library generated from the
# checked-in export list, which proves the plugin compiles and links without
# shipping a byte of NDI code.
include("${CMAKE_SOURCE_DIR}/cmake/NDIDefExports.cmake")
set(NDI_DEF_FILE "${CMAKE_SOURCE_DIR}/third_party/ndi/Processing.NDI.Lib.x64.def")

if(WIN32)
    set(NDI_SDK_PATH "C:/Program Files/NDI/NDI 6 SDK"
        CACHE PATH "Path to the Standard NDI SDK")
    set(NDI_DLL_NAME "Processing.NDI.Lib.x64")

    if(EXISTS "${NDI_SDK_PATH}/Include/Processing.NDI.Lib.h")
        message(STATUS "NDI: using installed SDK at ${NDI_SDK_PATH}")
        set(NDI_INCLUDE_DIR "${NDI_SDK_PATH}/Include")
        set(NDI_LINK_LIB "${NDI_SDK_PATH}/Lib/x64/${NDI_DLL_NAME}.lib")
        set(NDI_USING_STUB FALSE)
    else()
        message(STATUS "NDI: SDK not found - linking against the stub import library (CI mode)")
        set(NDI_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/ndi/include")
        set(NDI_USING_STUB TRUE)

        # Generate one empty function per exported name; the .def attached to
        # the stub target turns them into exports, and linking the target
        # gives the plugin a normal import library for ${NDI_DLL_NAME}.dll.
        ndi_def_exports("${NDI_DEF_FILE}" NDI_EXPORT_NAMES)
        set(NDI_STUB_SOURCE "// Generated from ${NDI_DLL_NAME}.def - do not edit.\n")
        foreach(_name IN LISTS NDI_EXPORT_NAMES)
            string(APPEND NDI_STUB_SOURCE "void ${_name}(void) {}\n")
        endforeach()
        file(WRITE "${CMAKE_BINARY_DIR}/ndi_stub.c" "${NDI_STUB_SOURCE}")

        add_library(ndi_stub SHARED "${CMAKE_BINARY_DIR}/ndi_stub.c" "${NDI_DEF_FILE}")
        set_target_properties(ndi_stub PROPERTIES
            OUTPUT_NAME "${NDI_DLL_NAME}"
            PREFIX "")
    endif()
else()
    # The "NDI SDK for Linux" installer unpacks into a folder of that name
    # wherever it is run; point NDI_SDK_PATH at it (or export NDI_SDK_PATH).
    if(DEFINED ENV{NDI_SDK_PATH})
        set(_ndi_sdk_default "$ENV{NDI_SDK_PATH}")
    else()
        set(_ndi_sdk_default "$ENV{HOME}/NDI SDK for Linux")
    endif()
    set(NDI_SDK_PATH "${_ndi_sdk_default}"
        CACHE PATH "Path to the Standard NDI SDK for Linux")
    set(NDI_SDK_LIB "${NDI_SDK_PATH}/lib/x86_64-linux-gnu/libndi.so.6")

    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" AND
       EXISTS "${NDI_SDK_PATH}/include/Processing.NDI.Lib.h" AND
       EXISTS "${NDI_SDK_LIB}")
        message(STATUS "NDI: using installed SDK at ${NDI_SDK_PATH}")
        set(NDI_INCLUDE_DIR "${NDI_SDK_PATH}/include")
        set(NDI_USING_STUB FALSE)
        # The SDK ships libndi.so.6 as a symlink chain; bundle the real file
        # under its SONAME, beside the .ofx (RUNPATH $ORIGIN finds it there),
        # with the SDK's licenses file (spec decisions 8 and 11).
        file(REAL_PATH "${NDI_SDK_LIB}" NDI_RUNTIME_FILE)
        file(GLOB NDI_LICENSE_FILES "${NDI_SDK_PATH}/licenses/*.txt")
        if(NOT NDI_LICENSE_FILES)
            message(FATAL_ERROR
                "NDI SDK at ${NDI_SDK_PATH} has no licenses/*.txt; the NDI "
                "redistribution terms require shipping it beside libndi.so.6.")
        endif()
        add_library(ndi_runtime SHARED IMPORTED)
        set_target_properties(ndi_runtime PROPERTIES
            IMPORTED_LOCATION "${NDI_SDK_LIB}"
            IMPORTED_SONAME "libndi.so.6")
        set(NDI_LINK_LIB ndi_runtime)
    else()
        message(STATUS "NDI: SDK for Linux not found at ${NDI_SDK_PATH} - "
                       "linking against a stub libndi.so.6 (CI mode)")
        set(NDI_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/ndi/include")
        set(NDI_USING_STUB TRUE)

        # Same export list as the Windows stub, as a shared object with the
        # real SONAME (libndi.so.6) so the plugin's NEEDED entry matches a
        # real SDK build. Every stub returns 0: a stub-linked plugin that
        # somehow reaches Resolve sees NDIlib_initialize() == false and runs
        # as a pass-through, never calls into garbage. The marker symbol lets
        # packaging refuse a stub (`nm -D libndi.so.6 | grep ndioutput_stub`).
        ndi_def_exports("${NDI_DEF_FILE}" NDI_EXPORT_NAMES)
        set(NDI_STUB_SOURCE
            "// Generated from Processing.NDI.Lib.x64.def - do not edit.\n"
            "// CI stub: no NDI code; every function returns 0.\n"
            "const int ndioutput_stub_libndi = 1;\n")
        foreach(_name IN LISTS NDI_EXPORT_NAMES)
            string(APPEND NDI_STUB_SOURCE "void* ${_name}(void) { return 0; }\n")
        endforeach()
        file(WRITE "${CMAKE_BINARY_DIR}/ndi_stub.c" "${NDI_STUB_SOURCE}")

        add_library(ndi_stub SHARED "${CMAKE_BINARY_DIR}/ndi_stub.c")
        set_target_properties(ndi_stub PROPERTIES
            OUTPUT_NAME "ndi"
            SOVERSION 6)
        set(NDI_RUNTIME_FILE "$<TARGET_FILE:ndi_stub>")
    endif()
endif()

# --- Shared compile settings -------------------------------------------------
if(MSVC)
    # /utf-8: sources hold UTF-8 string literals (log emoji, path tests);
    # without it MSVC decodes them through the local ANSI code page. MSVC
    # flags are scoped to C/C++ — nvcc rejects them on its own command line —
    # and reach CUDA host code via -Xcompiler.
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/W3;/utf-8;/MP>")
    add_compile_options("$<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8>")
endif()
if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
endif()
if(NDI_PLATFORM_LINUX)
    # GCC contracts a*b+c into FMA by default (-ffp-contract=fast) whenever
    # the target has FMA — always on aarch64, on x86-64 the moment anyone
    # raises -march. The CPU references must round like MSVC's (no
    # contraction) for the CUDA byte-identity test to mean anything (Linux
    # spec decision 14), so pin it off rather than rely on the baseline ISA.
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-ffp-contract=off>")
    add_compile_options("$<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-ffp-contract=off>")
endif()

# Byte-identity flag (ticket #22): the CUDA kernels must produce bytes equal
# to the shared CPU references, so nvcc may not contract the multiply-adds
# the host compilers leave separate (MSVC generates no FMA at the default
# /arch; GCC targets the x86-64 baseline, which has no FMA instructions —
# never add -march=native/-mfma, Linux spec decision 14). The kernels are
# memory-bound; the cost is noise. test_cuda_downscale is the enforcement.
set(NDI_CUDA_EXACT_MATH "$<$<COMPILE_LANGUAGE:CUDA>:--fmad=false>")

# --- BRAW immersive-metadata reader (ticket #26; Windows only so far) -------
# The Camera Metadata projection's SDK-facing half. The Windows Blackmagic
# RAW SDK ships no C++ header — it is generated from the vendored IDL
# (third_party/braw/Win/, Boost-style license, same provenance as the macOS
# shim) by midl.exe, which the VS generator drives automatically for .idl
# sources. VS_SETTINGS pins the output into one fixed directory so the
# include path below is config-independent; without it the header lands in
# the per-config IntDir where no other target could name it portably.
# Nothing Blackmagic is linked or bundled: the dispatch shim (compiled into
# BRAWImmersiveReader.cpp) resolves BlackmagicRawAPI.dll at runtime —
# exe-relative, so inside Resolve it binds Resolve's own shipped copy.
# Linux: ticket #77 (until then the capability is compiled out and the
# Camera Metadata projection soft-fails with a Stream Status message).
if(WIN32)
    set(NDI_BRAW_GEN_DIR "${CMAKE_BINARY_DIR}/braw_gen")
    file(MAKE_DIRECTORY "${NDI_BRAW_GEN_DIR}")
    set_source_files_properties(third_party/braw/Win/BlackmagicRawAPI.idl PROPERTIES
        VS_SETTINGS "OutputDirectory=${NDI_BRAW_GEN_DIR};HeaderFileName=BlackmagicRawAPI.h;InterfaceIdentifierFileName=BlackmagicRawAPI_i.c;ProxyFileName=BlackmagicRawAPI_p.c;TypeLibraryName=BlackmagicRawAPI.tlb")
    add_library(braw_reader STATIC
        src/BRAWImmersiveReader.cpp
        third_party/braw/Win/BlackmagicRawAPI.idl)
    target_include_directories(braw_reader PRIVATE
        "${NDI_BRAW_GEN_DIR}"
        third_party/braw/Win
        src)
    # oleaut32: BSTR/VARIANT plumbing the Windows BRAW API speaks; ole32: the
    # CoInitializeEx courtesy call. System DLLs, no loader-trap concern.
    target_link_libraries(braw_reader PUBLIC oleaut32 ole32)
endif()

# --- The plugin --------------------------------------------------------------
# Windows adds its Browse-dialog (#24) and Timeline (Auto) watcher (#25)
# implementations. Linux has neither yet (tickets #76, #75): their capability
# macros stay undefined there, so the plugin compiles them out and the
# features soft-fail.
set(NDI_PLUGIN_SOURCES src/NDIOutputPlugin.cpp)
if(WIN32)
    list(APPEND NDI_PLUGIN_SOURCES
        src/WinFileDialog.cpp
        src/WinTimelineWatch.cpp)
endif()
add_library(NDIOutput SHARED ${NDI_PLUGIN_SOURCES})
set_target_properties(NDIOutput PROPERTIES
    OUTPUT_NAME "NDIOutput"
    SUFFIX ".ofx"
    PREFIX "")
if(NDI_ENABLE_CUDA)
    target_sources(NDIOutput PRIVATE src/CudaGPUAcceleration.cu)
    # NDI_HAS_CUDA gates the plugin's GPU-native seam (NDIOutputPlugin.cpp);
    # without it the same source builds the CPU-only plugin.
    target_compile_definitions(NDIOutput PRIVATE NDI_HAS_CUDA)
    target_compile_options(NDIOutput PRIVATE ${NDI_CUDA_EXACT_MATH})
    # cudart must link STATICALLY: a cudart64_*.dll staged beside the plugin
    # would hit the same loader trap as z.dll (Windows never searches the
    # plugin's folder for its load-time imports) and silently empty the
    # Effects Library. test_plugin_delayload enforces this stays true. On
    # Linux a libcudart.so beside the plugin would load, but static keeps the
    # bundle to one runtime dependency (libndi.so.6); test_plugin_elf checks.
    set_target_properties(NDIOutput PROPERTIES CUDA_RUNTIME_LIBRARY Static)
    if(MSVC)
        # cudart_static.lib embeds /DEFAULTLIB:LIBCMT (it is built /MT); in
        # this /MD build that raises LNK4098, which the plugin's /WX would
        # make fatal. Dropping LIBCMT is the standard pairing for
        # cudart_static + /MD — its CRT needs resolve from the dynamic runtime.
        target_link_options(NDIOutput PRIVATE "/NODEFAULTLIB:LIBCMT")
    endif()
endif()
target_include_directories(NDIOutput PRIVATE
    openfx/include
    src
    "${NDI_INCLUDE_DIR}")
target_link_libraries(NDIOutput PRIVATE ZLIB::ZLIB)
if(WIN32)
    # ole32 + shell32 + user32: the browse buttons' IFileOpenDialog and its
    # GUI-thread guard (ticket #24, src/WinFileDialog.cpp). advapi32: the
    # timeline watcher's PEP 514 Python discovery reads the registry (ticket
    # #25, src/WinTimelineWatch.h). System DLLs, so no loader-trap concern.
    target_link_libraries(NDIOutput PRIVATE ole32 shell32 user32 advapi32)
    # The BRAW immersive-metadata reader (ticket #26; braw_reader above).
    target_link_libraries(NDIOutput PRIVATE braw_reader)
endif()

if(NDI_USING_STUB)
    target_link_libraries(NDIOutput PRIVATE ndi_stub)
else()
    target_link_libraries(NDIOutput PRIVATE "${NDI_LINK_LIB}")
endif()

if(WIN32)
    # Delay-load the NDI DLL (spec decision 12): Windows' loader never
    # searches a DLL's own folder for load-time imports of another DLL, so a
    # static import would make the plugin silently fail to load on machines
    # without an NDI runtime. Delay-loading defers resolution to first call;
    # before that call the plugin preloads the DLL from its own directory
    # (src/NDIRuntimeLoader.h, ticket #21), and test_plugin_delayload below
    # keeps the import delay-loaded forever.
    target_link_libraries(NDIOutput PRIVATE delayimp)
    # /WX: LNK4199 ("/DELAYLOAD ignored; no imports found") is a silent
    # revert to a load-time NDI import - exactly the plugin-vanishes failure.
    # Make any linker warning on this target fatal so it can never ship
    # quietly.
    target_link_options(NDIOutput PRIVATE "/DELAYLOAD:${NDI_DLL_NAME}.dll" "/WX")
else()
    # Linux ELF hygiene (spec decision 6). Resolve loads its own C++ runtime,
    # zlib, ... from /opt/resolve/libs into the same process, so the plugin
    # isolates itself from all of it:
    #  - hidden visibility + a version script exporting only the two OFX
    #    entry points: no statically linked symbol can interpose either way
    #  - -static-libstdc++/-static-libgcc: no libstdc++.so.6/libgcc_s.so.1
    #    NEEDED (the host's may be older or newer than ours)
    #  - --no-undefined / -z defs: an unresolved symbol fails the link, not
    #    the dlopen inside Resolve (where the plugin would just vanish)
    #  - RUNPATH=$ORIGIN (new dtags): the bundled libndi.so.6 beside the .ofx
    #    resolves without LD_LIBRARY_PATH. BUILD_WITH_INSTALL_RPATH keeps the
    #    build tree's binary identical (no build-dir RPATH leaks in).
    #  - --as-needed: NEEDED lists only libraries actually referenced
    # test_plugin_elf proves each one on every build.
    find_package(Threads REQUIRED)
    target_link_libraries(NDIOutput PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    set(NDI_EXPORT_MAP "${CMAKE_SOURCE_DIR}/src/LinuxExports.map")
    set_target_properties(NDIOutput PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        CUDA_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        BUILD_WITH_INSTALL_RPATH ON
        INSTALL_RPATH "\$ORIGIN"
        LINK_DEPENDS "${NDI_EXPORT_MAP}")
    target_link_options(NDIOutput PRIVATE
        -static-libstdc++
        -static-libgcc
        "-Wl,--version-script=${NDI_EXPORT_MAP}"
        -Wl,--no-undefined
        -Wl,-z,defs
        -Wl,--enable-new-dtags
        -Wl,--as-needed)
endif()

# --- Portable unit tests (LEARNINGS.md Tier 0; `make test` on macOS) --------
enable_testing()
set(NDI_PORTABLE_TESTS
    test_render_probe
    test_stream_resolution
    test_uyvy_encode
    test_cube_lut
    test_output_color
    test_timeline_color
    test_stereo_pair
    test_platform_paths
    test_ndi_loader
    test_stmap
    test_brawmap)
if(WIN32)
    # Windows-side seams (BRAW reader #26, browse dialog #24, watcher #25).
    list(APPEND NDI_PORTABLE_TESTS
        test_braw_reader
        test_win_file_dialog
        test_timeline_watch)
endif()
foreach(_t IN LISTS NDI_PORTABLE_TESTS)
    add_executable(${_t} tests/${_t}.cpp)
    target_include_directories(${_t} PRIVATE src)
    add_test(NAME ${_t} COMMAND ${_t})
    # Fixture paths in the tests are relative to the repo root.
    set_tests_properties(${_t} PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endforeach()
target_link_libraries(test_stmap PRIVATE ZLIB::ZLIB)
target_link_libraries(test_brawmap PRIVATE ZLIB::ZLIB)
if(WIN32)
    target_link_libraries(test_braw_reader PRIVATE braw_reader ZLIB::ZLIB)
    target_link_libraries(test_timeline_watch PRIVATE advapi32)  # registry discovery
endif()

# The export-list parser both NDI stubs are generated from (CMake script).
add_test(NAME test_ndi_def_exports
         COMMAND "${CMAKE_COMMAND}"
                 "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
                 "-DWORK_DIR=${CMAKE_BINARY_DIR}/test_ndi_def_exports"
                 -P "${CMAKE_SOURCE_DIR}/tests/test_ndi_def_exports.cmake")

# CUDA kernel-identity test (ticket #22): the fused kernels must be
# byte-identical to the shared CPU references. Compiles the module in
# directly, like test_metal_downscale does on macOS; runs on any machine with
# a CUDA device and skips cleanly without one (hosted CI compiles it, the
# workstations execute it — LEARNINGS.md Tier 0).
if(NDI_ENABLE_CUDA)
    add_executable(test_cuda_downscale
        tests/test_cuda_downscale.cpp
        src/CudaGPUAcceleration.cu)
    target_include_directories(test_cuda_downscale PRIVATE src)
    target_compile_options(test_cuda_downscale PRIVATE ${NDI_CUDA_EXACT_MATH})
    set_target_properties(test_cuda_downscale PROPERTIES CUDA_RUNTIME_LIBRARY Static)
    if(MSVC)
        target_link_options(test_cuda_downscale PRIVATE "/NODEFAULTLIB:LIBCMT") # see the plugin's note
    endif()
    target_link_libraries(test_cuda_downscale PRIVATE ZLIB::ZLIB)
    add_test(NAME test_cuda_downscale COMMAND test_cuda_downscale)
    set_tests_properties(test_cuda_downscale PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

if(WIN32)
    # Delay-load smoke test (Windows-only, so not in the portable list):
    # loads the freshly built .ofx with a System32-only import search - what
    # Resolve's plugin scanner amounts to - and fails on any load-time import
    # that would make the plugin silently vanish from the Effects Library.
    # wmain needs the wide entry point.
    add_executable(test_plugin_delayload tests/test_plugin_delayload.cpp)
    target_include_directories(test_plugin_delayload PRIVATE openfx/include)
    target_link_options(test_plugin_delayload PRIVATE "/ENTRY:wmainCRTStartup")
    add_test(NAME test_plugin_delayload
             COMMAND test_plugin_delayload "$<TARGET_FILE:NDIOutput>")
else()
    # ELF-hygiene smoke test (the Linux counterpart): NEEDED allowlist, glibc
    # ceiling, RUNPATH, export allowlist, and an isolated dlopen. Linked with a
    # static C++ runtime itself, so CI can run the Rocky-built binary in the
    # Ubuntu load-check container too.
    add_executable(test_plugin_elf tests/test_plugin_elf.cpp)
    target_include_directories(test_plugin_elf PRIVATE openfx/include)
    target_link_libraries(test_plugin_elf PRIVATE ${CMAKE_DL_LIBS})
    target_link_options(test_plugin_elf PRIVATE -static-libstdc++ -static-libgcc)
    add_test(NAME test_plugin_elf
             COMMAND test_plugin_elf "$<TARGET_FILE:NDIOutput>" "${NDI_RUNTIME_FILE}")
endif()

# --- Install: the OFX bundle tree --------------------------------------------
# Windows (spec decision 16): NDIOutput.ofx.bundle/Contents/Win64/NDIOutput.ofx,
# installed under C:\Program Files\Common Files\OFX\Plugins (the flat install
# in the old scaffold was wrong). Linux (decision 7):
# NDIOutput.ofx.bundle/Contents/Linux-x86-64/NDIOutput.ofx, installed under
# /usr/OFX/Plugins. The NDI runtime and its licenses file ride along when the
# real SDK is present (Windows decisions 11 and 13; Linux decisions 8 and 11);
# the installer tickets (#23, #78) own end-user packaging.
if(WIN32)
    set(NDI_BUNDLE_BIN_DIR "NDIOutput.ofx.bundle/Contents/Win64")
else()
    set(NDI_BUNDLE_BIN_DIR "NDIOutput.ofx.bundle/Contents/Linux-x86-64")
endif()
install(TARGETS NDIOutput
        RUNTIME DESTINATION "${NDI_BUNDLE_BIN_DIR}"
        LIBRARY DESTINATION "${NDI_BUNDLE_BIN_DIR}")
# The Timeline (Auto) watcher helper rides in Contents/Resources, same spot
# the macOS Makefile stages it (ticket #25; the watcher derives this path
# from its own module location — see WinTimelineWatch.h bundleResourcePath).
# Lua first (runs under Resolve's own fuscript.exe — nothing to install);
# the Python helper stays as the documented fallback for hosts without
# fuscript beside them.
install(FILES src/ndi_timeline_watch.lua src/ndi_timeline_watch.py
        DESTINATION "NDIOutput.ofx.bundle/Contents/Resources")
# Plugin icon: OFX hosts look for Contents/Resources/<plugin identifier>.png.
install(FILES src/LSVR.NDIOutput.png
        DESTINATION "NDIOutput.ofx.bundle/Contents/Resources")
if(WIN32)
    if(NOT NDI_USING_STUB)
        install(FILES
            "${NDI_SDK_PATH}/Bin/x64/${NDI_DLL_NAME}.dll"
            "${NDI_SDK_PATH}/Bin/x64/Processing.NDI.Lib.Licenses.txt"
            DESTINATION "${NDI_BUNDLE_BIN_DIR}")
    endif()
else()
    # libndi.so.6 is a load-time dependency on Linux (no delay-load), so the
    # bundle always carries one: the real runtime plus its licenses file, or —
    # stub builds only — the stub, so the staged tree still dlopens for the
    # CI load check. A stub tree loads but never streams; its artifacts are
    # named -STUB (BUILD.md, Linux section).
    install(FILES "${NDI_RUNTIME_FILE}"
            DESTINATION "${NDI_BUNDLE_BIN_DIR}"
            RENAME "libndi.so.6")
    if(NOT NDI_USING_STUB)
        install(FILES ${NDI_LICENSE_FILES} DESTINATION "${NDI_BUNDLE_BIN_DIR}")
    endif()
endif()

if(NDI_PLATFORM_LINUX)
    set(_ndi_platform "Linux")
else()
    set(_ndi_platform "Windows")
endif()
if(NDI_ENABLE_CUDA)
    message(STATUS "NDI Output ${NDI_VERSION_FILE} - ${_ndi_platform} build with "
                   "the CUDA GPU-native pipeline (ticket #22)")
else()
    message(STATUS "NDI Output ${NDI_VERSION_FILE} - ${_ndi_platform} CPU-only "
                   "build (NDI_ENABLE_CUDA=OFF)")
endif()
