// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ESS {

    /**
     * @brief Names one secret, for the actions that need nothing else: get-secret and
     * delete-secret.
     */
    struct SecretNameRequest {

        /**
         * @brief Secret name, within the caller's own account and namespace
         */
        std::string name;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static SecretNameRequest fromJson(const std::string &json) {
            return boost::json::value_to<SecretNameRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SecretNameRequest tag_invoke(boost::json::value_to_tag<SecretNameRequest>, boost::json::value const &v) {
            SecretNameRequest r;
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SecretNameRequest const &obj) {
            jv = {{"name", obj.name}};
        }
    };

}// namespace Euclid::Dto::ESS
