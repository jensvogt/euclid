// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/31/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/com/Variant.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESM {

    using Json::tag_invoke;

    /**
     * @brief One object attribute, as it now stands in the store.
     */
    struct ObjectAttributeResponse {

        /**
         * @brief ERN of the object
         */
        std::string ern;

        /**
         * @brief Attribute name
         */
        std::string name;

        /**
         * @brief Attribute value, typed
         */
        COM::Variant value;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this response from a JSON string
         */
        [[nodiscard]]
        static ObjectAttributeResponse fromJson(const std::string &json) {
            return boost::json::value_to<ObjectAttributeResponse>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(ObjectAttributeResponse, (), (ern, name, value))

}// namespace Euclid::Dto::ESM
