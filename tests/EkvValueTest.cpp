#define BOOST_TEST_MODULE EkvValueTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <string>

// Euclid includes
#include <euclid/database/entity/ekv/Item.h>
#include <euclid/database/entity/ekv/SortCondition.h>
#include <euclid/database/entity/ekv/Table.h>
#include <euclid/database/entity/ekv/Value.h>

using Euclid::Database::Entity::EKV::Item;
using Euclid::Database::Entity::EKV::KeySchema;
using Euclid::Database::Entity::EKV::KeyType;
using Euclid::Database::Entity::EKV::List;
using Euclid::Database::Entity::EKV::Map;
using Euclid::Database::Entity::EKV::SortCondition;
using Euclid::Database::Entity::EKV::Table;
using Euclid::Database::Entity::EKV::Value;
using Euclid::Database::Entity::COM::Binary;

// What the store promises about the values it holds: that what is written comes back, as what it
// was. Everything here is the pure part of that - JSON in, BSON out, BSON in, JSON out - which is
// where the promise is either kept or quietly broken.

namespace {

    // The round trip a stored item actually makes: the caller's JSON becomes a value, the value
    // becomes a document, the document is read back, and the caller is answered from it.
    boost::json::value roundTrip(const std::string &json) {

        const auto value = Value::FromJson(boost::json::parse(json));

        bsoncxx::builder::basic::document document;
        value.AppendTo(document, "v");
        const auto stored = document.extract();

        return Value::FromBson(stored.view()["v"].get_value()).ToJson();
    }

    Table suppliersTable() {
        Table table;
        table.name = "suppliers";
        table.accountId = "000000000000";
        table.partitionKey = KeySchema{.name = "supplierId", .type = KeyType::String};
        return table;
    }

    Table deliveriesTable() {
        Table table = suppliersTable();
        table.name = "deliveries";
        table.sortKey = KeySchema{.name = "deliveredAt", .type = KeyType::String};
        return table;
    }

}// namespace

// ── what a value survives ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(scalars_come_back_as_what_they_were) {

    // The whole reason for a typed value rather than "everything is a string": a number written as
    // a number is read as a number, and 3 does not become 3.0 on the way.
    BOOST_TEST(boost::json::serialize(roundTrip(R"("a string")")) == R"("a string")");
    BOOST_TEST(boost::json::serialize(roundTrip("3")) == "3");
    BOOST_TEST(boost::json::serialize(roundTrip("-42")) == "-42");
    // Compared as a number rather than as text: Boost.JSON writes a double in the shortest form
    // that reads back exactly, which for this one is "3.5E0" - the same value, spelled the way
    // every other euclid module already spells its doubles.
    BOOST_TEST(roundTrip("3.5").as_double() == 3.5);
    BOOST_TEST(boost::json::serialize(roundTrip("true")) == "true");
    BOOST_TEST(boost::json::serialize(roundTrip("false")) == "false");
    BOOST_TEST(boost::json::serialize(roundTrip("null")) == "null");
}

BOOST_AUTO_TEST_CASE(an_integer_does_not_become_a_float) {

    const auto value = Value::FromJson(boost::json::parse("7"));
    BOOST_TEST(value.holds<std::int64_t>());
    BOOST_TEST(!value.holds<double>());

    const auto fraction = Value::FromJson(boost::json::parse("7.0"));
    BOOST_TEST(fraction.holds<double>());
}

BOOST_AUTO_TEST_CASE(a_nested_document_comes_back_whole) {

    // A supplier record is not flat, which is the reason this value type nests at all.
    const std::string supplier = R"({"active":true,"contacts":[{"email":"a@example.com","primary":true},)"
                                 R"({"email":"b@example.com","primary":false}],"delivery":{"format":"onix3",)"
                                 R"("retries":3,"window":{"from":"22:00","to":"04:00"}},"supplierId":"4711"})";

    BOOST_TEST(boost::json::serialize(roundTrip(supplier)) == supplier);
}

BOOST_AUTO_TEST_CASE(an_empty_object_and_an_empty_list_are_kept) {

    BOOST_TEST(boost::json::serialize(roundTrip("{}")) == "{}");
    BOOST_TEST(boost::json::serialize(roundTrip("[]")) == "[]");
    BOOST_TEST(boost::json::serialize(roundTrip(R"({"a":{},"b":[]})")) == R"({"a":{},"b":[]})");
}

