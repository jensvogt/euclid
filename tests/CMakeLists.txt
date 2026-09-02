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

set(TRANSFER_HOME_BINARY euclid-transfer-home-prefix-tests)

add_executable(${TRANSFER_HOME_BINARY} TransferHomePrefixTest.cpp)
set_target_properties(${TRANSFER_HOME_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${TRANSFER_HOME_BINARY} PRIVATE euclidtransfer Boost::unit_test_framework)

add_test(NAME ${TRANSFER_HOME_BINARY} COMMAND ${TRANSFER_HOME_BINARY})
