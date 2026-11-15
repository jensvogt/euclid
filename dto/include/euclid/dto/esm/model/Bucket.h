// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    using std::chrono::system_clock;

    struct Bucket {

        /**
         * @brief Bucket owner
         */
        std::string owner;

        /**
         * @brief Bucket name
         */
        std::string name;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Size in bytes
         */
        long size{};

        /**
         * @brief Number of objects, not counting directory markers - see @ref directories.
         */
        long objects{};

        /**
         * @brief Number of directory markers - zero-byte keys ending in "/", which a listing shows
         * but which are not objects a client stored. See Entity::ESM::Bucket::directories.
         */
        long directories{};

        /**
         * @brief Queue tags
         */
        std::map<std::string, std::string> tags;

        /**
         * @brief Whether objects written to this bucket from now on are encrypted at rest
         */
        bool encrypted{};

        /**
         * @brief ERN of the EKM key they are encrypted under, empty when the bucket is not encrypted
         */
        std::string encryptionKeyErn;

        /**
         * @brief Whether this is one of euclid's own buckets rather than the caller's.
         *
         * Left out of list-buckets unless an administrator asks for it, and reported here so a
         * listing that does include them can say which rows they are. See
         * Database::Entity::ESM::Bucket::internal.
         */
        bool internal = false;

        /**
         * @brief Priority the notifications this bucket sends are given, empty when it sets none.
         *
         * @par
         * Nothing about the bucket depends on it - see Database::Entity::ESM::Bucket::priority.
         * Reported because a bucket whose notifications are arriving at a priority nobody set here
         * would be a thing to work out from the messages backwards.
         */
        std::string priority;

        /**
         * @brief Whether the bucket's events are held - see "esm stop-bucket".
         */
        bool stopped = false;

        /**
         * @brief Whether an upload over an existing key keeps the object it replaces as a
         * non-current version - see "esm set-bucket-versioning".
         */
        bool versioning = false;

        /**
         * @brief The most non-current versions an object keeps; 0 keeps them all.
         */
        long maxVersions = 0;

        /**
         * @brief Creation date
         */
        system_clock::time_point created;

        /**
         * @brief Last modification date
         */
        system_clock::time_point modified;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]]
        static Bucket fromJson(const std::string &json) {
            return boost::json::value_to<Bucket>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(Bucket, (), (owner, name, ern, size, objects, directories, tags, encrypted, encryptionKeyErn, internal, priority, stopped, versioning, maxVersions, created, modified))

}// namespace Euclid::Dto::ESM