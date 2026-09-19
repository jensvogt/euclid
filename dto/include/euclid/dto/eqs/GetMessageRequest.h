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
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::EQS {

    /**
     * @brief Asks for one message, by the id it was given when it was sent.
     *
     * @par
     * The message id, not the receipt handle: a receipt handle belongs to one delivery and is void
     * once that delivery's claim has expired, while the id names the message for as long as it
     * exists. Asking about a message is something one does after the fact, which is exactly when a
     * receipt handle is no longer any use.
     */
    struct GetMessageRequest {

        /**
         * @brief Message id.
         */
        std::string messageId{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

    private:

        friend GetMessageRequest tag_invoke(boost::json::value_to_tag<GetMessageRequest>, boost::json::value const &v) {
            GetMessageRequest r;
            r.messageId = Core::GetStringValue(v, "messageId");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, GetMessageRequest const &obj) {
            jv = {
                    {"messageId", obj.messageId},
            };
        }
    };
}// namespace Euclid::Dto::EQS
