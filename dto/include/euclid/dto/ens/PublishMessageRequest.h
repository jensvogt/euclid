//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <map>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/com/Variant.h>

namespace Euclid::Dto::ENS {

    struct PublishMessageRequest {

        /**
         * @brief Topic ERN
         */
        std::string ern{};

        /**
         * @brief Message body
         */
        std::string body{};

        /**
         * @brief Message attributes
         */
        std::map<std::string, COM::Variant> attributes{};

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
        static PublishMessageRequest fromJson(const std::string &json) {
            return boost::json::value_to<PublishMessageRequest>(Core::ParseJsonString(json));
        }

    private:

        friend PublishMessageRequest tag_invoke(boost::json::value_to_tag<PublishMessageRequest>, boost::json::value const &v) {
            PublishMessageRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.body = Core::GetStringValue(v, "body");
            r.attributes = Core::GetMapFromObject<std::string, COM::Variant>(v, "attributes");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, PublishMessageRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"body", obj.body},
                    {"attributes", boost::json::value_from(obj.attributes)},
            };
        }
    };

}