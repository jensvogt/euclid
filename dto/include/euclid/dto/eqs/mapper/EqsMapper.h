//
// Created by vogje01 on 8/16/26.
//

#pragma once

// C++ includes
#include <vector>

// Euclid includes
#include <euclid/database/entity/eqs/Message.h>
#include <euclid/database/entity/eqs/Queue.h>
#include <euclid/dto/eqs/model/Message.h>
#include <euclid/dto/eqs/model/Queue.h>

namespace Euclid::Dto::EQS {

    /**
     * @brief Maps between the EQS module's database entities and DTOs.
     */
    struct EqsMapper {

        /**
         * @brief Maps a Queue entity to a Queue DTO.
         *
         * @param entity source Queue entity from the database
         * @return Queue DTO
         */
        static Queue toDto(const Database::Entity::EQS::Queue &entity);

        /**
         * @brief Maps a list of Queue entities to a list of Queue DTOs.
         *
         * @param entities source Queue entities from the database
         * @return list of Queue DTOs
         */
        static std::vector<Queue> toDto(const std::vector<Database::Entity::EQS::Queue> &entities);

        /**
         * @brief Maps a Queue DTO to a Queue entity.
         *
         * @param dto source Queue DTO
         * @return Queue entity ready for persistence
         */
        static Database::Entity::EQS::Queue toEntity(const Queue &dto);

        /**
         * @brief Maps a Message entity to a Message DTO.
         *
         * @param entity source Message entity from the database
         * @return Message DTO
         */
        static Message toDto(const Database::Entity::EQS::Message &entity);

        /**
         * @brief Maps a list of Message entities to a list of Message DTOs.
         *
         * @param entities source Message entities from the database
         * @return list of Message DTOs
         */
        static std::vector<Message> toDto(const std::vector<Database::Entity::EQS::Message> &entities);

        /**
         * @brief Maps a Message DTO to a Message entity.
         *
         * @param dto source Message DTO
         * @return Message entity ready for persistence
         */
        static Database::Entity::EQS::Message toEntity(const Message &dto);

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

}// namespace Euclid::Dto::EQS