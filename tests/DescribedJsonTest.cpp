// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// The JSON rules of DescribedJson.h, through the DTOs that use them.
//
// The EQS, ENS, ESM, EKV, EKM and ESS DTOs used to convert themselves with a hand-written
// tag_invoke pair each, and now list their members once (BOOST_DESCRIBE_STRUCT) and convert through
// DescribedJson.h. That conversion
// has to read and write exactly what the hand-written pairs did - clients built against them send
// partial JSON and rely on what a missing field becomes. These pin the rules down:
//
//   - a missing or null member becomes its type's zero, not its initializer;
//   - kJsonDefaults members keep their initializer instead;
//   - kJsonRequired members throw when missing or null;
//   - a response writes BaseDto under "metadata", and reads it back from there;
//   - a time_point travels as ISO-8601, and nested DTOs and vectors of them convert too.
//

#define BOOST_TEST_MODULE DescribedJsonTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <chrono>
#include <string>

// Euclid includes
#include <euclid/core/DateTimeUtils.h>
#include <euclid/dto/ekm/CertificateResponse.h>
#include <euclid/dto/ekm/CreateCertificateRequest.h>
#include <euclid/dto/ekm/DeleteKeyRequest.h>
#include <euclid/dto/ekm/GetKeyResponse.h>
#include <euclid/dto/ekm/ListCertificatesResponse.h>
#include <euclid/dto/ekm/model/Certificate.h>
#include <euclid/dto/ekm/model/Key.h>
#include <euclid/dto/ekv/CreateTableRequest.h>
#include <euclid/dto/ekv/PutItemRequest.h>
#include <euclid/dto/ekv/QueryRequest.h>
#include <euclid/dto/ekv/ScanRequest.h>
#include <euclid/dto/ekv/TableDescription.h>
#include <euclid/dto/eqs/CreateQueueRequest.h>
#include <euclid/dto/eqs/CreateQueueResponse.h>
#include <euclid/dto/eqs/ListQueueRequest.h>
#include <euclid/dto/eqs/ReceiveMessagesResponse.h>
#include <euclid/dto/eqs/SendMessageRequest.h>
#include <euclid/dto/eqs/SetMessageAttributeRequest.h>
#include <euclid/dto/eqs/model/Queue.h>
#include <euclid/dto/esm/CreateBucketResponse.h>
#include <euclid/dto/esm/ListObjectsResponse.h>
#include <euclid/dto/esm/model/Object.h>
#include <euclid/dto/ens/CreateTopicRequest.h>
#include <euclid/dto/ens/ListTopicsResponse.h>
#include <euclid/dto/ens/PublishMessageRequest.h>
#include <euclid/dto/ens/SetTopicDeleteAfterDeliveryRequest.h>
#include <euclid/dto/ess/CreateSecretRequest.h>
#include <euclid/dto/ess/GetSecretResponse.h>
#include <euclid/dto/ess/ListSecretsRequest.h>
#include <euclid/dto/ess/ListSecretsResponse.h>
#include <euclid/dto/ess/SecretNameRequest.h>
#include <euclid/dto/ess/SecretResponse.h>
#include <euclid/dto/ess/UpdateSecretRequest.h>
#include <euclid/dto/ess/model/Secret.h>

namespace EQS = Euclid::Dto::EQS;
namespace ENS = Euclid::Dto::ENS;
namespace ESM = Euclid::Dto::ESM;
namespace EKV = Euclid::Dto::EKV;
namespace EKM = Euclid::Dto::EKM;
namespace ESS = Euclid::Dto::ESS;
namespace json = boost::json;

BOOST_AUTO_TEST_CASE(MissingMemberBecomesZeroNotItsInitializer) {
    // pageSize starts at 10 and sortColumn at "name", but a client that leaves them out has always
    // been read as having sent 0 and "", and the list handlers act on that.
    const auto request = json::value_to<EQS::ListQueueRequest>(json::parse(R"({"prefix":"a"})"));
    BOOST_TEST(request.prefix == "a");
    BOOST_TEST(request.pageSize == 0);
    BOOST_TEST(request.sortColumn.empty());
    BOOST_TEST(!request.includeInternal);
}

