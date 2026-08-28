//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/database/entity/ekm/Key.h>
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
    };

}// namespace Euclid::Dto::EQS