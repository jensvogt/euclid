
set(BINARY euclid-cli)

add_executable(${BINARY} src/http/HttpClient.cpp src/credentials/Credentials.cpp src/eqs/QueuesCli.cpp
        src/eam/EamCli.cpp src/esm/StorageCli.cpp src/main.cpp)
target_include_directories(${BINARY} PRIVATE include ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/../dto/include)
target_link_libraries(${BINARY} PRIVATE euclidcore eucliddto Boost::program_options Boost::json Boost::url OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
