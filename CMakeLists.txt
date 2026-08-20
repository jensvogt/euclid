cmake_minimum_required(VERSION 3.28)

# -------------------------------------------------------------------------
# Read version.txt - single source of truth, bumped by release-please
# -------------------------------------------------------------------------
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" VERSION_STRING_RAW)
string(STRIP "${VERSION_STRING_RAW}" VERSION_STRING)

project(euclid VERSION ${VERSION_STRING} LANGUAGES CXX)

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

# -------------------------------------------------------------------------
# Collect every executable (euclid-mgr, euclid-cli, euclid-acc, euclid-queues,
# euclid-mon, ...) into a single flat <build>/bin/ directory, regardless of which
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
add_subdirectory(modules/queues)
add_subdirectory(modules/access)
add_subdirectory(modules/storage)
add_subdirectory(modules/monitoring)
add_subdirectory(cli)
add_subdirectory(tests)
