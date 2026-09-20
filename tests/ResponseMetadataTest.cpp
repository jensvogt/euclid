// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE ResponseMetadataTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/ekm/CertificateResponse.h>
#include <euclid/dto/ekm/CreateKeyResponse.h>
#include <euclid/dto/ekm/DeleteCertificateResponse.h>
#include <euclid/dto/ekm/DeleteKeyResponse.h>
#include <euclid/dto/ekm/GetKeyResponse.h>
#include <euclid/dto/ekm/ListCertificatesResponse.h>
#include <euclid/dto/ekm/ListKeysResponse.h>
#include <euclid/dto/ekm/RevokeKeyResponse.h>
#include <euclid/dto/ekm/SetKeyDescriptionResponse.h>
#include <euclid/dto/eqs/CreateQueueResponse.h>
#include <euclid/dto/eqs/GetMessageAttributeResponse.h>
#include <euclid/dto/eqs/GetMessageCountResponse.h>
#include <euclid/dto/eqs/GetMessageMetadataResponse.h>
#include <euclid/dto/eqs/GetMessageResponse.h>
#include <euclid/dto/eqs/GetQueueErnResponse.h>
#include <euclid/dto/eqs/GetQueueResponse.h>
#include <euclid/dto/eqs/ListMessagesResponse.h>
#include <euclid/dto/eqs/ListQueueResponse.h>
#include <euclid/dto/eqs/ReceiveMessagesResponse.h>
#include <euclid/dto/eqs/SendMessageResponse.h>

// Every response DTO inherits region/accountId/user from BaseDto, and BaseDto's docstring says they
// are "serialized as a nested 'metadata' object". Inheriting them is not the same as sending them:
// a DTO's value_from writes the fields it names and nothing else, so a response could carry the
// members, populate them, and still put none of it on the wire.
//
// That is not hypothetical. EKM and EQS each carried a private applyMetadata() - three lines
// copying the authenticated user into those fields - that nothing ever called, and most of their
// response DTOs had no "metadata" in value_from to receive it if it had. Three EQS responses did
// serialize the object, and shipped {"region":"","user":"","accountId":""} on every call. None of
// it failed: an empty string serializes as happily as a full one.
//
// So these assert the wire format rather than the members. A response type that stops emitting
// metadata fails here, which is the regression that previously had nothing to trip over.

using Euclid::Dto::BaseDto;

namespace {

    constexpr auto kRegion = "eu-central-1";
    constexpr auto kAccountId = "000000000000";
    constexpr auto kUser = "jane.tester";

    // Serializes a response with its caller identity filled in and returns the "metadata" object,
    // failing the test if there is not one.
    template<typename Response>
    void assertCarriesCallerIdentity(const char *name) {

        Response response{};
        static_cast<BaseDto &>(response).region = kRegion;
        static_cast<BaseDto &>(response).accountId = kAccountId;
        static_cast<BaseDto &>(response).user = kUser;

        const auto parsed = boost::json::parse(response.toJson());

        BOOST_TEST_CONTEXT(name) {
            BOOST_TEST_REQUIRE(parsed.is_object());
            BOOST_TEST_REQUIRE(parsed.as_object().contains("metadata"));
            BOOST_TEST_REQUIRE(parsed.as_object().at("metadata").is_object());

            const auto &metadata = parsed.as_object().at("metadata").as_object();
            BOOST_TEST(std::string(metadata.at("region").as_string()) == kRegion);
            BOOST_TEST(std::string(metadata.at("accountId").as_string()) == kAccountId);
            BOOST_TEST(std::string(metadata.at("user").as_string()) == kUser);
        }
    }

}// namespace

BOOST_AUTO_TEST_CASE(EveryKeyManagementResponseCarriesTheCallerIdentity) {

    using namespace Euclid::Dto::EKM;

    assertCarriesCallerIdentity<CreateKeyResponse>("CreateKeyResponse");
    assertCarriesCallerIdentity<DeleteKeyResponse>("DeleteKeyResponse");
    assertCarriesCallerIdentity<RevokeKeyResponse>("RevokeKeyResponse");
    assertCarriesCallerIdentity<SetKeyDescriptionResponse>("SetKeyDescriptionResponse");
    assertCarriesCallerIdentity<GetKeyResponse>("GetKeyResponse");
    assertCarriesCallerIdentity<ListKeysResponse>("ListKeysResponse");
    assertCarriesCallerIdentity<CertificateResponse>("CertificateResponse");
    assertCarriesCallerIdentity<ListCertificatesResponse>("ListCertificatesResponse");
    assertCarriesCallerIdentity<DeleteCertificateResponse>("DeleteCertificateResponse");
}

