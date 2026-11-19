// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// C++ includes
#include <map>
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/DescribedJson.h>

namespace Euclid::Dto::ESS {

    using Json::tag_invoke;

    using std::chrono::system_clock;

    /**
     * @brief A stored secret as it is reported to a caller: everything about it except the secret.
     *
     * @par
     * There is deliberately no value field here. A secret's value leaves the server through
     * get-secret and nothing else, so a list, a create or a rotation cannot answer with one by
     * accident - the type it answers with has nowhere to put it.
     */
    struct Secret {

        /**
         * @brief Secret name, as chosen when it was created
         */
        std::string name;

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief What the secret is for; empty if nothing was said
         */
        std::string description;

        /**
         * @brief ERN of the EKM key the value is encrypted under
         */
        std::string encryptionKeyErn;

        /**
         * @brief How many times the value has been set, counting the first
         */
        long version{};

        /**
         * @brief When the value was last changed
         */
        system_clock::time_point rotated{};

        /**
         * @brief Secret tags
         */
        std::map<std::string, std::string> tags;

        /**
         * @brief Creation date
         */
        system_clock::time_point created{};

        /**
         * @brief Last modification date
         */
        system_clock::time_point modified{};

        /**
         * @brief Serializes this object to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this object from a JSON string
         */
        [[nodiscard]]
        static Secret fromJson(const std::string &json) {
            return boost::json::value_to<Secret>(Core::ParseJsonString(json));
        }
    };

    BOOST_DESCRIBE_STRUCT(Secret, (), (name, ern, description, encryptionKeyErn, version, rotated, tags, created, modified))

}// namespace Euclid::Dto::ESS
