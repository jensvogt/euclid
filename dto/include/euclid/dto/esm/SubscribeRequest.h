//
// Created by vogje01 on 8/27/26.
//

#pragma once

#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    struct SubscribeRequest {

        /**
         * @brief ERN of the bucket objects are created in.
         */
        std::string sourceErn;

        /**
         * @brief Delivery protocol. Only "SQS" is supported for now.
         */
        std::string type;

        /**
         * @brief ERN of the delivery target. For type "SQS", an EQS queue ERN.
         */
        std::string targetErn;

        /**
         * @brief Which object events to deliver, empty for all of them: "esm.object.created",
         * "esm.object.updated", "esm.object.deleted".
         */
        std::vector<std::string> eventTypes;

        /**
         * @brief Only deliver objects whose key starts with this, empty for the whole bucket.
         */
        std::string prefix;

        /**
         * @brief Whether directory markers - the zero-byte objects whose key ends in "/" - count
         * as objects worth delivering. Off by default: most subscribers want files.
         */
        bool directories = false;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static SubscribeRequest fromJson(const std::string &json) {
            return boost::json::value_to<SubscribeRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SubscribeRequest tag_invoke(boost::json::value_to_tag<SubscribeRequest>, boost::json::value const &v) {
            SubscribeRequest r;
            r.sourceErn = Core::GetStringValue(v, "sourceErn");
            r.type = Core::GetStringValue(v, "type");
            r.targetErn = Core::GetStringValue(v, "targetErn");
            r.eventTypes = Core::GetStringArrayValue(v, "eventTypes");
            r.prefix = Core::GetStringValue(v, "prefix");
            r.directories = Core::GetBoolValue(v, "directories");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SubscribeRequest const &obj) {
            jv = {
                    {"sourceErn", obj.sourceErn},
                    {"type", obj.type},
                    {"targetErn", obj.targetErn},
                    {"eventTypes", boost::json::value_from(obj.eventTypes)},
                    {"prefix", obj.prefix},
                    {"directories", obj.directories},
            };
        }
    };
}
