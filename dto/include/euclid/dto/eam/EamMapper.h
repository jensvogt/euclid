//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/database/entity/eam/User.h>
#include <euclid/database/entity/eam/UserGroup.h>
#include <euclid/dto/eam/model/User.h>
#include <euclid/dto/eam/model/UserGroup.h>

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

        /**
         * @brief Maps a UserGroup entity to a UserGroup DTO.
         *
         * @param entity source UserGroup entity from the database
         * @return UserGroup DTO
         */
        static UserGroup toDto(const Database::Entity::EAM::UserGroup &entity);

        /**
         * @brief Maps a list of UserGroup entities to a list of UserGroup DTOs.
         *
         * @param entities source UserGroup entities from the database
         * @return list of UserGroup DTOs
         */
        static std::vector<UserGroup> toDto(const std::vector<Database::Entity::EAM::UserGroup> &entities);
    };

}// namespace Euclid::Dto::EAM
