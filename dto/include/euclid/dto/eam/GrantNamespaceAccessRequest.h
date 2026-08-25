#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct GrantNamespaceAccessRequest {

        /**
         * @brief User ERN to grant access to
         */
        std::string user;

        /**
         * @brief Account the namespace belongs to
         */
        std::string accountId;

        /**
         * @brief Namespace within accountId to grant access to
         */
        std::string ns;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GrantNamespaceAccessRequest tag_invoke(boost::json::value_to_tag<GrantNamespaceAccessRequest>, boost::json::value const &v) {
            GrantNamespaceAccessRequest r;
            r.user = Core::GetStringValue(v, "user");
            r.accountId = Core::GetStringValue(v, "accountId");
            r.ns = Core::GetStringValue(v, "namespace");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GrantNamespaceAccessRequest const &obj) {
            jv = {
                    {"user", obj.user},
                    {"accountId", obj.accountId},
                    {"namespace", obj.ns},
            };
        }
    };

}
