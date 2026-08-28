//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/ekm/model/Key.h>

namespace Euclid::Dto::EKM {

    struct ListKeysResponse : BaseDto {

        /**
         * @brief Keys list
         */
        std::vector<Key> keys;

        /**
         * @brief total number of users
         */
        long total{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListKeysResponse tag_invoke(boost::json::value_to_tag<ListKeysResponse>, boost::json::value const &v) {
            ListKeysResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.keys = boost::json::value_to<std::vector<Key> >(v.at("keys"));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListKeysResponse const &obj) {
            jv = {
                    {"keys", boost::json::value_from(obj.keys)},
                    {"total", obj.total},
            };
        }
    };

}