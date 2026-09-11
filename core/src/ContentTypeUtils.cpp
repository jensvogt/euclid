// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>

// Libmagic includes
#include <magic.h>

// Euclid includes
#include <euclid/core/Configuration.h>
#include <euclid/core/ContentTypeUtils.h>
#include <euclid/core/LogStream.h>

namespace Euclid::Core {

    namespace {

        constexpr const char *kDefaultContentType = "application/octet-stream";

        struct MagicHandle {

            magic_t magic = magic_open(MAGIC_MIME_TYPE);
            std::mutex mutex;

            MagicHandle() {
                if (!magic) {
                    log_error << "Failed to open libmagic";
                    return;
                }
                const auto magicFile = Configuration::instance().getOr<std::string>("euclid.magic-file", std::string());
                if (magic_load(magic, magicFile.empty() ? nullptr : magicFile.c_str()) != 0) {
                    log_error << "Failed to load magic database, file: " << magicFile;
                    magic_close(magic);
                    magic = nullptr;
                }
            }

            ~MagicHandle() {
                if (magic) magic_close(magic);
            }
        };

        MagicHandle &handle() {
            static MagicHandle instance;
            return instance;
        }

        // Extensions worth believing over an inconclusive sniff, and they are all text formats
        // because that is where sniffing gives up: libmagic recognizes a binary format from the
        // few magic bytes at its start, but recognizes JSON, XML or CSV only by parsing the whole
        // document - so the first bytes of a large one are indistinguishable from the text they
        // are, and a 6 kB JSON object comes back as text/plain.
        const std::unordered_map<std::string, std::string> &extensionTypes() {
            static const std::unordered_map<std::string, std::string> types = {
                    {"json", "application/json"},
                    {"jsonl", "application/x-ndjson"},
                    {"ndjson", "application/x-ndjson"},
                    {"xml", "application/xml"},
                    {"yaml", "application/yaml"},
                    {"yml", "application/yaml"},
                    {"csv", "text/csv"},
                    {"tsv", "text/tab-separated-values"},
                    {"html", "text/html"},
                    {"htm", "text/html"},
                    {"css", "text/css"},
                    {"js", "text/javascript"},
                    {"mjs", "text/javascript"},
                    {"md", "text/markdown"},
                    {"svg", "image/svg+xml"},
                    {"sql", "application/sql"},
                    {"ics", "text/calendar"},
            };
            return types;
        }

        // What libmagic says when it recognized nothing: that this is text, or that these are
        // bytes. Neither describes a format, which is what makes the extension the better answer.
        bool isGeneric(const std::string &contentType) {
            return contentType.empty() || contentType == "text/plain" || contentType == kDefaultContentType;
        }

    }// namespace

    std::string ContentTypeUtils::fromContent(const std::string &content) {

        auto &h = handle();
        if (!h.magic) return kDefaultContentType;

        std::lock_guard lock(h.mutex);
        const char *result = magic_buffer(h.magic, content.data(), content.size());
        return result ? result : kDefaultContentType;
    }

    std::string ContentTypeUtils::fromFile(const std::string &filePath) {

        auto &h = handle();
        if (!h.magic) return kDefaultContentType;

        std::lock_guard lock(h.mutex);
        const char *result = magic_file(h.magic, filePath.c_str());
        return result ? result : kDefaultContentType;
    }

    std::string ContentTypeUtils::fromKey(const std::string &key) {

        const auto slash = key.find_last_of('/');
        const auto name = slash == std::string::npos ? std::string_view(key) : std::string_view(key).substr(slash + 1);

        // A leading dot is a hidden file rather than an extension - ".json" is a file called
        // .json, not a nameless JSON document.
        const auto dot = name.find_last_of('.');
        if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) return {};

        std::string extension(name.substr(dot + 1));
        std::ranges::transform(extension, extension.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });

        const auto &types = extensionTypes();
        const auto it = types.find(extension);
        return it == types.end() ? std::string() : it->second;
    }

    std::string ContentTypeUtils::detect(const std::string &content, const std::string &key) {

        // The content decides whenever it says something specific: a key can be named anything,
        // and an object whose bytes are a PNG is a PNG whatever it was stored as.
        auto sniffed = fromContent(content);
        if (!isGeneric(sniffed)) return sniffed;

        if (auto byKey = fromKey(key); !byKey.empty()) return byKey;

        // Neither knew. Text is still more than nothing, so keep what was sniffed.
        return sniffed.empty() ? kDefaultContentType : sniffed;
    }

}// namespace Euclid::Core
