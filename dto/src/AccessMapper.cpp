#include <euclid/dto/access/AccessMapper.h>

namespace Euclid::Dto::Access {

    User AccessMapper::toDto(const Database::Entity::Access::User &entity) {
        User dto;
        dto.userId = entity.userId;
        dto.email = entity.email;
        dto.accountId = entity.accountId;
        dto.region = entity.region;
        dto.isAdmin = entity.isAdmin;
        return dto;
    }

    std::vector<User> AccessMapper::toDto(const std::vector<Database::Entity::Access::User> &entities) {
        std::vector<User> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

}// namespace Euclid::Dto::Access