// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Euclid includes
#include <euclid/core/ModuleClient.h>

namespace Euclid::Core {

    /**
     * @brief Fetches an application's artifact out of ESM's object storage.
     *
     * @par Why through the module rather than off its disk
     * The manager used to read the file straight out of `euclid.modules.esm.data-dir`, which was
     * cheaper and worked because the two are on one host. A worker is not - it has no such
     * directory and no database - so that path cannot be the one a worker takes, and having two
     * would mean the one that matters is the one nobody debugs. See docs/worker-nodes.md §3.1.
     *
     * @par What it costs, stated plainly
     * An application whose artifact is not already on disk cannot start while ESM is down. That is
     * a real dependency the filesystem path did not have. It is narrow: the download happens only
     * when the local copy is missing or its content hash differs from the object's, so it is a
     * first deploy or a new build, not every restart - and the manager starts ESM before it
     * reconciles applications anyway.
     *
     * @par What it buys besides the worker
     * ESM decrypts on the way out, so the manager no longer needs the encryption key, EKM, or
     * ObjectCipher to run an application out of an encrypted bucket. The bytes that arrive are the
     * bytes the runtime has to exec.
     *
     * @par The protocol is shared; the transport is not
     * Which is why @ref Download takes the call rather than making it. The manager reaches ESM
     * over its Unix socket, resolving it out of the module repository it already has. A worker has
     * neither a module socket nor a database, so it goes through the gateway as a signed client -
     * the same four actions, a different way of getting them there. Choosing one here would have
     * made this the manager's fetcher with the worker's one written beside it, which is precisely
     * the two-paths problem step 3 existed to remove.
     *
     * @par
     * That parameter is also what lets this live in core at all: with the call supplied, nothing
     * here needs the database, and core does not depend on it.
     *
     * @author jensvogt47\@gmail.com
     */
    namespace Artifact {

        /**
         * @brief How this reaches ESM: one action, some headers, a body, and whatever came back.
         *
         * @par
         * Deliberately the shape of ModuleClient::CallAt() minus the socket, because that is the
         * shape both callers already have. A manager binds the socket it resolved; a worker binds
         * its gateway client and its signing credentials.
         */
        using Call = std::function<ModuleClient::ModuleResponse(const std::string &action,
                                                                const std::vector<std::pair<std::string, std::string> > &headers,
                                                                const std::string &body)>;

        /**
         * @brief How much of an object one call carries, and the line between a single-shot
         * download and a sequence of parts.
         *
         * @par
         * Read from `euclid.modules.eap.artifact-part-size`, defaulting to eight megabytes -
         * matching the transfer servers, for the same reasons.
         */
        [[nodiscard]] long PartSize();

        /**
         * @brief What one object is, and who is asking for it.
         */
        struct Request {

            /**
             * @brief The bucket holding the artifact, as an ERN.
             */
            std::string bucketErn;

            /**
             * @brief The object key within it.
             */
            std::string key;

            /**
             * @brief The object's size as the database records it, which decides whether this is
             * one call or a sequence of them: ESM refuses a single-shot get-object for anything at
             * or above the part size the caller names.
             */
            long size{};

            /**
             * @brief Scope headers, sent when known. Not what authorises the call - the manager
             * calls as euclid's own inter-module principal - but what makes the request say which
             * installation it is about, the way every other module call does.
             */
            std::string accountId;
            std::string nameSpace;
            std::string region;
        };

        namespace Detail {

            /**
             * @brief Whether an object can be fetched with a single get-object.
             *
             * @par
             * Separated so it can be asserted without ESM, because getting it wrong is silent in
             * the direction that matters. EsmServer::handleGetObject answers 413 when the object's
             * size is **at or above** the part size the request names, so the boundary is strictly
             * less-than. A `<=` here would send every artifact whose size lands exactly on the
             * part size down the single-shot path and have it refused - a deploy that fails only
             * for one file size, which is not a pattern anybody looks for.
             */
            [[nodiscard]] constexpr bool FitsInOneCall(const long size, const long partSize) noexcept {
                return size < partSize;
            }

            /**
             * @brief Whether the object's size is known at all, which has to be asked before
             * FitsInOneCall() is believed.
             *
             * @par
             * A master that does not send the size leaves Request::size at zero, and zero is below
             * any part size - so an unknown size reads as a small one and takes the single-shot
             * path, which ESM refuses for anything at or above the part size. That is the wrong
             * direction to fail in: downloading in parts asks ESM for the size in create-download
             * and therefore works whether or not the caller knew it, while one call only works when
             * the size is known to be small. An unknown size is not a small one.
             *
             * @par
             * This is not hypothetical across versions: a worker older than the master that places
             * work on it never read the field, so every artifact it fetched - 84 MB included - went
             * down the single-shot path and was refused on every tick, for ever.
             */
            [[nodiscard]] constexpr bool SizeIsKnown(const long size) noexcept {
                return size > 0;
            }

            /**
             * @brief How many parts an object of this size is fetched in.
             *
             * @par
             * The ceiling, and that is the whole point: a floor silently drops the last partial
             * part, which does not fail - it writes a truncated artifact, renames it into place,
             * and leaves a jar that is almost right. The md5 check on the next pass would notice
             * and download it again, equally truncated, for ever.
             */
            [[nodiscard]] constexpr long PartCount(const long size, const long partSize) noexcept {
                if (size <= 0 || partSize <= 0) return 0;
                return (size + partSize - 1) / partSize;
            }

        }// namespace Detail

        /**
         * @brief Downloads one object to `target`, replacing whatever is there.
         *
         * @par
         * Written to a temporary beside the target and renamed over it, for the reason the
         * credentials file is: a process reading the artifact while it is being written would see
         * a partial one, and a download that failed halfway must not leave something that looks
         * like an artifact behind for the next pass to find and believe.
         *
         * @param request what to fetch.
         * @param target where to put it.
         * @param call how to reach ESM - see @ref Call.
         * @return true when `target` holds the object.
         */
        [[nodiscard]] bool Download(const Request &request, const std::filesystem::path &target, const Call &call);

    }// namespace Artifact

}// namespace Euclid::Core
