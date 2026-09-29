// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EQS {

    /**
     * @brief Replaces the body of a message already on a queue.
     *
     * @par
     * The whole body, not a patch. A message body is opaque to EQS - it has no notion of the
     * structure a caller put in it - so there is nothing here that could merge two of them, and a
     * partial update would have to be expressed in a format EQS would then have to understand.
     *
     * @author jensvogt47\@gmail.com
     */
    struct UpdateMessageBodyRequest {

        /**
         * @brief Message ID of the message to rewrite.
         */
        std::string messageId{};

        /**
         * @brief The new body.
         *
         * @par
         * An empty body is accepted, because send-message accepts one: a message whose meaning is
         * entirely in its attributes is a thing callers send, and refusing to update one to the
         * shape it could have been sent as would be a rule that applies only the second time.
         */
        std::string body{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]]
        static UpdateMessageBodyRequest fromJson(const std::string &json) {
            return boost::json::value_to<UpdateMessageBodyRequest>(Core::ParseJsonString(json));
        }

    private:

        friend UpdateMessageBodyRequest tag_invoke(boost::json::value_to_tag<UpdateMessageBodyRequest>, boost::json::value const &v) {
            UpdateMessageBodyRequest r;
            r.messageId = Core::GetStringValue(v, "messageId");
            r.body = Core::GetStringValue(v, "body");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, UpdateMessageBodyRequest const &obj) {
            jv = {
                    {"messageId", obj.messageId},
                    {"body", obj.body},
            };
        }
    };

}// namespace Euclid::Dto::EQS
