set(BINARY euclid-sftp)
add_executable(${BINARY} src/main.cpp src/SftpServer.cpp src/SftpSession.cpp)
set_target_properties(${BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(${BINARY} PRIVATE include)
# libssh guards its server-side API behind WITH_SERVER, which the installed headers never define
# themselves - the symbols are in the library, but sftp_server_new()/sftp_server_free() stay
# invisible without this.
target_compile_definitions(${BINARY} PRIVATE WITH_SERVER)
target_link_libraries(${BINARY} PRIVATE euclidcore eucliddb euclidtransfer ssh Boost::thread Boost::program_options)
