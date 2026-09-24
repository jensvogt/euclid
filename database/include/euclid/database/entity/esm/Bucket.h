// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 01/06/2023.
//

#pragma once

// C++ includes
#include <chrono>
#include <map>
#include <optional>
#include <string>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value-fwd.hpp>

// Euclid includes
#include <euclid/database/entity/BaseEntity.h>

namespace Euclid::Database::Entity::ESM {

    using std::chrono::system_clock;

    /**
     * @brief Storage bucket entity
     *
     * @author jensvogt47\@gmail.com
     */
    struct Bucket final : BaseEntity {

        /**
         * @brief Owner
         */
        std::string owner;

        /**
         * @brief ID
         */
        std::string oid;

        /**
         * @brief Bucket name
         */
        std::string name;

        /**
         * @brief Bucket ERN
         */
        std::string ern;

        /**
         * @brief Queue tags
         */
        std::map<std::string, std::string> tags;

        /**
         * @brief ERN of the EKM key objects written to this bucket are encrypted under, or empty
         * if the bucket stores objects in the clear.
         *
         * @par
         * A bucket-wide setting that applies from the moment it is made: objects already in the
         * bucket are left exactly as they were stored, and each object records the key it is
         * actually under (see Object::encryptionKeyErn), so a bucket can hold objects written
         * before encryption was enabled, after it, and under a previous key, all readable. This
         * field only ever answers "what happens to the next object written here".
         */
        std::string encryptionKeyErn;

        /**
         * @brief Whether this bucket is euclid's own plumbing rather than a user's bucket.
         *
         * @par
         * An internal bucket is an ordinary bucket in every respect that matters - objects are
         * written, read, copied, moved and deleted exactly the same way - except that it is left
         * out of list-buckets and the bucket count, so nothing offers it to somebody who did not
         * make it and has no reason to act on it. The bucket applications are deployed from is the
         * case this exists for: its contents are artifacts EAP puts there and replaces on a
         * redeploy, and an operator browsing their own buckets should not have to step around it.
         *
         * @par
         * Hidden, not protected - the same bargain Entity::EQS::Queue::internal makes. A caller
         * that knows the name can still use the bucket, which is exactly what the component that
         * created it does.
         */
        bool internal = false;

        /**
         * @brief Priority the notifications this bucket sends are given, or empty for none.
         *
         * @par
         * The bucket itself does nothing with it. A bucket is not consumed from and has no queue of
         * its own, so there is nothing here for a priority to mean - it exists only to be handed on,
         * to the messages a subscription of this bucket turns an object event into. "Everything that
         * lands in this bucket is urgent" is the statement it makes, and the queue on the other side
         * of the subscription is where that statement finally has an effect.
         *
         * @par
         * Empty means the bucket says nothing, which is not the same as saying MEDIUM: a notification
         * from a bucket with no priority set takes the target queue's own default, exactly as it did
         * before buckets had this. That distinction is why this is a string and not a
         * MessagePriority - the enum has no way to spell "unset", and defaulting it to MEDIUM would
         * have every bucket silently overriding every queue.
         *
         * @par
         * Less specific than the object's own. Whatever writes an object may set a priority in its
         * system attributes, and that is a statement about one object where this is a statement about
         * all of them - so it wins. See NotificationPriority().
         */
        std::string priority;

        /**
         * @brief Bucket size in bytes
         */
        int64_t size{};

        /**
         * @brief Number of objects, not counting directory markers - see @ref directories.
         */
        int64_t objects{};

        /**
         * @brief Number of directory markers.
         *
         * @par
         * Kept apart from @ref objects because they answer different questions. A directory marker
         * is a zero-byte object whose key ends in "/", stored so that an empty directory stays in
         * existence for a transfer client to change into; it is not something a client put in the
         * bucket, and every path that reads the bucket already leaves it out - a listing hides it,
         * touch-object will not announce it, and the incremental counters never counted it.
         *
         * @par
         * Counted rather than merely excluded, because a transfer bucket is mostly structure: the
         * FTP landing area held one file and four directories, and reporting "1 object" alone
         * makes four rows a listing does show look like nothing at all.
         */
        int64_t directories{};

        /**
         * @brief Creation date
         */
        system_clock::time_point created = system_clock::now();

        /**
         * @brief Last modification date
         */
        system_clock::time_point modified = system_clock::now();

        /**
         * @brief Converts the entity to a MongoDB document
         *
         * @return entity as a MongoDB document.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Converts the MongoDB document to an entity
         *
         * @param document MongoDB document.
         */
        static Bucket fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

    /**
     * @brief The priority a notification about one object should carry.
     *
     * @par
     * Two statements can be in play and they are not the same size. A bucket's priority says
     * "everything from here is urgent"; the priority in an object's system attributes says "this one
     * is". The narrower statement wins, which is the only ordering that lets a bucket set a floor
     * without taking away the ability to say more about a particular object.
     *
     * @par
     * Both empty means empty, and that is deliberate rather than a default of MEDIUM: an empty
     * answer leaves the target queue's own default in force, which is what happened before a bucket
     * could carry a priority at all. Returning MEDIUM here would silently override every queue that
     * had chosen something else.
     *
     * @par
     * Not validated here, only chosen. Whether the string names a real priority was settled when it
     * was stored - set-bucket-priority refuses one that does not - and the reader downstream falls
     * back to the queue default for anything it cannot parse.
     *
     * @param objectPriority the priority in the object's system attributes, or empty.
     * @param bucketPriority the bucket's own, or empty.
     * @return the one to put on the notification, or empty to leave it unsaid.
     */
    inline std::string NotificationPriority(const std::string &objectPriority, const std::string &bucketPriority) {
        return !objectPriority.empty() ? objectPriority : bucketPriority;
    }

}// namespace Euclid::Database::Entity::SQS