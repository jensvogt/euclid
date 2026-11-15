// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/18/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ENS {

    using Json::tag_invoke;

    struct GetMessageAttributeRequest {

        /**
         * @brief Message ID
         */
        std::string messageId{};

        /**
         * @brief Key of the attribute to look up
         */
        std::string key{};

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
        static GetMessageAttributeRequest fromJson(const std::string &json) {
            return boost::json::value_to<GetMessageAttributeRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(GetMessageAttributeRequest, (), (messageId, key))

}