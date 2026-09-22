// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <map>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/com/Variant.h>

namespace Euclid::Dto::EQS {

    /**
     * @brief One message within a send-message-batch request.
     *
     * @par
     * Deliberately the same fields SendMessageRequest carries, minus the queue - a batch names the
     * queue once. Anything a caller can say about a single message they can say about one in a
     * batch, so moving from one to the other is not a change of what can be expressed.
     */
    struct SendMessageBatchEntry {

        /**
         * @brief Message body
         */
        std::string body{};

        /**
         * @brief Message attributes
         */
        std::map<std::string, COM::Variant> attributes{};

        /**
         * @brief Euclid's own attributes, carried across every hop and never mixed into the
         * caller's - see Entity::EQS::Message::systemAttributes.
         */
        std::map<std::string, COM::Variant> systemAttributes{};

        /**
         * @brief Message priority, i.e. "LOW", "MEDIUM" or "HIGH". Empty takes the queue's own.
         */
        std::string priority{};

    private:

        friend SendMessageBatchEntry tag_invoke(boost::json::value_to_tag<SendMessageBatchEntry>, boost::json::value const &v) {
            SendMessageBatchEntry e;
            e.body = Core::GetStringValue(v, "body");
            e.attributes = Core::GetMapFromObject<std::string, COM::Variant>(v, "attributes");
            e.systemAttributes = Core::GetMapFromObject<std::string, COM::Variant>(v, "systemAttributes");
            // Not defaulted to "MEDIUM" the way SendMessageRequest does: empty means "the queue's
            // own", and a batch that quietly turned every unset entry into MEDIUM would override
            // the queue default that a single send of the same message respects.
            e.priority = Core::GetStringValue(v, "priority");
            return e;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SendMessageBatchEntry const &obj) {
            jv = {
                    {"body", obj.body},
                    {"attributes", boost::json::value_from(obj.attributes)},
                    {"systemAttributes", boost::json::value_from(obj.systemAttributes)},
                    {"priority", obj.priority},
            };
        }
    };

    /**
     * @brief Request to put several messages on one queue in a single call.
     *
     * @par
     * The queue is named once and the messages carry no ern of their own: a batch is one queue's
     * worth by construction, which is what lets the server resolve and authorize the queue a single
     * time and write every message in one insert.
     */
    struct SendMessageBatchRequest {

        /**
         * @brief Queue ERN, or a bare queue name - resolved server-side against the caller's own
         * account and namespace, the same as every other eqs request.
         */
        std::string ern{};

        /**
         * @brief The messages, in the order they are to be sent. Their positions are what a
         * rejection is reported against, so the order is part of the contract rather than an
         * accident of how they were written down.
         */
        std::vector<SendMessageBatchEntry> messages{};

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
        static SendMessageBatchRequest fromJson(const std::string &json) {
            return boost::json::value_to<SendMessageBatchRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SendMessageBatchRequest tag_invoke(boost::json::value_to_tag<SendMessageBatchRequest>, boost::json::value const &v) {
            SendMessageBatchRequest r;
            r.ern = Core::GetStringValue(v, "ern");

            // Parsed here rather than trusted: "messages" being absent, or being something other
            // than an array, is a request whose shape is wrong and the handler answers 400 for it.
            // An element that is not an object is left as a default-constructed entry and refused
            // by the handler against its index, so one malformed entry does not cost the others.
            if (v.is_object()) {
                if (const auto *messages = v.as_object().if_contains("messages"); messages != nullptr && messages->is_array()) {
                    for (const auto &entry: messages->as_array()) {
                        r.messages.push_back(entry.is_object() ? boost::json::value_to<SendMessageBatchEntry>(entry) : SendMessageBatchEntry{});
                    }
                }
            }
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SendMessageBatchRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"messages", boost::json::value_from(obj.messages)},
            };
        }
    };

}// namespace Euclid::Dto::EQS
