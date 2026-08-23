//
// Created by vogje01 on 8/23/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::Storage {

    using std::chrono::system_clock;

    struct Object {

        /**
         * @brief ERN
         */
        std::string ern;

        /**
         * @brief Bucket ERN
         */
        std::string bucketErn;

        /**
         * @brief Object key
         */
        std::string key;

        /**
         * @brief Size in bytes
         */
        long size{};

        /**
         * @brief Lifecycle status: CREATED, UPLOADING, UPLOADED or COMPLETED
         */
        std::string status;

        /**
         * @brief MIME content type; empty until status reaches COMPLETED
         */
        std::string contentType;

        /**
         * @brief MD5 checksum, hex-encoded; empty until status reaches COMPLETED
         */
        std::string md5Sum;

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
        static Object fromJson(const std::string &json) {
            return boost::json::value_to<Object>(Core::ParseJsonString(json));
        }

    private:

        friend Object tag_invoke(boost::json::value_to_tag<Object>, boost::json::value const &v) {
            Object r;
            r.bucketErn = Core::GetStringValue(v, "bucketErn");
            r.ern = Core::GetStringValue(v, "ern");
            r.key = Core::GetStringValue(v, "key");
            r.size = Core::GetLongValue(v, "size");
            r.status = Core::GetStringValue(v, "status");
            r.contentType = Core::GetStringValue(v, "contentType");
            r.md5Sum = Core::GetStringValue(v, "md5Sum");
            r.created = Core::GetDatetimeValue(v, "created");
            r.modified = Core::GetDatetimeValue(v, "modified");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, Object const &obj) {
            jv = {
                    {"bucketErn", obj.bucketErn},
                    {"ern", obj.ern},
                    {"key", obj.key},
                    {"size", obj.size},
                    {"status", obj.status},
                    {"contentType", obj.contentType},
                    {"md5Sum", obj.md5Sum},
                    {"created", Core::DateTimeUtils::ToISO8601(obj.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(obj.modified)},
            };
        }
    };

}// namespace Euclid::Dto::Storage