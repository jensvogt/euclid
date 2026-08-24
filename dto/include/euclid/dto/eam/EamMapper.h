//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/database/entity/eam/User.h>
#include <euclid/dto/eam/model/User.h>

namespace Euclid::Dto::EAM {

    /**
     * @brief Maps between the access module's database entities and DTOs.
     */
    struct EamMapper {

        /**
         * @brief Maps a User entity to a User DTO.
         *
         * @param entity source User entity from the database
         * @return User DTO
         */
        static User toDto(const Database::Entity::EAM::User &entity);

        /**
         * @brief Maps a list of User entities to a list of User DTOs.
         *
         * @param entities source User entities from the database
         * @return list of User DTOs
         */
        static std::vector<User> toDto(const std::vector<Database::Entity::EAM::User> &entities);
    };

}// namespace Euclid::Dto::EAM
