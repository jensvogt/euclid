#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EAM {

    struct ChangeNamespaceRequest {

        /**
         * @brief Namespace to switch the caller's active session to. Empty clears it back to
         * unscoped.
         */
        std::string ns;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ChangeNamespaceRequest tag_invoke(boost::json::value_to_tag<ChangeNamespaceRequest>, boost::json::value const &v) {
            ChangeNamespaceRequest r;
            r.ns = Core::GetStringValue(v, "namespace");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ChangeNamespaceRequest const &obj) {
            jv = {
                    {"namespace", obj.ns},
            };
        }
    };

}
