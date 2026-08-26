//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct PurgeAllTopicsRequest {

        /**
         * @brief Region of the topics
         */
        std::string region;

        /**
         * @brief Account ID of the topics
         */
        std::string accountId;

        /**
         * @brief Name space
         */
        std::string nameSpace;

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
        static PurgeAllTopicsRequest fromJson(const std::string &json) {
            return boost::json::value_to<PurgeAllTopicsRequest>(Core::ParseJsonString(json));
        }

    private:

        friend PurgeAllTopicsRequest tag_invoke(boost::json::value_to_tag<PurgeAllTopicsRequest>, boost::json::value const &v) {
            PurgeAllTopicsRequest r;
            r.region = Core::GetStringValue(v, "region");
            r.accountId = Core::GetStringValue(v, "accountId");
            r.nameSpace = Core::GetStringValue(v, "nameSpace");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, PurgeAllTopicsRequest const &obj) {
            jv = {
                    {"region", obj.region},
                    {"accountId", obj.accountId},
                    {"nameSpace", obj.nameSpace},
            };
        }
    };
}