#include <../include/euclid/dto/eqs/mapper/EqsMapper.h>

namespace Euclid::Dto::EQS {

    Database::Entity::COM::Variant EqsMapper::toEntity(const COM::Variant &dto) {
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

    COM::Variant EqsMapper::toDto(const Database::Entity::COM::Variant &entity) {
        COM::Variant variant;
        std::visit([&variant]<typename T>(const T &val) {
            if constexpr (std::is_same_v<T, Database::Entity::COM::Binary>) {
                variant.value = COM::Binary(val.begin(), val.end());
            } else {
                variant.value = val;
            }
        }, entity.value);
        return variant;
    }

    Queue EqsMapper::toDto(const Database::Entity::EQS::Queue &entity) {
        Queue dto;
        dto.name = entity.name;
        dto.owner = entity.owner;
        dto.ern = entity.ern;
        dto.delay = entity.delay;
        dto.tags = entity.tags;
        dto.size = entity.size;
        dto.available = entity.available;
        dto.delayed = entity.delayed;
        dto.invisible = entity.invisible;
        dto.visibility = entity.visibility;
        dto.maxMessageLength = entity.maxMessageLength;
        dto.maxReceiveCount = entity.maxReceiveCount;
        dto.deadLetterQueueArn = entity.deadLetterQueueErn;
        dto.priority = Database::Entity::EQS::MessagePriorityToString(entity.priority);
        dto.status = Database::Entity::EQS::QueueStatusToString(entity.status);
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<Queue> EqsMapper::toDto(const std::vector<Database::Entity::EQS::Queue> &entities) {
        std::vector<Queue> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

    Database::Entity::EQS::Queue EqsMapper::toEntity(const Queue &dto) {
        Database::Entity::EQS::Queue entity;
        entity.name = dto.name;
        entity.owner = dto.owner;
        entity.ern = dto.ern;
        entity.size = dto.size;
        entity.delay = dto.delay;
        entity.available = dto.available;
        entity.delayed = dto.delayed;
        entity.invisible = dto.invisible;
        entity.visibility = dto.visibility;
        entity.maxMessageLength = dto.maxMessageLength;
        entity.maxReceiveCount = dto.maxReceiveCount;
        entity.deadLetterQueueErn = dto.deadLetterQueueArn;
        entity.priority = Database::Entity::EQS::MessagePriorityFromString(dto.priority);
        entity.status = Database::Entity::EQS::QueueStatusFromString(dto.status);
        entity.tags = dto.tags;
        entity.created = dto.created;
        entity.modified = dto.modified;
        return entity;
    }

    Message EqsMapper::toDto(const Database::Entity::EQS::Message &entity) {
        Message dto;
        dto.ern = entity.ern;
        dto.queueErn = entity.queueErn;
        dto.messageId = entity.messageId;
        dto.status = Database::Entity::EQS::MessageStatusToString(entity.status);
        dto.priority = Database::Entity::EQS::MessagePriorityToString(entity.priority);
        dto.body = entity.body;
        dto.size = entity.size;
        dto.contentType = entity.contentType;
        dto.receiptHandle = entity.receiptHandle;
        for (const auto &[key, attr]: entity.attributes) {
            dto.attributes[key] = toDto(attr);
        }
        dto.lastReceived = entity.lastReceived;
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<Message> EqsMapper::toDto(const std::vector<Database::Entity::EQS::Message> &entities) {
        std::vector<Message> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

    Database::Entity::EQS::Message EqsMapper::toEntity(const Message &dto) {
        Database::Entity::EQS::Message entity;
        entity.ern = dto.ern;
        entity.queueErn = dto.queueErn;
        entity.messageId = dto.messageId;
        entity.status = Database::Entity::EQS::MessageStatusFromString(dto.status);
        entity.priority = Database::Entity::EQS::MessagePriorityFromString(dto.priority);
        entity.body = dto.body;
        entity.receiptHandle = dto.receiptHandle;
        for (const auto &[key, variant]: dto.attributes) {
            entity.attributes[key] = toEntity(variant);
        }
        entity.lastReceived = dto.lastReceived;
        entity.created = dto.created;
        entity.modified = dto.modified;
        return entity;
    }

}// namespace Euclid::Dto::EQS