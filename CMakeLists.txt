cmake_minimum_required(VERSION 3.28)

# -------------------------------------------------------------------------
# Read version.txt - single source of truth, bumped by release-please
# -------------------------------------------------------------------------
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" VERSION_STRING_RAW)
string(STRIP "${VERSION_STRING_RAW}" VERSION_STRING)

project(euclid VERSION ${VERSION_STRING} LANGUAGES CXX)

if (MSVC)
    # vcpkg's x64-windows-static triplet builds all dependencies (Boost, etc.) against
    # the static CRT; match that here so every target in this project links against the
    # same CRT, otherwise linking against those libs fails with LNK2038/LNK2005 CRT
    # symbol mismatches.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # /MP compiles multiple .cpp files of a target in parallel, but they all
    # write debug info into one shared .pdb; /FS makes cl.exe serialize those
    # writes instead of racing (C1041: cannot open program database).
    # /bigobj raises the number-of-sections limit in a single .obj, needed by
    # heavily-templated Boost.Beast/Boost.Asio translation units (C1128).
    add_compile_options(/MP /FS /bigobj)
endif ()

# -------------------------------------------------------------------------
# Warnings
#
# Not -Werror, deliberately: a warning that stops the build is one somebody
# silences in a hurry, and the point of turning these on is to read them. They
# are on for euclid's own targets and not for anything vcpkg builds, whose
# headers arrive through -isystem and are therefore already quiet.
# -------------------------------------------------------------------------
if (NOT MSVC)
    add_compile_options(
            -Wall
            -Wextra

            # The ones worth having beyond -Wextra, each because it catches a
            # class of bug rather than a style:
            -Wcast-qual              # casting away const, which is how a "read-only" argument is written to
            -Wconversion             # a narrowing that silently changes a value - the arithmetic half of a size bug
            -Wsign-conversion        # signed/unsigned mix-ups in index and size arithmetic
            -Wnon-virtual-dtor       # deleting through a base pointer without a virtual destructor
            -Woverloaded-virtual     # an override that does not override, and is never called
            -Wdouble-promotion       # a float silently widened, usually in a format string
            -Wformat=2               # a format string that does not match its arguments
            -Wimplicit-fallthrough   # a switch case falling through without saying so
            -Wnull-dereference
            -Wshadow                 # a name hiding another, which is how the wrong variable gets assigned

            # Off, and not because it is inconvenient: GCC raises it for a designated initializer
            # that omits a member which *has* a default member initializer, which is the whole
            # point of writing one. Every occurrence in this codebase is that, and leaving it on
            # buries the 30 warnings worth reading under 200 that say nothing.
            -Wno-missing-field-initializers
    )
endif ()

# -------------------------------------------------------------------------
# Hardening
#
# What the compiler and loader can do about the mistakes a review misses:
# a smashed stack caught at return, a bounds violation caught at the call,
# a relocation table that cannot be rewritten once the process is running.
#
# Every flag is tested rather than assumed. This builds on Linux, macOS on
# arm64 and Windows, and half of these are x86-only or GNU-only - a flag the
# compiler rejects is a build that fails on somebody else's machine.
# -------------------------------------------------------------------------
if (NOT MSVC)
    include(CheckCXXCompilerFlag)
    include(CheckLinkerFlag)

    function(euclid_add_compile_option_if_supported flag)
        string(MAKE_C_IDENTIFIER "HAVE_CXX${flag}" have)
        check_cxx_compiler_flag("${flag}" ${have})
        if (${have})
            add_compile_options("${flag}")
        endif ()
    endfunction()

    function(euclid_add_link_option_if_supported flag)
        string(MAKE_C_IDENTIFIER "HAVE_LINK${flag}" have)
        check_linker_flag(CXX "${flag}" ${have})
        if (${have})
            add_link_options("${flag}")
        endif ()
    endfunction()

    # Bounds and precondition checks inside libstdc++ itself: vector::operator[],
    # string::front() on an empty string, an invalid iterator range. The single
    # most useful of these for the kind of bug that reads past the end of a
    # container, and it needs no source change to benefit.
    euclid_add_compile_option_if_supported(-D_GLIBCXX_ASSERTIONS)

    # A canary between the locals and the return address, for any frame with an
    # array or an address-taken local. "strong" is the setting worth having:
    # "all" costs more than it returns and plain "-fstack-protector" covers too
    # little to matter.
    euclid_add_compile_option_if_supported(-fstack-protector-strong)

    # A large stack allocation cannot skip past the guard page into whatever is
    # next - which is what turns "allocated too much" into "wrote somewhere
    # else".
    euclid_add_compile_option_if_supported(-fstack-clash-protection)

    # Indirect branch tracking and a shadow stack where the hardware has them,
    # which is what makes a corrupted return address unusable rather than merely
    # wrong. x86 only; the test is what keeps arm64 building.
    euclid_add_compile_option_if_supported(-fcf-protection=full)

    # Only in an optimized build. _FORTIFY_SOURCE rewrites calls whose sizes the
    # optimizer can see, so at -O0 it does nothing except warn that it is doing
    # nothing.
    add_compile_options($<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=3>)

    # The GOT and the relocation tables are made read-only once the loader has
    # finished with them, rather than staying writable for the life of the
    # process. "now" is what makes "relro" cover the GOT at all.
    euclid_add_link_option_if_supported(LINKER:-z,relro)
    euclid_add_link_option_if_supported(LINKER:-z,now)
    euclid_add_link_option_if_supported(LINKER:-z,noexecstack)

    # Position independence, so the loader can put the image somewhere an
    # attacker has to find first.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif ()

