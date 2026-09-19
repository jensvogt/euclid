// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/19/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/eqs/model/Message.h>

namespace Euclid::Dto::EQS {

    /**
     * @brief One message, as list-messages describes each of its own.
     *
     * @par
     * The same Message model rather than a shape of its own, so that what a listing shows and what
     * this shows cannot drift apart - a field added to one is in the other by construction.
     */
    struct GetMessageResponse : BaseDto {

        /**
         * @brief The message.
         */
        Message message;

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetMessageResponse tag_invoke(boost::json::value_to_tag<GetMessageResponse>, boost::json::value const &v) {
            GetMessageResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.message = boost::json::value_to<Message>(v.at("message"));
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetMessageResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"message", boost::json::value_from(obj.message)},
            };
        }
    };
}// namespace Euclid::Dto::EQS
