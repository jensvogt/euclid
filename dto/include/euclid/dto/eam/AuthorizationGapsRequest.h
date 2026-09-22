// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/12/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    /**
     * @brief Asks what shadow mode has recorded.
     */
    struct AuthorizationGapsRequest {

        /**
         * @brief Only gaps whose "<module>:<action>" starts with this; empty matches all.
         */
        std::string prefix;

        /**
         * @brief Maximum to return; 0 means no limit.
         */
        long pageSize = 50;

        /**
         * @brief Zero-based page index.
         */
        long pageIndex = 0;

        /**
         * @brief Forget everything recorded instead of listing it.
         *
         * @par
         * What an operator does after writing the grants a round of shadow mode called for, so the
         * next round shows what is still missing rather than what used to be.
         */
        bool clear = false;

        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        [[nodiscard]] static AuthorizationGapsRequest fromJson(const std::string &json) {
            return boost::json::value_to<AuthorizationGapsRequest>(Core::ParseJsonString(json));
        }

    private:

        friend AuthorizationGapsRequest tag_invoke(boost::json::value_to_tag<AuthorizationGapsRequest>, boost::json::value const &v) {
            AuthorizationGapsRequest r;
            r.prefix = Core::GetStringValue(v, "prefix");
            r.pageSize = Core::GetLongValue(v, "pageSize", 50);
            r.pageIndex = Core::GetLongValue(v, "pageIndex");
            r.clear = Core::GetBoolValue(v, "clear");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, AuthorizationGapsRequest const &obj) {
            jv = {
                    {"prefix", obj.prefix},
                    {"pageSize", obj.pageSize},
                    {"pageIndex", obj.pageIndex},
                    {"clear", obj.clear},
            };
        }
    };

}
