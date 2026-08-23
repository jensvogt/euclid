#include <euclid/dto/storage/StorageMapper.h>

namespace Euclid::Dto::Storage {

    Bucket StorageMapper::toDto(const Database::Entity::Storage::Bucket &entity) {
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

    std::vector<Bucket> StorageMapper::toDto(const std::vector<Database::Entity::Storage::Bucket> &entities) {
        std::vector<Bucket> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }


    Object StorageMapper::toDto(const Database::Entity::Storage::Object &entity) {
        Object dto;
        dto.bucketErn = entity.bucketErn;
        dto.ern = entity.ern;
        dto.key = entity.key;
        dto.size = entity.size;
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<Object> StorageMapper::toDto(const std::vector<Database::Entity::Storage::Object> &entities) {
        std::vector<Object> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

}// namespace Euclid::Dto::Storage