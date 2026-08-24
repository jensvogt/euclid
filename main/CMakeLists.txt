set(BINARY euclid-manager)

add_executable(${BINARY} src/main.cpp src/Controller.cpp src/GatewayServer.cpp src/ControllerPlatform.cpp)

set_target_properties(${BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${BINARY} PRIVATE include ../dto/include)

target_link_libraries(${BINARY} PRIVATE eucliddto Boost::program_options Boost::thread jwt-cpp::jwt-cpp ssh OpenSSL::SSL OpenSSL::Crypto)

if (WIN32)
    # Advapi32 provides the Service Control Manager API (OpenSCManager/CreateService/...)
    # used by Platform::InstallService for `euclid-manager --install`.
    target_link_libraries(${BINARY} PRIVATE Advapi32)
endif ()