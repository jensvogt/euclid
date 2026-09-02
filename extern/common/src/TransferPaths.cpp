//
// Created by vogje01 on 9/2/26.
//

// C++ includes
#include <sstream>
#include <vector>

// Euclid includes
#include <TransferPaths.h>

namespace Euclid::Transfer {

    namespace {

        constexpr auto kUserPlaceholder = "{user}";

        std::string substituteUser(const std::string &homeDirectory, const std::string &userId) {
            std::string expanded = homeDirectory;
            const std::string placeholder(kUserPlaceholder);
            for (auto at = expanded.find(placeholder); at != std::string::npos; at = expanded.find(placeholder, at + userId.size())) {
                expanded.replace(at, placeholder.size(), userId);
                if (userId.empty()) break;
            }
            return expanded;
        }

    }// namespace

    std::string HomePrefix(const std::string &homeDirectory, const std::string &userId) {

        const auto expanded = substituteUser(homeDirectory, userId);

        // Rebuilt from its segments rather than trimmed in place: that is what makes "/{user}/",
        // "{user}" and "//{user}//" the same prefix, and what leaves no way for a "." or ".."
        // to survive into a key.
        std::vector<std::string> segments;
        std::istringstream stream(expanded);
        std::string segment;
        while (std::getline(stream, segment, '/')) {
            if (segment.empty() || segment == "." || segment == "..") continue;
            segments.push_back(segment);
        }
        if (segments.empty()) return {};

        std::string prefix;
        for (const auto &part: segments) {
            prefix += part;
            prefix += '/';
        }
        return prefix;
    }

}// namespace Euclid::Transfer
