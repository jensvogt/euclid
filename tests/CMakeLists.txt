set(BINARY euclid-tests)

add_executable(${BINARY} SigV4Test.cpp)
set_target_properties(${BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${BINARY} COMMAND ${BINARY})

set(SCOPE_BINARY euclid-http-action-server-scope-tests)

add_executable(${SCOPE_BINARY} HttpActionServerScopeTest.cpp)
set_target_properties(${SCOPE_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${SCOPE_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${SCOPE_BINARY} COMMAND ${SCOPE_BINARY})

set(EVENTBUS_BINARY euclid-eventbus-external-tests)

add_executable(${EVENTBUS_BINARY} EventBusExternalTest.cpp)
set_target_properties(${EVENTBUS_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${EVENTBUS_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${EVENTBUS_BINARY} COMMAND ${EVENTBUS_BINARY})

set(EAP_APPLICATION_BINARY euclid-eap-application-tests)

add_executable(${EAP_APPLICATION_BINARY} EapApplicationTest.cpp)
set_target_properties(${EAP_APPLICATION_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${EAP_APPLICATION_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${EAP_APPLICATION_BINARY} COMMAND ${EAP_APPLICATION_BINARY})

set(HTTP_SIGNATURE_BINARY euclid-http-signature-tests)

add_executable(${HTTP_SIGNATURE_BINARY} HttpSignatureTest.cpp)
set_target_properties(${HTTP_SIGNATURE_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${HTTP_SIGNATURE_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${HTTP_SIGNATURE_BINARY} COMMAND ${HTTP_SIGNATURE_BINARY})

set(MONITORING_BINARY euclid-monitoring-collector-tests)

add_executable(${MONITORING_BINARY} MonitoringCollectorTest.cpp)
set_target_properties(${MONITORING_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${MONITORING_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${MONITORING_BINARY} COMMAND ${MONITORING_BINARY})

set(ESM_DIRECTORY_BINARY euclid-esm-directory-listing-tests)

add_executable(${ESM_DIRECTORY_BINARY} EsmDirectoryListingTest.cpp)
set_target_properties(${ESM_DIRECTORY_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${ESM_DIRECTORY_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${ESM_DIRECTORY_BINARY} COMMAND ${ESM_DIRECTORY_BINARY})

set(ESM_ATTRIBUTES_BINARY euclid-esm-object-attributes-tests)

add_executable(${ESM_ATTRIBUTES_BINARY} EsmObjectAttributesTest.cpp)
set_target_properties(${ESM_ATTRIBUTES_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${ESM_ATTRIBUTES_BINARY} PRIVATE eucliddto euclidtransfer Boost::unit_test_framework)

add_test(NAME ${ESM_ATTRIBUTES_BINARY} COMMAND ${ESM_ATTRIBUTES_BINARY})

set(OUTBOX_BINARY euclid-gateway-outbox-tests)

add_executable(${OUTBOX_BINARY} GatewayOutboxTest.cpp)
set_target_properties(${OUTBOX_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${OUTBOX_BINARY} PRIVATE ${CMAKE_SOURCE_DIR}/main/include)
target_link_libraries(${OUTBOX_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${OUTBOX_BINARY} COMMAND ${OUTBOX_BINARY})

set(WS_FRAME_BINARY euclid-gateway-ws-frame-tests)

add_executable(${WS_FRAME_BINARY} GatewayWsFrameTest.cpp)
set_target_properties(${WS_FRAME_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${WS_FRAME_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${WS_FRAME_BINARY} COMMAND ${WS_FRAME_BINARY})

set(WORKER_THREADS_BINARY euclid-worker-threads-config-tests)

add_executable(${WORKER_THREADS_BINARY} WorkerThreadsConfigTest.cpp)
set_target_properties(${WORKER_THREADS_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${WORKER_THREADS_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${WORKER_THREADS_BINARY} COMMAND ${WORKER_THREADS_BINARY})

set(LONG_POLL_BINARY euclid-long-poll-slots-tests)

add_executable(${LONG_POLL_BINARY} LongPollSlotsTest.cpp)
set_target_properties(${LONG_POLL_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${LONG_POLL_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${LONG_POLL_BINARY} COMMAND ${LONG_POLL_BINARY})

set(REDRIVE_BINARY euclid-eqs-redrive-dlq-tests)

add_executable(${REDRIVE_BINARY} RedriveDlqTest.cpp)
set_target_properties(${REDRIVE_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${REDRIVE_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${REDRIVE_BINARY} COMMAND ${REDRIVE_BINARY})

set(ROUTE_MATCH_BINARY euclid-eag-route-match-tests)

add_executable(${ROUTE_MATCH_BINARY} RouteMatchTest.cpp ${CMAKE_SOURCE_DIR}/modules/eag/src/RouteTable.cpp)
set_target_properties(${ROUTE_MATCH_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${ROUTE_MATCH_BINARY} PRIVATE ${CMAKE_SOURCE_DIR}/modules/eag/include)
target_link_libraries(${ROUTE_MATCH_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${ROUTE_MATCH_BINARY} COMMAND ${ROUTE_MATCH_BINARY})

set(START_ORDER_BINARY euclid-module-start-order-tests)

add_executable(${START_ORDER_BINARY} ModuleStartOrderTest.cpp)
set_target_properties(${START_ORDER_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${START_ORDER_BINARY} PRIVATE ${CMAKE_SOURCE_DIR}/main/include)
target_link_libraries(${START_ORDER_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${START_ORDER_BINARY} COMMAND ${START_ORDER_BINARY})

set(OBJECT_CIPHER_BINARY euclid-object-cipher-tests)

add_executable(${OBJECT_CIPHER_BINARY} ObjectCipherTest.cpp)
set_target_properties(${OBJECT_CIPHER_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${OBJECT_CIPHER_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${OBJECT_CIPHER_BINARY} COMMAND ${OBJECT_CIPHER_BINARY})

set(TRANSFER_HOME_BINARY euclid-transfer-home-prefix-tests)

add_executable(${TRANSFER_HOME_BINARY} TransferHomePrefixTest.cpp)
set_target_properties(${TRANSFER_HOME_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${TRANSFER_HOME_BINARY} PRIVATE euclidtransfer Boost::unit_test_framework)

add_test(NAME ${TRANSFER_HOME_BINARY} COMMAND ${TRANSFER_HOME_BINARY})
