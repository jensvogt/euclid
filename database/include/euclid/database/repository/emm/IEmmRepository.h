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
         * @brief Upserts one instance into a module's live instance pool.
         *
         * Creates the module document (using `module`'s static fields: executable, socketPath,
         * active, autoRestart, maxRestarts, args) if it doesn't exist yet, then upserts `instance`
         * into its `instances` array, matched by `instance.instanceId` - so restarts of the same
         * pool slot update that slot's existing entry in place rather than appending a new one.
         *
         * @param module module-level fields to upsert; its own `instances` field is ignored.
         * @param instance the single instance to upsert into the module's instances array.
         */
        virtual void upsertInstance(const Entity::Module &module, const Entity::ModuleInstance &instance) = 0;

        /**
         * @brief Permanently removes one instance from a module's live instance pool.
         *
         * Only for instances that are truly gone (autoscaler scale-down, or given up on after
         * exceeding maxRestarts) - not for ordinary state transitions (crashed/stopped instances
         * that may still restart stay in the pool via upsertInstance() instead).
         *
         * @param moduleName name of the module the instance belongs to.
         * @param instanceId stable identifier of the instance to remove.
         */
        virtual void removeInstance(const std::string &moduleName, const std::string &instanceId) = 0;

        /**
         * @brief Removes the specified element or elements from the collection or data structure.
         *
         * @param name The name of the module to be removed.
         */
        /**
         * @brief Records the instance limits somebody asked for, without disturbing anything else.
         *
         * @par
         * Writes only Entity::Module::desiredMinInstances/desiredMaxInstances. It has to be its own
         * call rather than a read-modify-write of the whole document: upsertInstance() rewrites the
         * module's fields from the manager's own configuration on every instance state change, so a
         * limit stored through that path would last until the next crash or scale event. These two
         * fields are the one thing it does not touch.
         *
         * @param name module name
         * @param minInstances the floor asked for, or -1 to leave the current request alone
         * @param maxInstances the ceiling asked for, or -1 to leave the current request alone
         * @return true if a module of that name exists and was updated
         */
        virtual bool setDesiredInstances(const std::string &name, int minInstances, int maxInstances) = 0;

        /**
         * @brief Records the worker thread count somebody asked for.
         *
         * @par
         * Writes only Entity::Module::desiredThreads. Separate from setDesiredInstances() because
         * the two answer different questions - how many processes the pool runs, and how many
         * threads each of those processes serves requests with - and are set independently.
         *
         * @param name module name
         * @param threads number of worker threads the module should start with
         * @return true if a module of that name exists and was updated
         */
        virtual bool setDesiredThreads(const std::string &name, int threads) = 0;

        /**
         * @brief Records that a module should be stopped, or should run again.
         *
         * @par
         * Desired state, not an instruction: the manager acts on it every reconcile tick, so a
         * module stopped here stays stopped through crashes, scale events and manager restarts
         * until it is set back. Nothing else writes this field.
         *
         * @param name module name
         * @param stopped true to stop the module and keep it stopped, false to let it run again
         * @return true if a module of that name exists and was updated
         */
        virtual bool setDesiredStopped(const std::string &name, bool stopped) = 0;

        /**
         * @brief Records that a module should be restarted, stamping Module::restartRequestedAt
         * with the current time.
         *
         * @par
         * The one EMM call that is an event rather than a setting. The manager acts on it once and
         * remembers it has, so asking again restarts the module again - which is the whole point of
         * a timestamp rather than a flag somebody would have to clear.
         *
         * @param name module name
         * @return true if a module of that name exists and was updated
         */
        virtual bool requestRestart(const std::string &name) = 0;

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