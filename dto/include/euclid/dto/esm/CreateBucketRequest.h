//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESM {

    struct CreateBucketRequest {

        /**
         * @brief Name of the queue
         */
        std::string name;

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateBucketRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateBucketRequest>(Core::ParseJsonString(json));
        }

    private:

        friend CreateBucketRequest tag_invoke(boost::json::value_to_tag<CreateBucketRequest>, boost::json::value const &v) {
            CreateBucketRequest r;
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateBucketRequest const &obj) {
            jv = {
                    {"name", obj.name},
            };
        }
    };
}