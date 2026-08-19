//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/sqs/model/Queue.h>

namespace Euclid::Dto::SQS {

    struct ListQueueResponse : BaseDto {

        /**
         * @brief Queue list
         */
        std::vector<Queue> queues;

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

        friend ListQueueResponse tag_invoke(boost::json::value_to_tag<ListQueueResponse>, boost::json::value const &v) {
            ListQueueResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.queues = boost::json::value_to<std::vector<Queue> >(v.at("queues"));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListQueueResponse const &obj) {
            jv = {
                    {"queues", boost::json::value_from(obj.queues)},
                    {"total", obj.total},
            };
        }
    };

}