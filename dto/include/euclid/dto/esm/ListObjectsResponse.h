// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/23/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/esm/model/Object.h>

namespace Euclid::Dto::ESM {

    struct ListObjectsResponse : BaseDto {

        /**
         * @brief Bucket list
         */
        std::vector<Object> objects;

        /**
         * @brief total number of object
         */
        long total{};

        /**
         * @brief The directories one level below the prefix, each ending in "/".
         *
         * @par
         * Only filled when the request named a delimiter - see ListObjectsRequest::delimiter.
         * Empty otherwise, and empty in a level that holds nothing but objects.
         *
         * @par
         * A directory here is not an object: it is a path that objects lie under, whether or not
         * anything was ever written to mark it. A bucket is flat, so these are derived rather
         * than stored.
         */
        std::vector<std::string> commonPrefixes;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListObjectsResponse tag_invoke(boost::json::value_to_tag<ListObjectsResponse>, boost::json::value const &v) {
            ListObjectsResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.objects = boost::json::value_to<std::vector<Object> >(v.at("objects"));
            r.total = Core::GetLongValue(v, "total");
            if (const auto *prefixes = v.is_object() ? v.as_object().if_contains("commonPrefixes") : nullptr;
                prefixes != nullptr && prefixes->is_array()) {
                for (const auto &prefix: prefixes->as_array()) {
                    if (prefix.is_string()) r.commonPrefixes.emplace_back(prefix.as_string());
                }
            }
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListObjectsResponse const &obj) {
            jv = {
                    {"objects", boost::json::value_from(obj.objects)},
                    {"total", obj.total},
                    {"commonPrefixes", boost::json::value_from(obj.commonPrefixes)},
            };
        }
    };

}// namespace Euclid::Dto::ESM