set(LIBRARY eucliddb)
set(SOURCES src/Database.cpp src/entity/module/Module.cpp src/entity/queues/Variant.cpp
        src/entity/queues/QueueAttribute.cpp src/entity/queues/RedrivePolicy.cpp src/entity/queues/Queue.cpp
        src/entity/queues/Message.cpp src/entity/queues/PriorityWeights.cpp
        src/repository/module/MongoModuleRepository.cpp src/repository/queues/MongoQueuesRepository.cpp
        src/entity/access/User.cpp src/repository/access/MongoAccessRepository.cpp
        src/entity/monitoring/MonitoringData.cpp src/repository/monitoring/MongoMonitoringRepository.cpp
        src/entity/storage/Bucket.cpp src/entity/storage/Object.cpp src/repository/storage/MongoStorageRepository.cpp)

add_library(${LIBRARY} STATIC ${SOURCES})
target_include_directories(${LIBRARY} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../core/include)
target_link_libraries(${LIBRARY} PUBLIC euclidcore mongo::bsoncxx_static mongo::mongocxx_static Boost::boost Boost::json Boost::log
        Boost::log_setup PRIVATE OpenSSL::SSL OpenSSL::Crypto Boost::thread Boost::process Boost::date_time Boost::url
        Boost::chrono LibArchive::LibArchive unofficial::libmagic::libmagic)
