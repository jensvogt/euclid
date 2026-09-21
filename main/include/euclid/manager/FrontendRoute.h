// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Euclid::main {

    /**
     * @brief Turning a browser's URL path into a file of the euclid-web build, or into nothing.
     *
     * @par Why this is a header of its own
     * The manager is an executable rather than a library, so nothing in it can be linked into a
     * test - the same reason @BacklogTarget.h exists. This is the one piece of the gateway that
     * takes a string off the wire and hands back a path on the local disk, which is exactly the
     * shape of code that must be checked rather than reasoned about: everything the gateway serves
     * from here is served without a token, so a path that escapes the frontend directory is the
     * whole filesystem published to anyone who can reach port 5566.
     */
    namespace Frontend {

        namespace detail {

            /**
             * @brief Decodes %XX escapes in a URL path.
             *
             * Deliberately not the decoder in @HttpUtils: that one is for query strings, where '+'
             * means a space. In a path '+' is an ordinary character, and a file genuinely called
             * "a+b.js" would otherwise be looked for under the name "a b.js".
             *
             * @param value raw path, as it arrived in the request target.
             * @return the decoded path; an escape that is not two hex digits is left as written.
             */
            inline std::string PercentDecode(const std::string_view value) {

                std::string decoded;
                decoded.reserve(value.size());
                for (std::size_t i = 0; i < value.size(); ++i) {
                    if (value[i] == '%' && i + 2 < value.size()) {
                        const auto hex = std::string(value.substr(i + 1, 2));
                        if (std::ranges::all_of(hex, [](const unsigned char c) { return std::isxdigit(c) != 0; })) {
                            decoded += static_cast<char>(std::stoi(hex, nullptr, 16));
                            i += 2;
                            continue;
                        }
                    }
                    decoded += value[i];
                }
                return decoded;
            }

        }// namespace detail

        /**
         * @brief The file a request path should be answered with, if any.
         *
         * @par Containment
         * The decoded path is joined to the root, normalized and then canonicalized, and the result
         * has to still be inside the root or nothing is returned. Doing it that way - rather than
         * looking for ".." in the text - is what makes it hold for the spellings that do not
         * contain any: "%2e%2e%2f" arrives as "../" only after decoding, "a/../../b" contains no
         * ".." once normalized, and a symlink planted in the directory contains none at any point
         * yet still leads out. Canonicalizing first and asking where the answer landed covers all
         * three, because it asks about the file rather than about the string.
         *
         * @par The index fallback
         * euclid-web is a single-page application: "/buckets" and "/users/42" are routes its own
         * router resolves in the browser, and there is no such file on disk. A path that names no
         * file is therefore answered with index.html so the application can boot and route itself -
         * but only when the path has no extension. A missing "main-A1B2C3.js" is a broken deploy,
         * and answering it with HTML turns that into a syntax error inside a script tag pointing at
         * the wrong line of the wrong file; a plain 404 says what actually happened.
         *
         * @param root directory the euclid-web build was installed into.
         * @param urlPath request target with its query string already removed.
         * @return the file to serve, or nothing if the path does not resolve inside root.
         */
        inline std::optional<std::filesystem::path> ResolveFile(const std::filesystem::path &root, const std::string_view urlPath) {

            namespace fs = std::filesystem;

            if (root.empty()) return std::nullopt;

            std::error_code ec;
            const auto base = fs::weakly_canonical(root, ec);
            if (ec || !fs::is_directory(base, ec)) return std::nullopt;

            auto index = [&base]() -> std::optional<fs::path> {
                std::error_code indexEc;
                auto file = base / "index.html";
                if (!fs::is_regular_file(file, indexEc)) return std::nullopt;
                return file;
            };

            const auto decoded = detail::PercentDecode(urlPath);

            // A NUL is never part of a path a browser meant to ask for, and every use of one here
            // is an attempt to end a name early somewhere further down.
            if (decoded.find('\0') != std::string::npos) return std::nullopt;

            const auto start = decoded.find_first_not_of('/');
            const auto relative = start == std::string::npos ? std::string() : decoded.substr(start);
            if (relative.empty()) return index();

            // Anything that is a path in its own right - "/etc/shadow" after the leading slashes
            // were stripped cannot be, but "C:\Windows\..." or "\\host\share\..." can - would
            // replace the root rather than extend it when joined, so it never gets to be joined.
            const fs::path requested(relative);
            if (requested.is_absolute() || !requested.root_name().empty()) return std::nullopt;

            const auto candidate = fs::weakly_canonical((base / requested).lexically_normal(), ec);
            if (ec) return std::nullopt;

            const auto inside = candidate.lexically_relative(base);
            if (inside.empty() || *inside.begin() == "..") return std::nullopt;

            if (fs::is_regular_file(candidate, ec)) return candidate;

            return candidate.has_extension() ? std::nullopt : index();
        }

        /**
         * @brief The content-type to serve a frontend file as.
         *
         * @par Why the extension decides, and not libmagic
         * @ContentTypeUtils sniffs the bytes and only consults the name when the sniff says nothing
         * specific, which is the right order for stored objects - an object named .txt that is a
         * PNG is a PNG. It is the wrong order here. A browser refuses to execute a module script or
         * apply a stylesheet whose content-type is not the expected one, and libmagic answers for
         * JavaScript with whatever language its heuristics liked best that day ("text/x-c" is a
         * common one for minified bundles). That is a blank page with a MIME-type error in the
         * console, from a deploy where every file is intact. What a build emits is known from its
         * name, so the name is believed.
         *
         * @param file file about to be served.
         * @return the content-type, including a charset for the text formats.
         */
        inline std::string ContentType(const std::filesystem::path &file) {

            static const std::unordered_map<std::string, std::string> kTypes{
                    {"html", "text/html; charset=utf-8"},
                    {"htm", "text/html; charset=utf-8"},
                    {"js", "text/javascript; charset=utf-8"},
                    {"mjs", "text/javascript; charset=utf-8"},
                    {"css", "text/css; charset=utf-8"},
                    {"json", "application/json; charset=utf-8"},
                    {"map", "application/json; charset=utf-8"},
                    {"webmanifest", "application/manifest+json; charset=utf-8"},
                    {"xml", "application/xml; charset=utf-8"},
                    {"txt", "text/plain; charset=utf-8"},
                    {"csv", "text/csv; charset=utf-8"},
                    {"svg", "image/svg+xml"},
                    {"png", "image/png"},
                    {"jpg", "image/jpeg"},
                    {"jpeg", "image/jpeg"},
                    {"gif", "image/gif"},
                    {"webp", "image/webp"},
                    {"avif", "image/avif"},
                    {"ico", "image/vnd.microsoft.icon"},
                    {"woff", "font/woff"},
                    {"woff2", "font/woff2"},
                    {"ttf", "font/ttf"},
                    {"otf", "font/otf"},
                    {"eot", "application/vnd.ms-fontobject"},
                    {"wasm", "application/wasm"},
                    {"pdf", "application/pdf"},
                    {"mp4", "video/mp4"},
                    {"webm", "video/webm"},
            };

            auto extension = file.extension().string();
            if (extension.empty()) return "application/octet-stream";
            extension.erase(0, 1);
            std::ranges::transform(extension, extension.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });

            const auto it = kTypes.find(extension);
            return it == kTypes.end() ? "application/octet-stream" : it->second;
        }

        /**
         * @brief Whether a file is the application's entry document rather than one of its assets.
         *
         * Decides how long the answer may be cached, and the two cases are opposites. Everything a
         * build emits beside index.html carries a content hash in its name, so a given name is that
         * exact content forever and may be held for as long as the browser likes. index.html is the
         * one name that does not change between deploys and is also the file that names all the
         * others - cached, it goes on asking for the bundles of the version it was built against,
         * which the next deploy has already deleted.
         *
         * @param file file about to be served.
         * @return true if this is index.html.
         */
        inline bool IsIndex(const std::filesystem::path &file) {
            return file.filename() == "index.html";
        }

    }// namespace Frontend

}// namespace Euclid::main
