set(LIBRARY eucliddto)
set(SOURCES src/ModuleMapper.cpp
        src/AccessMapper.cpp
        src/SqsMapper.cpp
        include/euclid/dto/access/AccessMapper.h
        include/euclid/dto/access/DeleteUserRequest.h
        include/euclid/dto/access/ListUserRequest.h
        include/euclid/dto/access/ListUserResponse.h
        include/euclid/dto/access/LoginRequest.h
        include/euclid/dto/access/LoginResponse.h
        include/euclid/dto/access/RegisterRequest.h
        include/euclid/dto/access/RegisterResponse.h
        include/euclid/dto/access/model/User.h
        include/euclid/dto/sqs/CreateQueueRequest.h
        include/euclid/dto/sqs/CreateQueueResponse.h
        include/euclid/dto/sqs/model/Queue.h
        include/euclid/dto/sqs/model/Message.h
        include/euclid/dto/sqs/model/Variant.h
        include/euclid/dto/sqs/SqsMapper.h)

add_library(${LIBRARY} STATIC ${SOURCES})

set_target_properties(${LIBRARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${LIBRARY} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)

target_link_libraries(${LIBRARY} PUBLIC euclidcore eucliddb)