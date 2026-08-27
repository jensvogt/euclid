//
// Created by vogje01 on 8/27/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    using std::chrono::system_clock;

    struct Subscription {

        /**
         * @brief Euclid resource name of the subscription
         */
        std::string ern;

        /**
         * @brief ERN of the topic messages are published to
         */
        std::string sourceErn;

        /**
         * @brief Delivery protocol
         */
        std::string type;

        /**
         * @brief ERN of the delivery target
         */
        std::string targetErn;

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
        static Subscription fromJson(const std::string &json) {
            return boost::json::value_to<Subscription>(Core::ParseJsonString(json));
        }

    private:

        friend Subscription tag_invoke(boost::json::value_to_tag<Subscription>, boost::json::value const &v) {
            Subscription r;
            r.ern = Core::GetStringValue(v, "ern");
            r.sourceErn = Core::GetStringValue(v, "sourceErn");
            r.type = Core::GetStringValue(v, "type");
            r.targetErn = Core::GetStringValue(v, "targetErn");
            r.created = Core::GetDatetimeValue(v, "created");
            r.modified = Core::GetDatetimeValue(v, "modified");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, Subscription const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"sourceErn", obj.sourceErn},
                    {"type", obj.type},
                    {"targetErn", obj.targetErn},
                    {"created", Core::DateTimeUtils::ToISO8601(obj.created)},
                    {"modified", Core::DateTimeUtils::ToISO8601(obj.modified)},
            };
        }
    };

}// namespace Euclid::Dto::ENS
