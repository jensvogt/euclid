// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/database/entity/eqs/MessagePriority.h>

namespace Euclid::Database::Entity::EQS {

    /**
     * @brief Whether a whole send-message-batch request should be refused before any of it is looked at.
     *
     * @par
     * The two things wrong with a batch rather than with a message in it. Both fail the request
     * outright, which is the house rule for a request whose *shape* is wrong - as distinct from a
     * message inside it that cannot be sent, which is reported against its index and lets the rest
     * through.
     *
     * @par
     * An empty batch is refused rather than answered with "sent nothing". A caller that sent no
     * messages meant to send some, and a 200 saying zero were sent reads as success to every piece
     * of code that checks a status and moves on.
     *
     * @param count how many messages the request carries
     * @param maxBatchSize the most this installation accepts in one request
     * @return the reason to refuse, phrased for whoever is sending, or empty to go ahead
     */
    inline std::string BatchRefusal(const long count, const long maxBatchSize) {

        if (count <= 0) {
            return "messages is empty - a batch has to carry at least one message";
        }
        if (count > maxBatchSize) {
            return "batch carries " + std::to_string(count) + " messages, and this installation accepts " +
                   std::to_string(maxBatchSize) + " at a time - see euclid.modules.eqs.max-batch-size";
        }
        return {};
    }

    /**
     * @brief Whether one message in a batch should be refused, leaving the rest to be sent.
     *
     * @par
     * The same two checks send-message makes, in one place so a batch cannot drift from the
     * single send and start accepting what it refuses - or the reverse, which is worse, because
     * the caller would have no way to send the message at all.
     *
     * @par
     * The size is the body alone, which is what Message::size counts and what get-queue-metadata
     * reports, so the limit an operator sets is measured in the units they compare it against.
     * Attributes travel alongside and are not counted.
     *
     * @param bodySize length of the message body in bytes
     * @param maxMessageLength the queue's effective limit - see EffectiveMaxMessageLength()
     * @param priority the priority asked for, empty to take the queue's own
     * @return the reason to refuse this message, or empty to send it
     */
    inline std::string BatchEntryRefusal(const long bodySize, const long maxMessageLength, const std::string &priority) {

        if (bodySize > maxMessageLength) {
            return "message is " + std::to_string(bodySize) + " bytes, and this queue accepts " +
                   std::to_string(maxMessageLength);
        }
        if (!priority.empty() && !TryMessagePriorityFromString(priority).has_value()) {
            return R"(priority must be "LOW", "MEDIUM" or "HIGH", not ")" + priority + R"(")";
        }
        return {};
    }

}// namespace Euclid::Database::Entity::EQS