BOOST_AUTO_TEST_CASE(attribute_order_is_the_same_every_time) {

    // Stored in name order rather than in the order the caller happened to write them, so that two
    // writes of the same record produce the same document and a diff of two reads means something.
    BOOST_TEST(boost::json::serialize(roundTrip(R"({"c":3,"a":1,"b":2})")) == R"({"a":1,"b":2,"c":3})");
}

BOOST_AUTO_TEST_CASE(binary_survives_the_database_and_reads_back_as_base64) {

    // Nothing can write one over the wire, but the store holds them, and what it answers with has
    // to be something JSON can carry.
    const Value binary{Binary{0x00, 0x01, 0xFE, 0xFF}};

    bsoncxx::builder::basic::document document;
    binary.AppendTo(document, "v");
    const auto stored = document.extract();

    const auto read = Value::FromBson(stored.view()["v"].get_value());
    BOOST_REQUIRE(read.holds<Binary>());
    BOOST_TEST(read.get<Binary>().size() == 4);
    BOOST_TEST(boost::json::serialize(read.ToJson()) == R"("AAH+/w==")");
}

// ── what a value refuses ─────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(an_attribute_name_the_store_cannot_hold_is_refused) {

    // Refused rather than escaped: an escaped name would come back as something other than what
    // was written, which is a worse answer than "no".
    BOOST_CHECK_THROW(std::ignore = Value::FromJson(boost::json::parse(R"({"$set":1})")), std::runtime_error);
    BOOST_CHECK_THROW(std::ignore = Value::FromJson(boost::json::parse(R"({"a.b":1})")), std::runtime_error);
    BOOST_CHECK_THROW(std::ignore = Value::FromJson(boost::json::parse(R"({"":1})")), std::runtime_error);

    // Nested, too - the rule is about names, not about depth.
    BOOST_CHECK_THROW(std::ignore = Value::FromJson(boost::json::parse(R"({"a":{"b":{"$inc":1}}})")), std::runtime_error);

    // And a dollar anywhere but the front is a perfectly ordinary name.
    BOOST_CHECK_NO_THROW(std::ignore = Value::FromJson(boost::json::parse(R"({"price$":1})")));
}

