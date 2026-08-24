//
// Created by vogje01 on 29/05/2023.
//

#pragma once

// C++ standard includes
#include <string>


// Euclid includes
#include <euclid/core/LogStream.h>
#include <euclid/database/Database.h>
#include <euclid/database/entity/emm/Module.h>
#include <euclid/database/repository/emm/IEmmRepository.h>

namespace Euclid::Database {

    /**
     * @brief Module MongoDB database.
     *
     * Controls all the AwsMock modules.
     *
     * @author jens.vogt\@opitz-consulting.com
     */
    class MongoEmmRepository final : public IEmmRepository {

    public:

        /**
         * @brief Singleton instance
         */
        static MongoEmmRepository &instance() {
            static MongoEmmRepository moduleDatabase;
            return moduleDatabase;
        }

        /**
         * @brief Upserts one instance into a module's live instance pool. See IEmmRepository.
         */
        void upsertInstance(const Entity::Module &module, const Entity::ModuleInstance &instance) override;

        /**
         * @brief Permanently removes one instance from a module's live instance pool. See IEmmRepository.
         */
        void removeInstance(const std::string &moduleName, const std::string &instanceId) override;

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

        static constexpr auto COLLECTION = "emm_module";
    };

}// namespace Euclid::Database