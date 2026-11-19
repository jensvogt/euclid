// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::EAM {

    using Json::tag_invoke;

    struct CreateNamespaceRequest {

        /**
         * @brief Account the namespace belongs to
         */
        std::string accountId;

        /**
         * @brief Namespace name, unique within accountId
         */
        std::string name;

        /**
         * @brief Free-text description of the namespace's purpose
         */
        std::string description;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(CreateNamespaceRequest, (), (accountId, name, description))

}
