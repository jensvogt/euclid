//
// Created by vogje01 on 9/2/26.
//

// C++ includes
#include <algorithm>
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

    std::vector<std::string> HomeDirectoryKeys(const std::string &homePrefix, const std::vector<std::string> &directories) {

        std::vector<std::string> keys;
        for (const auto &directory: directories) {

            // Reuses HomePrefix's own normalisation, with no user to substitute: a configured
            // directory and a home template are the same kind of thing - a path fragment that has
            // to end up as a safe key prefix - and normalising them two different ways is how the
            // two would eventually disagree about "/incoming/" versus "incoming".
            const auto normalised = HomePrefix(directory, "");
            if (normalised.empty()) continue;

            // Emitted one level at a time so the parents exist before their children. Nothing
            // creates a directory implicitly here: a marker is a single object, and a client
            // cannot walk into "incoming/mix" if "incoming" itself has nothing standing for it.
            for (std::size_t at = normalised.find('/'); at != std::string::npos; at = normalised.find('/', at + 1)) {
                if (auto key = homePrefix + normalised.substr(0, at + 1); std::ranges::find(keys, key) == keys.end()) keys.push_back(std::move(key));
            }
        }
        return keys;
    }

}// namespace Euclid::Transfer