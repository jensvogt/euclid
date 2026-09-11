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

set(ENS_PUBLISH_PRIORITY_BINARY euclid-ens-publish-priority-tests)

add_executable(${ENS_PUBLISH_PRIORITY_BINARY} EnsPublishPriorityTest.cpp)
set_target_properties(${ENS_PUBLISH_PRIORITY_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${ENS_PUBLISH_PRIORITY_BINARY} PRIVATE eucliddto eucliddb Boost::unit_test_framework)

add_test(NAME ${ENS_PUBLISH_PRIORITY_BINARY} COMMAND ${ENS_PUBLISH_PRIORITY_BINARY})

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

set(CERTIFICATE_BINARY euclid-certificate-utils-tests)

add_executable(${CERTIFICATE_BINARY} CertificateUtilsTest.cpp)
set_target_properties(${CERTIFICATE_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${CERTIFICATE_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${CERTIFICATE_BINARY} COMMAND ${CERTIFICATE_BINARY})

set(DIR_UTILS_BINARY euclid-dir-utils-tests)

add_executable(${DIR_UTILS_BINARY} DirUtilsTest.cpp)
set_target_properties(${DIR_UTILS_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${DIR_UTILS_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${DIR_UTILS_BINARY} COMMAND ${DIR_UTILS_BINARY})

set(LOG_CHANNEL_BINARY euclid-log-channel-tests)

add_executable(${LOG_CHANNEL_BINARY} LogChannelTest.cpp)
set_target_properties(${LOG_CHANNEL_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${LOG_CHANNEL_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${LOG_CHANNEL_BINARY} COMMAND ${LOG_CHANNEL_BINARY})

set(MODULE_LOG_LEVEL_BINARY euclid-module-log-level-tests)

add_executable(${MODULE_LOG_LEVEL_BINARY} ModuleLogLevelTest.cpp)
set_target_properties(${MODULE_LOG_LEVEL_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${MODULE_LOG_LEVEL_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${MODULE_LOG_LEVEL_BINARY} COMMAND ${MODULE_LOG_LEVEL_BINARY})

set(SECRET_BINARY euclid-ess-secret-tests)

add_executable(${SECRET_BINARY} SecretTest.cpp)
set_target_properties(${SECRET_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${SECRET_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${SECRET_BINARY} COMMAND ${SECRET_BINARY})

set(DOCUMENT_STORE_BINARY euclid-emd-document-store-tests)

add_executable(${DOCUMENT_STORE_BINARY} DocumentStoreTest.cpp)
set_target_properties(${DOCUMENT_STORE_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${DOCUMENT_STORE_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${DOCUMENT_STORE_BINARY} COMMAND ${DOCUMENT_STORE_BINARY})

set(EVENT_BUS_MEMORY_BINARY euclid-eventbus-memory-tests)

add_executable(${EVENT_BUS_MEMORY_BINARY} EventBusMemoryTest.cpp)
set_target_properties(${EVENT_BUS_MEMORY_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${EVENT_BUS_MEMORY_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${EVENT_BUS_MEMORY_BINARY} COMMAND ${EVENT_BUS_MEMORY_BINARY})

set(MONITORING_BACKEND_BINARY euclid-emo-backend-tests)

add_executable(${MONITORING_BACKEND_BINARY} MonitoringBackendTest.cpp)
set_target_properties(${MONITORING_BACKEND_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${MONITORING_BACKEND_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${MONITORING_BACKEND_BINARY} COMMAND ${MONITORING_BACKEND_BINARY})

set(TTL_CACHE_BINARY euclid-ttl-cache-tests)

add_executable(${TTL_CACHE_BINARY} TtlCacheTest.cpp)
set_target_properties(${TTL_CACHE_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${TTL_CACHE_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${TTL_CACHE_BINARY} COMMAND ${TTL_CACHE_BINARY})

set(RETENTION_BINARY euclid-eqs-message-retention-tests)

add_executable(${RETENTION_BINARY} MessageRetentionTest.cpp)
set_target_properties(${RETENTION_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${RETENTION_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${RETENTION_BINARY} COMMAND ${RETENTION_BINARY})

set(CONTENT_TYPE_BINARY euclid-content-type-tests)

add_executable(${CONTENT_TYPE_BINARY} ContentTypeTest.cpp)
set_target_properties(${CONTENT_TYPE_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${CONTENT_TYPE_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${CONTENT_TYPE_BINARY} COMMAND ${CONTENT_TYPE_BINARY})

set(OIDC_BINARY euclid-oidc-tests)

add_executable(${OIDC_BINARY} OidcTest.cpp)
set_target_properties(${OIDC_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${OIDC_BINARY} PRIVATE euclidcore jwt-cpp::jwt-cpp OpenSSL::SSL OpenSSL::Crypto Boost::unit_test_framework)

add_test(NAME ${OIDC_BINARY} COMMAND ${OIDC_BINARY})

set(SAML_BINARY euclid-saml-tests)

add_executable(${SAML_BINARY} SamlTest.cpp)
set_target_properties(${SAML_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${SAML_BINARY} PRIVATE euclidcore LibXml2::LibXml2
        unofficial::xmlsec::xmlsec1 unofficial::xmlsec::xmlsec1-openssl
        OpenSSL::SSL OpenSSL::Crypto Boost::unit_test_framework)

add_test(NAME ${SAML_BINARY} COMMAND ${SAML_BINARY})

set(ONELOGIN_BINARY euclid-onelogin-tests)

add_executable(${ONELOGIN_BINARY} OneLoginTest.cpp ${CMAKE_SOURCE_DIR}/cli/src/eam/OneLogin.cpp)
set_target_properties(${ONELOGIN_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${ONELOGIN_BINARY} PRIVATE ${CMAKE_SOURCE_DIR}/cli/include)
target_link_libraries(${ONELOGIN_BINARY} PRIVATE euclidcore Boost::json OpenSSL::SSL OpenSSL::Crypto Boost::unit_test_framework)

add_test(NAME ${ONELOGIN_BINARY} COMMAND ${ONELOGIN_BINARY})

set(EKV_VALUE_BINARY euclid-ekv-value-tests)

add_executable(${EKV_VALUE_BINARY} EkvValueTest.cpp)
set_target_properties(${EKV_VALUE_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${EKV_VALUE_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${EKV_VALUE_BINARY} COMMAND ${EKV_VALUE_BINARY})

set(TOPIC_RETENTION_BINARY euclid-ens-topic-retention-tests)

add_executable(${TOPIC_RETENTION_BINARY} TopicRetentionTest.cpp)
set_target_properties(${TOPIC_RETENTION_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${TOPIC_RETENTION_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${TOPIC_RETENTION_BINARY} COMMAND ${TOPIC_RETENTION_BINARY})

set(TOPIC_DELIVERY_BINARY euclid-ens-topic-delivery-tests)

add_executable(${TOPIC_DELIVERY_BINARY} TopicDeliveryTest.cpp)
set_target_properties(${TOPIC_DELIVERY_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${TOPIC_DELIVERY_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${TOPIC_DELIVERY_BINARY} COMMAND ${TOPIC_DELIVERY_BINARY})

set(SOCKET_BIND_BINARY euclid-unix-socket-bind-tests)

add_executable(${SOCKET_BIND_BINARY} UnixSocketServerBindTest.cpp)
set_target_properties(${SOCKET_BIND_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${SOCKET_BIND_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${SOCKET_BIND_BINARY} COMMAND ${SOCKET_BIND_BINARY})