BOOST_AUTO_TEST_CASE(a_value_that_nests_too_deeply_is_refused) {

    std::string deep;
    for (int i = 0; i < Value::kMaxDepth + 2; ++i) deep += R"({"a":)";
    deep += "1";
    for (int i = 0; i < Value::kMaxDepth + 2; ++i) deep += "}";

    BOOST_CHECK_THROW(std::ignore = Value::FromJson(boost::json::parse(deep)), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(only_some_types_can_be_a_key) {

    BOOST_TEST(Value{std::string("4711")}.IsKeyable());
    BOOST_TEST(Value{std::int64_t{4711}}.IsKeyable());
    BOOST_TEST(Value{4.711}.IsKeyable());
    BOOST_TEST(Value{Binary{0x01}}.IsKeyable());

    BOOST_TEST(!Value{}.IsKeyable());// null
    BOOST_TEST(!Value{true}.IsKeyable());
    BOOST_TEST(!Value{List{}}.IsKeyable());
    BOOST_TEST(!Value{Map{}}.IsKeyable());
}

// ── keys ─────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(an_item_takes_its_key_from_its_attributes) {

    const auto attributes = Value::FromJson(boost::json::parse(R"({"supplierId":"4711","name":"Libri"})")).get<Map>();

    std::string error;
    const auto item = Item::FromAttributes(suppliersTable(), attributes, error);

    BOOST_REQUIRE_MESSAGE(item.has_value(), error);
    BOOST_TEST(item->partitionKey.get<std::string>() == "4711");
    BOOST_TEST(!item->sortKey.has_value());

    // The key attributes stay in the item, so what is read back is what was written.
    BOOST_TEST(item->attributes.contains("supplierId"));
}

BOOST_AUTO_TEST_CASE(an_item_without_the_key_attribute_is_refused) {

    const auto attributes = Value::FromJson(boost::json::parse(R"({"name":"Libri"})")).get<Map>();

    std::string error;
    BOOST_TEST(!Item::FromAttributes(suppliersTable(), attributes, error).has_value());
    BOOST_TEST(error.find("supplierId") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_key_of_the_wrong_type_is_refused) {

    // The table says its key is a string; this one is a number. Stored anyway, it would be an item
    // that no lookup could ever find.
    const auto attributes = Value::FromJson(boost::json::parse(R"({"supplierId":4711})")).get<Map>();

    std::string error;
    BOOST_TEST(!Item::FromAttributes(suppliersTable(), attributes, error).has_value());
    BOOST_TEST(error.find("has to be a string") != std::string::npos);
    BOOST_TEST(error.find("is a number") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_table_with_a_sort_key_needs_both) {

    std::string error;

    const auto withoutSort = Value::FromJson(boost::json::parse(R"({"supplierId":"4711"})")).get<Map>();
    BOOST_TEST(!Item::FromAttributes(deliveriesTable(), withoutSort, error).has_value());
    BOOST_TEST(error.find("deliveredAt") != std::string::npos);

    const auto withBoth = Value::FromJson(boost::json::parse(R"({"supplierId":"4711","deliveredAt":"2026-09-10T08:00:00Z"})")).get<Map>();
    const auto item = Item::FromAttributes(deliveriesTable(), withBoth, error);
    BOOST_REQUIRE_MESSAGE(item.has_value(), error);
    BOOST_REQUIRE(item->sortKey.has_value());
    BOOST_TEST(item->sortKey->get<std::string>() == "2026-09-10T08:00:00Z");
}

BOOST_AUTO_TEST_CASE(an_item_round_trips_through_a_document) {

    const auto attributes = Value::FromJson(boost::json::parse(R"({"supplierId":"4711","deliveredAt":"2026-09-10T08:00:00Z",)"
                                                               R"("files":12,"ok":true})"))
                                    .get<Map>();

    std::string error;
    auto item = Item::FromAttributes(deliveriesTable(), attributes, error);
    BOOST_REQUIRE_MESSAGE(item.has_value(), error);
    item->created = item->modified = std::chrono::system_clock::now();

    const auto document = item->toDocument();
    const auto read = Item::fromDocument(document.view());

    BOOST_TEST(read.tableName == "deliveries");
    BOOST_TEST(read.partitionKey.get<std::string>() == "4711");
    BOOST_REQUIRE(read.sortKey.has_value());
    BOOST_TEST(read.sortKey->get<std::string>() == "2026-09-10T08:00:00Z");
    BOOST_TEST(read.attributes.at("files").get<std::int64_t>() == 12);
    BOOST_TEST(read.attributes.at("ok").get<bool>());
}

// ── the table's own shape ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_table_round_trips_with_and_without_a_sort_key) {

    auto table = deliveriesTable();
    table.ern = "ern:ekv:eu-central-1:000000000000:development:table:deliveries";
    table.created = table.modified = std::chrono::system_clock::now();

    const auto read = Table::fromDocument(table.toDocument().view());
    BOOST_TEST(read.name == "deliveries");
    BOOST_TEST(read.ern == table.ern);
    BOOST_TEST(read.partitionKey.name == "supplierId");
    BOOST_REQUIRE(read.sortKey.has_value());
    BOOST_TEST(read.sortKey->name == "deliveredAt");

    auto plain = suppliersTable();
    plain.created = plain.modified = std::chrono::system_clock::now();

    // Absent, not empty: a table with no sort key must not read back as one whose sort key is
    // called "".
    const auto readPlain = Table::fromDocument(plain.toDocument().view());
    BOOST_TEST(!readPlain.sortKey.has_value());
}

BOOST_AUTO_TEST_CASE(key_types_are_read_from_what_the_api_calls_them) {

    BOOST_TEST((Euclid::Database::Entity::EKV::KeyTypeFromString("string").value() == KeyType::String));
    BOOST_TEST((Euclid::Database::Entity::EKV::KeyTypeFromString("NUMBER").value() == KeyType::Number));
    BOOST_TEST((Euclid::Database::Entity::EKV::KeyTypeFromString("b").value() == KeyType::Binary));
    BOOST_TEST(!Euclid::Database::Entity::EKV::KeyTypeFromString("timestamp").has_value());
}

BOOST_AUTO_TEST_CASE(sort_operators_are_read_from_what_the_api_calls_them) {

    BOOST_TEST((SortCondition::OperatorFromString("eq").value() == SortCondition::Operator::Equals));
    BOOST_TEST((SortCondition::OperatorFromString("ge").value() == SortCondition::Operator::GreaterOrEqual));
    BOOST_TEST((SortCondition::OperatorFromString("between").value() == SortCondition::Operator::Between));
    BOOST_TEST((SortCondition::OperatorFromString("begins-with").value() == SortCondition::Operator::BeginsWith));

    // Absent means "the whole partition", which is a query, not a mistake.
    BOOST_TEST((SortCondition::OperatorFromString("").value() == SortCondition::Operator::None));

    BOOST_TEST(!SortCondition::OperatorFromString("contains").has_value());
}
