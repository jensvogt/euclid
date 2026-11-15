// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/31/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    /**
     * @brief Names the object attribute to remove.
     */
    struct DeleteObjectAttributeRequest {

        /**
         * @brief ERN of the object
         */
        std::string ern;

        /**
         * @brief Attribute name
         */
        std::string name;

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
        static DeleteObjectAttributeRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteObjectAttributeRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(DeleteObjectAttributeRequest, (), (ern, name))

}// namespace Euclid::Dto::ESM