BOOST_AUTO_TEST_CASE(NullMemberBecomesZero) {
    const auto request = json::value_to<EQS::ListQueueRequest>(json::parse(R"({"prefix":null,"pageSize":null})"));
    BOOST_TEST(request.prefix.empty());
    BOOST_TEST(request.pageSize == 0);
}

BOOST_AUTO_TEST_CASE(JsonDefaultsKeepTheirInitializer) {
    const auto request = json::value_to<EQS::CreateQueueRequest>(json::parse(R"({"name":"q","delay":null})"));
    BOOST_TEST(request.name == "q");
    BOOST_TEST(request.visibility == 30);
    BOOST_TEST(request.maxRetries == 3);
    BOOST_TEST(request.maxMessageLength == 1024 * 1024);
    BOOST_TEST(request.priority == "MEDIUM");
    // Not listed, so zero like any other.
    BOOST_TEST(request.delay == 0);

    const auto queue = json::value_to<EQS::Queue>(json::parse(R"({"name":"q"})"));
    BOOST_TEST(queue.visibility == 30);
    BOOST_TEST(queue.maxReceiveCount == 3);
}

BOOST_AUTO_TEST_CASE(JsonRequiredMemberThrowsWhenMissingOrNull) {
    // Without this a set-message-attribute request with no value would set the attribute to 0.
    BOOST_CHECK_THROW(json::value_to<EQS::SetMessageAttributeRequest>(json::parse(R"({"messageId":"m","key":"k"})")), std::exception);
    BOOST_CHECK_THROW(json::value_to<EQS::SetMessageAttributeRequest>(json::parse(R"({"messageId":"m","key":"k","value":null})")), std::exception);
    BOOST_CHECK_THROW(json::value_to<EQS::ReceiveMessagesResponse>(json::parse(R"({})")), std::exception);
}

BOOST_AUTO_TEST_CASE(WrongTypeThrows) {
    BOOST_CHECK_THROW(json::value_to<EQS::ListQueueRequest>(json::parse(R"({"pageSize":"ten"})")), std::exception);
    BOOST_CHECK_THROW(json::value_to<EQS::ListQueueRequest>(json::parse(R"({"prefix":5})")), std::exception);
}

BOOST_AUTO_TEST_CASE(ResponseCarriesMetadata) {
    EQS::CreateQueueResponse response;
    response.region = "eu-central-1";
    response.accountId = "000000000000";
    response.user = "admin";
    response.name = "q";
    response.ern = "ern:eqs:q";

    const auto written = json::parse(response.toJson());
    BOOST_TEST(written.at("metadata").at("region").as_string() == "eu-central-1");
    BOOST_TEST(written.at("name").as_string() == "q");
    BOOST_TEST(!written.as_object().contains("region"));

    const auto back = json::value_to<EQS::CreateQueueResponse>(written);
    BOOST_TEST(back.region == "eu-central-1");
    BOOST_TEST(back.user == "admin");
    BOOST_TEST(back.ern == "ern:eqs:q");
}

BOOST_AUTO_TEST_CASE(NestedDtosAndTimesRoundTrip) {
    EQS::ReceiveMessagesResponse response;
    EQS::Message message;
    message.messageId = "m1";
    message.size = 42;
    message.created = Euclid::Core::DateTimeUtils::FromISO8601("2026-11-15T10:11:12.345Z");
    response.messages.push_back(message);

    const auto written = json::parse(json::serialize(json::value_from(response)));
    BOOST_TEST(written.at("messages").at(0).at("created").as_string().starts_with("2026-11-15T10:11:12.345"));

    const auto back = json::value_to<EQS::ReceiveMessagesResponse>(written);
    BOOST_REQUIRE(back.messages.size() == 1);
    BOOST_TEST(back.messages[0].messageId == "m1");
    BOOST_TEST(back.messages[0].size == 42);
    BOOST_TEST((back.messages[0].created == message.created));
}

BOOST_AUTO_TEST_CASE(HandWrittenReaderStillWins) {
    // SendMessageRequest keeps its own reader - an empty priority means MEDIUM - and takes the
    // generic writer.
    const auto request = EQS::SendMessageRequest::fromJson(R"({"ern":"e","body":"b","priority":""})");
    BOOST_TEST(request.priority == "MEDIUM");
    BOOST_TEST(json::parse(request.toJson()).at("priority").as_string() == "MEDIUM");
}