# -------------------------------------------------------------------------
# Sanitizers
#
#   cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DEUCLID_SANITIZE=address,undefined
#
# Off by default and not a build type of its own, so that any build can be
# repeated with them on without a second configuration to keep in step.
#
# A sanitized build is slower and uses more memory, which is why it belongs in
# CI over the test suites rather than in anybody's working build. What it finds
# is what review does not: a read one byte past a buffer, a use of memory after
# it was freed, an integer overflow that produced a plausible-looking size.
# -------------------------------------------------------------------------
set(EUCLID_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. \"address,undefined\". Empty for none.")

if (EUCLID_SANITIZE AND NOT MSVC)
    message(STATUS "Building with sanitizers: ${EUCLID_SANITIZE}")

    # Frame pointers so a report names the functions rather than the addresses,
    # and no tail-call elision so the frame that did it is still on the stack.
    add_compile_options(-fsanitize=${EUCLID_SANITIZE} -fno-omit-frame-pointer -fno-optimize-sibling-calls -g)
    add_link_options(-fsanitize=${EUCLID_SANITIZE})
endif ()

include(cmake/ReadVersion.cmake)

# -------------------------------------------------------------------------
# Generate core/include/euclid/core/Version.h from version.txt
# -------------------------------------------------------------------------
configure_file(${CMAKE_SOURCE_DIR}/cmake/version.h.in ${CMAKE_SOURCE_DIR}/core/include/euclid/core/Version.h)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_FIND_PACKAGE_NO_SYSTEM_PACKAGE_REGISTRY ON)
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON)
set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)

if (WIN32)
    # Target Windows 10+ so boost::winapi exposes WaitOnAddress/WakeByAddress*
    # (required by boost::atomic's wait/notify support); otherwise it falls
    # back to a Windows 7 baseline that lacks these APIs.
    # NOMINMAX keeps Windows.h from defining max/min macros, which otherwise
    # break calls like std::numeric_limits<T>::max() (e.g. in bsoncxx headers).
    add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
endif ()

# -------------------------------------------------------------------------
# Collect every executable (euclid-mgr, euclid-cli, euclid-acc, euclid-queues,
# euclid-mon, ...) into a single flat <build>/bin/ storage, regardless of which
# subdirectory add_executable() was called from and regardless of single- vs
# multi-config generator - so packaging/installer scripts have one predictable path.
# -------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach (OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTPUTCONFIG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTPUTCONFIG} ${CMAKE_BINARY_DIR}/bin)
endforeach ()

find_package(LibArchive REQUIRED)
find_package(bsoncxx CONFIG REQUIRED)
find_package(mongocxx CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS filesystem thread process date_time program_options json url chrono log log_setup unit_test_framework locale)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_package(libssh CONFIG REQUIRED)

find_package(unofficial-libmagic CONFIG REQUIRED)

# SAML: assertions are XML signatures, and verifying one correctly is a library's job rather than
# ours - see Core::SamlResponseVerifier. libxml2 comes along with it (and was already here
# transitively); zlib is what the SAML redirect binding means by DEFLATE.
find_package(LibXml2 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(unofficial-xmlsec CONFIG REQUIRED)

find_library(TCMALLOC_LIBRARY NAMES tcmalloc tcmalloc_minimal)
if (TCMALLOC_LIBRARY)
    message(STATUS "tcmalloc found, linking all targets against ${TCMALLOC_LIBRARY}")
    link_libraries(${TCMALLOC_LIBRARY})
else ()
    message(STATUS "tcmalloc not found, using default allocator")
endif ()

enable_testing()

add_subdirectory(core)
add_subdirectory(database)
add_subdirectory(dto)
add_subdirectory(main)
add_subdirectory(modules/eqs)
add_subdirectory(modules/ens)
add_subdirectory(modules/eam)
add_subdirectory(modules/esm)
add_subdirectory(modules/ekm)
add_subdirectory(modules/ess)
add_subdirectory(modules/ekv)
add_subdirectory(modules/emd)
add_subdirectory(modules/emm)
add_subdirectory(modules/emo)
add_subdirectory(modules/ets)
add_subdirectory(modules/eap)
add_subdirectory(modules/ead)
add_subdirectory(modules/ees)
add_subdirectory(modules/eag)
add_subdirectory(extern/common)
add_subdirectory(extern/ftp)
add_subdirectory(extern/sftp)
add_subdirectory(cli)
add_subdirectory(tests)
