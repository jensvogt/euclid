//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EQS {

    struct DeleteQueueRequest {

        /**
         * @brief ERN of the queue
         */
        std::string ern;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]]
        static DeleteQueueRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteQueueRequest>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteQueueRequest tag_invoke(boost::json::value_to_tag<DeleteQueueRequest>, boost::json::value const &v) {
            DeleteQueueRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteQueueRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
            };
        }
    };
}
