# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# A standalone build, on purpose: this is what an application's own repository looks like, so it
# does not belong to euclid's build tree and finds euclid-cdk the way anybody else's project would.
#
#     cmake -B build -DCMAKE_BUILD_TYPE=Release
#     cmake --build build -j 64
#
# If euclid-cdk is installed somewhere cmake does not look by itself:
#
#     cmake -B build -DCMAKE_PREFIX_PATH=/usr/local -DCMAKE_BUILD_TYPE=Release

cmake_minimum_required(VERSION 3.28)
project(echo-worker VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(euclid-cdk CONFIG REQUIRED)

add_executable(echo-worker main.cpp)
set_target_properties(echo-worker PROPERTIES CXX_SCAN_FOR_MODULES OFF)

# euclid::cdk is the shared library; euclid::cdk-static is the same SDK linked in.
#
# Static is worth considering for an application euclid deploys. The artifact is one file copied out
# of a bucket onto whichever host runs it - there is no package manager on the other side and
# nothing to install a matching libeuclid-cdk.so - so a static binary is one that cannot arrive and
# fail to start for want of a library. The cost is size and having to redeploy to pick up an SDK fix.
target_link_libraries(echo-worker PRIVATE euclid::cdk)

# Position-independent and stripped of nothing: the manager sets the exec bit on an artifact it
# copies down, and that is all it does to it.
install(TARGETS echo-worker RUNTIME DESTINATION bin)
