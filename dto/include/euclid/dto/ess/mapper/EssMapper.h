//
// Created by vogje01 on 9/8/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/database/entity/ess/Secret.h>
#include <euclid/dto/ess/model/Secret.h>

namespace Euclid::Dto::ESS {

    /**
     * @brief Maps between the ESS module's database entities and DTOs.
     */
    struct EssMapper {

        /**
         * @brief Maps a secret entity to a secret DTO.
         *
         * @par
         * The value is deliberately not carried across: the DTO has no field for it, so no handler
         * can answer with one by forgetting to clear it. get-secret puts the decrypted value in
         * its own response field instead, which is the one place that happens.
         *
         * @param entity source secret entity from the database
         * @return secret DTO
         */
        static Secret toDto(const Database::Entity::ESS::Secret &entity);

        /**
         * @brief Maps a list of secret entities to a list of secret DTOs.
         *
         * @param entities source secret entities from the database
         * @return list of secret DTOs
         */
        static std::vector<Secret> toDto(const std::vector<Database::Entity::ESS::Secret> &entities);
    };

}// namespace Euclid::Dto::ESS
