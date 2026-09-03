//
// Created by vogje01 on 9/4/26.
//

#pragma once

// Euclid includes
#include <euclid/core/JsonUtils.h>

namespace Euclid::Dto::EQS {

    /**
     * @brief Sets a queue's default visibility timeout.
     *
     * @par
     * The queue-level counterpart to SetMessageVisibilityRequest: that one changes how long a
     * single message stays invisible, this one changes the figure every message received from the
     * queue starts with. Messages already in flight keep the window they were given - their
     * timeout was fixed when they were received, and the new default applies from the next
     * receive onwards.
     */
    struct SetQueueVisibilityRequest {

        /**
         * @brief Queue ERN
         */
        std::string ern{};

        /**
         * @brief Visibility in seconds
         */
        long visibility{};

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
        static SetQueueVisibilityRequest fromJson(const std::string &json) {
            return boost::json::value_to<SetQueueVisibilityRequest>(Core::ParseJsonString(json));
        }

    private:

        friend SetQueueVisibilityRequest tag_invoke(boost::json::value_to_tag<SetQueueVisibilityRequest>, boost::json::value const &v) {
            SetQueueVisibilityRequest r;
            r.ern = Core::GetStringValue(v, "ern");
            r.visibility = Core::GetLongValue(v, "visibility");
            return r;
        }

        friend void tag_invoke(boost::json::value_from_tag, boost::json::value &jv, SetQueueVisibilityRequest const &obj) {
            jv = {
                    {"ern", obj.ern},
                    {"visibility", obj.visibility},
            };
        }
    };

}
