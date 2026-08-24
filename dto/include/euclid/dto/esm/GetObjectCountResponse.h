//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::ESM {

    struct GetObjectCountResponse : BaseDto {

        /**
         * @brief Bucket ERN
         */
        std::string ern{};

        /**
         * @brief Object count
         */
        long count{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetObjectCountResponse tag_invoke(boost::json::value_to_tag<GetObjectCountResponse>, boost::json::value const &v) {
            GetObjectCountResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.ern = Core::GetStringValue(v, "ern");
            r.count = Core::GetLongValue(v, "count");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetObjectCountResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"count", obj.count},
            };
        }
    };

}