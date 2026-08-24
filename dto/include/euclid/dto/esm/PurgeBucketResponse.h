//
// Created by vogje01 on 8/24/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::ESM {

    struct PurgeBucketResponse : BaseDto {

        /**
         * @brief Bucket ERN
         */
        std::string ern{};

        /**
         * @brief Number of objects removed from the bucket
         */
        long count{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend PurgeBucketResponse tag_invoke(boost::json::value_to_tag<PurgeBucketResponse>, boost::json::value const &v) {
            PurgeBucketResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.ern = Core::GetStringValue(v, "ern");
            r.count = Core::GetLongValue(v, "count");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, PurgeBucketResponse const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"count", obj.count},
            };
        }
    };

}// namespace Euclid::Dto::ESM
