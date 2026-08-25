#include <euclid/dto/eam/EamMapper.h>

namespace Euclid::Dto::EAM {

    User EamMapper::toDto(const Database::Entity::EAM::User &entity) {
        User dto;
        dto.userId = entity.userId;
        dto.ern = entity.ern;
        dto.email = entity.email;
        dto.accountId = entity.accountId;
        dto.region = entity.region;
        for (const auto &grant: entity.accountGrants) {
            dto.accountGrants.push_back({.accountId = grant.accountId, .namespaces = grant.namespaces, .isAdmin = grant.isAdmin, .granted = grant.granted});
        }
        dto.created = entity.created;
        dto.modified = entity.modified;
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

    UserGroup EamMapper::toDto(const Database::Entity::EAM::UserGroup &entity) {
        UserGroup dto;
        dto.ern = entity.ern;
        dto.name = entity.name;
        dto.accountId = entity.accountId;
        dto.region = entity.region;
        dto.userIds = entity.userIds;
        dto.description = entity.description;
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<UserGroup> EamMapper::toDto(const std::vector<Database::Entity::EAM::UserGroup> &entities) {
        std::vector<UserGroup> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

    Account EamMapper::toDto(const Database::Entity::EAM::Account &entity) {
        Account dto;
        dto.accountId = entity.accountId;
        dto.name = entity.name;
        dto.ern = entity.ern;
        dto.description = entity.description;
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<Account> EamMapper::toDto(const std::vector<Database::Entity::EAM::Account> &entities) {
        std::vector<Account> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

    Namespace EamMapper::toDto(const Database::Entity::EAM::Namespace &entity) {
        Namespace dto;
        dto.accountId = entity.accountId;
        dto.name = entity.name;
        dto.ern = entity.ern;
        dto.description = entity.description;
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<Namespace> EamMapper::toDto(const std::vector<Database::Entity::EAM::Namespace> &entities) {
        std::vector<Namespace> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }
}// namespace Euclid::Dto::EAM