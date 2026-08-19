//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/database/entity/access/User.h>
#include <euclid/dto/access/model/User.h>

namespace Euclid::Dto::Access {

    /**
     * @brief Maps between the access module's database entities and DTOs.
     */
    struct AccessMapper {

        /**
         * @brief Maps a User entity to a User DTO.
         *
         * @param entity source User entity from the database
         * @return User DTO
         */
        static User toDto(const Database::Entity::Access::User &entity);

        /**
         * @brief Maps a list of User entities to a list of User DTOs.
         *
         * @param entities source User entities from the database
         * @return list of User DTOs
         */
        static std::vector<User> toDto(const std::vector<Database::Entity::Access::User> &entities);
    };

}// namespace Euclid::Dto::Access
