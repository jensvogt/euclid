//
// Created by vogje01 on 5/24/26.
//

#pragma once

// C++ includes
#include <chrono>

// Euclid includes
#include <euclid/dto/module/ModuleProcess.h>
#include <euclid/database/entity/module/Module.h>

namespace Euclid::Dto {

    /**
     * @brief Maps between ServiceProcess (controller) and Module (database entity)
     */
    struct ModuleMapper {

        /**
         * @brief Maps a ServiceProcess to a Module entity
         *
         * @param svc  source service process from controller
         * @return     Module entity ready for persistence
         */
        static Database::Entity::Module toEntity(const ModuleProcess &svc);

        /**
         * @brief Maps a Module entity back to a ModuleProcess
         *
         * @param m   source Module entity from the database
         * @return    ServiceConfig for registering with controller
         */
        static ModuleProcess toDto(const Database::Entity::Module &m);
    };

} // namespace Euclid::Database::Entity