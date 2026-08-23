//
// Created by vogje01 on 8/23/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/storage/model/Bucket.h>

namespace Euclid::Dto::Storage {

    struct ListBucketsResponse : BaseDto {

        /**
         * @brief Bucket list
         */
        std::vector<Bucket> buckets;

        /**
         * @brief total number of buckets
         */
        long total{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListBucketsResponse tag_invoke(boost::json::value_to_tag<ListBucketsResponse>, boost::json::value const &v) {
            ListBucketsResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.buckets = boost::json::value_to<std::vector<Bucket> >(v.at("buckets"));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListBucketsResponse const &obj) {
            jv = {
                    {"buckets", boost::json::value_from(obj.buckets)},
                    {"total", obj.total},
            };
        }
    };

}// namespace Euclid::Dto::Storage
