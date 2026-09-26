// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EQS {

    struct DeleteQueueRequest {

        /**
         * @brief ERN of the queue
         */
        std::string ern;

        /**
         * @brief Delete only while the queue holds no messages; otherwise refuse.
         *
         * @par
         * Deleting a queue discards what is in it, and whatever was still being delivered into it
         * with it. That is right for an operator who has decided, and wrong for anything
         * automated: a provisioning run that no longer lists a queue must not be able to drop a
         * backlog on the way past. A caller that means "remove it if nothing is using it" says so
         * and gets a 409 naming the depth instead. Off by default.
         */
        bool ifEmpty{};

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]]
        std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]]
        static DeleteQueueRequest fromJson(const std::string &json) {
            return boost::json::value_to<DeleteQueueRequest>(Core::ParseJsonString(json));
        }

    private:

        friend DeleteQueueRequest tag_invoke(boost::json::value_to_tag<DeleteQueueRequest>, boost::json::value const &v) {
            DeleteQueueRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.ifEmpty = Core::GetBoolValue(v, "ifEmpty");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, DeleteQueueRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"ifEmpty", obj.ifEmpty},
            };
        }
    };
}
