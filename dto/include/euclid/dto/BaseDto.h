//
// Created by vogje01 on 8/17/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto {

    /**
     * @brief Caller/account identity common to every response DTO, resolved server-side from
     * the authenticated request and serialized as a nested "metadata" object so each DTO's own
     * fields stay at the top level of the JSON.
     *
     * Not used on request DTOs: the server resolves the caller's identity from the bearer
     * token rather than trusting client-supplied values. The request ID that correlates a
     * request with its response travels as the "x-euclid-request-id" response header instead
     * of a body field, since - unlike region/account/user - it isn't part of the caller's
     * identity and is generated fresh per response regardless of what the client sent.
     */
    struct BaseDto {

        /**
         * @brief Region the request/response applies to
         */
        std::string region;

        /**
         * @brief Account ID
         */
        std::string accountId;

        /**
         * @brief ID of the user the request is made on behalf of
         */
        std::string user;

        friend BaseDto tag_invoke(boost::json::value_to_tag<BaseDto>, boost::json::value const &v) {
            BaseDto r;
            r.region = Core::GetStringValue(v, "region");
            r.user = Core::GetStringValue(v, "user");
            r.accountId = Core::GetStringValue(v, "accountId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, BaseDto const &obj) {
            jv = {
                    {"region", obj.region},
                    {"user", obj.user},
                    {"accountId", obj.accountId},
            };
        }
    };

    /**
     * @brief Reads the "metadata" sub-object of a DTO's JSON representation, if present.
     *
     * @param v JSON value of the enclosing DTO
     * @return the parsed metadata, or a default-constructed BaseDto if no "metadata" key is present
     */
    inline BaseDto GetMetadata(const boost::json::value &v) {
        if (Core::AttributeExists(v, "metadata")) {
            return boost::json::value_to<BaseDto>(v.at("metadata"));
        }
        return {};
    }

}// namespace Euclid::Dto