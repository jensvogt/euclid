set(LIBRARY eucliddb)
set(SOURCES src/Database.cpp src/entity/module/Module.cpp src/entity/eqs/Variant.cpp
        src/entity/eqs/QueueAttribute.cpp src/entity/eqs/RedrivePolicy.cpp src/entity/eqs/Queue.cpp
        src/entity/eqs/Message.cpp src/entity/eqs/PriorityWeights.cpp
        src/repository/emm/MongoEmmRepository.cpp src/repository/eqs/MongoEqsRepository.cpp
        src/entity/eam/User.cpp src/repository/eam/MongoEamRepository.cpp
        src/entity/monitoring/MonitoringData.cpp src/repository/emo/MongoEmoRepository.cpp
        src/entity/esm/Bucket.cpp src/entity/esm/Object.cpp src/repository/esm/MongoEsmRepository.cpp)

add_library(${LIBRARY} STATIC ${SOURCES})
target_include_directories(${LIBRARY} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/../core/include)
target_link_libraries(${LIBRARY} PUBLIC euclidcore mongo::bsoncxx_static mongo::mongocxx_static Boost::boost Boost::json Boost::log
        Boost::log_setup PRIVATE OpenSSL::SSL OpenSSL::Crypto Boost::thread Boost::process Boost::date_time Boost::url
        Boost::chrono LibArchive::LibArchive unofficial::libmagic::libmagic)
