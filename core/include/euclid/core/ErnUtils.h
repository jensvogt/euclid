//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <string>

// Euclid includes
#include <euclid/core/Configuration.h>

namespace Euclid::Core {

    /**
     * @brief Create Euclid Resource Name (ERN)
     *
     * @param service module name
     * @param accountId account ID
     * @param resourceId resourceId
     */
    inline std::string createErn(const std::string &service, const std::string &accountId, const std::string &resourceId) {
        const auto region = Configuration::instance().getOr<std::string>("euclid.region", "eu-central-1");
        return "ern:euclid:" + service + ":" + region + ":" + accountId + ":" + resourceId;
    }

    /**
     * @brief Creates a SQS ERN
     *
     * @param accountId account ID
     * @param name resource name
     * @return resource ERN
     */
    inline std::string createSqsQueueErn(const std::string &accountId, const std::string &name) {
        return createErn("queues", accountId, "queue:" + name);
    }

    /**
     * @brief Creates a SQS ERN
     *
     * @param accountId account ID
     * @param messageId message ID
     * @return resource ERN
     */
    inline std::string createSqsMessageErn(const std::string &accountId, const std::string &messageId) {
        return createErn("queues", accountId, "message:" + messageId);
    }

    /**
     * @brief Creates an access user ERN
     *
     * @param accountId account ID
     * @param name resource name
     * @return resource ERN
     */
    inline std::string createAccessUserErn(const std::string &accountId, const std::string &name) {
        return createErn("access", accountId, "user:" + name);
    }

    /**
     * @brief Creates an access user ERN
     *
     * @param name resource name
     * @param accountId account ID
     * @return resource ERN
     */
    inline std::string createAccessUserGroupErn(const std::string &accountId, const std::string &name) {
        return createErn("access", accountId, "usergroup:" + name);
    }

    /**
     * @brief Extracts the account ID from an ERN, i.e. the fifth colon-separated field of
     * "ern:euclid:{service}:{region}:{accountId}:{resourceType}:{resourceId}".
     *
     * @param ern ERN to parse
     * @return the account ID, or an empty string if @p ern doesn't have enough fields.
     */
    inline std::string accountIdFromErn(const std::string &ern) {
        std::size_t pos = 0;
        for (int field = 0; field < 4; ++field) {
            pos = ern.find(':', pos);
            if (pos == std::string::npos) return {};
            ++pos;
        }
        const auto end = ern.find(':', pos);
        return end == std::string::npos ? ern.substr(pos) : ern.substr(pos, end - pos);
    }

}