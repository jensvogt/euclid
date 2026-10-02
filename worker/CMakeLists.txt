
set(BINARY euclid-wrk)

add_executable(${BINARY} src/main.cpp src/WorkerClient.cpp)
set_target_properties(${BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${BINARY} PRIVATE include ${CMAKE_BINARY_DIR}/generated)

# euclidclient for the transport (see cli/CMakeLists.txt), euclidcore for the artifact fetcher and
# everything under it. Deliberately *not* eucliddb: a worker has no database credentials and no
# business holding any, and the link line is the cheapest place to keep that true.
target_link_libraries(${BINARY} PRIVATE euclidclient euclidcore Boost::program_options Boost::json OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
