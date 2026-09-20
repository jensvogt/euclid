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

# ---------------------------------------------------------------------------------------------
# EAG route type
# ---------------------------------------------------------------------------------------------
set(ROUTE_TYPE_BINARY euclid-eag-route-type-tests)

add_executable(${ROUTE_TYPE_BINARY} RouteTypeTest.cpp)
set_target_properties(${ROUTE_TYPE_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${ROUTE_TYPE_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${ROUTE_TYPE_BINARY} COMMAND ${ROUTE_TYPE_BINARY})

# ---------------------------------------------------------------------------------------------
# EAG upload key
# ---------------------------------------------------------------------------------------------
set(UPLOAD_TARGET_BINARY euclid-eag-upload-target-tests)

add_executable(${UPLOAD_TARGET_BINARY} UploadTargetTest.cpp ${CMAKE_SOURCE_DIR}/modules/eag/src/UploadTarget.cpp)
set_target_properties(${UPLOAD_TARGET_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${UPLOAD_TARGET_BINARY} PRIVATE ${CMAKE_SOURCE_DIR}/modules/eag/include)
target_link_libraries(${UPLOAD_TARGET_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${UPLOAD_TARGET_BINARY} COMMAND ${UPLOAD_TARGET_BINARY})

# ---------------------------------------------------------------------------------------------
# EAG upload streaming
# ---------------------------------------------------------------------------------------------
set(UPLOAD_STREAM_BINARY euclid-eag-upload-stream-tests)

add_executable(${UPLOAD_STREAM_BINARY} UploadStreamTest.cpp
        ${CMAKE_SOURCE_DIR}/modules/eag/src/ProxyServer.cpp
        ${CMAKE_SOURCE_DIR}/modules/eag/src/ClientStream.cpp
        ${CMAKE_SOURCE_DIR}/modules/eag/src/RouteTable.cpp
        ${CMAKE_SOURCE_DIR}/modules/eag/src/Backends.cpp
        ${CMAKE_SOURCE_DIR}/modules/eag/src/BasicAuthenticator.cpp
        ${CMAKE_SOURCE_DIR}/modules/eag/src/ListenerCertificate.cpp
        ${CMAKE_SOURCE_DIR}/modules/eag/src/ModuleCall.cpp
        ${CMAKE_SOURCE_DIR}/modules/eag/src/UploadTarget.cpp)
set_target_properties(${UPLOAD_STREAM_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${UPLOAD_STREAM_BINARY} PRIVATE ${CMAKE_SOURCE_DIR}/modules/eag/include)
target_link_libraries(${UPLOAD_STREAM_BINARY} PRIVATE eucliddb euclidcore Boost::unit_test_framework OpenSSL::SSL OpenSSL::Crypto)

add_test(NAME ${UPLOAD_STREAM_BINARY} COMMAND ${UPLOAD_STREAM_BINARY})

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

set(TRANSFER_AUTHORIZER_BINARY euclid-transfer-authorizer-tests)

add_executable(${TRANSFER_AUTHORIZER_BINARY} TransferAuthorizerTest.cpp)
set_target_properties(${TRANSFER_AUTHORIZER_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${TRANSFER_AUTHORIZER_BINARY} PRIVATE euclidtransfer Boost::unit_test_framework)

add_test(NAME ${TRANSFER_AUTHORIZER_BINARY} COMMAND ${TRANSFER_AUTHORIZER_BINARY})

set(TRANSFER_DEFINITION_BINARY euclid-transfer-definition-change-tests)

add_executable(${TRANSFER_DEFINITION_BINARY} TransferDefinitionChangeTest.cpp)
set_target_properties(${TRANSFER_DEFINITION_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${TRANSFER_DEFINITION_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${TRANSFER_DEFINITION_BINARY} COMMAND ${TRANSFER_DEFINITION_BINARY})

set(TRANSFER_SCOPE_BINARY euclid-transfer-storage-scope-tests)

add_executable(${TRANSFER_SCOPE_BINARY} TransferStorageScopeTest.cpp)
set_target_properties(${TRANSFER_SCOPE_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${TRANSFER_SCOPE_BINARY} PRIVATE euclidtransfer Boost::unit_test_framework)

add_test(NAME ${TRANSFER_SCOPE_BINARY} COMMAND ${TRANSFER_SCOPE_BINARY})

set(QUEUE_OWNERSHIP_BINARY euclid-eqs-queue-ownership-tests)

add_executable(${QUEUE_OWNERSHIP_BINARY} QueueOwnershipTest.cpp)
set_target_properties(${QUEUE_OWNERSHIP_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${QUEUE_OWNERSHIP_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${QUEUE_OWNERSHIP_BINARY} COMMAND ${QUEUE_OWNERSHIP_BINARY})

set(CLI_COMPLETION_BINARY euclid-cli-completion-tests)

add_executable(${CLI_COMPLETION_BINARY} CliCompletionTest.cpp ../cli/src/help/CliCompletion.cpp)
set_target_properties(${CLI_COMPLETION_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(${CLI_COMPLETION_BINARY} PRIVATE ../cli/include)

target_link_libraries(${CLI_COMPLETION_BINARY} PRIVATE Boost::unit_test_framework)

add_test(NAME ${CLI_COMPLETION_BINARY} COMMAND ${CLI_COMPLETION_BINARY})

set(QUEUE_COUNTER_BINARY euclid-eqs-queue-counter-tests)

add_executable(${QUEUE_COUNTER_BINARY} QueueCounterTest.cpp)
set_target_properties(${QUEUE_COUNTER_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${QUEUE_COUNTER_BINARY} PRIVATE eucliddb Boost::unit_test_framework Boost::thread)

add_test(NAME ${QUEUE_COUNTER_BINARY} COMMAND ${QUEUE_COUNTER_BINARY})

set(BUCKET_COUNTER_BINARY euclid-esm-bucket-counter-tests)

add_executable(${BUCKET_COUNTER_BINARY} BucketCounterTest.cpp)
set_target_properties(${BUCKET_COUNTER_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${BUCKET_COUNTER_BINARY} PRIVATE eucliddb Boost::unit_test_framework Boost::thread)

add_test(NAME ${BUCKET_COUNTER_BINARY} COMMAND ${BUCKET_COUNTER_BINARY})

set(PURGE_PAGING_BINARY euclid-esm-purge-paging-tests)

add_executable(${PURGE_PAGING_BINARY} PurgePagingTest.cpp)
set_target_properties(${PURGE_PAGING_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${PURGE_PAGING_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${PURGE_PAGING_BINARY} COMMAND ${PURGE_PAGING_BINARY})

set(PURGE_JOB_BINARY euclid-esm-purge-job-tests)

add_executable(${PURGE_JOB_BINARY} PurgeJobTest.cpp)
set_target_properties(${PURGE_JOB_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${PURGE_JOB_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${PURGE_JOB_BINARY} COMMAND ${PURGE_JOB_BINARY})

set(STICKY_CALL_BINARY euclid-sticky-call-tests)

add_executable(${STICKY_CALL_BINARY} StickyCallTest.cpp)
set_target_properties(${STICKY_CALL_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(${STICKY_CALL_BINARY} PRIVATE ${CMAKE_SOURCE_DIR}/extern/common/include)

target_link_libraries(${STICKY_CALL_BINARY} PRIVATE eucliddb euclidcore Boost::unit_test_framework)

add_test(NAME ${STICKY_CALL_BINARY} COMMAND ${STICKY_CALL_BINARY})

set(BACKLOG_TARGET_BINARY euclid-backlog-target-tests)

add_executable(${BACKLOG_TARGET_BINARY} BacklogTargetTest.cpp)
set_target_properties(${BACKLOG_TARGET_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_include_directories(${BACKLOG_TARGET_BINARY} PRIVATE ${CMAKE_SOURCE_DIR}/main/include)

target_link_libraries(${BACKLOG_TARGET_BINARY} PRIVATE Boost::unit_test_framework)

add_test(NAME ${BACKLOG_TARGET_BINARY} COMMAND ${BACKLOG_TARGET_BINARY})

set(GATEWAY_MODULES_BINARY euclid-gateway-module-list-tests)

add_executable(${GATEWAY_MODULES_BINARY} GatewayModuleListTest.cpp)
set_target_properties(${GATEWAY_MODULES_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
target_compile_definitions(${GATEWAY_MODULES_BINARY} PRIVATE EUCLID_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(${GATEWAY_MODULES_BINARY} PRIVATE Boost::unit_test_framework)

add_test(NAME ${GATEWAY_MODULES_BINARY} COMMAND ${GATEWAY_MODULES_BINARY})

set(AUDIT_EVENT_BINARY euclid-audit-event-tests)

add_executable(${AUDIT_EVENT_BINARY} AuditEventTest.cpp)
set_target_properties(${AUDIT_EVENT_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${AUDIT_EVENT_BINARY} PRIVATE eucliddb euclidcore Boost::unit_test_framework)

add_test(NAME ${AUDIT_EVENT_BINARY} COMMAND ${AUDIT_EVENT_BINARY})

set(BACKGROUND_WORK_BINARY euclid-background-work-tests)

add_executable(${BACKGROUND_WORK_BINARY} BackgroundWorkTest.cpp)
set_target_properties(${BACKGROUND_WORK_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${BACKGROUND_WORK_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${BACKGROUND_WORK_BINARY} COMMAND ${BACKGROUND_WORK_BINARY})

set(UPLOAD_STATUS_BINARY euclid-object-upload-status-tests)

add_executable(${UPLOAD_STATUS_BINARY} ObjectUploadStatusTest.cpp)
set_target_properties(${UPLOAD_STATUS_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${UPLOAD_STATUS_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${UPLOAD_STATUS_BINARY} COMMAND ${UPLOAD_STATUS_BINARY})

set(ENS_RESEND_BINARY euclid-ens-resend-tests)

add_executable(${ENS_RESEND_BINARY} EnsResendTest.cpp)
set_target_properties(${ENS_RESEND_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${ENS_RESEND_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${ENS_RESEND_BINARY} COMMAND ${ENS_RESEND_BINARY})

set(APPLICATION_LOAD_BINARY euclid-application-load-tests)

add_executable(${APPLICATION_LOAD_BINARY} ApplicationLoadTest.cpp)
set_target_properties(${APPLICATION_LOAD_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${APPLICATION_LOAD_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${APPLICATION_LOAD_BINARY} COMMAND ${APPLICATION_LOAD_BINARY})

set(MODULE_DOCUMENT_BINARY euclid-module-document-tests)

add_executable(${MODULE_DOCUMENT_BINARY} ModuleDocumentTest.cpp)
set_target_properties(${MODULE_DOCUMENT_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${MODULE_DOCUMENT_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${MODULE_DOCUMENT_BINARY} COMMAND ${MODULE_DOCUMENT_BINARY})

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

# ---------------------------------------------------------------------------------------------
# JSON log format
# ---------------------------------------------------------------------------------------------
set(JSON_LOG_BINARY euclid-json-log-format-tests)

add_executable(${JSON_LOG_BINARY} JsonLogFormatTest.cpp)
set_target_properties(${JSON_LOG_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${JSON_LOG_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${JSON_LOG_BINARY} COMMAND ${JSON_LOG_BINARY})

set(SINK_LEVEL_BINARY euclid-sink-level-tests)

add_executable(${SINK_LEVEL_BINARY} SinkLevelTest.cpp)
set_target_properties(${SINK_LEVEL_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${SINK_LEVEL_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${SINK_LEVEL_BINARY} COMMAND ${SINK_LEVEL_BINARY})

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

set(EMM_EXPORT_IMPORT_BINARY euclid-emm-export-import-tests)

add_executable(${EMM_EXPORT_IMPORT_BINARY} EmmExportImportTest.cpp ${CMAKE_SOURCE_DIR}/modules/emm/src/ExportImport.cpp)
set_target_properties(${EMM_EXPORT_IMPORT_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${EMM_EXPORT_IMPORT_BINARY} PRIVATE ${CMAKE_SOURCE_DIR}/modules/emm/include)
target_link_libraries(${EMM_EXPORT_IMPORT_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${EMM_EXPORT_IMPORT_BINARY} COMMAND ${EMM_EXPORT_IMPORT_BINARY})

set(MAX_MESSAGE_LENGTH_BINARY euclid-max-message-length-tests)

add_executable(${MAX_MESSAGE_LENGTH_BINARY} MaxMessageLengthTest.cpp)
set_target_properties(${MAX_MESSAGE_LENGTH_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${MAX_MESSAGE_LENGTH_BINARY} PRIVATE eucliddto Boost::unit_test_framework)

add_test(NAME ${MAX_MESSAGE_LENGTH_BINARY} COMMAND ${MAX_MESSAGE_LENGTH_BINARY})

set(PERMISSION_VOCABULARY_BINARY euclid-permission-vocabulary-tests)

add_executable(${PERMISSION_VOCABULARY_BINARY} PermissionVocabularyTest.cpp)
set_target_properties(${PERMISSION_VOCABULARY_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

# This test reads the module sources rather than linking them - what it checks is the text of the
# dispatch tables against Core::Permissions::All().
target_compile_definitions(${PERMISSION_VOCABULARY_BINARY} PRIVATE EUCLID_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(${PERMISSION_VOCABULARY_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${PERMISSION_VOCABULARY_BINARY} COMMAND ${PERMISSION_VOCABULARY_BINARY})

set(BUILTIN_ROLE_BINARY euclid-builtin-role-tests)

add_executable(${BUILTIN_ROLE_BINARY} BuiltinRoleTest.cpp)
set_target_properties(${BUILTIN_ROLE_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${BUILTIN_ROLE_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${BUILTIN_ROLE_BINARY} COMMAND ${BUILTIN_ROLE_BINARY})

set(AUTHORIZATION_BINARY euclid-authorization-tests)

add_executable(${AUTHORIZATION_BINARY} AuthorizationTest.cpp)
set_target_properties(${AUTHORIZATION_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${AUTHORIZATION_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${AUTHORIZATION_BINARY} COMMAND ${AUTHORIZATION_BINARY})

set(ROLE_REPOSITORY_BINARY euclid-role-repository-tests)

add_executable(${ROLE_REPOSITORY_BINARY} RoleRepositoryTest.cpp)
set_target_properties(${ROLE_REPOSITORY_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${ROLE_REPOSITORY_BINARY} PRIVATE eucliddb Boost::unit_test_framework)

add_test(NAME ${ROLE_REPOSITORY_BINARY} COMMAND ${ROLE_REPOSITORY_BINARY})

set(AUTHORIZATION_GATE_BINARY euclid-authorization-gate-tests)

add_executable(${AUTHORIZATION_GATE_BINARY} AuthorizationGateTest.cpp)
set_target_properties(${AUTHORIZATION_GATE_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${AUTHORIZATION_GATE_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${AUTHORIZATION_GATE_BINARY} COMMAND ${AUTHORIZATION_GATE_BINARY})

set(RESPONSE_METADATA_BINARY euclid-response-metadata-tests)

add_executable(${RESPONSE_METADATA_BINARY} ResponseMetadataTest.cpp)
set_target_properties(${RESPONSE_METADATA_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_link_libraries(${RESPONSE_METADATA_BINARY} PRIVATE eucliddto Boost::unit_test_framework)

add_test(NAME ${RESPONSE_METADATA_BINARY} COMMAND ${RESPONSE_METADATA_BINARY})

set(APPLICATION_CLEANUP_BINARY euclid-application-cleanup-tests)

add_executable(${APPLICATION_CLEANUP_BINARY} ApplicationCleanupTest.cpp)
set_target_properties(${APPLICATION_CLEANUP_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${APPLICATION_CLEANUP_BINARY} PRIVATE ${CMAKE_SOURCE_DIR}/main/include)
target_link_libraries(${APPLICATION_CLEANUP_BINARY} PRIVATE euclidcore Boost::unit_test_framework)

add_test(NAME ${APPLICATION_CLEANUP_BINARY} COMMAND ${APPLICATION_CLEANUP_BINARY})

set(BACKUP_RETENTION_BINARY euclid-backup-retention-tests)

add_executable(${BACKUP_RETENTION_BINARY} BackupRetentionTest.cpp)
set_target_properties(${BACKUP_RETENTION_BINARY} PROPERTIES CXX_SCAN_FOR_MODULES OFF)

target_include_directories(${BACKUP_RETENTION_BINARY} PRIVATE ${CMAKE_SOURCE_DIR}/modules/emm/include)
target_link_libraries(${BACKUP_RETENTION_BINARY} PRIVATE Boost::unit_test_framework)

add_test(NAME ${BACKUP_RETENTION_BINARY} COMMAND ${BACKUP_RETENTION_BINARY})
