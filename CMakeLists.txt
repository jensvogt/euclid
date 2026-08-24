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
add_subdirectory(modules/eam)
add_subdirectory(modules/esm)
add_subdirectory(modules/emo)
add_subdirectory(cli)
add_subdirectory(tests)
