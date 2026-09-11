// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 01/06/2023.
//

#pragma once

// C++ includes
#include <algorithm>
#include <map>
#include <string>

namespace Euclid::Database::Entity::EQS {

    /**
     * @brief SQS message attribute entity
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    enum class MessageStatus {
        AVAILABLE,
        DELAYED,
        INVISIBLE,
        UNKNOWN
    };

    static std::map<MessageStatus, std::string> MessageStatusNames{
            {MessageStatus::AVAILABLE, "AVAILABLE"},
            {MessageStatus::DELAYED, "DELAYED"},
            {MessageStatus::INVISIBLE, "INVISIBLE"},
            {MessageStatus::UNKNOWN, "UNKNOWN"},
    };

    [[maybe_unused]]
    static std::string MessageStatusToString(const MessageStatus &messageStatus) {
        return MessageStatusNames[messageStatus];
    }

    [[maybe_unused]]
    static MessageStatus MessageStatusFromString(const std::string &messageStatus) {
        const auto it = std::ranges::find_if(MessageStatusNames, [&messageStatus](const auto &pair) { return pair.second == messageStatus; });
        return it != MessageStatusNames.end() ? it->first : MessageStatus::UNKNOWN;
    }

}// namespace Euclid::Database::Entity::SQS