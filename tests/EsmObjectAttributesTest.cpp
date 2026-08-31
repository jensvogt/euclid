#define BOOST_TEST_MODULE EsmObjectAttributesTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <ranges>

// Euclid includes
#include <euclid/database/entity/esm/Object.h>
#include <euclid/dto/esm/EsmMapper.h>
#include <euclid/dto/esm/SetObjectAttributesRequest.h>
#include <euclid/dto/esm/model/Object.h>
#include <TransferStorage.h>

using Euclid::Database::Entity::COM::Binary;
using Euclid::Database::Entity::COM::Variant;
using Euclid::Dto::ESM::EsmMapper;

// An object attribute is a typed value, and the type is the point: the storage module never looks
// at these, so whatever a client put in has to come back out as the same C++ type - through
// MongoDB, through the entity-to-DTO mapping, and through JSON.

namespace {

    Euclid::Database::Entity::ESM::Object objectWithAttributes() {
        Euclid::Database::Entity::ESM::Object object;
        object.bucketErn = "ern:esm:eu-central-1:000000000000:development:bucket:transfer";
        object.key = "mix/PIM-4269.xml";
        object.size = 45242;
        object.attributes["source"] = Variant(std::string("pim"));
        object.attributes["revision"] = Variant(7L);
        object.attributes["ratio"] = Variant(0.25);
        object.attributes["final"] = Variant(true);
        object.attributes["thumbnail"] = Variant(Binary{0x00, 0x01, 0xFE, 0xFF});
        return object;
    }

}// namespace

BOOST_AUTO_TEST_CASE(AttributesSurviveABsonRoundTrip) {
    const auto object = objectWithAttributes();

    const auto document = object.toDocument();
    const auto restored = Euclid::Database::Entity::ESM::Object::fromDocument(document.view());

    BOOST_TEST_REQUIRE(restored.attributes.size() == 5U);
    BOOST_TEST(restored.attributes.at("source").get<std::string>() == "pim");
    BOOST_TEST(restored.attributes.at("revision").get<long>() == 7L);
    BOOST_TEST(restored.attributes.at("ratio").get<double>() == 0.25);
    BOOST_TEST(restored.attributes.at("final").get<bool>());
    BOOST_TEST(restored.attributes.at("thumbnail").get<Binary>() == (Binary{0x00, 0x01, 0xFE, 0xFF}));
}

BOOST_AUTO_TEST_CASE(MapperCarriesAttributesToTheDto) {
    const auto dto = EsmMapper::toDto(objectWithAttributes());

    BOOST_TEST_REQUIRE(dto.attributes.size() == 5U);
    BOOST_TEST(dto.attributes.at("source").get<std::string>() == "pim");
    BOOST_TEST(dto.attributes.at("revision").get<long>() == 7L);
    BOOST_TEST(dto.attributes.at("final").get<bool>());
    // Binary crosses the entity/DTO boundary as a different byte type, hence the explicit copy in
    // the mapper rather than a plain assignment.
    BOOST_TEST(dto.attributes.at("thumbnail").get<Euclid::Dto::COM::Binary>().size() == 4U);
}

BOOST_AUTO_TEST_CASE(AttributesSurviveAJsonRoundTrip) {
    const auto dto = EsmMapper::toDto(objectWithAttributes());

    const auto restored = Euclid::Dto::ESM::Object::fromJson(dto.toJson());

    BOOST_TEST_REQUIRE(restored.attributes.size() == 5U);
    BOOST_TEST(restored.attributes.at("source").get<std::string>() == "pim");
    BOOST_TEST(restored.attributes.at("revision").get<long>() == 7L);
    BOOST_TEST(restored.attributes.at("ratio").get<double>() == 0.25);
    BOOST_TEST(restored.attributes.at("final").get<bool>());
    BOOST_TEST(restored.attributes.at("thumbnail").get<Euclid::Dto::COM::Binary>() == (Euclid::Dto::COM::Binary{0x00, 0x01, 0xFE, 0xFF}));
}

