#include <euclid/dto/ekm/mapper/EkmMapper.h>

namespace Euclid::Dto::EKM {

    Key EkmMapper::toDto(const Database::Entity::EKM::Key &entity) {
        Key dto;
        dto.ern = entity.ern;
        dto.name = entity.name;
        dto.algorithm = entity.algorithm;
        dto.length = entity.length;
        dto.tags = entity.tags;
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<Key> EkmMapper::toDto(const std::vector<Database::Entity::EKM::Key> &entities) {
        std::vector<Key> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

    Database::Entity::EKM::Key EkmMapper::toEntity(const Key &dto) {
        Database::Entity::EKM::Key entity;
        entity.ern = dto.ern;
        entity.name = dto.name;
        entity.algorithm = dto.algorithm;
        entity.length = dto.length;
        entity.tags = dto.tags;
        entity.created = dto.created;
        entity.modified = dto.modified;
        return entity;
    }

}// namespace Euclid::Dto::EKM