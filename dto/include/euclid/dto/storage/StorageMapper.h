//
// Created by vogje01 on 8/23/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/database/entity/storage/Bucket.h>
#include <euclid/database/entity/storage/Object.h>
#include <euclid/dto/storage/model/Bucket.h>
#include <euclid/dto/storage/model/Object.h>

namespace Euclid::Dto::Storage {

    /**
     * @brief Maps between the storage module's database entities and DTOs.
     */
    struct StorageMapper {

        /**
         * @brief Maps a Bucket entity to a Bucket DTO.
         *
         * @param entity source Bucket entity from the database
         * @return Bucket DTO
         */
        static Bucket toDto(const Database::Entity::Storage::Bucket &entity);

        /**
         * @brief Maps a list of Bucket entities to a list of Bucket DTOs.
         *
         * @param entities source Bucket entities from the database
         * @return list of Bucket DTOs
         */
        static std::vector<Bucket> toDto(const std::vector<Database::Entity::Storage::Bucket> &entities);

        /**
         * @brief Maps a Object entity to a Object DTO.
         *
         * @param entity source Object entity from the database
         * @return Object DTO
         */
        static Object toDto(const Database::Entity::Storage::Object &entity);

        /**
         * @brief Maps a list of Object entities to a list of Object DTOs.
         *
         * @param entities source Object entities from the database
         * @return list of Object DTOs
         */
        static std::vector<Object> toDto(const std::vector<Database::Entity::Storage::Object> &entities);
    };

}// namespace Euclid::Dto::Storage