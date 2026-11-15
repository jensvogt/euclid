// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// The JSON rules of DescribedJson.h, through the EQS DTOs that use them.
//
// The EQS, ENS and ESM DTOs used to convert themselves with a hand-written tag_invoke pair each, and now list
// their members once (BOOST_DESCRIBE_STRUCT) and convert through DescribedJson.h. That conversion
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

namespace EQS = Euclid::Dto::EQS;
namespace ENS = Euclid::Dto::ENS;
namespace ESM = Euclid::Dto::ESM;
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