BOOST_AUTO_TEST_CASE(EnsTopicMaxMessageLengthDefaultsToOneMebibyte) {
    // Read as zero, a topic created without the field would refuse every message published to it.
    const auto request = json::value_to<ENS::CreateTopicRequest>(json::parse(R"({"name":"t"})"));
    BOOST_TEST(request.maxMessageLength == 1024 * 1024);

    const auto topics = json::value_to<ENS::ListTopicsResponse>(json::parse(R"({"topics":[{"name":"t"}],"total":1})"));
    BOOST_REQUIRE(topics.topics.size() == 1);
    BOOST_TEST(topics.topics[0].maxMessageLength == 1024 * 1024);
}

BOOST_AUTO_TEST_CASE(EnsListResponseRequiresItsList) {
    BOOST_CHECK_THROW(json::value_to<ENS::ListTopicsResponse>(json::parse(R"({"total":0})")), std::exception);
}

BOOST_AUTO_TEST_CASE(EnsPublishKeepsItsOwnReader) {
    const auto request = ENS::PublishMessageRequest::fromJson(R"({"ern":"e","body":"m","priority":""})");
    BOOST_TEST(request.priority == "MEDIUM");
}

BOOST_AUTO_TEST_CASE(EnsRoundTrip) {
    ENS::SetTopicDeleteAfterDeliveryRequest request;
    request.ern = "ern:ens:t";
    request.deleteAfterDelivery = true;
    const auto back = ENS::SetTopicDeleteAfterDeliveryRequest::fromJson(request.toJson());
    BOOST_TEST(back.ern == "ern:ens:t");
    BOOST_TEST(back.deleteAfterDelivery);
}

BOOST_AUTO_TEST_CASE(EsmResponseReadsBackItsMetadata) {
    ESM::CreateBucketResponse response;
    response.name = "b";
    response.region = "eu-central-1";
    response.user = "admin";
    const auto written = json::parse(json::serialize(json::value_from(response)));
    BOOST_TEST(written.at("metadata").at("region").as_string() == "eu-central-1");
    BOOST_TEST(!written.as_object().contains("region"));

    const auto read = json::value_to<ESM::CreateBucketResponse>(written);
    BOOST_TEST(read.user == "admin");
    BOOST_TEST(read.name == "b");
}

BOOST_AUTO_TEST_CASE(EsmObjectKeepsItsOwnWriter) {
    // restoredUntil is written only when an archived object has been restored.
    ESM::Object object;
    object.key = "k";
    BOOST_TEST(!json::value_from(object).as_object().contains("restoredUntil"));
    object.restoredUntil = "2026-12-01T00:00:00Z";
    BOOST_TEST(json::value_from(object).at("restoredUntil").as_string() == "2026-12-01T00:00:00Z");

    // And lifecycleState stays STANDARD when the JSON says nothing about it.
    BOOST_TEST(json::value_to<ESM::Object>(json::parse(R"({"key":"k"})")).lifecycleState == "STANDARD");
}

BOOST_AUTO_TEST_CASE(EsmListObjectsReadsCommonPrefixesLeniently) {
    const auto response = json::value_to<ESM::ListObjectsResponse>(
            json::parse(R"({"objects":[],"total":0,"commonPrefixes":["a/",5,"b/"]})"));
    BOOST_REQUIRE(response.commonPrefixes.size() == 2);
    BOOST_TEST(response.commonPrefixes[1] == "b/");
}

