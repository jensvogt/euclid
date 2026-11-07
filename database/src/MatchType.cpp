// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <regex>
#include <string_view>

// Euclid includes
#include <euclid/database/MatchType.h>

namespace Euclid::Database {

    std::optional<MatchType> ParseMatchType(const std::string &name) {
        if (name.empty() || name == "prefix") return MatchType::Prefix;
        if (name == "endsWith") return MatchType::EndsWith;
        if (name == "contains") return MatchType::Contains;
        if (name == "regex") return MatchType::Regex;
        return std::nullopt;
    }

    // Names are full of regex metacharacters - every file extension contributes a "." - so an
    // unescaped literal quietly matches more than was asked for: "jvo/file.xml" would also select
    // "jvo/fileXxml". Harmless in a listing, not harmless in touch-object or purge-bucket, which
    // act on what it selects.
    std::string QuoteRegex(const std::string &text) {
        std::string quoted;
        quoted.reserve(text.size());
        for (const char c: text) {
            if (std::string_view(R"(\^$.|?*+()[]{})").contains(c)) quoted += '\\';
            quoted += c;
        }
        return quoted;
    }

    std::string MatchPattern(const std::string &text, const MatchType type) {
        switch (type) {
            case MatchType::EndsWith:
                return QuoteRegex(text) + "$";
            case MatchType::Contains:
                return QuoteRegex(text);
            case MatchType::Regex:
                return text;
            case MatchType::Prefix:
            default:
                return "^" + QuoteRegex(text);
        }
    }

    bool Matches(const std::string &value, const std::string &text, const MatchType type) {
        if (text.empty()) return true;
        switch (type) {
            case MatchType::EndsWith:
                return value.ends_with(text);
            case MatchType::Contains:
                return value.contains(text);
            case MatchType::Regex:
                try {
                    return std::regex_search(value, std::regex(text, std::regex::ECMAScript));
                } catch (const std::regex_error &) {
                    return false;
                }
            case MatchType::Prefix:
            default:
                return value.starts_with(text);
        }
    }

    std::optional<std::string> ParseMatch(const std::string &text, const std::string &name, MatchType &type) {
        const auto parsed = ParseMatchType(name);
        if (!parsed) return "matchType must be prefix, endsWith, contains or regex";
        type = *parsed;

        // Compiled here only to be refused here. The in-memory store compiles it the same way
        // (ECMAScript); Mongo's PCRE accepts a little more, so this errs towards refusing.
        if (type == MatchType::Regex && !text.empty()) {
            try {
                std::regex(text, std::regex::ECMAScript);
            } catch (const std::regex_error &e) {
                return std::string("Invalid regular expression: ") + e.what();
            }
        }
        return std::nullopt;
    }

}// namespace Euclid::Database
