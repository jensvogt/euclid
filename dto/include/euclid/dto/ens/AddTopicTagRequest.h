//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct AddTopicTagRequest {

        /**
         * @brief Topic ERN
         */
        std::string ern;

        /**
         * @brief Key
         */
        std::string key;

        /**
         * @brief Value
         */
        std::string value;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static AddTopicTagRequest fromJson(const std::string &json) {
            return boost::json::value_to<AddTopicTagRequest>(Core::ParseJsonString(json));
        }

    private:

        friend AddTopicTagRequest tag_invoke(boost::json::value_to_tag<AddTopicTagRequest>, boost::json::value const &v) {
            AddTopicTagRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.key = Core::GetStringValue(v, "key");
            r.value = Core::GetStringValue(v, "value");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, AddTopicTagRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"key", obj.key},
                    {"value", obj.value},
            };
        }
    };
}