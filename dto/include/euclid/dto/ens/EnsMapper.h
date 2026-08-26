//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/database/entity/ens/Message.h>
#include <euclid/database/entity/ens/Topic.h>
#include <euclid/dto/ens/model/Message.h>
#include <euclid/dto/ens/model/Topic.h>

namespace Euclid::Dto::ENS {

    /**
     * @brief Maps between the ENS module's database entities and DTOs.
     */
    struct EnsMapper {

        /**
         * @brief Maps a Topic entity to a Topic DTO.
         *
         * @param entity source Topic entity from the database
         * @return Topic DTO
         */
        static Topic toDto(const Database::Entity::ENS::Topic &entity);

        /**
         * @brief Maps a list of Topic entities to a list of Topic DTOs.
         *
         * @param entities source Topic entities from the database
         * @return list of Topic DTOs
         */
        static std::vector<Topic> toDto(const std::vector<Database::Entity::ENS::Topic> &entities);

        /**
         * @brief Maps a Topic DTO to a Topic entity.
         *
         * @param dto source Topic DTO
         * @return Topic entity ready for persistence
         */
        static Database::Entity::ENS::Topic toEntity(const Topic &dto);

        /**
         * @brief Maps a Message entity to a Message DTO.
         *
         * @param entity source Message entity from the database
         * @return Message DTO
         */
        static Message toDto(const Database::Entity::ENS::Message &entity);

        /**
         * @brief Maps a list of Message entities to a list of Message DTOs.
         *
         * @param entities source Message entities from the database
         * @return list of Message DTOs
         */
        static std::vector<Message> toDto(const std::vector<Database::Entity::ENS::Message> &entities);

        /**
         * @brief Maps a Message DTO to a Message entity.
         *
         * @param dto source Message DTO
         * @return Message entity ready for persistence
         */
        static Database::Entity::ENS::Message toEntity(const Message &dto);

        /**
         * @brief Maps a message attribute DTO to a message attribute entity.
         *
         * @param dto source Variant DTO
         * @return Variant entity ready for persistence
         */
        static Database::Entity::COM::Variant toEntity(const COM::Variant &dto);

        /**
         * @brief Maps a message attribute entity to a message attribute DTO.
         *
         * @param entity source Variant entity from the database
         * @return Variant DTO
         */
        static COM::Variant toDto(const Database::Entity::COM::Variant &entity);
    };

}// namespace Euclid::Dto::ENS