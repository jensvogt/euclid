set(LIBRARY euclidcore)
set(SOURCES src/LogStream.cpp src/Configuration.cpp src/SystemUtils.cpp src/HttpUtils.cpp src/UuidUtils.cpp
        src/DateTimeUtils.cpp src/PasswordUtils.cpp src/JwtUtils.cpp src/UnixSocketServer.cpp
        src/HttpActionServer.cpp src/CronExpression.cpp src/Scheduler.cpp src/CryptoUtils.cpp src/SigV4.cpp
        src/HttpSignature.cpp
        src/ContentTypeUtils.cpp include/euclid/core/ErnUtils.h src/monitoring/MonitoringCollector.cpp
        src/monitoring/MetricsPusher.cpp src/ZipUtils.cpp src/WsFrame.cpp src/EventPusher.cpp)

add_library(${LIBRARY} STATIC ${SOURCES})
set_target_properties(${LIBRARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(${LIBRARY} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${LIBRARY} PUBLIC Boost::boost Boost::json Boost::log Boost::log_setup
        PRIVATE OpenSSL::SSL OpenSSL::Crypto Boost::thread Boost::process Boost::date_time Boost::url Boost::chrono
        LibArchive::LibArchive unofficial::libmagic::libmagic jwt-cpp::jwt-cpp)
