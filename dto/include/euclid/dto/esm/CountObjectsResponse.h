// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::ESM {

    /**
     * @brief How many objects a bucket holds under a prefix, counted at the moment of asking.
     */
    struct CountObjectsResponse : BaseDto {

        /**
         * @brief Bucket ERN
         */
        std::string ern{};

        /**
         * @brief The prefix this count was taken under, echoed so an answer is readable on its
         * own - a bare number does not say what it counted.
         */
        std::string prefix{};

        /**
         * @brief Whether directory markers were counted.
         */
        bool includeDirectories{false};

        /**
         * @brief Object count
         */
        long count{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CountObjectsResponse tag_invoke(boost::json::value_to_tag<CountObjectsResponse>, boost::json::value const &v) {
            CountObjectsResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.ern = Core::GetStringValue(v, "ern");
            r.prefix = Core::GetStringValue(v, "prefix");
            r.includeDirectories = Core::GetBoolValue(v, "includeDirectories");
            r.count = Core::GetLongValue(v, "count");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CountObjectsResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"prefix", obj.prefix},
                    {"includeDirectories", obj.includeDirectories},
                    {"count", obj.count},
            };
        }
    };

}// namespace Euclid::Dto::ESM
