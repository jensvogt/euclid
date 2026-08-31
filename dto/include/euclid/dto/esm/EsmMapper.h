//
// Created by vogje01 on 8/23/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/database/entity/com/Variant.h>
#include <euclid/database/entity/esm/Bucket.h>
#include <euclid/database/entity/esm/Object.h>
#include <euclid/database/entity/esm/Subscription.h>
#include <euclid/dto/esm/model/Bucket.h>
#include <euclid/dto/esm/model/Object.h>
#include <euclid/dto/esm/model/Subscription.h>

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

        /**
         * @brief Maps a Subscription entity to a Subscription DTO.
         *
         * @param entity source Subscription entity from the database
         * @return Subscription DTO
         */
        static Subscription toDto(const Database::Entity::ESM::Subscription &entity);

        /**
         * @brief Maps a list of Subscription entities to a list of Subscription DTOs.
         *
         * @param entities source Subscription entities from the database
         * @return list of Subscription DTOs
         */
        static std::vector<Subscription> toDto(const std::vector<Database::Entity::ESM::Subscription> &entities);

        /**
         * @brief Maps an object attribute entity to an object attribute DTO.
         *
         * @param entity source Variant entity from the database
         * @return Variant DTO
         */
        static COM::Variant toDto(const Database::Entity::COM::Variant &entity);

        /**
         * @brief Maps an object attribute DTO to an object attribute entity.
         *
         * @param dto source Variant DTO
         * @return Variant entity ready for persistence
         */
        static Database::Entity::COM::Variant toEntity(const COM::Variant &dto);
    };

}// namespace Euclid::Dto::ESM