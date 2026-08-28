set(BINARY euclid-ekm)

add_executable(${BINARY} src/main.cpp src/EkmServer.cpp)

set_target_properties(${BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${BINARY} PRIVATE include)

target_link_libraries(${BINARY} PRIVATE eucliddto Boost::thread Boost::program_options)
