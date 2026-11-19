// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EKM {

    using Json::tag_invoke;

    using std::chrono::system_clock;

    struct Key {

        /**
         * @brief Key name
         */
        std::string name;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief What the key is for, as given when it was created; empty if none was
         */
        std::string description;

        /**
         * @brief Key algorithm
         */
        std::string algorithm;

        /**
         * @brief Queue tags
         */
        std::map<std::string, std::string> tags;

        /**
         * @brief Key length in bits
         */
        long length = 0;

        /**
         * @brief Point in time at which this key will be permanently deleted, ISO8601. Empty if
         * the key is not scheduled for deletion.
         */
        std::string deletionDate;

        /**
         * @brief Lifecycle status: AVAILABLE, REVOKED, or PENDING_DELETION. Only AVAILABLE keys
         * can be used to encrypt; decryption works regardless of status.
         */
        std::string status = "AVAILABLE";

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
        static Key fromJson(const std::string &json) {
            return boost::json::value_to<Key>(Core::ParseJsonString(json));
        }

    private:

        // Only the writer stays hand-written, and only for one field: a key that is not scheduled
        // for deletion has always gone out without a "deletionDate" at all rather than with an
        // empty one, which the generic writer cannot express. Reading is the generic one, so the
        // members the old reader had to name by hand - including "length", which it once forgot -
        // are read because the struct lists them.
        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, Key const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"name", obj.name},
                    {"description", obj.description},
                    {"algorithm", obj.algorithm},
                    {"length", obj.length},
                    {"status", obj.status},
                    {"tags", boost::json::value_from(obj.tags)},
                    {"created", Core::DateTimeUtils::ToISO8601(obj.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(obj.modified)},
            };
            if (!obj.deletionDate.empty()) {
                jv.as_object()["deletionDate"] = obj.deletionDate;
            }
        }
    };

    BOOST_DESCRIBE_STRUCT(Key, (), (name, ern, description, algorithm, tags, length, deletionDate, status, created, modified))

}// namespace Euclid::Dto::EKM
