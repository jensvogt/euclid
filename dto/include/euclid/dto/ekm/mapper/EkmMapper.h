// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/database/entity/ekm/Certificate.h>
#include <euclid/database/entity/ekm/Key.h>
#include <euclid/dto/ekm/model/Certificate.h>
#include <euclid/dto/ekm/model/Key.h>

namespace Euclid::Dto::EKM {

    /**
     * @brief Maps between the EKM module's database entities and DTOs.
     */
    struct EkmMapper {

        /**
         * @brief Maps a key entity to a key DTO.
         *
         * @param entity source key entity from the database
         * @return key DTO
         */
        static Key toDto(const Database::Entity::EKM::Key &entity);

        /**
         * @brief Maps a list of key entities to a list of key DTOs.
         *
         * @param entities source key entities from the database
         * @return list of key DTOs
         */
        static std::vector<Key> toDto(const std::vector<Database::Entity::EKM::Key> &entities);

        /**
         * @brief Maps a key DTO to a key entity.
         *
         * @param dto source key DTO
         * @return key entity ready for persistence
         */
        static Database::Entity::EKM::Key toEntity(const Key &dto);

        /**
         * @brief Maps a certificate entity to a certificate DTO.
         *
         * @par
         * The private key is deliberately not carried across: the DTO has no field for it, so
         * there is no way for a handler to answer with one by forgetting to clear it.
         *
         * @param entity source certificate entity from the database
         * @return certificate DTO
         */
        static Certificate toDto(const Database::Entity::EKM::Certificate &entity);

        /**
         * @brief Maps a list of certificate entities to a list of certificate DTOs.
         *
         * @param entities source certificate entities from the database
         * @return list of certificate DTOs
         */
        static std::vector<Certificate> toDto(const std::vector<Database::Entity::EKM::Certificate> &entities);
    };

}// namespace Euclid::Dto::EQS