BOOST_AUTO_TEST_CASE(EkvKeyAttributesKeepTheirJsonNames) {
    // The members are partitionKey and sortKey - the hand-written pair they replace wrote those
    // two keys while the members were called partitionKeyName and sortKeyName, so the rename is
    // what keeps the JSON a client already sends readable.
    EKV::CreateTableRequest request;
    request.name = "t";
    request.partitionKey = "pk";
    request.partitionKeyType = "number";
    request.sortKey = "sk";
    request.sortKeyType = "string";
    const auto written = json::parse(json::serialize(json::value_from(request)));
    BOOST_TEST(written.at("partitionKey").as_string() == "pk");
    BOOST_TEST(written.at("sortKey").as_string() == "sk");
    BOOST_TEST(!written.as_object().contains("partitionKeyName"));

    const auto back = json::value_to<EKV::CreateTableRequest>(written);
    BOOST_TEST(back.name == "t");
    BOOST_TEST(back.partitionKey == "pk");
    BOOST_TEST(back.sortKeyType == "string");

    // The same names on the answer side, which is where a client reads the schema back.
    EKV::TableDescription description;
    description.name = "t";
    description.partitionKey = "pk";
    description.itemCount = 7;
    BOOST_TEST(json::value_from(description).at("partitionKey").as_string() == "pk");
    BOOST_TEST(json::value_to<EKV::TableDescription>(json::parse(R"({"sortKey":"sk"})")).sortKey == "sk");
}

BOOST_AUTO_TEST_CASE(EkvFreeFormMembersRoundTrip) {
    // The item, the key and the scan bounds are free-form JSON, so they stay json::value members
    // and travel unchanged; absent, they read back null, as the hand-written reader left them.
    EKV::PutItemRequest put;
    put.table = "t";
    put.item = json::parse(R"({"pk":"a","n":5})");
    const auto written = json::parse(put.toJson());
    BOOST_TEST(written.at("item").at("n").as_int64() == 5);
    BOOST_TEST(json::value_to<EKV::PutItemRequest>(written).item.at("pk").as_string() == "a");

    const auto scan = json::value_to<EKV::ScanRequest>(json::parse(R"({"table":"t"})"));
    BOOST_TEST(scan.table == "t");
    BOOST_TEST(scan.attributeValue.is_null());
    BOOST_TEST(scan.attributeUpper.is_null());
}

BOOST_AUTO_TEST_CASE(EkvQueryForwardFollowsTheJsonNotItsInitializer) {
    // forward starts true, but a client that leaves it out has always been read as false, and the
    // query handler orders by that - the zero-value rule has to keep doing it.
    const auto request = json::value_to<EKV::QueryRequest>(json::parse(R"({"table":"t"})"));
    BOOST_TEST(!request.forward);
    BOOST_TEST(EKV::QueryRequest{}.forward);
}

BOOST_AUTO_TEST_CASE(EkmDeleteKeyRequestKeepsItsDefaultWindow) {
    // pendingWindowInDays starts at seven and the old reader passed 7 to GetLongValue, so a request
    // that leaves it out is still read as seven. Zero would be an error instead: the delete-key
    // handler refuses any window below a day.
    const auto omitted = json::value_to<EKM::DeleteKeyRequest>(json::parse(R"({"keyId":"k"})"));
    BOOST_TEST(omitted.keyId == "k");
    BOOST_TEST(omitted.pendingWindowInDays == 7);

    const auto given = json::value_to<EKM::DeleteKeyRequest>(json::parse(R"({"keyId":"k","pendingWindowInDays":30})"));
    BOOST_TEST(given.pendingWindowInDays == 30);
}

BOOST_AUTO_TEST_CASE(EkmKeyWritesDeletionDateOnlyWhenScheduled) {
    // The one EKM DTO that keeps its writer: a key with no deletion scheduled has never carried a
    // "deletionDate" on the wire, where the generic writer would put an empty one there.
    EKM::Key key;
    key.name = "key-0001";
    key.length = 256;
    key.status = "REVOKED";
    key.tags = {{"env", "test"}};

    const auto written = json::parse(json::serialize(json::value_from(key)));
    BOOST_TEST(!written.as_object().contains("deletionDate"));

    // Reading is the generic one, so every listed member comes back - including "length", which the
    // old hand-written pair wrote but only learned to read after a round trip had lost it.
    const auto back = json::value_to<EKM::Key>(written);
    BOOST_TEST(back.name == "key-0001");
    BOOST_TEST(back.length == 256);
    BOOST_TEST(back.status == "REVOKED");
    BOOST_TEST(back.tags.at("env") == "test");

    key.deletionDate = "2026-12-01T00:00:00Z";
    BOOST_TEST(json::value_from(key).at("deletionDate").as_string() == "2026-12-01T00:00:00Z");
}

