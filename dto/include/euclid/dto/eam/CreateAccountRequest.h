#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct CreateAccountRequest {

        /**
         * @brief Account ID, unique across the deployment
         */
        std::string accountId;

        /**
         * @brief Human-readable account name
         */
        std::string name;

        /**
         * @brief Free-text description of the account's purpose
         */
        std::string description;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend CreateAccountRequest tag_invoke(boost::json::value_to_tag<CreateAccountRequest>, boost::json::value const &v) {
            CreateAccountRequest r;
            r.accountId = Core::GetStringValue(v, "accountId");
            r.name = Core::GetStringValue(v, "name");
            r.description = Core::GetStringValue(v, "description");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateAccountRequest const &obj) {
            jv = {
                    {"accountId", obj.accountId},
                    {"name", obj.name},
                    {"description", obj.description},
            };
        }
    };

}
