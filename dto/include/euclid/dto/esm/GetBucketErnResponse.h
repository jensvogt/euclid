//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::ESM {

    struct GetBucketErnResponse : BaseDto {

        /**
         * @brief Bucket ERN
         */
        std::string ern{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetBucketErnResponse tag_invoke(boost::json::value_to_tag<GetBucketErnResponse>, boost::json::value const &v) {
            GetBucketErnResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.ern = Core::GetStringValue(v, "ern");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetBucketErnResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
            };
        }
    };

}