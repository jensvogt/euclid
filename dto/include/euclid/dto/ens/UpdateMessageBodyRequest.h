// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    /**
     * @brief Replaces the body of a message already published to a topic.
     *
     * @par
     * The whole body, not a patch - a message body is opaque to ENS, so there is nothing here that
     * could merge two of them.
     *
     * @par
     * What this does and does not reach is worth being plain about: a topic fans a message out to
     * its subscribers when it is published, so the copies that already left are gone and cannot be
     * rewritten. This changes the message ENS still holds - what list-messages and get-message
     * answer with, and what resend-messages would send.
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
         * An empty body is accepted, because publish-message accepts one.
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

}// namespace Euclid::Dto::ENS
