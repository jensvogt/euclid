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
     * @brief Names one attribute of one object, with the value to store.
     *
     * @par
     * Shared by add-object-attribute and set-object-attribute, which differ only in what they
     * expect to find already there: adding refuses an attribute that exists, setting refuses one
     * that does not. The two carry identical fields, so one request type serves both - the same
     * way EQS's queue tags reuse AddQueueTagRequest for add and set.
     */
    struct ObjectAttributeRequest {

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
        static ObjectAttributeRequest fromJson(const std::string &json) {
            return boost::json::value_to<ObjectAttributeRequest>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(ObjectAttributeRequest, (), (ern, name, value))

}// namespace Euclid::Dto::ESM
