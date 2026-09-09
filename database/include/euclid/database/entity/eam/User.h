//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <optional>
#include <string>
#include <vector>

// MongoDB includes
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view-fwd.hpp>

namespace Euclid::Database::Entity::EAM {

    /**
     * @brief An  (mirrors AWS IAM)-style access key pair, used to authenticate SigV4-signed service calls.
     */
    struct AccessKey {

        /**
         * @brief Public identifier, e.g. "AKIA...". Sent in the SigV4 credential scope.
         */
        std::string accessKeyId;

        /**
         * @brief Secret used to derive the SigV4 signing key. Stored as generated (not hashed) -
         * unlike a password, the server must be able to recompute an HMAC from it to verify
         * signatures, which a one-way hash would not allow.
         */
        std::string secretAccessKey;

        /**
         * @brief Whether this key can currently be used to sign requests.
         */
        bool active{true};

        /**
         * @brief Creation timestamp, ISO8601.
         */
        std::string created;

        /**
         * @brief modification timestamp, ISO8601.
         */
        std::string modified;
    };

    /**
     * @brief A login session, created each time a user successfully logs in.
     *
     * Used to derive "currently active users" (a session is active while now < expiresAt) -
     * there's no server-side revocation, so a session simply lasts as long as the JWT it was
     * issued alongside.
     */
    struct Session {

        /**
         * @brief Creation timestamp, ISO8601.
         */
        std::string createdAt;

        /**
         * @brief Expiry timestamp, ISO8601. Matches the JWT's own expiry.
         */
        std::string expiresAt;
    };

    /**
     * @brief Explicit grant of access to a specific account and, within it, a specific set of
     * namespaces. A User can hold any number of these, one per account it has been granted
     * access to - see Core::HttpActionServer::GrantLookup for how this is enforced.
     */
    struct AccountGrant {

        /**
         * @brief Account this grant applies to.
         */
        std::string accountId;

        /**
         * @brief Namespaces within accountId this user may access. An empty vector means no
         * namespace has been granted yet (not "all namespaces").
         */
        std::vector<std::string> namespaces;

        /**
         * @brief Whether this user administers accountId itself (may manage its namespaces and
         * other users' grants on it), independent of global administrator status (see
         * Database::IsEamAdmin()).
         */
        bool isAdmin{false};

        /**
         * @brief Timestamp the grant was created, ISO8601.
         */
        std::string granted;
    };

    /**
     * @brief A SAML assertion this user has already been let in with.
     */
    struct SeenAssertion {

        /**
         * @brief The assertion's ID, as the identity provider minted it.
         */
        std::string assertionId;

        /**
         * @brief When the assertion stops being valid, ISO8601 - after which its ID is no longer
         * worth remembering, because it would be refused on its own expiry.
         */
        std::string expiresAt;
    };

    struct User {

        /**
         * ID
         */
        std::string oid;

        /**
         * @brief User ID
         */
        std::string userId;

        /**
         * @brief Euclid resource name, e.g. "ern:euclid:eam:eu-central-1:<accountId>:user:<userId>"
         * (see Core::createEamUserErn()).
         */
        std::string ern;

        /**
         * @brief Hashed user password, as produced by Core::PasswordUtils::Hash().
         */
        std::string password;

        /**
         * @brief User email
         */
        std::string email;

        /**
         * @brief Account the user belongs to
         */
        std::string accountId;

        /**
         * @brief Region the user was registered in
         */
        std::string region;

        /**
         * @brief Euclid-style access keys owned by this user, used for SigV4-signed service calls.
         */
        /**
         * @brief Whether this user may log in with a password.
         *
         * @par
         * False for a technical principal - the identity an application runs as, created by EAP
         * alongside the application itself. Such a principal has no password worth guessing, no
         * interactive session and no way in through eam login; all it has is an access key, which
         * is enough to sign the calls the application makes and nothing else. Existing users
         * default to true, so this changes nothing for anybody who was here before it.
         */
        bool loginEnabled{true};

        /**
         * @brief Which federation this user signs in through - "oidc", "saml" - or empty for a
         * user that signs in with a password.
         *
         * @par
         * Part of what a federated login is matched on, not decoration: an OIDC subject and a SAML
         * NameID come from unrelated namespaces, and one that happens to read like the other must
         * not find this user.
         */
        std::string federatedProvider;

        /**
         * @brief The provider's own identifier for this person - an OIDC "sub" claim or a SAML
         * NameID - or empty for a password user.
         *
         * @par
         * A provider's subject is the only thing about a person that is guaranteed not to change:
         * names and email addresses get edited, and matching a federated login on either of those
         * would either orphan the account or, worse, hand it to whoever inherits the old address.
         * This is what a federated login is matched on; the user ID remains what everything else
         * in euclid refers to.
         */
        std::string federatedSubject;

        /**
         * @brief ERNs of the resources this user may act on, or empty for no restriction.
         *
         * @par
         * Account and namespace grants say *where* a caller may work; this says *what* it may
         * touch inside that account - the buckets and queues an application was deployed with,
         * and nothing else. Empty means unrestricted, which is what every human and every user
         * written before this field existed is: narrowing a person down to a fixed resource list
         * is not what this is for.
         */
        std::vector<std::string> resourceGrants;

        std::vector<AccessKey> accessKeys;

        /**
         * @brief SAML assertions recently accepted for this user, so that none of them is accepted
         * twice.
         *
         * @par
         * Replay protection has to be shared, not per process: eam runs as a pool, and an
         * assertion refused by the instance that has already seen it would simply be presented to
         * the next one. This is the state every instance already reads on every login, so it is
         * where the record goes. Entries are dropped as they expire, the same way sessions are, so
         * this stays a handful of items - an assertion is valid for minutes.
         */
        std::vector<SeenAssertion> seenAssertions;

        /**
         * @brief Login sessions, one appended per successful login and the expired ones dropped
         * as the next login adds its own.
         *
         * They are pruned rather than kept because this document is read on every authenticated
         * request - the access-key lookup and the grant lookup both fetch it - so anything that
         * grows here is paid for by every request in the installation, not by the logins that
         * caused it.
         */
        std::vector<Session> sessions;

        /**
         * @brief Explicit per-(account, namespace) grants held by this user, in addition to
         * accountId/region above (the user's home account). Empty means no additional accounts
         * have been granted - a global administrator (see Database::IsEamAdmin()) bypasses this
         * list entirely.
         */
        std::vector<AccountGrant> accountGrants;

        /**
         * @brief Creation timestamp, ISO8601.
         */
        std::chrono::system_clock::time_point created;

        /**
         * @brief Creation timestamp, ISO8601.
         */
        std::chrono::system_clock::time_point modified;

        /**
         * @brief Converts the entity to a MongoDB document
         *
         * @return entity as a MongoDB document.
         */
        [[nodiscard]]
        bsoncxx::document::value toDocument() const;

        /**
         * @brief Converts the MongoDB document to an entity
         *
         * @param document MongoDB document.
         */
        static User fromDocument(const std::optional<bsoncxx::document::view> &document);
    };

}