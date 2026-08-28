set(LIBRARY eucliddto)
set(SOURCES src/EmmMapper.cpp src/EamMapper.cpp src/EqsMapper.cpp src/EsmMapper.cpp src/EnsMapper.cpp src/EkmMapper.cpp)

add_library(${LIBRARY} STATIC ${SOURCES})

set_target_properties(${LIBRARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${LIBRARY} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)

target_link_libraries(${LIBRARY} PUBLIC euclidcore eucliddb)