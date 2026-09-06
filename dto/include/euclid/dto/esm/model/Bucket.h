//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

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
         * @brief Number of objects
         */
        long objects{};

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

    private:

        friend Bucket tag_invoke(boost::json::value_to_tag<Bucket>, boost::json::value const &v) {
            Bucket r;
            r.owner = Core::GetStringValue(v, "owner");
            r.name = Core::GetStringValue(v, "name");
            r.ern = Core::GetStringValue(v, "ern");
            r.size = Core::GetLongValue(v, "size");
            r.objects = Core::GetLongValue(v, "objects");
            r.tags = Core::GetMapFromObject<std::string, std::string>(v, "tags");
            r.encrypted = Core::GetBoolValue(v, "encrypted");
            r.encryptionKeyErn = Core::GetStringValue(v, "encryptionKeyErn");
            r.internal = Core::GetBoolValue(v, "internal");
            r.created = Core::GetDatetimeValue(v, "created");
            r.modified = Core::GetDatetimeValue(v, "modified");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, Bucket const &obj) {
            jv = {
                    {"owner", obj.owner},
                    {"name", obj.name},
                    {"ern", obj.ern},
                    {"size", obj.size},
                    {"objects", obj.objects},
                    {"tags", boost::json::value_from(obj.tags)},
                    {"encrypted", obj.encrypted},
                    {"encryptionKeyErn", obj.encryptionKeyErn},
                    {"internal", obj.internal},
                    {"created", Core::DateTimeUtils::ToISO8601(obj.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(obj.modified)},
            };
        }
    };

}// namespace Euclid::Dto::ESM