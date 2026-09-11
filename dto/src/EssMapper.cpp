// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/8/26.
//

#include <euclid/dto/ess/mapper/EssMapper.h>

namespace Euclid::Dto::ESS {

    Secret EssMapper::toDto(const Database::Entity::ESS::Secret &entity) {
        Secret dto;
        dto.name = entity.name;
        dto.ern = entity.ern;
        dto.description = entity.description;
        dto.encryptionKeyErn = entity.encryptionKeyErn;
        dto.version = entity.version;
        dto.rotated = entity.rotated;
        dto.tags = entity.tags;
        dto.created = entity.created;
        dto.modified = entity.modified;
        return dto;
    }

    std::vector<Secret> EssMapper::toDto(const std::vector<Database::Entity::ESS::Secret> &entities) {
        std::vector<Secret> dtos;
        dtos.reserve(entities.size());
        for (const auto &entity: entities) {
            dtos.push_back(toDto(entity));
        }
        return dtos;
    }

}// namespace Euclid::Dto::ESS
