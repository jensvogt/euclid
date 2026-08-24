#include <euclid/database/entity/esm/ObjectStatus.h>
#include <euclid/dto/esm/EsmMapper.h>

namespace Euclid::Dto::ESM {

    Bucket EsmMapper::toDto(const Database::Entity::ESM::Bucket &entity) {
        Bucket dto;
        dto.region = entity.region;
        dto.owner = entity.owner;
        dto.name = entity.name;
        dto.ern = entity.ern;
        dto.size = entity.size;
        dto.objects = entity.objects;
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<Bucket> EsmMapper::toDto(const std::vector<Database::Entity::ESM::Bucket> &entities) {
        std::vector<Bucket> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }


    Object EsmMapper::toDto(const Database::Entity::ESM::Object &entity) {
        Object dto;
        dto.bucketErn = entity.bucketErn;
        dto.ern = entity.ern;
        dto.key = entity.key;
        dto.size = entity.size;
        dto.status = Database::Entity::ESM::ObjectStatusToString(entity.status);
        dto.contentType = entity.contentType;
        dto.md5Sum = entity.md5Sum;
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<Object> EsmMapper::toDto(const std::vector<Database::Entity::ESM::Object> &entities) {
        std::vector<Object> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

}// namespace Euclid::Dto::ESM