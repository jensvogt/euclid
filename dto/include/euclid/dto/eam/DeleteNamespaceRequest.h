#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct DeleteNamespaceRequest {

        /**
         * @brief Account the namespace belongs to
         */
        std::string accountId;

        /**
         * @brief Namespace name to delete
         */
        std::string name;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend DeleteNamespaceRequest tag_invoke(boost::json::value_to_tag<DeleteNamespaceRequest>, boost::json::value const &v) {
            DeleteNamespaceRequest r;
            r.accountId = Core::GetStringValue(v, "accountId");
            r.name = Core::GetStringValue(v, "name");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteNamespaceRequest const &obj) {
            jv = {
                    {"accountId", obj.accountId},
                    {"name", obj.name},
            };
        }
    };

}
