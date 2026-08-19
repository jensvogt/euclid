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

namespace Euclid::Database::Entity::Access {

    /**
     * @brief An AWS-style access key pair, used to authenticate SigV4-signed service calls.
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
        std::string createdAt;
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
         * @brief Whether this user has administrator privileges (e.g. can register new users).
         */
        bool isAdmin{false};

        /**
         * @brief AWS-style access keys owned by this user, used for SigV4-signed service calls.
         */
        std::vector<AccessKey> accessKeys;

        /**
         * @brief Login sessions, one appended per successful login. Not pruned once expired.
         */
        std::vector<Session> sessions;

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