BOOST_AUTO_TEST_CASE(EkmCreateCertificateClampsItsValidityDefaults) {
    // A validity period or key length of zero or less has always been replaced by the default, so
    // that a request that sends neither - or sends nonsense - still gets a usable certificate. The
    // hand-written reader keeps that rule; the writer it lost is the generic one.
    const auto request = json::value_to<EKM::CreateCertificateRequest>(
            json::parse(R"({"name":"web","validDays":0,"keyBits":-1})"));
    BOOST_TEST(request.name == "web");
    BOOST_TEST(request.validDays == 825);
    BOOST_TEST(request.keyBits == 2048);
    BOOST_TEST(json::value_from(request).at("validDays").as_int64() == 825);
}

BOOST_AUTO_TEST_CASE(EkmCertificateCarriesItsDatesAndContainers) {
    EKM::Certificate certificate;
    certificate.name = "web";
    certificate.generated = true;
    certificate.subjectAltNames = {"DNS:localhost", "IP:127.0.0.1"};
    certificate.tags = {{"env", "test"}};
    certificate.notAfter = Euclid::Core::DateTimeUtils::FromISO8601("2027-01-01T00:00:00Z");

    const auto written = json::parse(json::serialize(json::value_from(certificate)));
    BOOST_TEST(written.at("notAfter").as_string().starts_with("2027-01-01T00:00:00"));
    BOOST_TEST(written.at("subjectAltNames").as_array().size() == 2);

    const auto back = json::value_to<EKM::Certificate>(written);
    BOOST_TEST(back.generated);
    BOOST_TEST(back.subjectAltNames[1] == "IP:127.0.0.1");
    BOOST_TEST(back.tags.at("env") == "test");
    BOOST_TEST((back.notAfter == certificate.notAfter));
}

BOOST_AUTO_TEST_CASE(EkmResponsesRequireTheObjectTheyWrap) {
    // The old readers took the nested object with at(), which throws when it is missing; without
    // kJsonRequired the generic reader would answer with an empty key or an empty listing instead.
    BOOST_REQUIRE_THROW(json::value_to<EKM::GetKeyResponse>(json::parse(R"({"total":1})")), boost::system::system_error);
    BOOST_REQUIRE_THROW(json::value_to<EKM::ListCertificatesResponse>(json::parse(R"({"total":1})")), boost::system::system_error);

    const auto response = json::value_to<EKM::CertificateResponse>(
            json::parse(R"({"certificate":{"name":"web","generated":true}})"));
    BOOST_TEST(response.certificate.name == "web");
    BOOST_TEST(response.certificate.generated);
}

BOOST_AUTO_TEST_CASE(EssSecretCarriesItsDatesAndTags) {
    // The model behind every ESS answer. Three instants, a tag map and the rotation counter are
    // what a caller auditing "which secrets nobody has rotated in a year" reads, and none of them
    // was named by hand any more than the rest.
    ESS::Secret secret;
    secret.name = "db-password";
    secret.encryptionKeyErn = "ern:euclid:ekm:000000000000:key-0001";
    secret.version = 3;
    secret.tags = {{"env", "test"}};
    secret.rotated = Euclid::Core::DateTimeUtils::FromISO8601("2026-11-01T10:00:00Z");

    const auto written = json::parse(json::serialize(json::value_from(secret)));
    BOOST_TEST(written.at("rotated").as_string().starts_with("2026-11-01T10:00:00"));
    BOOST_TEST(std::string(written.at("tags").as_object().at("env").as_string()) == "test");

    const auto back = json::value_to<ESS::Secret>(written);
    BOOST_TEST(back.name == "db-password");
    BOOST_TEST(back.encryptionKeyErn == secret.encryptionKeyErn);
    BOOST_TEST(back.version == 3);
    BOOST_TEST(back.tags.at("env") == "test");
    BOOST_TEST((back.rotated == secret.rotated));

    // A listing entry whose value has never been recorded as rotated: absent, the instant is the
    // epoch and the tags are empty, which is what the hand-written reader answered.
    const auto sparse = json::value_to<ESS::Secret>(json::parse(R"({"name":"api-token"})"));
    BOOST_TEST((sparse.rotated == std::chrono::system_clock::time_point{}));
    BOOST_TEST(sparse.tags.empty());
}

