// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// C++ includes
#include <algorithm>
#include <chrono>
#include <fstream>

// Boost includes
#include <boost/json.hpp>

// Euclid includes
#include <ArtifactFetcher.h>
#include <euclid/core/Configuration.h>
#include <euclid/core/HttpActionServer.h>
#include <euclid/core/JwtUtils.h>
#include <euclid/core/LogStream.h>
#include <euclid/core/ModuleClient.h>
#include <euclid/database/RepositoryFactory.h>

namespace Euclid::Manager::Artifact {

    namespace {

        using Core::ModuleClient::ModuleResponse;

        // How much of an object one call carries, and the line between a single-shot download and
        // a sequence of parts. Eight megabytes, matching the transfer servers' default: it is a
        // size a module can hold in memory per call without thought, and it makes the part count
        // of a large jar a number rather than a stream.
        long PartSize() {
            constexpr long kDefaultPartSize = 8L * 1024 * 1024;
            return std::max(1L, Core::Configuration::instance().getOr<long>("euclid.modules.eap.artifact-part-size", kDefaultPartSize));
        }

        // Euclid's own inter-module traffic, which is what this is: the manager fetching bytes it
        // is about to exec, not a user asking for a file.
        //
        // The manager can mint this because it holds the signing secret already - it mints every
        // application's credentials with the same call. Nothing is widened by using it: anything
        // holding HttpActionServer::JwtSecret() can already mint a token for any principal, which
        // is exactly why a worker must never hold it and must be issued its credentials instead
        // (worker-nodes.md §3.2). A worker reaching this code path would pass its own token, for
        // its own principal, granted esm:get-object on the artifact bucket.
        std::string systemToken() {
            // Minted per fetch and good for minutes rather than hours: it is used by the calls
            // this function is about to make and never written anywhere.
            constexpr std::chrono::seconds kShortLived{300};
            return Core::JwtUtils::CreateToken(Database::kSystemPrincipal, Core::HttpActionServer::JwtSecret(), kShortLived);
        }

        // Which socket to call. Any running instance will do - an object download reads a file out
        // of the shared data directory, so every instance of ESM can serve it, and the sticky
        // selection the transfer servers do is about surviving a scale-down mid-upload rather than
        // about which instance holds what.
        std::optional<std::string> esmSocket() {

            for (const auto &module: Database::RepositoryFactory::instance().emmRepository()->findAll()) {
                if (module.name != "esm") continue;
                for (const auto &instance: module.instances) {
                    if (instance.state == Database::Entity::ModuleState::RUNNING && !instance.socketPath.empty()) {
                        return instance.socketPath;
                    }
                }
            }
            return std::nullopt;
        }

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
        // complete one, and the md5 check would simply re-download it every pass while the
        // application started on whatever was there.
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
        bool downloadWhole(const std::string &socket, const Request &request, const std::string &token,
                           const std::filesystem::path &target) {

            const auto response = Core::ModuleClient::CallAt(
                    socket, "esm", "get-object", token,
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
        bool downloadInParts(const std::string &socket, const Request &request, const std::string &token,
                             const std::filesystem::path &target) {

            const auto created = Core::ModuleClient::CallAt(
                    socket, "esm", "create-download", token, scoped(request, {}),
                    boost::json::serialize(boost::json::object{{"bucketErn", request.bucketErn}, {"key", request.key}}));

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
                    const auto response = Core::ModuleClient::CallAt(
                            socket, "esm", "download-part", token,
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
            std::ignore = Core::ModuleClient::CallAt(socket, "esm", "complete-download", token, scoped(request, {}),
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

    bool Download(const Request &request, const std::filesystem::path &target) {

        const auto socket = esmSocket();
        if (!socket.has_value()) {
            // The one failure this path has that reading the file off disk did not. Said plainly,
            // because "could not download the artifact" with no running ESM behind it would send
            // somebody looking at the bucket.
            log_error << "No running instance of ESM to fetch the application artifact from, key: " << request.key;
            return false;
        }

        const auto token = systemToken();

        return Detail::FitsInOneCall(request.size, PartSize())
                       ? downloadWhole(*socket, request, token, target)
                       : downloadInParts(*socket, request, token, target);
    }

}// namespace Euclid::Manager::Artifact
