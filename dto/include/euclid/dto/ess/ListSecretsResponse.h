//
// Created by vogje01 on 9/8/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/ess/model/Secret.h>

namespace Euclid::Dto::ESS {

    /**
     * @brief The secrets an account holds, without any of their values.
     */
    struct ListSecretsResponse : BaseDto {

        /**
         * @brief Secrets list
         */
        std::vector<Secret> secrets;

        /**
         * @brief Total number of secrets matching the request, ignoring paging
         */
        long total{};

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend ListSecretsResponse tag_invoke(boost::json::value_to_tag<ListSecretsResponse>, boost::json::value const &v) {
            ListSecretsResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.secrets = boost::json::value_to<std::vector<Secret> >(v.at("secrets"));
            r.total = Core::GetLongValue(v, "total");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, ListSecretsResponse const &obj) {
            jv = {
                    {"secrets", boost::json::value_from(obj.secrets)},
                    {"total", obj.total},
            };
        }
    };

}// namespace Euclid::Dto::ESS
