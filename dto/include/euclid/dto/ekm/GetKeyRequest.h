// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/19/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EKM {

    /**
     * @brief Asks for one key, by ERN or by name.
     *
     * @par
     * A name is resolved in the caller's own account and namespace - the pair create-key built the
     * ERN from - so it means "my key of that name". An ERN names one key in the installation and is
     * what an encrypted bucket or secret carries.
     *
     * @par
     * The key's description, never its material: what this asks for is what list-keys shows.
     */
    struct GetKeyRequest {

        /**
         * @brief Key ERN, or empty when the key is named instead.
         */
        std::string ern{};

        /**
         * @brief Key name, used when no ERN is given.
         */
        std::string name{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetKeyRequest tag_invoke(boost::json::value_to_tag<GetKeyRequest>, boost::json::value const &v) {
            GetKeyRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetKeyRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"name", obj.name},
            };
        }
    };
}// namespace Euclid::Dto::EKM