BOOST_AUTO_TEST_CASE(EveryQueueResponseCarriesTheCallerIdentity) {

    using namespace Euclid::Dto::EQS;

    assertCarriesCallerIdentity<CreateQueueResponse>("CreateQueueResponse");
    assertCarriesCallerIdentity<GetQueueErnResponse>("GetQueueErnResponse");
    assertCarriesCallerIdentity<GetQueueResponse>("GetQueueResponse");
    assertCarriesCallerIdentity<GetMessageResponse>("GetMessageResponse");
    assertCarriesCallerIdentity<ListQueueResponse>("ListQueueResponse");
    assertCarriesCallerIdentity<ListMessagesResponse>("ListMessagesResponse");
    assertCarriesCallerIdentity<SendMessageResponse>("SendMessageResponse");
    assertCarriesCallerIdentity<ReceiveMessagesResponse>("ReceiveMessagesResponse");
    assertCarriesCallerIdentity<GetMessageCountResponse>("GetMessageCountResponse");
    assertCarriesCallerIdentity<GetMessageAttributeResponse>("GetMessageAttributeResponse");
    assertCarriesCallerIdentity<GetMessageMetadataResponse>("GetMessageMetadataResponse");
}

BOOST_AUTO_TEST_CASE(TheIdentityIsNestedRatherThanMixedInWithTheResponsesOwnFields) {

    // The nesting is the point of a "metadata" object rather than three more top-level keys: a
    // response's own fields stay where a caller expects them, and a DTO that happens to have a
    // field called "name" or "region" of its own does not collide with the caller's. EQS's
    // get-queue-metadata is exactly that case - its region and accountId describe the queue, not
    // whoever asked - which is why it is not in the lists above.
    Euclid::Dto::EKM::CreateKeyResponse response;
    response.name = "key-0001";
    response.region = kRegion;

    const auto parsed = boost::json::parse(response.toJson());
    const auto &object = parsed.as_object();

    BOOST_TEST(object.contains("name"));
    BOOST_TEST(std::string(object.at("name").as_string()) == "key-0001");

    // The caller's region is in the metadata object and nowhere else.
    BOOST_TEST(!object.contains("region"));
    BOOST_TEST(std::string(object.at("metadata").as_object().at("region").as_string()) == kRegion);
}

BOOST_AUTO_TEST_CASE(AResponseReadsBackTheIdentityItWasSent) {

    // The SDKs parse these, so the metadata has to survive a round trip rather than only being
    // written. GetMetadata() reads the same nested object value_from writes.
    Euclid::Dto::EKM::GetKeyResponse sent;
    sent.region = kRegion;
    sent.accountId = kAccountId;
    sent.user = kUser;
    sent.key.name = "key-0002";

    const auto received = boost::json::value_to<Euclid::Dto::EKM::GetKeyResponse>(
            boost::json::parse(sent.toJson()));

    BOOST_TEST(received.region == kRegion);
    BOOST_TEST(received.accountId == kAccountId);
    BOOST_TEST(received.user == kUser);
    BOOST_TEST(received.key.name == "key-0002");
}

BOOST_AUTO_TEST_CASE(AnIdentityNobodyFilledInIsEmptyRatherThanAbsent) {

    // What an unauthenticated or unwired path produces. Asserted so the difference between "no
    // metadata block" and "an empty one" stays visible: the second is what the three EQS
    // responses shipped for as long as nothing called applyMetadata().
    const Euclid::Dto::EKM::CreateKeyResponse response;
    const auto parsed = boost::json::parse(response.toJson());

    BOOST_TEST_REQUIRE(parsed.as_object().contains("metadata"));
    const auto &metadata = parsed.as_object().at("metadata").as_object();

    BOOST_TEST(std::string(metadata.at("region").as_string()).empty());
    BOOST_TEST(std::string(metadata.at("accountId").as_string()).empty());
    BOOST_TEST(std::string(metadata.at("user").as_string()).empty());
}
