//
// Created by vogje01 on 5/24/26.
//

#pragma once

// C++ includes
#include <chrono>

// Euclid includes
#include <euclid/dto/emm/ModuleProcess.h>
#include <euclid/database/entity/emm/Module.h>

namespace Euclid::Dto {

    /**
     * @brief Maps between ModuleProcess (controller) and Module/ModuleInstance (database entities)
     */
    struct EmmMapper {

        /**
         * @brief Maps a ModuleProcess's module-level (static/config) fields to a Module entity.
         * The returned entity's `instances` is left empty - callers pass it alongside the
         * separately-mapped ModuleInstance to IEmmRepository::upsertInstance(), which only reads
         * this entity's module-level fields.
         *
         * @param svc  source service process from controller
         * @return     Module entity ready for persistence
         */
        static Database::Entity::Module toModuleEntity(const ModuleProcess &svc);

        /**
         * @brief Maps a ModuleProcess's per-instance (live/dynamic) fields to a ModuleInstance entity.
         *
         * @param svc  source service process from controller
         * @return     ModuleInstance entity ready for persistence
         */
        static Database::Entity::ModuleInstance toInstanceEntity(const ModuleProcess &svc);
    };

} // namespace Euclid::Database::Entity