//
// Created by vogje01 on 9/2/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::ESM {

    /**
     * @brief The bucket as it is now, and how much had to be rewritten to get there.
     */
    struct RenameBucketResponse : BaseDto {

        /**
         * @brief New name of the bucket
         */
        std::string name;

        /**
         * @brief New ERN of the bucket - it carries the name, so it changed too
         */
        std::string ern;

        /**
         * @brief Number of objects whose bucket reference and ERN were rewritten
         */
        long objects = 0;

        /**
         * @brief Number of bucket subscriptions repointed at the new ERN
         */
        long subscriptions = 0;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend RenameBucketResponse tag_invoke(boost::json::value_to_tag<RenameBucketResponse>, boost::json::value const &v) {
            RenameBucketResponse r;
            r.name = Core::GetStringValue(v, "name");
            r.ern = Core::GetStringValue(v, "ern");
            r.objects = Core::GetLongValue(v, "objects");
            r.subscriptions = Core::GetLongValue(v, "subscriptions");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, RenameBucketResponse const &obj) {
            jv = {
                    {"name", obj.name},
                    {"ern", obj.ern},
                    {"objects", obj.objects},
                    {"subscriptions", obj.subscriptions},
            };
        }
    };

}// namespace Euclid::Dto::ESM
