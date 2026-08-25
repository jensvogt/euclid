#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto {

    struct AccountGrant {

        /**
         * @brief Account this grant applies to
         */
        std::string accountId;

        /**
         * @brief Namespaces within accountId this user may access
         */
        std::vector<std::string> namespaces;

        /**
         * @brief Whether this user administers accountId itself
         */
        bool isAdmin{false};

        /**
         * @brief Timestamp the grant was created
         */
        std::string granted;

    private:

        friend AccountGrant tag_invoke(boost::json::value_to_tag<AccountGrant>, boost::json::value const &v) {
            AccountGrant r;
            r.accountId = Core::GetStringValue(v, "accountId");
            r.namespaces = Core::GetStringArrayValue(v, "namespaces");
            r.isAdmin = Core::GetBoolValue(v, "isAdmin");
            r.granted = Core::GetStringValue(v, "granted");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, AccountGrant const &obj) {
            jv = {
                    {"accountId", obj.accountId},
                    {"namespaces", boost::json::value_from(obj.namespaces)},
                    {"isAdmin", obj.isAdmin},
                    {"granted", obj.granted},
            };
        }
    };
}
