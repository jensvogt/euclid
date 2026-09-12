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

namespace Euclid::Dto::EAM {

    /**
     * @brief Lists the roles the caller's account can bind.
     */
    struct ListRolesRequest {

        /**
         * @brief Only roles whose name starts with this; empty matches all.
         */
        std::string prefix{};

        /**
         * @brief Maximum to return.
         */
        long pageSize = 10;

        /**
         * @brief Zero-based page index.
         */
        long pageIndex = 0;

        /**
         * @brief Field to sort by.
         */
        std::string sortColumn = "name";

        /**
         * @brief "asc" or "desc".
         */
        std::string sortDirection = "asc";

        /**
         * @brief Whether to include the built-in roles, which are not stored and are not paged. True by default in the handler, since a caller listing roles almost always wants to see what it can bind.
         */
        bool includeBuiltin = true;

        /**
         * @brief Serializes this to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this from a JSON string
         */
        [[nodiscard]] static ListRolesRequest fromJson(const std::string &json) {
            return boost::json::value_to<ListRolesRequest>(Core::ParseJsonString(json));
        }

    private:

        friend ListRolesRequest tag_invoke(boost::json::value_to_tag<ListRolesRequest>, boost::json::value const &v) {
            ListRolesRequest r;
            r.prefix = Core::GetStringValue(v, "prefix");
            r.pageSize = Core::GetLongValue(v, "pageSize", 10);
            r.pageIndex = Core::GetLongValue(v, "pageIndex", 0);
            r.sortColumn = Core::GetStringValue(v, "sortColumn");
            r.sortDirection = Core::GetStringValue(v, "sortDirection");
            r.includeBuiltin = Core::GetBoolValue(v, "includeBuiltin");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListRolesRequest const &obj) {
            jv = {
                    {"prefix", obj.prefix},
                    {"pageSize", obj.pageSize},
                    {"pageIndex", obj.pageIndex},
                    {"sortColumn", obj.sortColumn},
                    {"sortDirection", obj.sortDirection},
                    {"includeBuiltin", obj.includeBuiltin},
            };
        }
    };

}
