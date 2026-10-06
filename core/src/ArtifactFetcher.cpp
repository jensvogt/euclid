// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <fstream>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <euclid/core/ArtifactFetcher.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/LogStream.h>

namespace Euclid::Core::Artifact {

    namespace {

        std::vector<std::pair<std::string, std::string> > scoped(const Request &request,
                                                                 std::vector<std::pair<std::string, std::string> > headers) {
            // Sent only when known: an empty header value reads to the module exactly like an
            // absent one.
            if (!request.region.empty()) headers.emplace_back("x-euclid-region", request.region);
            if (!request.accountId.empty()) headers.emplace_back("x-euclid-account-id", request.accountId);
            if (!request.nameSpace.empty()) headers.emplace_back("x-euclid-namespace", request.nameSpace);
            return headers;
        }

        // Writes the bytes, then moves them into place. Never writes `target` directly: a
        // half-written artifact that the next reconcile pass finds is indistinguishable from a
        // complete one, and the md5 check would simply re-download it every pass while whatever is
        // there gets started.
        bool writeAtomically(const std::filesystem::path &target, const std::string &bytes) {

            const auto temporary = std::filesystem::path(target).concat(".part");

            std::error_code ec;
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                if (!out) {
                    log_error << "Could not open the artifact for writing, path: " << temporary.string();
                    return false;
                }
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (!out) {
                    log_error << "Could not write the artifact, path: " << temporary.string();
                    std::filesystem::remove(temporary, ec);
                    return false;
                }
            }

            std::filesystem::rename(temporary, target, ec);
            if (ec) {
                log_error << "Could not move the artifact into place, path: " << target.string() << ", error: " << ec.message();
                std::filesystem::remove(temporary, ec);
                return false;
            }
            return true;
        }

        // One call. ESM refuses this outright for an object at or above the part size, which is
        // why the size is checked before choosing rather than after being refused.
        bool downloadWhole(const Request &request, const std::filesystem::path &target, const Call &call) {

            const auto response = call("get-object",
                                       scoped(request, {{"x-euclid-bucket-ern", request.bucketErn},
                                                        {"x-euclid-key", request.key},
                                                        {"x-euclid-part-size", std::to_string(PartSize())}}),
                                       "");

            if (!response.ok()) {
                log_error << "Could not download the application artifact, key: " << request.key
                          << ", status: " << response.describe();
                return false;
            }
            return writeAtomically(target, response.body);
        }

        // create-download, then one call per part, then complete-download. The same sequence the
        // transfer servers use, and the same reason: an artifact is routinely larger than anything
        // worth moving in one response.
        bool downloadInParts(const Request &request, const std::filesystem::path &target, const Call &call) {

            const auto created = call("create-download", scoped(request, {}),
                                      boost::json::serialize(boost::json::object{{"bucketErn", request.bucketErn},
                                                                                 {"key", request.key}}));
            if (!created.ok()) {
                log_error << "Could not start the artifact download, key: " << request.key
                          << ", status: " << created.describe();
                return false;
            }

            std::string downloadId;
            long size = 0;
            try {
                const auto parsed = boost::json::parse(created.body);
                downloadId = std::string(parsed.at("downloadId").as_string());
                size = parsed.at("size").to_number<long>();
            } catch (const std::exception &e) {
                log_error << "Could not read the artifact download id, key: " << request.key << ", error: " << e.what();
                return false;
            }

            const auto temporary = std::filesystem::path(target).concat(".part");
            std::error_code ec;

            const auto partSize = PartSize();
            const auto parts = Detail::PartCount(size, partSize);
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                if (!out) {
                    log_error << "Could not open the artifact for writing, path: " << temporary.string();
                    return false;
                }

                for (long partNumber = 1; partNumber <= parts; ++partNumber) {
                    const auto response = call("download-part",
                                               scoped(request, {{"x-euclid-download-id", downloadId},
                                                                {"x-euclid-part-number", std::to_string(partNumber)},
                                                                {"x-euclid-part-size", std::to_string(partSize)}}),
                                               "");

                    if (!response.ok()) {
                        log_error << "Artifact download part failed, key: " << request.key << ", part: " << partNumber
                                  << " of " << parts << ", status: " << response.describe();
                        out.close();
                        std::filesystem::remove(temporary, ec);
                        return false;
                    }
                    out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
                    if (!out) {
                        log_error << "Could not write the artifact, path: " << temporary.string();
                        out.close();
                        std::filesystem::remove(temporary, ec);
                        return false;
                    }
                }
            }

            // Best effort, as it is for the transfer servers: the bytes are already on disk, and
            // ESM discards a download's scratch state on its own schedule, so failing to say we
            // are done is not a reason to throw away a complete artifact.
            std::ignore = call("complete-download", scoped(request, {}),
                               boost::json::serialize(boost::json::object{{"downloadId", downloadId}}));

            std::filesystem::rename(temporary, target, ec);
            if (ec) {
                log_error << "Could not move the artifact into place, path: " << target.string() << ", error: " << ec.message();
                std::filesystem::remove(temporary, ec);
                return false;
            }

            log_info << "Application artifact downloaded in parts, key: " << request.key << ", size: " << size << ", parts: " << parts;
            return true;
        }

    }// namespace

    long PartSize() {
        constexpr long kDefaultPartSize = 8L * 1024 * 1024;
        return std::max(1L, Configuration::instance().getOr<long>("euclid.modules.eap.artifact-part-size", kDefaultPartSize));
    }

    bool Download(const Request &request, const std::filesystem::path &target, const Call &call) {

        if (!call) {
            log_error << "No transport to fetch the application artifact with, key: " << request.key;
            return false;
        }

        // Both halves, and in this order: an unknown size is not a small one - see SizeIsKnown.
        return Detail::SizeIsKnown(request.size) && Detail::FitsInOneCall(request.size, PartSize())
                       ? downloadWhole(request, target, call)
                       : downloadInParts(request, target, call);
    }

}// namespace Euclid::Core::Artifact
