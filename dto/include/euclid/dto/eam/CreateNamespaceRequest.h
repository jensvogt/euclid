#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct CreateNamespaceRequest {

        /**
         * @brief Account the namespace belongs to
         */
        std::string accountId;

        /**
         * @brief Namespace name, unique within accountId
         */
        std::string name;

        /**
         * @brief Free-text description of the namespace's purpose
         */
        std::string description;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CreateNamespaceRequest tag_invoke(boost::json::value_to_tag<CreateNamespaceRequest>, boost::json::value const &v) {
            CreateNamespaceRequest r;
            r.accountId = Core::GetStringValue(v, "accountId");
            r.name = Core::GetStringValue(v, "name");
            r.description = Core::GetStringValue(v, "description");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateNamespaceRequest const &obj) {
            jv = {
                    {"accountId", obj.accountId},
                    {"name", obj.name},
                    {"description", obj.description},
            };
        }
    };

}