BOOST_AUTO_TEST_CASE(SetObjectAttributesRequestCarriesTypedValues) {
    Euclid::Dto::ESM::SetObjectAttributesRequest request;
    request.ern = "ern:esm:eu-central-1:000000000000:development:object:transfer/mix/PIM-4269.xml";
    request.attributes["source"] = Euclid::Dto::COM::Variant(std::string("pim"));
    request.attributes["revision"] = Euclid::Dto::COM::Variant(7L);
    request.attributes["final"] = Euclid::Dto::COM::Variant(true);

    const auto restored = Euclid::Dto::ESM::SetObjectAttributesRequest::fromJson(request.toJson());

    BOOST_TEST(restored.ern == request.ern);
    BOOST_TEST_REQUIRE(restored.attributes.size() == 3U);
    BOOST_TEST(restored.attributes.at("source").get<std::string>() == "pim");
    BOOST_TEST(restored.attributes.at("revision").get<long>() == 7L);
    BOOST_TEST(restored.attributes.at("final").get<bool>());

    // The handler replaces the object's whole map with what it parsed, so a request carrying no
    // attributes at all has to parse as empty rather than as "leave them alone" - that is how an
    // object's attributes are cleared.
    const auto cleared = Euclid::Dto::ESM::SetObjectAttributesRequest::fromJson(R"({"ern":"ern:esm:x"})");
    BOOST_TEST(cleared.attributes.empty());
}

BOOST_AUTO_TEST_CASE(MapperRoundTripsAttributesBackToTheEntity) {
    // set-object-attributes goes the other way through the mapper than a listing does.
    for (const auto &original: objectWithAttributes().attributes | std::views::values) {
        const auto restored = EsmMapper::toEntity(EsmMapper::toDto(original));
        BOOST_TEST((restored.value.index() == original.value.index()));
    }

    const auto binary = EsmMapper::toEntity(EsmMapper::toDto(Variant(Binary{0x00, 0x01, 0xFE, 0xFF})));
    BOOST_TEST(binary.get<Binary>() == (Binary{0x00, 0x01, 0xFE, 0xFF}));
}

BOOST_AUTO_TEST_CASE(TransferProvenanceHeaderIsReadableAsAttributes) {
    // The contract between a transfer server and put-object: what AttributesHeader() writes into
    // x-euclid-attributes is what ESM's handler parses back out, element by element.
    const auto header = Euclid::Transfer::AttributesHeader({{"transferServer", "ftp-server"}, {"transferUser", "admin"}});
    BOOST_TEST_REQUIRE(!header.empty());

    std::map<std::string, Variant> attributes;
    for (const auto parsed = boost::json::parse(header); const auto &element: parsed.as_object()) {
        attributes[std::string(element.key())] = EsmMapper::toEntity(boost::json::value_to<Euclid::Dto::COM::Variant>(element.value()));
    }

    BOOST_TEST_REQUIRE(attributes.size() == 2U);
    BOOST_TEST(attributes.at("transferServer").get<std::string>() == "ftp-server");
    BOOST_TEST(attributes.at("transferUser").get<std::string>() == "admin");

    // Nothing to say means no header at all, rather than an empty JSON object the handler would
    // then have to treat as "clear the attributes".
    BOOST_TEST(Euclid::Transfer::AttributesHeader({}).empty());
}

BOOST_AUTO_TEST_CASE(AnObjectWithoutAttributesStaysEmpty) {
    // The field is optional in every direction: objects written before it existed have no
    // "attributes" document at all, and must still read back.
    Euclid::Database::Entity::ESM::Object object;
    object.key = "report.json";

    const auto document = object.toDocument();
    const auto restored = Euclid::Database::Entity::ESM::Object::fromDocument(document.view());
    BOOST_TEST(restored.attributes.empty());

    const auto dto = Euclid::Dto::ESM::Object::fromJson(EsmMapper::toDto(restored).toJson());
    BOOST_TEST(dto.attributes.empty());
}
