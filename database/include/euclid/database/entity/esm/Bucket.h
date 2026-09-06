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
     * @author jens.vogt\@opitz-consulting.com
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
         * @brief Bucket size in bytes
         */
        int64_t size{};

        /**
         * @brief Number of objects
         */
        int64_t objects{};

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

}// namespace Euclid::Database::Entity::SQS