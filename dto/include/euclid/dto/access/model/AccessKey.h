//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::Access {

    /**
     * @brief Access key summary, as returned by list-access-keys.
     *
     * Deliberately omits secretAccessKey - the secret is only ever returned once, from
     * create-access-key, and is never stored or echoed back afterward.
     */
    struct AccessKey {

        /**
         * @brief Public identifier, e.g. "AKIA...".
         */
        std::string accessKeyId;

        /**
         * @brief Whether this key can currently be used to sign requests.
         */
        bool active{true};

        /**
         * @brief Creation timestamp, ISO8601.
         */
        std::string createdAt;

    private:

        friend AccessKey tag_invoke(boost::json::value_to_tag<AccessKey>, boost::json::value const &v) {
            AccessKey r;
            r.accessKeyId = Core::GetStringValue(v, "accessKeyId");
            r.active = Core::GetBoolValue(v, "active");
            r.createdAt = Core::GetStringValue(v, "createdAt");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, AccessKey const &obj) {
            jv = {
                    {"accessKeyId", obj.accessKeyId},
                    {"active", obj.active},
                    {"createdAt", obj.createdAt},
            };
        }
    };

}
