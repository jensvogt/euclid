//
// Created by vogje01 on 9/2/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EKM {

    /**
     * @brief The key as it now reads.
     *
     * @par
     * Only the description can have changed - nothing about the key material, its status or its
     * lifecycle is touched by describing it - so this carries just enough to identify the key the
     * text now belongs to.
     */
    struct SetKeyDescriptionResponse {

        /**
         * @brief Euclid resource name
         */
        std::string ern;

        /**
         * @brief Key name, as "ekm list-keys" and "ekm delete-key" refer to it
         */
        std::string name;

        /**
         * @brief The description now stored with the key
         */
        std::string description;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this response from a JSON string
         */
        [[nodiscard]] static SetKeyDescriptionResponse fromJson(const std::string &json) {
            return boost::json::value_to<SetKeyDescriptionResponse>(Core::ParseJsonString(json));
        }

    private:

        friend SetKeyDescriptionResponse tag_invoke(boost::json::value_to_tag<SetKeyDescriptionResponse>, boost::json::value const &v) {
            SetKeyDescriptionResponse r;
            r.ern = Core::GetStringValue(v, "ern");
            r.name = Core::GetStringValue(v, "name");
            r.description = Core::GetStringValue(v, "description");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetKeyDescriptionResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"name", obj.name},
                    {"description", obj.description},
            };
        }
    };
}
