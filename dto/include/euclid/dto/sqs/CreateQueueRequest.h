//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::SQS {

    struct CreateQueueRequest {

        /**
         * @brief Name of the queue
         */
        std::string name;

        /**
         * @brief Visibility in seconds
         */
        long visibility = 30;

        /**
         * @brief Maximal number of reties
         */
        long maxRetries = 3;

        /**
         * @brief Maximal length of a message in bytes
         */
        long maxMessageLength = 1024 * 1024;

        /**
         * @brief Name of the dead letter queue. Can be empty.
         */
        std::string dlqName;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateQueueRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateQueueRequest>(Core::ParseJsonString(json));
        }

    private:

        friend CreateQueueRequest tag_invoke(boost::json::value_to_tag<CreateQueueRequest>, boost::json::value const &v) {
            CreateQueueRequest r;
            r.name = Core::GetStringValue(v, "name");
            r.visibility = Core::GetLongValue(v, "visibility");
            r.maxRetries = Core::GetLongValue(v, "maxRetries");
            r.maxMessageLength = Core::GetLongValue(v, "maxMessageLength");
            r.dlqName = Core::GetStringValue(v, "dlqName");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateQueueRequest const &obj) {
            jv = {
                    {"name", obj.name},
                    {"visibility", obj.visibility},
                    {"maxRetries", obj.maxRetries},
                    {"maxMessageLength", obj.maxMessageLength},
                    {"dlqName", obj.dlqName},
            };
        }
    };
}