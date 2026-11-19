// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    /**
     * @brief Every permission a role can hold.
     *
     * @par
     * Generated from the modules' own dispatch tables, so it is exactly what can be granted -
     * the modules that are not bindable at all contribute nothing and are named separately, so a
     * caller can say why they are missing rather than wondering.
     */
    struct ListPermissionsResponse : BaseDto {

        /**
         * @brief Every permission, sorted, as "<module>:<action>".
         */
        std::vector<std::string> permissions;

        /**
         * @brief The modules that have permissions, sorted.
         */
        std::vector<std::string> modules;

        /**
         * @brief The modules whose actions can never be granted - today "emd" and "emm".
         */
        std::vector<std::string> unbindableModules;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(ListPermissionsResponse, (BaseDto), (permissions, modules, unbindableModules))

}
