//
// Created by vogje01 on 8/26/26.
//

#include <euclid/dto/ens/EnsMapper.h>

namespace Euclid::Dto::ENS {

    Database::Entity::COM::Variant EnsMapper::toEntity(const COM::Variant &dto) {
        Database::Entity::COM::Variant entityVariant;
        std::visit([&entityVariant]<typename T>(const T &val) {
            if constexpr (std::is_same_v<T, COM::Binary>) {
                entityVariant.value = Database::Entity::COM::Binary(val.begin(), val.end());
            } else {
                entityVariant.value = val;
            }
        }, dto.value);
        return entityVariant;
    }

    COM::Variant EnsMapper::toDto(const Database::Entity::COM::Variant &entity) {
        COM::Variant variant;
        std::visit([&variant]<typename T>(const T &val) {
            if constexpr (std::is_same_v<T, Database::Entity::COM::Binary>) {
                variant.value = Database::Entity::COM::Binary(val.begin(), val.end());
            } else {
                variant.value = val;
            }
        }, entity.value);
        return variant;
    }

    Topic EnsMapper::toDto(const Database::Entity::ENS::Topic &entity) {
        Topic dto;
        dto.region = entity.region;
        dto.name = entity.name;
        dto.owner = entity.owner;
        dto.ern = entity.ern;
        dto.tags = entity.tags;
        dto.size = entity.size;
        dto.messages = entity.available;
        dto.maxMessageLength = entity.maxMessageLength;
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<Topic> EnsMapper::toDto(const std::vector<Database::Entity::ENS::Topic> &entities) {
        std::vector<Topic> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

    Database::Entity::ENS::Topic EnsMapper::toEntity(const Topic &dto) {
        Database::Entity::ENS::Topic entity;
        entity.region = dto.region;
        entity.name = dto.name;
        entity.owner = dto.owner;
        entity.ern = dto.ern;
        entity.size = dto.size;
        entity.available = dto.messages;
        entity.maxMessageLength = dto.maxMessageLength;
        entity.tags = dto.tags;
        entity.created = dto.created;
        entity.modified = dto.modified;
        return entity;
    }

    Message EnsMapper::toDto(const Database::Entity::ENS::Message &entity) {
        Message dto;
        dto.ern = entity.ern;
        dto.topicErn = entity.topicErn;
        dto.messageId = entity.messageId;
        dto.body = entity.body;
        dto.md5Body = entity.md5Body;
        for (const auto &[key, attr]: entity.attributes) {
            dto.attributes[key] = toDto(attr);
        }
        dto.md5Attributes = entity.md5Attributes;
        dto.lastReceived = entity.lastReceived;
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<Message> EnsMapper::toDto(const std::vector<Database::Entity::ENS::Message> &entities) {
        std::vector<Message> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

    Database::Entity::ENS::Message EnsMapper::toEntity(const Message &dto) {
        Database::Entity::ENS::Message entity;
        entity.ern = dto.ern;
        entity.topicErn = dto.topicErn;
        entity.messageId = dto.messageId;
        entity.body = dto.body;
        entity.md5Body = dto.md5Body;
        entity.md5Attributes = dto.md5Attributes;
        for (const auto &[key, variant]: dto.attributes) {
            entity.attributes[key] = toEntity(variant);
        }
        entity.lastReceived = dto.lastReceived;
        entity.created = dto.created;
        entity.modified = dto.modified;
        return entity;
    }

}// namespace Euclid::Dto::ENS