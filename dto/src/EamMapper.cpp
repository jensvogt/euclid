#include <euclid/dto/eam/EamMapper.h>

namespace Euclid::Dto::EAM {

    User EamMapper::toDto(const Database::Entity::EAM::User &entity) {
        User dto;
        dto.userId = entity.userId;
        dto.email = entity.email;
        dto.accountId = entity.accountId;
        dto.region = entity.region;
        dto.isAdmin = entity.isAdmin;
        return dto;
    }

    std::vector<User> EamMapper::toDto(const std::vector<Database::Entity::EAM::User> &entities) {
        std::vector<User> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

}// namespace Euclid::Dto::EAM