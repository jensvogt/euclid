#include <euclid/dto/storage/StorageMapper.h>

namespace Euclid::Dto::Storage {

    Bucket StorageMapper::toDto(const Database::Entity::Storage::Bucket &entity) {
        Bucket dto;
        dto.region = entity.region;
        dto.owner = entity.owner;
        dto.name = entity.name;
        dto.ern = entity.ern;
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

}// namespace Euclid::Dto::Storage
