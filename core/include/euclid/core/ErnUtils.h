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
     * @brief Create Euclid Resource Name (ERN) for a namespace-scoped resource.
     *
     * Use for resource types whose name is only unique within (accountId, nameSpace) - e.g. EQS
     * queues, ENS topics, ESM buckets/objects - so that same-named resources in different
     * namespaces of the same account don't collide into the same ERN (which the unique index on
     * "ern" alone would then reject as a duplicate, even though the compound
     * (accountId, namespace, name) index says they're legitimately distinct resources). Resource
     * types that aren't namespace-scoped (EAM users/groups/accounts/namespaces themselves, or
     * anything keyed by a randomly generated ID rather than a user-chosen name) should keep using
     * the plain @ref createErn overload above instead.
     *
     * @param service module name
     * @param accountId account ID
     * @param nameSpace namespace within accountId the resource belongs to; empty means unscoped,
     * same as every namespace-scoped entity's own nameSpace field
     * @param resourceId resource ID
     */
    inline std::string createErn(const std::string &service, const std::string &accountId, const std::string &nameSpace, const std::string &resourceId) {
        const auto region = Configuration::instance().getOr<std::string>("euclid.region", "eu-central-1");
        return "ern:" + service + ":" + region + ":" + accountId + ":" + nameSpace + ":" + resourceId;
    }

    /**
     * @brief Resolves a client-supplied resource reference to a full ERN.
     *
     * @par
     * A client names the resource it wants; an ERN additionally carries the region, account and
     * namespace it lives in. Those three are properties of the caller's session rather than of the
     * request, and the server is the only party that knows them authoritatively - a client would
     * have to be told them out of band, and every client language would need its own copy of this
     * to get it right. So a bare name is resolved here, against the caller's own account and
     * namespace.
     *
     * @par
     * That also bounds what a name can reach: a resolved name always lands in the caller's own
     * namespace, so naming a resource in someone else's is not something this can express. A full
     * ERN is still accepted unchanged - cross-namespace work (the CLI, the RUI, administration)
     * needs it, and it keeps every client written before this working - but such a request is
     * subject to the per-resource grant checks exactly as it was.
     *
     * @param service module name, e.g. "ens"
     * @param resourceType resource type within that module, e.g. "topic"
     * @param accountId the caller's account
     * @param nameSpace the caller's namespace, from the request's x-euclid-namespace header
     * @param value either a full ERN (returned unchanged) or a bare resource name
     * @return the resolved ERN, or an empty string if value was empty
     */
    inline std::string resolveErn(const std::string &service, const std::string &resourceType,
                                  const std::string &accountId, const std::string &nameSpace,
                                  const std::string &value) {
        // "ern:" is what tells the two apart. A resource may not be named such that this is
        // ambiguous - see the name validation each create-* action applies.
        if (value.empty() || value.starts_with("ern:")) return value;
        return createErn(service, accountId, nameSpace, resourceType + ":" + value);
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
     * @param nameSpace namespace within accountId the queue belongs to; empty means unscoped
     * @param name resource name
     * @return resource ERN
     */
    inline std::string createEqsQueueErn(const std::string &accountId, const std::string &nameSpace, const std::string &name) {
        return createErn("eqs", accountId, nameSpace, "queue:" + name);
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
     * @param nameSpace namespace within accountId the topic belongs to; empty means unscoped
     * @param name resource name
     * @return resource ERN
     */
    inline std::string createEnsTopicErn(const std::string &accountId, const std::string &nameSpace, const std::string &name) {
        return createErn("ens", accountId, nameSpace, "topic:" + name);
    }

    /**
     * @brief Creates a ENS subscription ERN
     *
     * @param accountId account ID
     * @param subscriptionId subscription ID
     * @return resource ERN
     */
    inline std::string createEnsSubscriptionErn(const std::string &accountId, const std::string &subscriptionId) {
        return createErn("ens", accountId, "subscription:" + subscriptionId);
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
     * @brief Creates an EKM key ERN
     *
     * @param accountId account ID
     * @param keyId key ID
     * @return resource ERN
     */
    inline std::string createEkmKeyErn(const std::string &accountId, const std::string &keyId) {
        return createErn("ekm", accountId, "key:" + keyId);
    }

    /**
     * @brief Creates a ESM bucket ERN
     *
     * @param accountId account ID
     * @param nameSpace namespace within accountId the bucket belongs to; empty means unscoped
     * @param name resource name
     * @return resource ERN
     */
    inline std::string createEsmBucketErn(const std::string &accountId, const std::string &nameSpace, const std::string &name) {
        return createErn("esm", accountId, nameSpace, "bucket:" + name);
    }

    /**
     * @brief Creates a ESM object ERN
     *
     * @param accountId account ID
     * @param nameSpace namespace within accountId the object's bucket belongs to; empty means
     * unscoped
     * @param name resource name
     * @return resource ERN
     */
    inline std::string createEsmObjectErn(const std::string &accountId, const std::string &nameSpace, const std::string &name) {
        return createErn("esm", accountId, nameSpace, "object:" + name);
    }

    /**
     * @brief Creates an ERN for an EAP application.
     *
     * @param accountId account ID
     * @param name application ID
     * @return the application's ERN
     */
    inline std::string createEapApplicationErn(const std::string &accountId, const std::string &name) {
        return createErn("eap", accountId, "application:" + name);
    }

    /**
     * @brief Creates an ERN for an ETS transfer server.
     *
     * @param accountId account ID
     * @param name server ID
     * @return the transfer server's ERN
     */
    inline std::string createEtsServerErn(const std::string &accountId, const std::string &name) {
        return createErn("ets", accountId, "transferServer:" + name);
    }

    /**
     * @brief Extracts the account ID from an ERN, i.e. the fourth colon-separated field of
     * "ern:{service}:{region}:{accountId}:{resourceId}" (createErn()'s format - resourceId
     * itself is "{resourceType}:{resourceName}", already a single field here). accountId sits at
     * the same position in the namespace-scoped createErn() overload's output too
     * ("ern:{service}:{region}:{accountId}:{nameSpace}:{resourceId}"), since nameSpace is
     * inserted after it - this helper works unchanged for both.
     *
     * @param ern ERN to parse
     * @return the account ID, or an empty string if @p ern doesn't have enough fields.
     */
    inline std::string accountIdFromErn(const std::string &ern) {
        std::size_t pos = 0;
        for (int field = 0; field < 3; ++field) {
            pos = ern.find(':', pos);
            if (pos == std::string::npos) return {};
            ++pos;
        }
        const auto end = ern.find(':', pos);
        return end == std::string::npos ? ern.substr(pos) : ern.substr(pos, end - pos);
    }

}