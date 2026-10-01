
# The transport a euclid client speaks, on its own so more than one client can speak it.
#
# euclid-cli was the only one for a long time, so these two lived in its executable. euclid-worker
# is the second (docs/worker-nodes.md §2: a worker is a euclid client, not a module - it reaches
# euclid through the gateway, signs with RFC 9421, and has no database and no module sockets), and
# compiling the same two files into two executables is how two copies of a signing implementation
# come to disagree.
#
# Deliberately just these two. The per-module CLIs above them are command-line parsing, which a
# worker has no use for.
set(CLIENT_LIBRARY euclidclient)

add_library(${CLIENT_LIBRARY} STATIC src/http/HttpClient.cpp src/credentials/Credentials.cpp)
set_target_properties(${CLIENT_LIBRARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${CLIENT_LIBRARY} PUBLIC include)
target_link_libraries(${CLIENT_LIBRARY} PUBLIC euclidcore Boost::json Boost::url PRIVATE OpenSSL::SSL OpenSSL::Crypto)

set(BINARY euclid-cli)

add_executable(${BINARY} src/eam/EamCli.cpp src/eam/OneLogin.cpp src/ekv/EkvCli.cpp src/eqs/EqsCli.cpp
        src/ens/EnsCli.cpp src/esm/EsmCli.cpp src/ead/EadCli.cpp src/ees/EesCli.cpp src/ekm/EkmCli.cpp src/ets/EtsCli.cpp src/eap/EapCli.cpp src/eag/EagCli.cpp src/emm/EmmCli.cpp src/ess/EssCli.cpp src/help/CliCompletion.cpp src/main.cpp)

target_include_directories(${BINARY} PRIVATE include ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated ${CMAKE_SOURCE_DIR}/../dto/include)
target_link_libraries(${BINARY} PRIVATE ${CLIENT_LIBRARY} euclidcore eucliddto Boost::program_options Boost::json Boost::url OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
