//
// Created by vogje01 on 8/16/26.
//

#pragma once

// Euclid includes
#include <euclid/core/Configuration.h>

namespace Euclid::Core {

    /**
     * @brief Create Euclid Resource Name (ERN)
     *
     * @param service module name
     * @param accountId account ID
     * @param resourceId resource ID
     */
    inline std::string createErn(const std::string &service, const std::string &accountId, const std::string &resourceId) {
        const auto region = Configuration::instance().getOr<std::string>("euclid.region", "eu-central-1");
        return "ern:" + service + ":" + region + ":" + accountId + ":" + resourceId;
    }

    /**
     * @brief Creates an EAM user ERN
     *
     * @param accountId account ID
     * @param name resource name
     * @return resource ERN
     */
    inline std::string createEamUserErn(const std::string &accountId, const std::string &name) {
        return createErn("eam", accountId, "user:" + name);
    }

    /**
     * @brief Creates an EAM user group ERN
     *
     * @param accountId account ID
     * @param name resource name
     * @return resource ERN
     */
    inline std::string createEamUserGroupErn(const std::string &accountId, const std::string &name) {
        return createErn("eam", accountId, "userGroup:" + name);
    }

    /**
     * @brief Creates an EAM account ERN
     *
     * @param accountId account ID
     * @return resource ERN
     */
    inline std::string createEamAccountErn(const std::string &accountId) {
        return createErn("eam", accountId, "account:" + accountId);
    }

    /**
     * @brief Creates an EAM namespace ERN
     *
     * @param accountId account ID
     * @param name namespace name
     * @return resource ERN
     */
    inline std::string createEamNamespaceErn(const std::string &accountId, const std::string &name) {
        return createErn("eam", accountId, "namespace:" + name);
    }

    /**
     * @brief Creates a EQS queue ERN
     *
     * @param accountId account ID
     * @param name resource name
     * @return resource ERN
     */
    inline std::string createEqsQueueErn(const std::string &accountId, const std::string &name) {
        return createErn("eqs", accountId, "queue:" + name);
    }

    /**
     * @brief Creates a EQS message ERN
     *
     * @param accountId account ID
     * @param messageId message ID
     * @return resource ERN
     */
    inline std::string createEqsMessageErn(const std::string &accountId, const std::string &messageId) {
        return createErn("eqs", accountId, "message:" + messageId);
    }

    /**
     * @brief Creates a ENS topic ERN
     *
     * @param accountId account ID
     * @param name resource name
     * @return resource ERN
     */
    inline std::string createEnsTopicErn(const std::string &accountId, const std::string &name) {
        return createErn("ens", accountId, "queue:" + name);
    }

    /**
     * @brief Creates a ENS message ERN
     *
     * @param accountId account ID
     * @param messageId message ID
     * @return resource ERN
     */
    inline std::string createEnsMessageErn(const std::string &accountId, const std::string &messageId) {
        return createErn("ens", accountId, "message:" + messageId);
    }

    /**
     * @brief Creates a ESM bucket ERN
     *
     * @param accountId account ID
     * @param name resource name
     * @return resource ERN
     */
    inline std::string createEsmBucketErn(const std::string &accountId, const std::string &name) {
        return createErn("esm", accountId, "bucket:" + name);
    }

    /**
     * @brief Creates a ESM object ERN
     *
     * @param accountId account ID
     * @param name resource name
     * @return resource ERN
     */
    inline std::string createEsmObjectErn(const std::string &accountId, const std::string &name) {
        return createErn("esm", accountId, "object:" + name);
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