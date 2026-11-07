// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#define BOOST_TEST_MODULE MatchTypeTest
#include <boost/test/unit_test.hpp>

// C++ includes
#include <regex>
#include <string>

// Euclid includes
#include <euclid/database/MatchType.h>

// How a list request's "prefix" is matched: the wire names, the Mongo pattern each type produces,
// and the quoting that keeps every type but "regex" literal.

using Euclid::Database::MatchType;

namespace {

    // What Mongo would answer for the pattern, near enough: both the in-memory store and this use
    // ECMAScript and regex_search.
    bool selects(const std::string &pattern, const std::string &value) {
        return std::regex_search(value, std::regex(pattern, std::regex::ECMAScript));
    }

}// namespace

BOOST_AUTO_TEST_CASE(ParsesTheWireNamesAndTreatsEmptyAsPrefix) {
    BOOST_TEST((Euclid::Database::ParseMatchType("") == MatchType::Prefix));
    BOOST_TEST((Euclid::Database::ParseMatchType("prefix") == MatchType::Prefix));
    BOOST_TEST((Euclid::Database::ParseMatchType("endsWith") == MatchType::EndsWith));
    BOOST_TEST((Euclid::Database::ParseMatchType("contains") == MatchType::Contains));
    BOOST_TEST((Euclid::Database::ParseMatchType("regex") == MatchType::Regex));
}

BOOST_AUTO_TEST_CASE(RefusesUnknownAndMiscasedNames) {
    BOOST_TEST(!Euclid::Database::ParseMatchType("Prefix").has_value());
    BOOST_TEST(!Euclid::Database::ParseMatchType("endswith").has_value());
    BOOST_TEST(!Euclid::Database::ParseMatchType("glob").has_value());
}

BOOST_AUTO_TEST_CASE(QuotesEveryMetacharacter) {
    BOOST_TEST(Euclid::Database::QuoteRegex("plain-name_1") == "plain-name_1");
    BOOST_TEST(Euclid::Database::QuoteRegex(R"(\^$.|?*+()[]{})") == R"(\\\^\$\.\|\?\*\+\(\)\[\]\{\})");
    BOOST_TEST(Euclid::Database::QuoteRegex("a.b") == R"(a\.b)");
}

BOOST_AUTO_TEST_CASE(BuildsThePatternForEachType) {
    BOOST_TEST(Euclid::Database::MatchPattern("a.b", MatchType::Prefix) == R"(^a\.b)");
    BOOST_TEST(Euclid::Database::MatchPattern("a.b", MatchType::EndsWith) == R"(a\.b$)");
    BOOST_TEST(Euclid::Database::MatchPattern("a.b", MatchType::Contains) == R"(a\.b)");
    BOOST_TEST(Euclid::Database::MatchPattern("a.b", MatchType::Regex) == "a.b");
}

BOOST_AUTO_TEST_CASE(OnlyRegexReadsTheTextAsAPattern) {
    using Euclid::Database::MatchPattern;

    BOOST_TEST(selects(MatchPattern("file.xml", MatchType::Prefix), "file.xml.bak"));
    BOOST_TEST(!selects(MatchPattern("file.xml", MatchType::Prefix), "fileXxml"));
    BOOST_TEST(!selects(MatchPattern("file", MatchType::Prefix), "my-file"));

    BOOST_TEST(selects(MatchPattern(".xml", MatchType::EndsWith), "file.xml"));
    BOOST_TEST(!selects(MatchPattern(".xml", MatchType::EndsWith), "file.xml.bak"));
    BOOST_TEST(!selects(MatchPattern(".xml", MatchType::EndsWith), "fileXxml"));

    BOOST_TEST(selects(MatchPattern("(1)", MatchType::Contains), "copy (1) of"));
    BOOST_TEST(!selects(MatchPattern("(1)", MatchType::Contains), "copy 1 of"));

    BOOST_TEST(selects(MatchPattern("^f.*l$", MatchType::Regex), "fileXxml"));
}

BOOST_AUTO_TEST_CASE(MatchesInProcessTheSameWay) {
    using Euclid::Database::Matches;

    BOOST_TEST(Matches("anything", "", MatchType::Regex));
    BOOST_TEST(Matches("orders-in", "orders", MatchType::Prefix));
    BOOST_TEST(!Matches("my-orders", "orders", MatchType::Prefix));
    BOOST_TEST(Matches("orders-in", "-in", MatchType::EndsWith));
    BOOST_TEST(Matches("my-orders-in", "ders", MatchType::Contains));
    BOOST_TEST(Matches("orders-in", "^o.*n$", MatchType::Regex));
    BOOST_TEST(!Matches("orders-in", "([", MatchType::Regex));
}

BOOST_AUTO_TEST_CASE(ParseMatchRefusesWhatWouldOtherwiseBeA500) {
    MatchType type{};

    BOOST_TEST(!Euclid::Database::ParseMatch("x", "", type).has_value());
    BOOST_TEST((type == MatchType::Prefix));

    const auto unknown = Euclid::Database::ParseMatch("x", "glob", type);
    BOOST_REQUIRE(unknown.has_value());
    BOOST_TEST(*unknown == "matchType must be prefix, endsWith, contains or regex");

    const auto invalid = Euclid::Database::ParseMatch("([", "regex", type);
    BOOST_REQUIRE(invalid.has_value());
    BOOST_TEST(invalid->starts_with("Invalid regular expression: "));

    // Empty text is no filter at all, so there is nothing to compile.
    BOOST_TEST(!Euclid::Database::ParseMatch("", "regex", type).has_value());
    BOOST_TEST(!Euclid::Database::ParseMatch("([", "contains", type).has_value());
}
