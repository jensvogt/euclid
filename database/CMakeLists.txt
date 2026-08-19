set(LIBRARY eucliddb)
set(SOURCES src/Database.cpp src/entity/module/Module.cpp
        src/entity/sqs/Variant.cpp src/entity/sqs/QueueAttribute.cpp
        src/entity/sqs/RedrivePolicy.cpp src/entity/sqs/Queue.cpp src/entity/sqs/Message.cpp
        src/repository/module/MongoModuleRepository.cpp src/repository/sqs/MongoSQSRepository.cpp
        src/entity/access/User.cpp src/repository/access/MongoAccessRepository.cpp
        src/entity/monitoring/MonitoringData.cpp src/repository/monitoring/MongoMonitoringRepository.cpp)

add_library(${LIBRARY} STATIC ${SOURCES})
target_include_directories(${LIBRARY} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../core/include)
target_link_libraries(${LIBRARY} PUBLIC euclidcore mongo::bsoncxx_static mongo::mongocxx_static Boost::boost Boost::json Boost::log
        Boost::log_setup PRIVATE OpenSSL::SSL OpenSSL::Crypto Boost::thread Boost::process Boost::date_time Boost::url
        Boost::chrono LibArchive::LibArchive magic)
