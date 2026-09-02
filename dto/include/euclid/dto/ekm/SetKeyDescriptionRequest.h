//
// Created by vogje01 on 9/2/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EKM {

    /**
     * @brief Names the key whose description should be replaced, and what to replace it with.
     *
     * @par
     * The description is free text recorded with a key to say what it is for - create-key takes one,
     * and this is how it is corrected afterwards. An empty description is a valid one: it clears
     * whatever the key carried, rather than being taken as "leave it alone".
     */
    struct SetKeyDescriptionRequest {

        /**
         * @brief ERN of the key to describe
         */
        std::string ern;

        /**
         * @brief What the key is for; empty clears the current description
         */
        std::string description;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static SetKeyDescriptionRequest fromJson(const std::string &json) {
            return boost::json::value_to<SetKeyDescriptionRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SetKeyDescriptionRequest tag_invoke(boost::json::value_to_tag<SetKeyDescriptionRequest>, boost::json::value const &v) {
            SetKeyDescriptionRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.description = Core::GetStringValue(v, "description");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetKeyDescriptionRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"description", obj.description},
            };
        }
    };
}
