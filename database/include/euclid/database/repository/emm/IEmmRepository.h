//
// Created by vogje01 on 5/24/26.
//

#pragma once

// C++ includes
#include <optional>
#include <string>
#include <vector>

// Euclid includes
#include <euclid/database/entity/emm/Module.h>

namespace Euclid::Database {

    /**
     * @brief Interface for managing module repository operations.
     *
     * Provides an abstraction for storing, retrieving, and managing
     * module-related data.
     */
    class IEmmRepository {

    public:
        /**
         * @brief Virtual destructor for the IModuleRepository interface.
         *
         * Ensures derived classes' destructor is invoked correctly
         * during object destruction to release resources.
         */
        virtual ~IEmmRepository() = default;

        /**
         * @brief Inserts a new module or updates an existing one in the repository.
         *
         * If a module with the same identifier already exists, its data will be updated
         * with the provided module information. Otherwise, a new module will be added
         * to the repository.
         *
         * @param module The module to be inserted or updated in the repository.
         */
        virtual void upsert(const Entity::Module &module) = 0;

        /**
         * @brief Removes the specified element or elements from the collection or data structure.
         *
         * @param name The name of the module to be removed.
         */
        virtual void remove(const std::string &name) = 0;

        /**
         * @brief Searches for a module by its name.
         *
         * @param name The name of the module to search for.
         * @return The item matching the given name, or nullptr if no match is found.
         */
        virtual std::optional<Entity::Module> findByName(const std::string &name) const = 0;

        /**
         * @brief Locates a module in the repository by its unique identifier.
         *
         * Searches for a module matching the specified identifier and returns it
         * if found. If no matching module exists, an empty optional is returned.
         *
         * @param oid The unique identifier of the module to search for.
         * @return An optional containing the found module, or an empty optional
         *         if no module with the given identifier exists.
         */
        virtual std::optional<Entity::Module> findById(const std::string &oid) const = 0;

        /**
         * @brief Finds and retrieves all available entities or objects.
         *
         * @return A collection containing all entities or objects found.
         */
        virtual std::vector<Entity::Module> findAll() const = 0;

        /**
         * @brief Checks if a module with the specified name exists in the repository.
         *
         * @param name The name of the module to check for existence.
         * @return True if a module with the given name exists, otherwise false.
         */
        virtual bool exists(const std::string &name) const = 0;

        /**
         * @brief Retrieves the total count of modules in the repository.
         *
         * @return The total number of modules as a long integer.
         */
        virtual long count() const = 0;

        /**
         * @brief Removes all entries from the module repository, leaving it in an empty state.
         *
         * This method is intended to clear all stored data, and the repository will contain no entities after its execution.
         *
         * This is a pure virtual function and must be implemented by derived classes.
         */
        virtual void clear() = 0;
    };

} // namespace Euclid::Database