#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct DeleteAccountRequest {

        /**
         * @brief Account ID to delete
         */
        std::string accountId;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend DeleteAccountRequest tag_invoke(boost::json::value_to_tag<DeleteAccountRequest>, boost::json::value const &v) {
            DeleteAccountRequest r;
            r.accountId = Core::GetStringValue(v, "accountId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteAccountRequest const &obj) {
            jv = {
                    {"accountId", obj.accountId},
            };
        }
    };

}
