//
// Created by vogje01 on 9/8/26.
//

#pragma once

// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/entity/ess/Secret.h>
#include <euclid/database/repository/ess/IEssRepository.h>

namespace Euclid::Database {

    using namespace bsoncxx::builder::basic;

    /**
     * @brief ESS MongoDB database.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoEssRepository final : public IEssRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoEssRepository();

        /**
         * @brief Singleton instance
         */
        static MongoEssRepository &instance() {
            static MongoEssRepository secretDatabase;
            return secretDatabase;
        }

        /**
         * @brief Update or insert a secret
         *
         * @param secret secret entity, with its value already encrypted
         * @return the stored secret
         */
        Entity::ESS::Secret upsertSecret(Entity::ESS::Secret &secret) override;

        /**
         * @brief Find a secret by its account, namespace and name
         *
         * @param accountId account the secret belongs to
         * @param namespaceName namespace the secret belongs to
         * @param name secret name
         * @return optional secret
         */
        [[nodiscard]]
        std::optional<Entity::ESS::Secret> findSecretByName(const std::string &accountId, const std::string &namespaceName, const std::string &name) const override;

        /**
         * @brief Find a secret by its ERN
         *
         * @param ern ERN of the secret
         * @return optional secret
         */
        [[nodiscard]]
        std::optional<Entity::ESS::Secret> findSecretByErn(const std::string &ern) const override;

        /**
         * @brief Whether a secret of that name exists
         *
         * @param accountId account the secret belongs to
         * @param namespaceName namespace the secret belongs to
         * @param name secret name
         * @return true if it exists
         */
        [[nodiscard]]
        bool secretExists(const std::string &accountId, const std::string &namespaceName, const std::string &name) const override;

        /**
         * @brief List the secrets of an account
         *
         * @param accountId account the secrets belong to
         * @param namespaceName namespace the secrets belong to; empty means don't filter by namespace
         * @param prefix only secrets whose name starts with this prefix are returned
         * @param pageSize maximum number of secrets to return; 0 or less means no limit
         * @param pageIndex zero-based page index, applied when pageSize is set
         * @param sortColumn field to sort by; empty means unsorted
         * @param sortDirection sort direction ("asc", "desc")
         * @return list of secrets
         */
        [[nodiscard]]
        std::vector<Entity::ESS::Secret> listSecrets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix, long pageSize, long pageIndex, const std::string &sortColumn, const std::string &sortDirection) const override;

        /**
         * @brief Get the total number of secrets
         *
         * @param accountId only secrets belonging to this account are counted
         * @param namespaceName only secrets in this namespace are counted; empty means don't filter by namespace
         * @param prefix only secrets whose name starts with this prefix are counted
         * @return total number of secrets
         */
        [[nodiscard]]
        long countSecrets(const std::string &accountId, const std::string &namespaceName, const std::string &prefix) const override;

        /**
         * @brief Delete a secret
         *
         * @param accountId account the secret belongs to
         * @param namespaceName namespace the secret belongs to
         * @param name secret name
         * @return number of secrets deleted
         */
        long deleteSecret(const std::string &accountId, const std::string &namespaceName, const std::string &name) override;

    private:

        static constexpr auto SECRET_COLLECTION = "ess_secret";

        /**
         * @brief Creates the indexes secret lookup needs, if they do not already exist.
         */
        static void ensureIndexes();
    };

}// namespace Euclid::Database
