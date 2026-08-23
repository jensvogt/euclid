//
// Created by vogje01 on 8/23/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/storage/model/Object.h>

namespace Euclid::Dto::Storage {

    struct ListObjectsResponse : BaseDto {

        /**
         * @brief Bucket list
         */
        std::vector<Object> objects;

        /**
         * @brief total number of object
         */
        long total{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListObjectsResponse tag_invoke(boost::json::value_to_tag<ListObjectsResponse>, boost::json::value const &v) {
            ListObjectsResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.objects = boost::json::value_to<std::vector<Object> >(v.at("objects"));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListObjectsResponse const &obj) {
            jv = {
                    {"objects", boost::json::value_from(obj.objects)},
                    {"total", obj.total},
            };
        }
    };

}// namespace Euclid::Dto::Storage