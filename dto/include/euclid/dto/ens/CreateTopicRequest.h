// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::ENS {

    struct CreateTopicRequest {

        /**
         * @brief Name of the queue
         */
        std::string name;

        /**
         * @brief Maximal length of a message in bytes.
         *
         * @par
         * One mebibyte when the caller omits the field, rather than the zero a plain read would
         * give: a client that says nothing about the size of its messages means "the usual", not
         * "a topic that accepts nothing" - and since a publish is measured against this, reading
         * an absent field as zero would refuse every message sent to such a topic.
         */
        long maxMessageLength = 1024 * 1024;

        /**
         * @brief Serializes this request to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this request from a JSON string
         */
        [[nodiscard]] static CreateTopicRequest fromJson(const std::string &json) {
            return boost::json::value_to<CreateTopicRequest>(Core::ParseJsonString(json));
        }

    private:

        friend CreateTopicRequest tag_invoke(boost::json::value_to_tag<CreateTopicRequest>, boost::json::value const &v) {
            CreateTopicRequest r;
            r.name = Core::GetStringValue(v, "name");
            r.maxMessageLength = Core::GetLongValue(v, "maxMessageLength", 1024 * 1024);
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, CreateTopicRequest const &obj) {
            jv = {
                    {"name", obj.name},
                    {"maxMessageLength", obj.maxMessageLength},
            };
        }
    };
}