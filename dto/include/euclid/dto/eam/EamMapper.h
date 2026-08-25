//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/database/entity/eam/Account.h>
#include <euclid/database/entity/eam/Namespace.h>
#include <euclid/database/entity/eam/User.h>
#include <euclid/database/entity/eam/UserGroup.h>
#include <euclid/dto/eam/model/Account.h>
#include <euclid/dto/eam/model/Namespace.h>
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

        /**
         * @brief Maps an Account entity to an Account DTO.
         *
         * @param entity source Account entity from the database
         * @return Account DTO
         */
        static Account toDto(const Database::Entity::EAM::Account &entity);

        /**
         * @brief Maps a list of Account entities to a list of Account DTOs.
         *
         * @param entities source Account entities from the database
         * @return list of Account DTOs
         */
        static std::vector<Account> toDto(const std::vector<Database::Entity::EAM::Account> &entities);

        /**
         * @brief Maps a Namespace entity to a Namespace DTO.
         *
         * @param entity source Namespace entity from the database
         * @return Namespace DTO
         */
        static Namespace toDto(const Database::Entity::EAM::Namespace &entity);

        /**
         * @brief Maps a list of Namespace entities to a list of Namespace DTOs.
         *
         * @param entities source Namespace entities from the database
         * @return list of Namespace DTOs
         */
        static std::vector<Namespace> toDto(const std::vector<Database::Entity::EAM::Namespace> &entities);
    };

}// namespace Euclid::Dto::EAM
