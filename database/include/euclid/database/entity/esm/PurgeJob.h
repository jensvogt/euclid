// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <chrono>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value-fwd.hpp>

// Euclid includes
#include <euclid/database/entity/BaseEntity.h>

namespace Euclid::Database::Entity::ESM {

    /**
     * @brief A removal that has been accepted and is not finished.
     *
     * @par Why the work is written down
     * An `--async` purge is answered at once and done on a thread, which is the only way to empty a
     * bucket that takes minutes. The thread is the problem: nothing outside the process knows it
     * exists. The autoscaler stops an instance it sees no requests on, a crash takes the thread
     * with it, and `emm restart-module` and a manager shutdown do the same - and in every case the
     * removal simply stopped, with no record that it had ever been asked for.
     *
     * @par
     * So the request is written here before the thread starts, and removed only when the work is
     * done. Whichever instance is alive picks up a job nobody is working on and carries on. That
     * costs one document per outstanding purge and makes the answer to "is it still going" a query
     * rather than a guess.
     *
     * @par Why it can be resumed at all
     * Because the removal takes a page at a time and always asks for the next page from the
     * beginning - see EsmServer's removeBucketObjectsPaged(). An interrupted run has removed whole
     * pages, not half of anything, so resuming is just carrying on, and the bucket document is
     * deleted last so there is still something to resume against.
     *
     * @author jensvogt47\@gmail.com
     */
    struct PurgeJob final : BaseEntity {

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief This job's own id, and what the caller is told so it can ask after it.
         */
        std::string jobId;

        /**
         * @brief Bucket whose objects are being removed.
         */
        std::string bucketErn;

        /**
         * @brief Key prefix the removal is limited to; empty is the whole bucket.
         */
        std::string prefix;

        /**
         * @brief Whether the bucket itself goes once its objects have.
         */
        bool deleteBucket{false};

        /**
         * @brief Whether each removed object is announced to the bucket's subscribers.
         *
         * @par
         * True is what a purge has always done and stays the default: a subscriber keeping an
         * index of keys needs to know which ones went, and "the bucket was purged" does not say.
         * It is a request-time choice rather than a running one, so it lives on the job - an async
         * purge is picked up by whichever instance claims it, possibly after a restart, and the
         * operator who asked for silence is long gone by then.
         */
        bool notify{true};

        /**
         * @brief Who asked, carried because the delete events each removed object raises say so -
         * and the instance that resumes the job is not the one that was asked.
         */
        std::string userId;

        /**
         * @brief The instance working this job, empty when nobody is.
         *
         * @par
         * Written by whoever claims it, and refreshed as the work proceeds - see claimedAt. A job
         * whose claim has gone stale is one whose worker died, and is free to be taken again.
         */
        std::string claimedBy;

        /**
         * @brief When the claim was last refreshed.
         *
         * @par
         * The heartbeat, not the claim time: a worker touches this after every page, so "stale"
         * means "no page finished recently" rather than "started a while ago". A purge of a large
         * bucket is legitimately long, and must not be stolen from a worker that is making
         * progress.
         */
        std::chrono::system_clock::time_point claimedAt{};

        /**
         * @brief Objects removed so far, across every worker that has touched this job.
         */
        long removedObjects{};

        /**
         * @brief Bytes removed so far.
         */
        long removedSize{};

        /**
         * @brief Creation timestamp
         */
        std::chrono::system_clock::time_point created = std::chrono::system_clock::now();

        /**
         * @brief Last modification timestamp
         */
        std::chrono::system_clock::time_point modified = std::chrono::system_clock::now();

        /**
         * @brief Converts the entity to a MongoDB document
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Converts a MongoDB document to an entity
         */
        static PurgeJob fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}// namespace Euclid::Database::Entity::ESM
