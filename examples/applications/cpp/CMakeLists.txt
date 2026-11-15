# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# A standalone build, on purpose: this is what an application's own repository looks like, so it
# does not belong to euclid's build tree and finds euclid-cdk the way anybody else's project would.
#
#     cmake -B build -DCMAKE_BUILD_TYPE=Release
#     cmake --build build -j 64
#
# If euclid-cdk or libarchive is installed somewhere cmake does not look by itself:
#
#     cmake -B build -DCMAKE_PREFIX_PATH=/usr/local -DCMAKE_BUILD_TYPE=Release

cmake_minimum_required(VERSION 3.28)
project(file-delivery VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Linked statically: the artifact is one file copied out of a bucket onto whichever worker runs it.
# There is no package manager on the other side to install a matching libeuclid-cdk.so, Boost,
# OpenSSL or libarchive, so a static binary is one that cannot arrive and fail to start for want of
# a library. The cost is size, and having to rebuild and redeploy to pick up a library fix.
#
# By default every library is linked in except the C library itself (libc, libm, libpthread):
# glibc does not support being linked statically into a program that resolves host names, which
# this one does to reach the gateway. Such a binary runs on any glibc distribution at least as new
# as the one it was built on, but not on musl - Alpine, which the examples/docker worker is.
#
# FILE_DELIVERY_FULLY_STATIC links the C library in too, for a binary that runs anywhere. Build it
# on Alpine (musl), where that is supported: on glibc it links, with warnings, and then cannot
# resolve host names on a worker whose glibc differs.
option(FILE_DELIVERY_FULLY_STATIC "Link the C library statically too (build on musl/Alpine)" OFF)

# Static archives in preference to shared libraries, for every find_library() from here on -
# including those euclid-cdk's own dependencies make. Must come before the find_package() calls.
set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_STATIC_LIBRARY_SUFFIX})
set(Boost_USE_STATIC_LIBS ON)
set(OPENSSL_USE_STATIC_LIBS ON)

find_package(euclid-cdk CONFIG REQUIRED)

# libarchive unpacks the ZIP files. On Debian/Ubuntu the package is libarchive-dev, on Alpine
# libarchive-dev (and libarchive-static), on Fedora libarchive-devel.
#
# A static libarchive needs the libraries it was itself built against - compression, ACLs,
# crypto, XML - which only its pkg-config file lists (Libs.private). Each is looked up as a static
# archive; one the distribution ships no archive of is a configure error naming it, rather than a
# link error later.
#
# libmagic classifies the delivered files by their content: on Debian/Ubuntu libmagic-dev, on
# Alpine file-dev and libmagic-static, on Fedora file-devel and file-static. Linked statically like
# the rest; its database (magic.mgc) is read at runtime, from where the build host's libmagic keeps
# it, or from the path in the MAGIC environment variable.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBARCHIVE REQUIRED libarchive)
pkg_check_modules(LIBXML2 libxml-2.0)
pkg_check_modules(LIBMAGIC REQUIRED libmagic)
set(_staticNames)
foreach (_name IN LISTS LIBARCHIVE_STATIC_LIBRARIES LIBXML2_STATIC_LIBRARIES LIBMAGIC_STATIC_LIBRARIES)
    if (_name IN_LIST _staticNames OR _name STREQUAL "m" OR _name STREQUAL "pthread" OR _name STREQUAL "dl")
        continue()
    endif ()
    find_library(_static_${_name} NAMES ${_name}
                 HINTS ${LIBARCHIVE_STATIC_LIBRARY_DIRS} ${LIBXML2_STATIC_LIBRARY_DIRS} ${LIBMAGIC_STATIC_LIBRARY_DIRS} NO_CACHE)
    if (NOT _static_${_name})
        message(FATAL_ERROR "No static lib${_name}${CMAKE_STATIC_LIBRARY_SUFFIX} - install its static (or -dev) package")
    endif ()
    list(APPEND _staticNames ${_name})
    list(APPEND STATIC_LIBRARIES ${_static_${_name}})
endforeach ()

add_executable(file-delivery main.cpp)
set_target_properties(file-delivery PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(file-delivery PRIVATE ${LIBARCHIVE_INCLUDE_DIRS} ${LIBMAGIC_INCLUDE_DIRS})
target_link_libraries(file-delivery PRIVATE euclid::cdk-static ${STATIC_LIBRARIES} m)

if (FILE_DELIVERY_FULLY_STATIC)
    target_link_options(file-delivery PRIVATE -static)
else ()
    # The C++ runtime too: a worker with an older libstdc++ than the build host's would otherwise
    # refuse to start the binary.
    target_link_options(file-delivery PRIVATE -static-libstdc++ -static-libgcc)
endif ()

# Position-independent and stripped of nothing: the manager sets the exec bit on an artifact it
# copies down, and that is all it does to it.
install(TARGETS file-delivery RUNTIME DESTINATION bin)
