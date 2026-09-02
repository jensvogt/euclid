set(LIBRARY euclidtransfer)
add_library(${LIBRARY} STATIC src/TransferContext.cpp src/TransferAuthenticator.cpp src/TransferStorage.cpp
        src/TransferPaths.cpp)
set_target_properties(${LIBRARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
# PUBLIC: euclid-ftp and euclid-sftp include these headers directly.
target_include_directories(${LIBRARY} PUBLIC include)
target_link_libraries(${LIBRARY} PUBLIC euclidcore eucliddb)
