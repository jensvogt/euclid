// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/20/26.
//

// C++ includes
#include <cctype>
#include <sstream>
#include <vector>

// Euclid includes
#include <UploadTarget.h>

namespace Euclid::EAG {

    namespace {

        // Percent-decoding, and only that: "+" is left alone, because it is a space in a query
        // string and a plain plus in a path, and this only ever reads paths. A malformed escape is
        // kept as the literal text it is rather than dropped - the segment checks below then judge
        // what it decoded to, which is the same thing they would have judged had it been written
        // out.
        std::string percentDecode(const std::string_view encoded) {

            std::string decoded;
            decoded.reserve(encoded.size());

            for (std::size_t i = 0; i < encoded.size(); ++i) {
                if (encoded[i] != '%' || i + 2 >= encoded.size()) {
                    decoded.push_back(encoded[i]);
                    continue;
                }
                const auto high = encoded[i + 1];
                const auto low = encoded[i + 2];
                if (!std::isxdigit(static_cast<unsigned char>(high)) || !std::isxdigit(static_cast<unsigned char>(low))) {
                    decoded.push_back(encoded[i]);
                    continue;
                }
                decoded.push_back(static_cast<char>(std::stoi(std::string{high, low}, nullptr, 16)));
                i += 2;
            }
            return decoded;
        }

        std::vector<std::string> segmentsOf(const std::string &path) {
            std::vector<std::string> segments;
            std::stringstream ss(path);
            for (std::string segment; std::getline(ss, segment, '/');) segments.push_back(segment);
            return segments;
        }

    }// namespace

    UploadKey ResolveUploadKey(const Database::Entity::EAG::Route &route, const std::string_view target) {

        auto path = std::string(target);
        if (const auto question = path.find('?'); question != std::string::npos) path = path.substr(0, question);

        // The route's prefix is not part of the key. Matching already established that the path is
        // beneath it, so this only has to remove it.
        if (path.size() >= route.path.size() && path.compare(0, route.path.size(), route.path) == 0) {
            path = path.substr(route.path.size());
        }

        path = percentDecode(path);

        while (path.starts_with('/')) path = path.substr(1);

        if (path.empty()) {
            return {.valid = false, .reason = "name the object after the route's path, e.g. PUT " + route.path + "/my-file.xml"};
        }

        // Every one of these would either escape the key prefix or produce a key that is not the
        // one the caller can see in their own URL. Refused rather than normalised away, because a
        // caller who wrote "a/../b" and gets "b" has been given a different object than the one
        // they asked for and has no way to know it.
        for (const auto &segment: segmentsOf(path)) {
            if (segment.empty()) {
                return {.valid = false, .reason = "the object key has an empty path segment"};
            }
            if (segment == "." || segment == "..") {
                return {.valid = false, .reason = R"(the object key may not contain "." or ".." segments)"};
            }
        }
        if (path.find('\0') != std::string::npos) {
            return {.valid = false, .reason = "the object key contains a null byte"};
        }

        auto prefix = route.upload.keyPrefix;
        if (!prefix.empty() && !prefix.ends_with('/')) prefix += '/';
        while (prefix.starts_with('/')) prefix = prefix.substr(1);

        return {.valid = true, .key = prefix + path};
    }

}// namespace Euclid::EAG
