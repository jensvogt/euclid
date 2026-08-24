set(LIBRARY eucliddto)
set(SOURCES src/EmmMapper.cpp
        src/EamMapper.cpp
        src/EqsMapper.cpp
        src/EsmMapper.cpp
        include/euclid/dto/eam/EamMapper.h
        include/euclid/dto/eam/DeleteUserRequest.h
        include/euclid/dto/eam/ListUserRequest.h
        include/euclid/dto/eam/ListUserResponse.h
        include/euclid/dto/eam/LoginRequest.h
        include/euclid/dto/eam/LoginResponse.h
        include/euclid/dto/eam/RegisterRequest.h
        include/euclid/dto/eam/RegisterResponse.h
        include/euclid/dto/eam/model/User.h
        include/euclid/dto/eqs/CreateQueueRequest.h
        include/euclid/dto/eqs/CreateQueueResponse.h
        include/euclid/dto/eqs/model/Queue.h
        include/euclid/dto/eqs/model/Message.h
        include/euclid/dto/eqs/model/Variant.h
        include/euclid/dto/eqs/EqsMapper.h
        include/euclid/dto/esm/CreateBucketRequest.h
        include/euclid/dto/esm/CreateBucketResponse.h
        include/euclid/dto/esm/CreateUploadRequest.h
        include/euclid/dto/esm/CreateUploadResponse.h
        include/euclid/dto/esm/CompleteUploadRequest.h
        include/euclid/dto/esm/CompleteUploadResponse.h
        include/euclid/dto/esm/DeleteBucketRequest.h
        include/euclid/dto/esm/ListBucketsRequest.h
        include/euclid/dto/esm/ListBucketsResponse.h
        include/euclid/dto/esm/UploadPartResponse.h
        include/euclid/dto/esm/model/Bucket.h
        include/euclid/dto/esm/EsmMapper.h)

add_library(${LIBRARY} STATIC ${SOURCES})

set_target_properties(${LIBRARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${LIBRARY} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)

target_link_libraries(${LIBRARY} PUBLIC euclidcore eucliddb)