// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#pragma once

// C++ includes
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/ess/Secret.h>

namespace Euclid::Database {

    /**
     * @brief Interface for ESS repository operations.
     *
     * @par
     * Everything a secret's value ever passes through, and deliberately narrow: there is no method
     * that returns every secret with its value, because nothing has a reason to ask for that and
     * an accidental listing is exactly how a secrets store stops being one.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class IEssRepository {

    public:

        virtual ~IEssRepository() = default;

        /**
         * @brief Inserts a new secret or updates the one with the same account, namespace and name.
         *
         * @param secret the secret to store, with its value already encrypted
         * @return the stored secret
         */
        virtual Entity::ESS::Secret upsertSecret(Entity::ESS::Secret &secret) = 0;

        /**
         * @brief Finds a secret by its account, namespace and name.
         *
         * @param accountId account the secret belongs to
         * @param namespaceName namespace the secret belongs to
         * @param name secret name
         * @return the secret if found, std::nullopt otherwise
         */
        [[nodiscard]]
        virtual std::optional<Entity::ESS::Secret> findSecretByName(const std::string &accountId, const std::string &namespaceName, const std::string &name) const = 0;

        /**
         * @brief Finds a secret by its ERN.
         *
         * @param ern ERN of the secret
         * @return the secret if found, std::nullopt otherwise
         */
        [[nodiscard]]
        virtual std::optional<Entity::ESS::Secret> findSecretByErn(const std::string &ern) const = 0;

        /**
         * @brief Whether a secret of that name exists.
         *
         * @param accountId account the secret belongs to
         * @param namespaceName namespace the secret belongs to
         * @param name secret name
         * @return true if it exists
         */
        [[nodiscard]]
        virtual bool secretExists(const std::string &accountId, const std::string &namespaceName, const std::string &name) const = 0;

        /**
         * @brief Lists the secrets of an account.
         *
         * @par
         * The rows come back whole, values included - the module strips them before answering,
         * since a list is metadata and decrypting a hundred values to throw them away would be
         * both slow and a good way to leak one.
         *
         * @param accountId only secrets belonging to this account are returned
         * @param namespaceName only secrets in this namespace are returned; empty means don't filter by namespace
         * @param prefix only secrets whose name starts with this prefix are returned; empty matches all
         * @param pageSize maximum number of secrets to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by (e.g. "name", "ern"); empty means unsorted
         * @param sortDirection "asc" or "desc"; anything else is treated as "desc"
         * @return the secrets found
         */
        [[nodiscard]]
        virtual std::vector<Entity::ESS::Secret> listSecrets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection = "asc") const = 0;

        /**
         * @brief Retrieves the total number of secrets.
         *
         * @param accountId only secrets belonging to this account are counted
         * @param namespaceName only secrets in this namespace are counted; empty means don't filter by namespace
         * @param prefix only secrets whose name starts with this prefix are counted
         * @return the number of secrets
         */
        [[nodiscard]]
        virtual long countSecrets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix = "") const = 0;

        /**
         * @brief Deletes a secret.
         *
         * @par
         * Immediately and for good: unlike an EKM key, a secret is the thing itself rather than
         * what protects other things, so there is nothing left to recover afterwards and no window
         * in which keeping it would help.
         *
         * @param accountId account the secret belongs to
         * @param namespaceName namespace the secret belongs to
         * @param name secret name
         * @return number of secrets deleted
         */
        virtual long deleteSecret(const std::string &accountId, const std::string &namespaceName, const std::string &name) = 0;
    };

}// namespace Euclid::Database
