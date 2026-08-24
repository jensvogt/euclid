//
// Created by vogje01 on 8/23/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/database/entity/esm/Bucket.h>
#include <euclid/database/entity/esm/Object.h>
#include <euclid/dto/esm/model/Bucket.h>
#include <euclid/dto/esm/model/Object.h>

namespace Euclid::Dto::ESM {

    /**
     * @brief Maps between the storage module's database entities and DTOs.
     */
    struct EsmMapper {

        /**
         * @brief Maps a Bucket entity to a Bucket DTO.
         *
         * @param entity source Bucket entity from the database
         * @return Bucket DTO
         */
        static Bucket toDto(const Database::Entity::ESM::Bucket &entity);

        /**
         * @brief Maps a list of Bucket entities to a list of Bucket DTOs.
         *
         * @param entities source Bucket entities from the database
         * @return list of Bucket DTOs
         */
        static std::vector<Bucket> toDto(const std::vector<Database::Entity::ESM::Bucket> &entities);

        /**
         * @brief Maps a Object entity to a Object DTO.
         *
         * @param entity source Object entity from the database
         * @return Object DTO
         */
        static Object toDto(const Database::Entity::ESM::Object &entity);

        /**
         * @brief Maps a list of Object entities to a list of Object DTOs.
         *
         * @param entities source Object entities from the database
         * @return list of Object DTOs
         */
        static std::vector<Object> toDto(const std::vector<Database::Entity::ESM::Object> &entities);
    };

}// namespace Euclid::Dto::ESM