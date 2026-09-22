// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <string>
#include <vector>

// Euclid includes
#include <euclid/core/JsonUtils.h>
#include <euclid/dto/BaseDto.h>

namespace Euclid::Dto::EQS {

    /**
     * @brief One message a batch would not send, and why.
     */
    struct SendMessageBatchFailure {

        /**
         * @brief Where the message sat in the request's own list.
         *
         * @par
         * The request index rather than anything the server minted, because the server minted
         * nothing for this one - a caller matches the rejection back to the message it sent by
         * position, which is the only thing the two sides share.
         */
        long index{};

        /**
         * @brief Why it was refused, in the same words a single send would have used.
         */
        std::string reason{};

    private:

        friend SendMessageBatchFailure tag_invoke(boost::json::value_to_tag<SendMessageBatchFailure>, boost::json::value const &v) {
            SendMessageBatchFailure f;
            f.index = Core::GetLongValue(v, "index");
            f.reason = Core::GetStringValue(v, "reason");
            return f;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SendMessageBatchFailure const &obj) {
            jv = {
                    {"index", obj.index},
                    {"reason", obj.reason},
            };
        }
    };

    /**
     * @brief What a send-message-batch did.
     *
     * @par
     * Counts, the ids of what was sent, and the failures named individually. The counts follow the
     * shape every other multi-item action in euclid uses (esm delete-objects answers "asked" and
     * "objects"); the failure list does not, and is here because the two cases differ. A delete
     * that skipped a key removed something that was already gone. A send that skipped a message
     * dropped it, and a producer holding "97 of 100" cannot act on that without knowing which
     * three to send again.
     */
    struct SendMessageBatchResponse : BaseDto {

        /**
         * @brief Queue the messages were sent to, resolved to a full ERN.
         */
        std::string ern{};

        /**
         * @brief How many messages the request carried.
         */
        long asked{};

        /**
         * @brief How many were stored. Always asked - failed.size().
         */
        long sent{};

        /**
         * @brief The ids of the messages that were sent, in request order.
         */
        std::vector<std::string> messageIds{};

        /**
         * @brief The messages that were not sent, each against its position in the request.
         */
        std::vector<SendMessageBatchFailure> failed{};

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }

        /**
         * @brief Deserializes this response from a JSON string
         */
        [[nodiscard]]
        static SendMessageBatchResponse fromJson(const std::string &json) {
            return boost::json::value_to<SendMessageBatchResponse>(Core::ParseJsonString(json));
        }

    private:

        friend SendMessageBatchResponse tag_invoke(boost::json::value_to_tag<SendMessageBatchResponse>, boost::json::value const &v) {
            SendMessageBatchResponse r;
            static_cast<BaseDto &>(r) = GetMetadata(v);
            r.ern = Core::GetStringValue(v, "ern");
            r.asked = Core::GetLongValue(v, "asked");
            r.sent = Core::GetLongValue(v, "sent");
            if (v.is_object()) {
                if (const auto *ids = v.as_object().if_contains("messageIds"); ids != nullptr && ids->is_array()) {
                    for (const auto &id: ids->as_array()) {
                        if (id.is_string()) r.messageIds.emplace_back(id.as_string());
                    }
                }
                if (const auto *failed = v.as_object().if_contains("failed"); failed != nullptr && failed->is_array()) {
                    for (const auto &failure: failed->as_array()) {
                        if (failure.is_object()) r.failed.push_back(boost::json::value_to<SendMessageBatchFailure>(failure));
                    }
                }
            }
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SendMessageBatchResponse const &obj) {
            jv = {
                    {"metadata", boost::json::value_from(static_cast<const BaseDto &>(obj))},
                    {"ern", obj.ern},
                    {"asked", obj.asked},
                    {"sent", obj.sent},
                    {"messageIds", boost::json::value_from(obj.messageIds)},
                    {"failed", boost::json::value_from(obj.failed)},
            };
        }
    };

}// namespace Euclid::Dto::EQS