BOOST_AUTO_TEST_CASE(EssListSecretsRequestFollowsTheJsonNotItsInitializer) {
    // pageSize and sortColumn start at 10 and "name", and a request that names neither has always
    // been read as 0 and "" - the old reader passed them straight through GetLongValue and
    // GetStringValue. The list-secrets handler pages and orders by whatever it is given, so the
    // reset is the behaviour a caller depends on, not an accident to preserve by luck.
    const auto request = json::value_to<ESS::ListSecretsRequest>(json::parse(R"({"prefix":"acme"})"));
    BOOST_TEST(request.prefix == "acme");
    BOOST_TEST(request.pageSize == 0);
    BOOST_TEST(request.sortColumn.empty());
    BOOST_TEST(ESS::ListSecretsRequest{}.pageSize == 10);
    BOOST_TEST(ESS::ListSecretsRequest{}.sortColumn == "name");
}

BOOST_AUTO_TEST_CASE(EssUpdateSecretKeepsAbsentApartFromEmpty) {
    // The one ESS DTO that keeps its hand-written pair. hasValue and hasDescription record whether
    // the JSON named the field at all, which is what separates a rotation from a change of
    // description, and an empty description is a legitimate way to clear one.
    const auto rotation = json::value_to<ESS::UpdateSecretRequest>(json::parse(R"({"name":"db","value":""})"));
    BOOST_TEST(rotation.hasValue);
    BOOST_TEST(!rotation.hasDescription);

    const auto cleared = json::value_to<ESS::UpdateSecretRequest>(json::parse(R"({"name":"db","description":""})"));
    BOOST_TEST(!cleared.hasValue);
    BOOST_TEST(cleared.hasDescription);
    BOOST_TEST(cleared.description.empty());

    // And the writer sends only what those flags say was given, so the CLI asking for a rotation
    // cannot arrive as a change of description as well.
    ESS::UpdateSecretRequest request;
    request.name = "db";
    request.hasValue = true;
    request.value = "rotated";
    const auto written = json::parse(request.toJson());
    BOOST_TEST(std::string(written.at("value").as_string()) == "rotated");
    BOOST_TEST(!written.as_object().contains("description"));
    BOOST_TEST(!written.as_object().contains("keyErn"));
}

BOOST_AUTO_TEST_CASE(EssCreateSecretLeavesTheKeyFieldPresentButEmpty) {
    // keyErn empty is the signal to use the namespace's own secrets key, so the field has to come
    // back as the empty string rather than being invented by the reader - and the writer keeps
    // naming it, as the CLI's create-secret has always done.
    const auto omitted = json::value_to<ESS::CreateSecretRequest>(json::parse(R"({"name":"db","value":"hunter2"})"));
    BOOST_TEST(omitted.keyErn.empty());

    ESS::CreateSecretRequest request;
    request.name = "db";
    request.value = "hunter2";
    BOOST_TEST(json::parse(request.toJson()).as_object().contains("keyErn"));
}

BOOST_AUTO_TEST_CASE(EssResponsesRequireTheSecretTheyWrap) {
    // The old readers took the nested object with at(), which throws when it is missing; without
    // kJsonRequired the generic reader would answer with an empty secret or an empty listing, and
    // a caller reading a listing would see "no secrets" where the answer was malformed.
    BOOST_REQUIRE_THROW(json::value_to<ESS::SecretResponse>(json::parse(R"({"total":1})")), boost::system::system_error);
    BOOST_REQUIRE_THROW(json::value_to<ESS::ListSecretsResponse>(json::parse(R"({"total":1})")), boost::system::system_error);
    BOOST_REQUIRE_THROW(json::value_to<ESS::GetSecretResponse>(json::parse(R"({"value":"hunter2"})")), boost::system::system_error);

    const auto response = json::value_to<ESS::GetSecretResponse>(
            json::parse(R"({"secret":{"name":"db","version":2},"value":"hunter2"})"));
    BOOST_TEST(response.secret.name == "db");
    BOOST_TEST(response.secret.version == 2);
    BOOST_TEST(response.value == "hunter2");
}
