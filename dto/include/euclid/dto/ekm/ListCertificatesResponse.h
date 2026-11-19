// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//
// Created by vogje01 on 9/7/26.
//

#pragma once

// C++ includes
#include <string>
#include <vector>

// C++ includes
#include <array>
#include <string_view>

// Euclid includes
#include <euclid/dto/BaseDto.h>
#include <euclid/dto/DescribedJson.h>
#include <euclid/dto/ekm/model/Certificate.h>

namespace Euclid::Dto::EKM {

    using Json::tag_invoke;

    struct ListCertificatesResponse : BaseDto {

        /**
         * @brief Certificates list
         */
        std::vector<Certificate> certificates;

        /**
         * @brief Total number of certificates matching the request, ignoring paging
         */
        long total{};

        /**
         * @brief The members that must be in the JSON: missing or null, reading throws.
         */
        static constexpr std::array<std::string_view, 1> kJsonRequired{"certificates"};

        /**
         * @brief Serializes this response to a JSON string
         */
        [[nodiscard]] std::string toJson() const {
            return boost::json::serialize(boost::json::value_from(*this));
        }
    };

    BOOST_DESCRIBE_STRUCT(ListCertificatesResponse, (BaseDto), (certificates, total))

}// namespace Euclid::Dto::EKM
