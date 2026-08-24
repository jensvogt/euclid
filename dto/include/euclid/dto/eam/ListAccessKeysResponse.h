//
// Created by vogje01 on 8/18/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eam/model/AccessKey.h>

namespace Euclid::Dto::EAM {

    struct ListAccessKeysResponse : BaseDto {

        /**
         * @brief The caller's access keys.
         */
        std::vector<AccessKey> accessKeys;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListAccessKeysResponse tag_invoke(boost::json::value_to_tag<ListAccessKeysResponse>, boost::json::value const &v) {
            ListAccessKeysResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.accessKeys = boost::json::value_to<std::vector<AccessKey> >(v.at("accessKeys"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListAccessKeysResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"accessKeys", boost::json::value_from(obj.accessKeys)},
            };
        }
    };

}
