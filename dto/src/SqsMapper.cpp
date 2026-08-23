#include <euclid/dto/sqs/SqsMapper.h>

namespace Euclid::Dto::SQS {

    Database::Entity::Queues::Variant SqsMapper::toEntity(const Variant &dto) {
        Database::Entity::Queues::Variant entityVariant;
        std::visit([&entityVariant]<typename T>(const T &val) {
            if constexpr (std::is_same_v<T, Binary>) {
                entityVariant.value = Database::Entity::Queues::Binary(val.begin(), val.end());
            } else {
                entityVariant.value = val;
            }
        }, dto.value);
        return entityVariant;
    }

    Variant SqsMapper::toDto(const Database::Entity::Queues::Variant &entity) {
        Variant variant;
        std::visit([&variant]<typename T>(const T &val) {
            if constexpr (std::is_same_v<T, Database::Entity::Queues::Binary>) {
                variant.value = Binary(val.begin(), val.end());
            } else {
                variant.value = val;
            }
        }, entity.value);
        return variant;
    }

    Queue SqsMapper::toDto(const Database::Entity::Queues::Queue &entity) {
        Queue dto;
        dto.region = entity.region;
        dto.name = entity.name;
        dto.owner = entity.owner;
        dto.ern = entity.ern;
        dto.delay = entity.delay;
        dto.tags = entity.tags;
        dto.size = entity.size;
        dto.messages = entity.available;
        dto.delayed = entity.delayed;
        dto.busy = entity.invisible;
        dto.visibility = entity.visibility;
        dto.maxMessageLength = entity.maxMessageLength;
        dto.maxReceiveCount = entity.maxReceiveCount;
        dto.deadLetterQueueArn = entity.deadLetterQueueErn;
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<Queue> SqsMapper::toDto(const std::vector<Database::Entity::Queues::Queue> &entities) {
        std::vector<Queue> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

    Database::Entity::Queues::Queue SqsMapper::toEntity(const Queue &dto) {
        Database::Entity::Queues::Queue entity;
        entity.region = dto.region;
        entity.name = dto.name;
        entity.owner = dto.owner;
        entity.ern = dto.ern;
        entity.size = dto.size;
        entity.delay = dto.delay;
        entity.available = dto.messages;
        entity.delayed = dto.delayed;
        entity.invisible = dto.busy;
        entity.visibility = dto.visibility;
        entity.maxMessageLength = dto.maxMessageLength;
        entity.maxReceiveCount = dto.maxReceiveCount;
        entity.deadLetterQueueErn = dto.deadLetterQueueArn;
        entity.tags = dto.tags;
        entity.created = dto.created;
        entity.modified = dto.modified;
        return entity;
    }

    Message SqsMapper::toDto(const Database::Entity::Queues::Message &entity) {
        Message dto;
        dto.ern = entity.ern;
        dto.queueErn = entity.queueErn;
        dto.messageId = entity.messageId;
        dto.status = Database::Entity::Queues::MessageStatusToString(entity.status);
        dto.priority = Database::Entity::Queues::MessagePriorityToString(entity.priority);
        dto.body = entity.body;
        dto.md5Body = entity.md5Body;
        dto.receiptHandle = entity.receiptHandle;
        for (const auto &[key, attr]: entity.attributes) {
            dto.attributes[key] = toDto(attr);
        }
        dto.md5Attributes = entity.md5Attributes;
        dto.lastReceived = entity.lastReceived;
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<Message> SqsMapper::toDto(const std::vector<Database::Entity::Queues::Message> &entities) {
        std::vector<Message> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

    Database::Entity::Queues::Message SqsMapper::toEntity(const Message &dto) {
        Database::Entity::Queues::Message entity;
        entity.ern = dto.ern;
        entity.queueErn = dto.queueErn;
        entity.messageId = dto.messageId;
        entity.status = Database::Entity::Queues::MessageStatusFromString(dto.status);
        entity.priority = Database::Entity::Queues::MessagePriorityFromString(dto.priority);
        entity.body = dto.body;
        entity.md5Body = dto.md5Body;
        entity.md5Attributes = dto.md5Attributes;
        entity.receiptHandle = dto.receiptHandle;
        for (const auto &[key, variant]: dto.attributes) {
            entity.attributes[key] = toEntity(variant);
        }
        entity.lastReceived = dto.lastReceived;
        entity.created = dto.created;
        entity.modified = dto.modified;
        return entity;
    }

}// namespace Euclid::Dto::SQS