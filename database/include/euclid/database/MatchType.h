// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <optional>
#include <string>

namespace Euclid::Database {

    /**
     * @brief How a list request's search text (its "prefix" field) is matched against a name.
     *
     * @par
     * The wire names are "prefix", "endsWith", "contains" and "regex"; an absent or empty
     * "matchType" is Prefix, which is what every list action did before the field existed. Every
     * type but Regex treats the text as a literal - it is quoted before it reaches Mongo - so a
     * search for "a.b" does not also find "aXb".
     */
    enum class MatchType { Prefix, EndsWith, Contains, Regex };

    /**
     * @brief Parses a wire name. Empty is Prefix; anything unknown is std::nullopt.
     */
    std::optional<MatchType> ParseMatchType(const std::string &name);

    /**
     * @brief Escapes every regular-expression metacharacter, so the text matches only itself.
     */
    std::string QuoteRegex(const std::string &text);

    /**
     * @brief The Mongo "$regex" pattern selecting names that match the text the given way.
     *
     * Prefix is "^" + quoted, EndsWith quoted + "$", Contains quoted, and Regex the text unchanged.
     */
    std::string MatchPattern(const std::string &text, MatchType type);

    /**
     * @brief The same test as MatchPattern(), in process - for what is filtered outside a query.
     * Empty text matches everything.
     */
    bool Matches(const std::string &value, const std::string &text, MatchType type);

    /**
     * @brief Parses a list request's matchType and checks its text, for a handler to refuse with
     * a 400 instead of letting a bad pattern reach the database as a 500.
     *
     * @param text the search text; empty means no filter, so it is never checked.
     * @param name the wire name of the match type.
     * @param type receives the parsed type on success.
     * @return the message to answer with, or std::nullopt when the request is good.
     */
    std::optional<std::string> ParseMatch(const std::string &text, const std::string &name, MatchType &type);

}// namespace Euclid::Database
