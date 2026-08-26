//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/ens/model/Topic.h>

namespace Euclid::Dto::ENS {

    struct ListTopicsResponse {

        /**
         * @brief Topic list
         */
        std::vector<Topic> topics;

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

        friend ListTopicsResponse tag_invoke(boost::json::value_to_tag<ListTopicsResponse>, boost::json::value const &v) {
            ListTopicsResponse r;
            r.topics = boost::json::value_to<std::vector<Topic> >(v.at("topics"));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListTopicsResponse const &obj) {
            jv = {
                    {"topics", boost::json::value_from(obj.topics)},
                    {"total", obj.total},
            };
        }
    };

}