//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EKM {

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

        friend Key tag_invoke(boost::json::value_to_tag<Key>, boost::json::value const &v) {
            Key r;
            r.ern = Core::GetStringValue(v, "ern");
            r.name = Core::GetStringValue(v, "name");
            r.description = Core::GetStringValue(v, "description");
            r.algorithm = Core::GetStringValue(v, "algorithm");
            // Written by the serializer below but never read back until now, so a Key that went
            // through JSON came out with a length of zero.
            r.length = Core::GetLongValue(v, "length");
            r.tags = Core::GetMapFromObject<std::string, std::string>(v, "tags");
            r.status = Core::GetStringValue(v, "status");
            r.created = Core::GetDatetimeValue(v, "created");
            r.modified = Core::GetDatetimeValue(v, "modified");
            r.deletionDate = Core::GetStringValue(v, "deletionDate");
            return r;
        }

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

}// namespace Euclid::Dto::EKM