//
// Created by vogje01 on 29/05/2023.
//

#pragma once

// C++ standard includes
#include <string>


// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/entity/module/Module.h>
#include <euclid/database/repository/module/IModuleRepository.h>

namespace Euclid::Database {

    /**
     * @brief Module MongoDB database.
     *
     * Controls all the AwsMock modules.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoModuleRepository final : public IModuleRepository {

    public:

        /**
         * @brief Constructor
         */
        explicit MongoModuleRepository() : _databaseName("euclid"), _moduleCollectionName("module") {}

        /**
         * @brief Singleton instance
         */
        static MongoModuleRepository &instance() {
            static MongoModuleRepository moduleDatabase;
            return moduleDatabase;
        }

        /**
         * @brief Update or insert modules
         *
         * @param module module entity
         */
        void upsert(const Entity::Module &module) override;

        /**
         * @brief Removes a module entity
         *
         * @param name module name
         */
        void remove(const std::string &name) override;

        /**
         * @brief Find by module name
         *
         * @param name module name
         * @return optional module
         */
        [[nodiscard]]
        std::optional<Entity::Module> findByName(const std::string &name) const override;

        /**
         * @brief Find by module ID
         *
         * @param oid module OID
         * @return optional module
         */
        [[nodiscard]]
        std::optional<Entity::Module> findById(const std::string &oid) const override;

        /**
         * @brief Find all modules
         *
         * @return list of all module entities.
         */
        [[nodiscard]]
        std::vector<Entity::Module> findAll() const override;

        /**
         * @brief Check the existence of the module by name
         *
         * @param name module name check existence
         * @return true if module exists
         */
        [[nodiscard]]
        bool exists(const std::string &name) const override;

        /**
         * @brief Get the total number of modules
         *
         * @return total number of modules
         */
        [[nodiscard]]
        long count() const override;

        /**
         * @brief Delete all modules
         */
        void clear() override;

    private:

        static constexpr auto COLLECTION = "module";

        /**
         * Database name
         */
        std::string _databaseName;

        /**
         * Module collection name
         */
        std::string _moduleCollectionName;
    };

}// namespace Euclid::Database